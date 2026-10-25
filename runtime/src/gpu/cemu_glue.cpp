#include "core/cemu_install.h"
#include "Common/precompiled.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/Filesystem/fsc.h"
#include "config/ActiveSettings.h"

#include <cstdio>

#include "core/config.h"
#include "core/log.h"

namespace wf {

void cemu_log_line(const std::string& s) { log_cat("cemu", "{}", s); }
bool cemu_log_all()
{
    static const bool all = std::getenv("WF_CEMU_LOG") != nullptr;
    return all;
}
void cemu_fatal(const char* what) { fatal("{}", what); }

const fs::path& user_data_dir()
{
    static const fs::path dir = std::getenv("WF_CACHE_DIR") ? fs::path(std::getenv("WF_CACHE_DIR")) : config().dataDir;
    return dir;
}

}

std::string CafeSystem::GetMlcStoragePath(uint64)
{
    return _pathToUtf8(wf::config().gameDir);
}

struct FSCVirtualFile
{
    FILE* fp = nullptr;
    uint64 size = 0;
};

FSCVirtualFile* fsc_open(const char* path, FSC_ACCESS_FLAG accessFlags, sint32* fscStatus, sint32)
{
    if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::WRITE_PERMISSION) || !HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE))
    {
        *fscStatus = FSC_STATUS_FILE_NOT_FOUND;
        return nullptr;
    }
    FILE* fp = _wfopen(_utf8ToPath(path).c_str(), L"rb");
    if (!fp)
    {
        *fscStatus = FSC_STATUS_FILE_NOT_FOUND;
        return nullptr;
    }
    _fseeki64(fp, 0, SEEK_END);
    auto* f = new FSCVirtualFile{fp, (uint64)_ftelli64(fp)};
    _fseeki64(fp, 0, SEEK_SET);
    *fscStatus = FSC_STATUS_OK;
    return f;
}

void fsc_close(FSCVirtualFile* file)
{
    if (!file) return;
    std::fclose(file->fp);
    delete file;
}

uint32 fsc_getFileSize(FSCVirtualFile* file) { return (uint32)file->size; }
uint32 fsc_readFile(FSCVirtualFile* file, void* buffer, uint32 size) { return (uint32)std::fread(buffer, 1, size, file->fp); }
uint32 fsc_setFileSeek(FSCVirtualFile* file, uint64 seek) { return _fseeki64(file->fp, (sint64)seek, SEEK_SET) == 0 ? FSC_STATUS_OK : FSC_STATUS_UNDEFINED; }
uint64 fsc_getFileSeek(FSCVirtualFile* file) { return (uint64)_ftelli64(file->fp); }
bool fsc_doesFileExist(const char* path, sint32)
{
    std::error_code ec;
    return fs::is_regular_file(_utf8ToPath(path), ec);
}

std::atomic_bool g_isGPUInitFinished = false;

uint64 PPCTimer_getRawTsc() { return __rdtsc(); }

std::mutex g_friend_notification_mutex;
std::vector<std::pair<std::string, int>> g_friend_notifications;

static std::optional<fs::path> find_cafe_font()
{
    std::error_code ec;
    if (auto cemu = wf::cemu_install_dir(); cemu && fs::exists(*cemu / "resources" / "sharedFonts" / "CafeStd.ttf", ec))
        return *cemu / "resources" / "sharedFonts" / "CafeStd.ttf";
    return std::nullopt;
}

uint8* extractCafeDefaultFont(sint32* size)
{
    static std::vector<uint8> font;
    if (font.empty())
    {
        if (auto cafe = find_cafe_font())
        {
            std::ifstream f(*cafe, std::ios::binary);
            font.assign(std::istreambuf_iterator<char>(f), {});
            if (!font.empty()) wf::log_info("UI font: {}", cafe->string());
        }
        if (font.empty())
        {
            wchar_t winDir[MAX_PATH];
            GetWindowsDirectoryW(winDir, MAX_PATH);
            std::ifstream f(fs::path(winDir) / "Fonts" / "segoeui.ttf", std::ios::binary);
            font.assign(std::istreambuf_iterator<char>(f), {});
            if (font.empty())
                wf::fatal("Could not load the Windows UI font (Fonts/segoeui.ttf)");
        }
    }
    *size = (sint32)font.size();
    return font.data();
}

void mic_updateOnAXFrame() {}
