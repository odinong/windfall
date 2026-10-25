#include "mods/mods.h"

#include <cstring>
#include <fstream>
#include <map>
#include <mutex>

#include <windows.h>

#include "core/archive.h"
#include "core/config.h"
#include "core/guest.h"
#include "core/log.h"
#include "gpu/gpu.h"
#include "mods/windfall_mod_api.h"
#include "ui/settings.h"

namespace wf::mods {
namespace fs = std::filesystem;

namespace {

std::once_flag g_once;
std::vector<Mod> g_mods;
std::mutex g_lock;
std::map<std::string, fs::path> g_resolved;
std::vector<std::pair<WindfallFrameCallback, void*>> g_tick;
std::atomic<uint32_t> g_ticks{0};

fs::path mods_dir()
{
    if (const wchar_t* e = _wgetenv(L"WF_MODS_DIR")) return e;
    return config().dataDir / "mods";
}

std::vector<uint8_t> read_file(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

std::string trim(std::string s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

void scan()
{
    std::error_code ec;
    fs::path root = mods_dir();
    if (!fs::exists(root, ec)) return;
    for (const auto& e : fs::directory_iterator(root, ec))
    {
        if (!e.is_directory(ec) || e.path().filename().string().starts_with(".")) continue;
        Mod m;
        m.dir = e.path();
        m.folder = e.path().filename().string();
        m.name = m.folder;
        std::ifstream ini(m.dir / "mod.ini");
        for (std::string line; std::getline(ini, line);)
        {
            size_t eq = line.find('=');
            if (eq == std::string::npos || line[0] == '#' || line[0] == ';') continue;
            std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
            if (k == "name") m.name = v;
            else if (k == "author") m.author = v;
            else if (k == "version") m.version = v;
            else if (k == "description") m.description = v;
        }
        m.hasContent = fs::exists(m.dir / "content", ec);
        m.hasPlugin = fs::exists(m.dir / "plugin.dll", ec);
        g_mods.push_back(std::move(m));
    }
    std::sort(g_mods.begin(), g_mods.end(), [](const Mod& a, const Mod& b) { return a.folder < b.folder; });
    log_info("mods: {} found", g_mods.size());
}

std::vector<const Mod*> enabled()
{
    std::map<std::string, bool> on;
    {
        std::lock_guard lk(settings::lock());
        on = settings::get().mods;
    }
    std::vector<const Mod*> out;
    for (const Mod& m : list())
        if (on.count(m.folder) && on[m.folder]) out.push_back(&m);
    return out;
}

bool is_archive_name(const std::string& n)
{
    std::string l = n;
    for (char& c : l) c = (char)std::tolower((unsigned char)c);
    return l.ends_with(".szs") || l.ends_with(".sarc") || l.ends_with(".pack") || l.ends_with(".arc");
}

std::vector<uint8_t> patch_archive(const std::vector<uint8_t>& original, const std::vector<fs::path>& dirs)
{
    bool compressed = archive::is_yaz0(original);
    archive::Sarc sarc;
    if (!archive::sarc_read(archive::yaz0_decompress(original), sarc)) return original;
    std::map<std::string, std::vector<fs::path>> nested;
    std::error_code ec;
    for (const fs::path& dir : dirs)
        for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            std::string rel = fs::relative(it->path(), dir, ec).generic_string();
            if (it->is_directory(ec))
            {
                if (archive::SarcFile* inner = sarc.find(rel); inner && is_archive_name(rel))
                {
                    nested[rel].push_back(it->path());
                    it.disable_recursion_pending();
                }
                continue;
            }
            std::vector<uint8_t> data = read_file(it->path());
            if (archive::SarcFile* f = sarc.find(rel))
                f->data = std::move(data);
            else
                sarc.files.push_back({rel, std::move(data), 0x2000});
        }
    for (auto& [name, ds] : nested)
        if (archive::SarcFile* f = sarc.find(name)) f->data = patch_archive(f->data, ds);
    std::vector<uint8_t> out = archive::sarc_write(sarc);
    return compressed ? archive::yaz0_compress(out) : out;
}

void api_log(const char* mod, const char* msg) { log_info("[mod {}] {}", mod ? mod : "?", msg ? msg : ""); }
int api_is_mapped(uint32_t a, uint32_t n) { return is_mapped(a, n) ? 1 : 0; }
uint8_t api_r8(uint32_t a) { return is_mapped(a, 1) ? rd8(a) : 0; }
uint16_t api_r16(uint32_t a) { return is_mapped(a, 2) ? rd16(a) : 0; }
uint32_t api_r32(uint32_t a) { return is_mapped(a, 4) ? rd32(a) : 0; }
float api_rf(uint32_t a) { return std::bit_cast<float>(api_r32(a)); }
void api_w8(uint32_t a, uint8_t v) { if (is_mapped(a, 1)) wr8(a, v); }
void api_w16(uint32_t a, uint16_t v) { if (is_mapped(a, 2)) wr16(a, v); }
void api_w32(uint32_t a, uint32_t v) { if (is_mapped(a, 4)) wr32(a, v); }
void api_wf(uint32_t a, float v) { api_w32(a, std::bit_cast<uint32_t>(v)); }
uint8_t* api_ptr(uint32_t a) { return is_mapped(a, 1) ? (uint8_t*)gptr(a) : nullptr; }
void api_on_tick(WindfallFrameCallback cb, void* user)
{
    std::lock_guard lk(g_lock);
    if (cb) g_tick.push_back({cb, user});
}
uint32_t api_frames() { return gpu::frame_count(); }
uint32_t api_ticks() { return g_ticks; }

const WindfallModAPI g_api = {
    WINDFALL_MOD_API_VERSION, api_log,
    api_is_mapped, api_r8, api_r16, api_r32, api_rf, api_w8, api_w16, api_w32, api_wf, api_ptr,
    api_on_tick, api_frames, api_ticks,
    0x1046F0B0, 0x10475A68,
};

}

const std::vector<Mod>& list()
{
    std::call_once(g_once, scan);
    return g_mods;
}

void start()
{
    for (const Mod* m : enabled())
    {
        if (!m->hasPlugin) continue;
        HMODULE dll = LoadLibraryW((m->dir / "plugin.dll").c_str());
        auto init = dll ? (WindfallModInit)GetProcAddress(dll, "windfall_mod_init") : nullptr;
        if (!init)
        {
            log_warn("mods: {}: plugin.dll could not be loaded (or has no windfall_mod_init)", m->folder);
            continue;
        }
        int r = init(&g_api);
        log_info("mods: {}: plugin started (returned {})", m->folder, r);
    }
    for (const Mod* m : enabled()) log_info("mods: {} enabled", m->name);
}

fs::path resolve_content(const std::string& relPath, const fs::path& original)
{
    {
        std::lock_guard lk(g_lock);
        if (auto it = g_resolved.find(relPath); it != g_resolved.end()) return it->second;
    }
    fs::path result = original;
    std::vector<fs::path> patchDirs;
    std::error_code ec;
    for (const Mod* m : enabled())
    {
        if (!m->hasContent) continue;
        fs::path p = m->dir / "content" / fs::u8path(relPath);
        if (fs::is_regular_file(p, ec))
        {
            result = p;
            patchDirs.clear();
        }
        else if (fs::is_directory(p, ec) && fs::is_regular_file(original, ec) && is_archive_name(relPath))
            patchDirs.push_back(p);
    }
    if (!patchDirs.empty())
    {
        fs::path cache = mods_dir() / ".cache";
        fs::create_directories(cache, ec);
        std::string key = relPath;
        for (char& c : key) if (c == '/' || c == '\\' || c == ':') c = '_';
        fs::path out = cache / key;
        std::vector<uint8_t> patched = patch_archive(read_file(result), patchDirs);
        std::ofstream(out, std::ios::binary).write((const char*)patched.data(), (std::streamsize)patched.size());
        log_info("mods: {} rebuilt with {} mod folder(s)", relPath, patchDirs.size());
        result = out;
    }
    else if (result != original)
        log_info("mods: {} replaced", relPath);
    std::lock_guard lk(g_lock);
    g_resolved[relPath] = result;
    return result;
}

void tick()
{
    g_ticks++;
    std::vector<std::pair<WindfallFrameCallback, void*>> cbs;
    {
        std::lock_guard lk(g_lock);
        cbs = g_tick;
    }
    for (auto& [cb, user] : cbs) cb(user);
}

}
