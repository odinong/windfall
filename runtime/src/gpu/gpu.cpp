#include "Common/precompiled.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/OS/libs/TCL/TCL.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "WindowSystem.h"
#include "config/CemuConfig.h"

#include <miniz.h>

#include "core/guest.h"
#include "core/log.h"
#include "gpu/gpu.h"

void Latte_Start();
namespace wf::game { extern std::atomic<int> g_testMode; }
void wf_gpu_dump_request(const std::string& dir, uint32 first, uint32 count);

#include <ctime>
#include <fstream>
#include <filesystem>

#include "ui/menu.h"

namespace wf::gpu {

namespace {

void start_screenshot_hook()
{
    if (const char* at = std::getenv("WF_CAPTURE_AT"))
    {
        uint32 frame = (uint32)std::strtoul(at, nullptr, 10);
        std::thread([frame] {
            while (LatteGPUState.frameCounter < frame) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            capture_frames();
        }).detach();
    }
    const char* list = std::getenv("WF_SHOT");
    if (!list) return;
    std::vector<uint32> frames;
    for (const char* p = list; *p;)
    {
        frames.push_back((uint32)std::strtoul(p, (char**)&p, 10));
        while (*p == ',' || *p == ' ') p++;
    }
    std::sort(frames.begin(), frames.end());
    std::string dir = std::getenv("WF_SHOT_DIR") ? std::getenv("WF_SHOT_DIR") : ".";
    std::thread([frames, dir] {
        for (uint32 target : frames)
        {
            while (LatteGPUState.frameCounter < target)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            uint32 frameNo = LatteGPUState.frameCounter;
            g_renderer->RequestScreenshot([dir, frameNo](const std::vector<uint8>& rgb, int w, int h, bool mainWindow) -> std::optional<std::string> {
                size_t pngSize = 0;
                void* png = tdefl_write_image_to_png_file_in_memory(rgb.data(), w, h, 3, &pngSize);
                std::string path = std::format("{}/shot_{:05}.png", dir, frameNo);
                if (FILE* fp = std::fopen(path.c_str(), "wb"))
                {
                    std::fwrite(png, 1, pngSize, fp);
                    std::fclose(fp);
                    log_info("screenshot: {} ({}x{})", path, w, h);
                }
                mz_free(png);
                return std::nullopt;
            });
            while (LatteGPUState.frameCounter < frameNo + 1)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }).detach();
}

}

void start()
{
    if (!InitializeGlobalVulkan())
        fatal("Vulkan is not available. Windfall needs a GPU with Vulkan 1.1 or newer and an up-to-date driver.");
    int w = 0, h = 0;
    WindowSystem::GetWindowPhysSize(w, h);
    try
    {
        g_renderer = std::make_unique<VulkanRenderer>();
        VulkanRenderer::GetInstance()->InitializeSurface({w, h}, true);
    }
    catch (const std::exception& e)
    {
        fatal("Failed to initialize the Vulkan renderer: {}", e.what());
    }
    LatteGPUState.isDRCPrimary = false;
    LatteOverlay_init();
    if (const char* o = std::getenv("WF_GAMEPAD_OVERLAY"))
        GetConfig().gamepad_overlay = *o == '1';
    Latte_Start();
    start_screenshot_hook();
    log_info("GPU thread started ({}x{})", w, h);
}

uint32_t frame_count() { return LatteGPUState.frameCounter; }

void save_screenshot()
{
    if (!g_renderer) return;
    std::filesystem::path dir;
    if (const char* profile = std::getenv("USERPROFILE")) dir = std::filesystem::path(profile) / "Pictures" / "Windfall";
    else dir = "screenshots";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char name[64];
    std::strftime(name, sizeof name, "Windfall %Y-%m-%d %H-%M-%S.png", &tm);
    std::filesystem::path path = dir / name;
    g_renderer->RequestScreenshot([path](const std::vector<uint8>& rgb, int w, int h, bool) -> std::optional<std::string> {
        size_t pngSize = 0;
        void* png = tdefl_write_image_to_png_file_in_memory(rgb.data(), w, h, 3, &pngSize);
        bool ok = false;
        if (FILE* fp = _wfopen(path.c_str(), L"wb"))
        {
            ok = std::fwrite(png, 1, pngSize, fp) == pngSize;
            std::fclose(fp);
        }
        mz_free(png);
        log_info("screenshot: {} ({}x{})", path.string(), w, h);
        ui::menu::toast(ok ? "Screenshot saved to Pictures\\Windfall" : "Couldn't save the screenshot");
        return std::nullopt;
    });
}

void capture_frames()
{
    if (!g_renderer) return;
    std::filesystem::path dir;
    if (const char* profile = std::getenv("USERPROFILE")) dir = std::filesystem::path(profile) / "Pictures" / "Windfall";
    else dir = "screenshots";
    auto t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char name[64];
    std::strftime(name, sizeof name, "capture %Y-%m-%d %H-%M-%S", &tm);
    dir /= name;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    static constexpr uint32 kFrames = 8;
    uint32 first = LatteGPUState.frameCounter + 3;
    {
        std::ofstream info(dir / "info.txt");
        uint32 pl = rd32(0x1047428C);
        char stage[9] = {};
        for (int i = 0; i < 8; i++) stage[i] = (char)rd8(0x104741E4 + i);
        info << std::format("stage {} room {}\nframes {} to {}\n", stage, (int)(int8_t)rd8(0x104741E4 + 10), first, first + kFrames - 1);
        if (pl)
            info << std::format("player {:.1f} {:.1f} {:.1f} angle {}\n", std::bit_cast<float>(rd32(pl + 0x314)), std::bit_cast<float>(rd32(pl + 0x318)),
                                std::bit_cast<float>(rd32(pl + 0x31C)), (int16_t)rd16(pl + 0x32A));
    }
    wf_gpu_dump_request(dir.string(), first, kFrames);
    std::thread([dir, first] {
        for (uint32 target = first; target < first + kFrames; target++)
        {
            while (LatteGPUState.frameCounter < target) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            uint32 frameNo = LatteGPUState.frameCounter;
            g_renderer->RequestScreenshot([dir, frameNo](const std::vector<uint8>& rgb, int w, int h, bool) -> std::optional<std::string> {
                size_t pngSize = 0;
                void* png = tdefl_write_image_to_png_file_in_memory(rgb.data(), w, h, 3, &pngSize);
                if (FILE* fp = _wfopen((dir / std::format("shot_{:05}.png", frameNo)).c_str(), L"wb"))
                {
                    std::fwrite(png, 1, pngSize, fp);
                    std::fclose(fp);
                }
                mz_free(png);
                return std::nullopt;
            });
            while (LatteGPUState.frameCounter < frameNo + 1) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        log_info("capture: {} frames in {}", kFrames, dir.string());
        ui::menu::toast("Captured 8 frames to Pictures\\Windfall");
    }).detach();
}

void toggle_frame_hold()
{
    static const char* kNames[] = {"Test 0: normal", "Test 1: no interpolation", "Test 2: no color/material interpolation",
                                   "Test 3: no camera-block interpolation", "Test 4: no model-matrix interpolation",
                                   "Test 5: no skinned-mesh interpolation", "Test 6: no rigid-part interpolation",
                                   "Test 7: no palm/model matrix interpolation", "Test 8: no main-view interpolation"};
    int mode = (game::g_testMode.load() + 1) % 9;
    game::g_testMode = mode;
    log_info("test mode {}", kNames[mode]);
    ui::menu::toast(kNames[mode]);
}

std::string status()
{
    return std::format("GPU: flips requested {}, frames {}, draw calls {}",
                       LatteGPUState.sharedArea ? _swapEndianU32(LatteGPUState.sharedArea->flipRequestCountBE) : 0,
                       LatteGPUState.frameCounter, LatteGPUState.drawCallCounter);
}

}
