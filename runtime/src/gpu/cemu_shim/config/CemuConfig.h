#pragma once
#include "config/ConfigValue.h"

enum UpscalingFilter { kLinearFilter, kBicubicFilter, kBicubicHermiteFilter, kNearestNeighborFilter };
enum FullscreenScaling { kKeepAspectRatio, kStretch };
enum class ScreenPosition { kDisabled = 0, kTopLeft, kTopCenter, kTopRight, kBottomLeft, kBottomCenter, kBottomRight };
enum GraphicAPI { kOpenGL = 0, kVulkan, kMetal };
enum AudioChannels { kMono = 0, kStereo, kSurround };

struct CemuConfig
{
    ConfigValue<bool> play_boot_sound{false};
    std::array<uint8, 16> vk_graphic_device_uuid{};
    ConfigValue<int> vsync{1};
    ConfigValue<bool> async_compile{true};
    ConfigValue<float> userDisplayGamma{2.2f};
    ConfigValue<bool> vk_accurate_barriers{true};
    struct
    {
        ScreenPosition position = ScreenPosition::kDisabled;
        uint32 text_color = 0xFFFFFFFF;
        sint32 text_scale = 100;
        bool fps = true;
        bool drawcalls = false;
        bool cpu_usage = false;
        bool cpu_per_core_usage = false;
        bool ram_usage = false;
        bool vram_usage = false;
        bool debug = false;
    } overlay{};
    struct
    {
        ScreenPosition position = ScreenPosition::kTopLeft;
        uint32 text_color = 0xFFFFFFFF;
        sint32 text_scale = 100;
        bool controller_profiles = false;
        bool controller_battery = false;
        bool shader_compiling = false;
        bool friends = false;
    } notification{};
    ConfigValue<sint32> upscale_filter{kBicubicFilter};
    ConfigValue<sint32> downscale_filter{kLinearFilter};
    ConfigValue<sint32> fullscreen_scaling{kKeepAspectRatio};

    std::atomic_bool gamepad_overlay{false};

    AudioChannels tv_channels = kStereo, pad_channels = kStereo;
    sint32 tv_volume = 100, pad_volume = 0;
    static int AudioChannelsToNChannels(AudioChannels c) { return c == kMono ? 1 : c == kSurround ? 6 : 2; }
};

inline CemuConfig& GetConfig()
{
    static CemuConfig config;
    return config;
}
