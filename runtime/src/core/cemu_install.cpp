#include "core/cemu_install.h"

#include <cstdlib>
#include <mutex>
#include <vector>

#include "core/log.h"

namespace wf {
namespace fs = std::filesystem;

namespace {

bool looks_like_cemu(const fs::path& dir)
{
    std::error_code ec;
    return fs::exists(dir / "resources" / "sharedFonts", ec) || fs::exists(dir / "Cemu.exe", ec);
}

std::optional<fs::path> search()
{
    std::vector<fs::path> roots;
    if (const wchar_t* e = _wgetenv(L"WF_CEMU_DIR")) roots.emplace_back(e);
    if (const wchar_t* p = _wgetenv(L"ProgramFiles")) roots.push_back(fs::path(p) / "Cemu");
    if (const wchar_t* a = _wgetenv(L"APPDATA")) roots.push_back(fs::path(a) / "Cemu");
    if (const wchar_t* u = _wgetenv(L"USERPROFILE"))
        for (const char* d : {"Downloads", "Desktop", "Documents", "Games"}) roots.push_back(fs::path(u) / d);
    std::error_code ec;
    for (const fs::path& root : roots)
    {
        if (looks_like_cemu(root)) return root;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec))
        {
            if (it.depth() >= 2) { it.disable_recursion_pending(); continue; }
            if (it->is_directory(ec) && looks_like_cemu(it->path())) return it->path();
        }
        ec.clear();
    }
    return std::nullopt;
}

}

std::optional<fs::path> cemu_install_dir()
{
    static std::once_flag once;
    static std::optional<fs::path> dir;
    std::call_once(once, [] {
        dir = search();
        if (dir) log_info("Cemu installation: {}", dir->string());
    });
    return dir;
}

}
