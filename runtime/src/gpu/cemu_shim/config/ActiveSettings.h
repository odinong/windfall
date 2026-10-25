#pragma once
#include "config/CemuConfig.h"

namespace wf { const fs::path& user_data_dir(); }

class ActiveSettings
{
    template<typename... TArgs> static fs::path GetPath(const fs::path& path, std::string_view format, TArgs&&... args)
    {
        return path / _utf8ToPath(fmt::format(fmt::runtime(format), std::forward<TArgs>(args)...));
    }
    static fs::path GetPath(const fs::path& path) { return path; }
public:
    template<typename... TArgs> static fs::path GetUserDataPath(TArgs&&... args) { return GetPath(wf::user_data_dir(), std::forward<TArgs>(args)...); }
    template<typename... TArgs> static fs::path GetCachePath(TArgs&&... args) { return GetPath(wf::user_data_dir(), std::forward<TArgs>(args)...); }
    static bool WaitForGX2DrawDoneEnabled() { return true; }
    static bool DumpShadersEnabled() { static const bool on = std::getenv("WF_DUMPSHADERS") != nullptr; return on; }
    static bool DumpTexturesEnabled() { return false; }
    static bool ShaderPreventInfiniteLoopsEnabled() { return false; }
    static bool ForceSamplerRoundToPrecision() { return false; }
    static bool FlushGPUCacheOnSwap() { return true; }
    static bool RenderUpsideDownEnabled() { return false; }
    static float GetTVGamma() { return 2.2f; }
    static float GetDRCGamma() { return 2.2f; }
    static uint32 GetPersistentId() { return 0x80000001; }
    static GraphicAPI GetGraphicsAPI() { return kVulkan; }
    static bool AudioOutputOnlyAux() { return false; }
};
