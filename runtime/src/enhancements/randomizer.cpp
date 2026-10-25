#include "enhancements/randomizer.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <fstream>
#include <mutex>

#include <sstream>
#include <thread>

#include "core/config.h"
#include "core/log.h"
#include "enhancements/randomizer_defaults.h"
#include "ui/settings.h"

namespace wf::rando {
namespace fs = std::filesystem;

namespace {

std::mutex g_lock;
Status g_status;

fs::path exe_dir()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

fs::path generator_dir() { return exe_dir() / "randomizer"; }

std::string read_text(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string yaml_value(const std::string& v)
{
    if (v.empty()) return "\"\"";
    if (v.find_first_of(":#'\"{}[],&*!|>%@`") != std::string::npos || v.front() == ' ' || v.back() == ' ')
    {
        std::string q = "\"";
        for (char c : v) q += c == '"' ? std::string("\\\"") : std::string(1, c);
        return q + "\"";
    }
    return v;
}

std::string set_keys(std::string doc, const std::map<std::string, std::string>& values)
{
    for (const auto& [k, v] : values)
    {
        std::string repl = k + ": " + yaml_value(v);
        size_t at = doc.rfind(k + ":", 0) == 0 ? 0 : doc.find("\n" + k + ":");
        if (at == std::string::npos)
        {
            doc += repl + "\n";
            continue;
        }
        if (at) at++;
        size_t end = doc.find('\n', at);
        doc.replace(at, end == std::string::npos ? std::string::npos : end - at, repl);
    }
    return doc;
}

std::string key_value(const std::string& doc, const std::string& key)
{
    size_t at = doc.rfind(key + ": ", 0) == 0 ? 0 : doc.find("\n" + key + ": ");
    if (at == std::string::npos) return {};
    at = doc.find(": ", at) + 2;
    size_t end = doc.find_first_of("\r\n", at);
    return doc.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

std::string utf8(const fs::path& p)
{
    std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

void set_status(bool running, bool ok, std::string message)
{
    std::lock_guard lk(g_lock);
    g_status.running = running;
    g_status.ok = ok;
    g_status.message = std::move(message);
}

std::string seed_of_game()
{
    fs::path log = spoiler_log();
    if (log.empty()) return {};
    return key_value(read_text(log), "seed");
}

void run_generator(std::string seed, std::map<std::string, std::string> options)
{
    std::error_code ec;
    fs::path work = work_dir(), out = work / "game";
    fs::create_directories(work, ec);
    fs::path srcData = generator_dir() / "data";
    if (!fs::exists(work / "data" / "logic" / "world.yaml") ||
        fs::last_write_time(srcData / "logic" / "world.yaml", ec) > fs::last_write_time(work / "data" / "logic" / "world.yaml", ec))
    {
        set_status(true, false, "Preparing the randomizer...");
        fs::remove_all(work / "data", ec);
        fs::copy(srcData, work / "data", fs::copy_options::recursive, ec);
        if (ec)
        {
            set_status(false, false, "Couldn't copy the randomizer's data: " + ec.message());
            return;
        }
    }
    fs::path original;
    {
        std::lock_guard lk(settings::lock());
        original = fs::u8path(settings::get().gameDir);
    }
    if (original.empty()) original = config().gameDir;
    std::string prefs = std::string(kDefaultPreferences) + "gameBaseDir: " + yaml_value(utf8(original)) + "\n" +
                        "outputDir: " + yaml_value(utf8(out)) + "\nplandomizerFile: \"\"\n";
    std::map<std::string, std::string> prefKeys, cfgKeys = options;
    for (const char* k : {"pig_color", "target_type", "camera", "first_person_camera", "gyroscope", "ui_display", "quiet_swift_sail",
                          "player_in_casual_clothes"})
        if (auto it = cfgKeys.find(k); it != cfgKeys.end())
        {
            prefKeys[k] = it->second;
            cfgKeys.erase(it);
        }
    cfgKeys["seed"] = seed.empty() ? "Windfall" : seed;
    std::ofstream(work / "preferences.yaml", std::ios::binary) << set_keys(prefs, prefKeys);
    std::ofstream(work / "config.yaml", std::ios::binary) << set_keys(kDefaultConfig, cfgKeys);

    set_status(true, false, "Generating the seed (this takes a minute)...");
    fs::path exe = generator_dir() / "wwhd_rando.exe", logPath = work / "generator.log";
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE logFile = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = logFile;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, work.c_str(), &si, &pi))
    {
        CloseHandle(logFile);
        set_status(false, false, "Couldn't start the randomizer (" + exe.string() + ")");
        return;
    }
    for (;;)
    {
        DWORD r = WaitForSingleObject(pi.hProcess, 500);
        std::string log = read_text(logPath);
        size_t end = log.find_last_not_of("\r\n");
        if (end != std::string::npos)
        {
            size_t start = log.find_last_of('\n', end);
            std::string last = log.substr(start == std::string::npos ? 0 : start + 1, end - (start == std::string::npos ? 0 : start + 1) + 1);
            if (!last.empty()) set_status(true, false, last);
        }
        if (r == WAIT_OBJECT_0) break;
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(logFile);
    std::string log = read_text(logPath);
    bool ok = fs::exists(out / "code" / "cking.rpx") && log.find("An error has occured") == std::string::npos;
    if (!ok)
    {
        std::string err = read_text(work / "Error Log.txt");
        log_error("randomizer: generation failed:\n{}\n{}", log, err);
        size_t nl = err.find_last_not_of("\r\n");
        std::string msg = err.empty() ? "see " + logPath.string() : err.substr(0, std::min<size_t>(nl + 1, 240));
        set_status(false, false, "Generation failed: " + msg);
        return;
    }
    {
        std::lock_guard lk(g_lock);
        g_status.seed = seed_of_game();
    }
    log_info("randomizer: generated seed {}", seed);
    set_status(false, true, "Seed ready. Restart Windfall to play it.");
}

}

fs::path work_dir() { return config().dataDir / "randomizer"; }

bool enabled()
{
    std::lock_guard lk(settings::lock());
    return settings::get().randoEnabled;
}

fs::path game_dir()
{
    if (const wchar_t* e = _wgetenv(L"WF_RANDO_GAME"); e && *e) return e;
    if (!enabled()) return {};
    fs::path g = work_dir() / "game";
    std::error_code ec;
    return fs::exists(g / "code" / "cking.rpx", ec) ? g : fs::path{};
}

bool g_playing = false;

void start()
{
    fs::path g = game_dir();
    g_playing = !g.empty();
    {
        std::lock_guard lk(g_lock);
        g_status.seed = seed_of_game();
    }
    if (!g.empty()) log_info("randomizer: playing seed {} from {}", status().seed, g.string());
}

bool playing() { return g_playing; }

void restart_game()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        log_info("randomizer: restarting Windfall");
        std::_Exit(0);
    }
    log_error("randomizer: couldn't restart Windfall");
}

void open_spoiler_log()
{
    fs::path log = spoiler_log();
    if (!log.empty()) ShellExecuteW(nullptr, L"open", log.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool generator_available()
{
    std::error_code ec;
    return fs::exists(generator_dir() / "wwhd_rando.exe", ec) && fs::exists(generator_dir() / "data" / "logic" / "world.yaml", ec);
}

void generate(const std::string& seed, const std::map<std::string, std::string>& options)
{
    {
        std::lock_guard lk(g_lock);
        if (g_status.running) return;
        g_status.running = true;
        g_status.ok = false;
        g_status.message = "Starting...";
    }
    std::thread(run_generator, seed, options).detach();
}

Status status()
{
    std::lock_guard lk(g_lock);
    return g_status;
}

fs::path spoiler_log()
{
    std::error_code ec;
    fs::path best;
    fs::file_time_type bestTime{};
    for (const auto& e : fs::directory_iterator(work_dir() / "logs", ec))
    {
        std::string n = e.path().filename().string();
        if (n.ends_with(" Spoiler Log.txt") && n.find("Non-Spoiler") == std::string::npos && e.last_write_time(ec) > bestTime)
        {
            best = e.path();
            bestTime = e.last_write_time(ec);
        }
    }
    return best;
}

}
