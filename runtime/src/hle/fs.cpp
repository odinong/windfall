#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "hle/hle.h"
#include "mods/mods.h"

namespace wf::hle {
namespace {

namespace fs = std::filesystem;

enum : int32_t { FS_OK = 0, FS_END = -2, FS_NOT_FOUND = -6, FS_NOT_FILE = -7, FS_NOT_DIR = -8, FS_FATAL = -0x400 };
constexpr uint32_t FLAG_DIR = 0x80000000, FLAG_FILE = 0x01000000;

struct OpenFile { FILE* f; std::string path; fs::path replaces; };
struct OpenDir { std::vector<fs::directory_entry> entries; size_t next = 0; };

std::mutex g_lock;
std::unordered_map<uint32_t, OpenFile> g_files;
std::unordered_map<uint32_t, OpenDir> g_dirs;
uint32_t g_nextHandle = 1;

fs::path map_path(const std::string& guest)
{
    std::string p = guest;
    for (char& c : p) if (c == '\\') c = '/';
    auto strip = [&](const char* prefix) -> std::string {
        size_t n = strlen(prefix);
        if (p.compare(0, n, prefix) == 0 && (p.size() == n || p[n] == '/'))
            return p.size() > n ? p.substr(n + 1) : std::string{};
        return "\x01";
    };
    std::string rest;
    if ((rest = strip("/vol/content")) != "\x01") return mods::resolve_content(rest, config().gameDir / "content" / fs::u8path(rest));
    if ((rest = strip("/vol/code")) != "\x01") return config().gameDir / "code" / fs::u8path(rest);
    if ((rest = strip("/vol/meta")) != "\x01") return config().gameDir / "meta" / fs::u8path(rest);
    if ((rest = strip("/vol/save")) != "\x01") return config().saveDir / fs::u8path(rest);
    if (!p.empty() && p[0] != '/') return config().gameDir / "content" / fs::u8path(p);
    return {};
}

void fill_stat(uint32_t st, const fs::path& p, bool isDir)
{
    memset(gptr(st), 0, 0x64);
    std::error_code ec;
    wr32(st + 0x00, isDir ? FLAG_DIR : FLAG_FILE);
    wr32(st + 0x04, 0x666);
    uint32_t size = isDir ? 0 : (uint32_t)fs::file_size(p, ec);
    wr32(st + 0x10, size);
    wr32(st + 0x14, size);
}

int32_t open_file(const std::string& guestPath, const std::string& mode, uint32_t handleOut)
{
    fs::path host = map_path(guestPath);
    if (host.empty())
    {
        log_warn("FS: unmapped path {}", guestPath);
        return FS_NOT_FOUND;
    }
    std::wstring m(mode.begin(), mode.end());
    if (m.find(L'b') == std::wstring::npos) m += L'b';
    if (m[0] != L'r') fs::create_directories(host.parent_path());
    fs::path replaces;
    if (m[0] == L'w')
    {
        replaces = host;
        host += L".partial";
    }
    FILE* f = _wfopen(host.c_str(), m.c_str());
    if (!f)
    {
        log_cat("fs", "open {} ({}) -> not found", guestPath, mode);
        return FS_NOT_FOUND;
    }
    std::lock_guard lk(g_lock);
    uint32_t h = g_nextHandle++;
    g_files[h] = {f, guestPath, replaces};
    wr32(handleOut, h);
    log_cat("fs", "open {} ({}) -> {}", guestPath, mode, h);
    return FS_OK;
}

int32_t open_dir(const std::string& guestPath, uint32_t handleOut)
{
    fs::path host = map_path(guestPath);
    std::error_code ec;
    if (host.empty() || !fs::is_directory(host, ec)) return FS_NOT_FOUND;
    OpenDir d;
    for (auto& e : fs::directory_iterator(host, ec)) d.entries.push_back(e);
    std::lock_guard lk(g_lock);
    uint32_t h = g_nextHandle++;
    g_dirs[h] = std::move(d);
    wr32(handleOut, h);
    return FS_OK;
}

int32_t stat_path(const std::string& guestPath, uint32_t st)
{
    fs::path host = map_path(guestPath);
    std::error_code ec;
    if (host.empty() || !fs::exists(host, ec)) return FS_NOT_FOUND;
    fill_stat(st, host, fs::is_directory(host, ec));
    return FS_OK;
}

void FSInit() {}
WF_EXPORT(coreinit, FSInit);
void FSShutdown() {}
WF_EXPORT(coreinit, FSShutdown);
int32_t FSAddClient(uint32_t, uint32_t) { return FS_OK; }
WF_EXPORT(coreinit, FSAddClient);
int32_t FSDelClient(uint32_t, uint32_t) { return FS_OK; }
WF_EXPORT(coreinit, FSDelClient);
void FSInitCmdBlock(uint32_t block) { memset(gptr(block), 0, 0xA80); }
WF_EXPORT(coreinit, FSInitCmdBlock);
int32_t FSSetCmdPriority(uint32_t, uint32_t) { return FS_OK; }
WF_EXPORT(coreinit, FSSetCmdPriority);
int32_t FSGetLastError(uint32_t) { return 0; }
WF_EXPORT(coreinit, FSGetLastError);
int32_t FSGetLastErrorCodeForViewer(uint32_t) { return 0; }
WF_EXPORT(coreinit, FSGetLastErrorCodeForViewer);
int32_t FSGetVolumeState(uint32_t) { return 1; }
WF_EXPORT(coreinit, FSGetVolumeState);
void FSSetStateChangeNotification(uint32_t, uint32_t) {}
WF_EXPORT(coreinit, FSSetStateChangeNotification);

int32_t FSGetCwd(uint32_t, uint32_t, uint32_t buf, uint32_t len, uint32_t)
{
    const char cwd[] = "/vol/content";
    if (len < sizeof(cwd)) return FS_FATAL;
    memcpy(gptr(buf), cwd, sizeof(cwd));
    return FS_OK;
}
WF_EXPORT(coreinit, FSGetCwd);

int32_t FSOpenFile(uint32_t, uint32_t, uint32_t path, uint32_t mode, uint32_t handleOut, uint32_t)
{
    return open_file(read_cstr(path), read_cstr(mode), handleOut);
}
WF_EXPORT(coreinit, FSOpenFile);

int32_t FSCloseFile(uint32_t, uint32_t, uint32_t h, uint32_t)
{
    std::lock_guard lk(g_lock);
    auto it = g_files.find(h);
    if (it == g_files.end()) return FS_FATAL;
    bool ok = std::fclose(it->second.f) == 0;
    if (const fs::path& to = it->second.replaces; !to.empty())
    {
        fs::path from = fs::path(to) += L".partial";
        if (!ok || !MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            log_error("FS: could not save {} (error {})", it->second.path, ok ? GetLastError() : 0);
    }
    g_files.erase(it);
    return FS_OK;
}
WF_EXPORT(coreinit, FSCloseFile);

int32_t FSReadFile(uint32_t, uint32_t, uint32_t dst, uint32_t size, uint32_t count, uint32_t h, uint32_t, uint32_t)
{
    FILE* f;
    {
        std::lock_guard lk(g_lock);
        auto it = g_files.find(h);
        if (it == g_files.end()) return FS_FATAL;
        f = it->second.f;
    }
    if (!size) return 0;
    size_t n = std::fread(gptr(dst), 1, (size_t)size * count, f);
    return (int32_t)(n / size);
}
WF_EXPORT(coreinit, FSReadFile);

int32_t FSWriteFile(uint32_t, uint32_t, uint32_t src, uint32_t size, uint32_t count, uint32_t h, uint32_t, uint32_t)
{
    FILE* f;
    {
        std::lock_guard lk(g_lock);
        auto it = g_files.find(h);
        if (it == g_files.end()) return FS_FATAL;
        f = it->second.f;
    }
    if (!size) return 0;
    size_t n = std::fwrite(gptr(src), 1, (size_t)size * count, f);
    std::fflush(f);
    return (int32_t)(n / size);
}
WF_EXPORT(coreinit, FSWriteFile);

int32_t FSSetPosFile(uint32_t, uint32_t, uint32_t h, uint32_t pos, uint32_t)
{
    std::lock_guard lk(g_lock);
    auto it = g_files.find(h);
    if (it == g_files.end()) return FS_FATAL;
    return _fseeki64(it->second.f, pos, SEEK_SET) == 0 ? FS_OK : FS_FATAL;
}
WF_EXPORT(coreinit, FSSetPosFile);

int32_t FSGetStatFile(uint32_t, uint32_t, uint32_t h, uint32_t st, uint32_t)
{
    std::string path;
    {
        std::lock_guard lk(g_lock);
        auto it = g_files.find(h);
        if (it == g_files.end()) return FS_FATAL;
        path = it->second.path;
    }
    return stat_path(path, st);
}
WF_EXPORT(coreinit, FSGetStatFile);

int32_t FSGetStat(uint32_t, uint32_t, uint32_t path, uint32_t st, uint32_t)
{
    int32_t r = stat_path(read_cstr(path), st);
    log_cat("fs", "stat {} -> {}", read_cstr(path), r);
    return r;
}
WF_EXPORT(coreinit, FSGetStat);

int32_t FSOpenDir(uint32_t, uint32_t, uint32_t path, uint32_t handleOut, uint32_t)
{
    return open_dir(read_cstr(path), handleOut);
}
WF_EXPORT(coreinit, FSOpenDir);

int32_t FSReadDir(uint32_t, uint32_t, uint32_t h, uint32_t entry, uint32_t)
{
    std::lock_guard lk(g_lock);
    auto it = g_dirs.find(h);
    if (it == g_dirs.end()) return FS_FATAL;
    OpenDir& d = it->second;
    if (d.next >= d.entries.size()) return FS_END;
    const fs::directory_entry& e = d.entries[d.next++];
    std::error_code ec;
    fill_stat(entry, e.path(), e.is_directory(ec));
    std::string name = e.path().filename().string();
    memset(gptr(entry + 0x64), 0, 0x80);
    memcpy(gptr(entry + 0x64), name.data(), std::min<size_t>(name.size(), 0x7F));
    return FS_OK;
}
WF_EXPORT(coreinit, FSReadDir);

int32_t FSCloseDir(uint32_t, uint32_t, uint32_t h, uint32_t)
{
    std::lock_guard lk(g_lock);
    g_dirs.erase(h);
    return FS_OK;
}
WF_EXPORT(coreinit, FSCloseDir);

std::string save_path(uint32_t slot, uint32_t path)
{
    std::string sub = slot == 0xFF ? "common" : std::format("user{:02X}", slot);
    std::string p = read_cstr(path);
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    return "/vol/save/" + sub + (p.empty() ? "" : "/" + p);
}

int32_t SAVEInit() { fs::create_directories(config().saveDir / "common"); return 0; }
WF_EXPORT(nn_save, SAVEInit);
void SAVEShutdown() {}
WF_EXPORT(nn_save, SAVEShutdown);
int32_t SAVEInitSaveDir(uint32_t slot)
{
    std::error_code ec;
    fs::create_directories(map_path(save_path(slot, 0)), ec);
    return 0;
}
WF_EXPORT(nn_save, SAVEInitSaveDir);
int32_t SAVEFlushQuota(uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
WF_EXPORT(nn_save, SAVEFlushQuota);
int32_t SAVEOpenFile(uint32_t, uint32_t, uint32_t slot, uint32_t path, uint32_t mode, uint32_t handleOut, uint32_t)
{
    return open_file(save_path(slot, path), read_cstr(mode), handleOut);
}
WF_EXPORT(nn_save, SAVEOpenFile);
int32_t SAVEOpenDir(uint32_t, uint32_t, uint32_t slot, uint32_t path, uint32_t handleOut, uint32_t)
{
    return open_dir(save_path(slot, path), handleOut);
}
WF_EXPORT(nn_save, SAVEOpenDir);
int32_t SAVEGetStat(uint32_t, uint32_t, uint32_t slot, uint32_t path, uint32_t st, uint32_t)
{
    return stat_path(save_path(slot, path), st);
}
WF_EXPORT(nn_save, SAVEGetStat);

}
}
