#include "core/setup.h"

#include <fstream>
#include <regex>
#include <vector>

#include <windows.h>
#include <bcrypt.h>
#include <shobjidl.h>

#include "core/log.h"
#include "ui/settings.h"

namespace wf::setup {
namespace fs = std::filesystem;

namespace {

constexpr const char* kTitleId = "0005000010143500";
constexpr const char* kRpxSha1 = "bdcbc4f815ae4c9b0a1f4a16238f96c197b38239";

std::string sha1_file(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return {};
    BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    std::vector<char> buf(1 << 20);
    while (f)
    {
        f.read(buf.data(), buf.size());
        if (f.gcount() > 0) BCryptHashData(hash, (PUCHAR)buf.data(), (ULONG)f.gcount(), 0);
    }
    unsigned char digest[20];
    BCryptFinishHash(hash, digest, 20, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    std::string hex;
    for (unsigned char b : digest) hex += std::format("{:02x}", b);
    return hex;
}

std::string meta_field(const std::string& xml, const char* name)
{
    std::smatch m;
    std::regex rx(std::string("<") + name + "[^>]*>([^<]*)<");
    return std::regex_search(xml, m, rx) ? m[1].str() : std::string();
}

}

fs::path locate(const fs::path& chosen)
{
    std::error_code ec;
    if (fs::exists(chosen / "code" / "cking.rpx", ec)) return chosen;
    for (const auto& e : fs::directory_iterator(chosen, ec))
        if (e.is_directory(ec) && fs::exists(e.path() / "code" / "cking.rpx", ec)) return e.path();
    return chosen;
}

fs::path pick_folder(HWND owner)
{
    fs::path result;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
    {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"Windfall: choose your Wind Waker HD dump (the folder with code, content and meta)");
        if (SUCCEEDED(dlg->Show(owner)))
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)))
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                {
                    result = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    return result;
}

fs::path setup_screen(const fs::path& start);

std::string verify(const fs::path& dir)
{
    std::error_code ec;
    if (!fs::exists(dir / "code" / "cking.rpx", ec))
    {
        if (fs::exists(dir, ec))
            for (const char* image : {".wud", ".wux", ".iso"})
                for (const auto& e : fs::directory_iterator(dir, ec))
                    if (e.path().extension() == image)
                        return "This folder has a disc image (" + std::string(image) + "). Windfall needs the decrypted dump: the folder with code, content and meta "
                               "(for example from Cemu's title installer, or dumped with dumpling).";
        return "This isn't a Wind Waker HD dump: there is no code\\cking.rpx in it. Choose the folder that contains the code, content and meta folders.";
    }
    std::ifstream mf(dir / "meta" / "meta.xml");
    std::string xml((std::istreambuf_iterator<char>(mf)), {});
    std::string titleId = meta_field(xml, "title_id"), version = meta_field(xml, "title_version"), product = meta_field(xml, "product_code");
    if (!titleId.empty() && titleId != kTitleId)
    {
        if (titleId == "0005000010143400") return "This is the Japanese release (WUP-P-BCZJ). Windfall supports the USA release (WUP-P-BCZE) only.";
        if (titleId == "0005000010143600") return "This is the European release (WUP-P-BCZP). Windfall supports the USA release (WUP-P-BCZE) only.";
        if (titleId.starts_with("0005000E")) return "This is an update or DLC package, not the game. Choose the game's own folder (title 0005000010143500).";
        return "This is a different title (" + titleId + "). Windfall runs The Wind Waker HD, USA (0005000010143500).";
    }
    std::string sha = sha1_file(dir / "code" / "cking.rpx");
    if (sha != kRpxSha1)
        return "code\\cking.rpx doesn't match the supported revision (USA v0" + std::string(version.empty() ? "" : ", this copy reports version " + version) +
               "). Windfall is built for that exact executable; an updated or modified copy won't work. (SHA-1 " + sha + ")";
    return {};
}

fs::path find_game_dir(const fs::path& fromCommandLine)
{
    std::string saved;
    {
        std::lock_guard lk(settings::lock());
        saved = settings::get().gameDir;
    }
    std::vector<fs::path> candidates;
    if (!fromCommandLine.empty()) candidates.push_back(fromCommandLine);
    if (!saved.empty()) candidates.push_back(fs::path(std::u8string((const char8_t*)saved.c_str())));
    for (const fs::path& c : candidates)
    {
        fs::path dir = locate(c);
        std::string why = verify(dir);
        if (why.empty()) return dir;
        log_warn("game folder {}: {}", dir.string(), why);
        if (!fromCommandLine.empty() && c == fromCommandLine && std::getenv("WF_NO_MSGBOX")) return {};
    }
    if (std::getenv("WF_NO_MSGBOX")) return {};
    fs::path dir = setup_screen(saved.empty() ? fs::path() : fs::path(std::u8string((const char8_t*)saved.c_str())));
    if (dir.empty()) return {};
    {
        std::lock_guard lk(settings::lock());
        std::u8string u8 = dir.u8string();
        settings::get().gameDir.assign((const char*)u8.data(), u8.size());
    }
    settings::save();
    log_info("game folder: {} (verified)", dir.string());
    return dir;
}

}
