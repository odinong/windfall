#include "core/log.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

#include <windows.h>

namespace wf {

namespace {

std::mutex s_lock;
FILE* s_file = nullptr;
std::set<std::string, std::less<>> s_quiet;
auto s_start = std::chrono::steady_clock::now();

void init_once()
{
    static bool done = false;
    if (done) return;
    done = true;
    s_file = std::fopen("windfall.log", "w");
    if (!s_file)
        if (const wchar_t* appdata = _wgetenv(L"APPDATA"))
        {
            std::wstring dir = std::wstring(appdata) + L"\\WWHDRecomp";
            CreateDirectoryW(dir.c_str(), nullptr);
            s_file = _wfopen((dir + L"\\windfall.log").c_str(), L"w");
        }
    if (const char* q = std::getenv("WF_LOG_QUIET"))
    {
        std::string_view s(q);
        while (!s.empty())
        {
            size_t c = s.find(',');
            s_quiet.emplace(s.substr(0, c));
            s = c == std::string_view::npos ? std::string_view{} : s.substr(c + 1);
        }
    }
}

}

bool log_enabled(std::string_view category)
{
    std::lock_guard lk(s_lock);
    init_once();
    return s_quiet.find(category) == s_quiet.end();
}

void log_write(LogLevel level, std::string_view category, std::string_view msg)
{
    std::lock_guard lk(s_lock);
    init_once();
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count();
    const char* lv = level == LogLevel::Warn ? "W " : level == LogLevel::Error ? "E " : "";
    std::string line = category.empty() ? std::format("[{:9.3f}] {}{}\n", t, lv, msg)
                                        : std::format("[{:9.3f}] {}[{}] {}\n", t, lv, category, msg);
    std::fputs(line.c_str(), stderr);
    if (s_file)
    {
        std::fputs(line.c_str(), s_file);
        std::fflush(s_file);
    }
}

void fatal_message(std::string_view msg)
{
    log_write(LogLevel::Error, "fatal", msg);
    if (!std::getenv("WF_NO_MSGBOX"))
        MessageBoxA(nullptr, std::string(msg).c_str(), "Windfall", MB_ICONERROR);
    std::_Exit(1);
}

}
