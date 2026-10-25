#include "core/game_build.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <format>
#include <fstream>
#include <thread>
#include <vector>

#include <windows.h>

#include "core/game_module.h"
#include "core/log.h"

namespace wf::game_build {
namespace fs = std::filesystem;
namespace {

fs::path exe_dir()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

fs::path toolchain() { return exe_dir() / "toolchain"; }

std::wstring quote(const fs::path& p) { return L"\"" + p.wstring() + L"\""; }

HANDLE spawn(const fs::path& exe, const std::wstring& args, const fs::path& log)
{
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return nullptr;
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out;
    si.hStdError = out;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = quote(exe) + L" " + args;
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                             exe.parent_path().c_str(), &si, &pi);
    CloseHandle(out);
    if (!ok) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

std::string tail(const fs::path& log, int lines = 12)
{
    std::ifstream f(log);
    std::deque<std::string> last;
    for (std::string l; std::getline(f, l);)
    {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        last.push_back(l);
        if ((int)last.size() > lines) last.pop_front();
    }
    std::string s;
    for (auto& l : last) s += l + "\n";
    return s;
}

int run(const fs::path& exe, const std::wstring& args, const fs::path& log, Progress& p)
{
    HANDLE h = spawn(exe, args, log);
    if (!h) return -1;
    while (WaitForSingleObject(h, 100) == WAIT_TIMEOUT)
        if (p.cancel)
        {
            TerminateProcess(h, 1);
            WaitForSingleObject(h, INFINITE);
            CloseHandle(h);
            return -1;
        }
    DWORD code = 1;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return (int)code;
}

int parallel_jobs()
{
    MEMORYSTATUSEX m{sizeof m};
    GlobalMemoryStatusEx(&m);
    uint64_t gb = m.ullAvailPhys >> 30;
    int byMemory = (int)std::max<uint64_t>(1, gb > 2 ? gb - 1 : 1);
    return std::clamp(std::min((int)std::thread::hardware_concurrency(), byMemory), 1, 32);
}

void fail(Progress& p, std::string why)
{
    std::lock_guard lk(p.lock);
    p.error = std::move(why);
    p.failed = true;
}

void steps(const fs::path& gameDir, Progress& p)
{
    using clock = std::chrono::steady_clock;
    fs::path out = game_module::built_dir(), work = out / "work", src = work / "src", obj = work / "obj";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(src, ec);
    fs::create_directories(obj, ec);
    fs::path tc = toolchain();
    fs::path python = tc / "python" / "python.exe", clang = tc / "llvm" / "clang.exe", lld = tc / "llvm" / "lld-link.exe";
    log_info("building the game module in {} ({} compile jobs)", out.string(), parallel_jobs());

    auto t0 = clock::now();
    fs::path rpx = gameDir / "code" / "cking.rpx";
    int code = run(python, L"-I " + quote(tc / "recomp" / "tools" / "recomp" / "recomp.py") + L" " + quote(rpx) + L" --out " + quote(src),
                   out / "translate.log", p);
    p.translateSeconds = std::chrono::duration<double>(clock::now() - t0).count();
    if (p.cancel) return fail(p, "Cancelled.");
    if (code != 0) return fail(p, "Translating the game's code failed:\n" + tail(out / "translate.log"));

    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(src))
        if (e.path().extension() == ".cpp") files.push_back(e.path());
    std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return fs::file_size(a) > fs::file_size(b); });
    p.total = (int)files.size();
    p.stage = 1;
    const std::wstring flags =
        L"--target=x86_64-pc-windows-msvc -std=c++20 -O2 -ffreestanding -nostdinc -fno-exceptions -fno-rtti -mno-stack-arg-probe "
        L"-mfma -ffp-contract=off -fno-strict-aliasing -DWF_FREESTANDING=1 -w -I" + quote(tc / "recomp" / "runtime" / "include");
    struct Job { HANDLE h; fs::path file; };
    std::vector<Job> running;
    size_t next = 0;
    int jobs = parallel_jobs();
    std::string error;
    while ((next < files.size() || !running.empty()) && error.empty() && !p.cancel)
    {
        while (running.size() < (size_t)jobs && next < files.size())
        {
            fs::path f = files[next++];
            fs::path o = obj / f.filename().replace_extension(".obj");
            HANDLE h = spawn(clang, flags + L" -c " + quote(f) + L" -o " + quote(o), fs::path(o).replace_extension(".log"));
            if (!h)
            {
                error = "couldn't start the compiler (" + clang.string() + ")";
                break;
            }
            running.push_back({h, f});
        }
        std::vector<HANDLE> hs;
        for (auto& j : running) hs.push_back(j.h);
        if (hs.empty()) break;
        DWORD r = WaitForMultipleObjects((DWORD)hs.size(), hs.data(), FALSE, 200);
        if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + hs.size())
        {
            Job j = running[r - WAIT_OBJECT_0];
            running.erase(running.begin() + (r - WAIT_OBJECT_0));
            DWORD c = 1;
            GetExitCodeProcess(j.h, &c);
            CloseHandle(j.h);
            if (c != 0)
                error = std::format("Compiling {} failed:\n{}", j.file.filename().string(),
                                    tail(obj / j.file.filename().replace_extension(".log")));
            p.done++;
        }
    }
    for (auto& j : running)
    {
        TerminateProcess(j.h, 1);
        WaitForSingleObject(j.h, INFINITE);
        CloseHandle(j.h);
    }
    if (p.cancel) return fail(p, "Cancelled.");
    if (!error.empty()) return fail(p, error);

    p.stage = 2;
    fs::path rsp = work / "link.rsp";
    {
        std::ofstream r(rsp);
        r << "-dll -noentry -nodefaultlib -machine:x64 -out:\"" << (work / "windfall_game.dll").string() << "\"\n";
        for (auto& f : files) r << "\"" << (obj / f.filename().replace_extension(".obj")).string() << "\"\n";
    }
    code = run(lld, L"@" + quote(rsp), out / "link.log", p);
    if (p.cancel) return fail(p, "Cancelled.");
    if (code != 0) return fail(p, "Linking the game code failed:\n" + tail(out / "link.log"));
    fs::rename(work / "windfall_game.dll", out / "windfall_game.dll", ec);
    if (ec) return fail(p, "Couldn't move the built game code into place: " + ec.message());
    log_info("game module built: translated in {:.0f}s, total {:.0f}s", p.translateSeconds.load(),
             std::chrono::duration<double>(clock::now() - t0).count());
    p.stage = 3;
}

void build(fs::path gameDir, Progress& p)
{
    steps(gameDir, p);
    std::error_code ec;
    fs::remove_all(game_module::built_dir() / "work", ec);
    p.finished = true;
}

}

bool available()
{
    fs::path tc = toolchain();
    return fs::exists(tc / "python" / "python.exe") && fs::exists(tc / "llvm" / "clang.exe") && fs::exists(tc / "llvm" / "lld-link.exe") &&
           fs::exists(tc / "recomp" / "tools" / "recomp" / "recomp.py");
}

void start(const fs::path& gameDir, Progress& p)
{
    std::thread([gameDir, &p] { build(gameDir, p); }).detach();
}

}
