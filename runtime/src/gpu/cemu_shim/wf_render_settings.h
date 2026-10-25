#pragma once
#include <atomic>
#include <cstdint>

struct WFRenderSettings
{
    std::atomic<float> fxaa{0.0f};
    std::atomic<float> sharpen{0.0f};
    std::atomic<float> brightness{0.0f};
    std::atomic<float> contrast{1.0f};
    std::atomic<float> saturation{1.0f};
    std::atomic<float> gamma{1.0f};
    std::atomic<int> anisotropy{0};
    std::atomic<int> textureFilter{0};
    std::atomic<uint32_t> samplerGeneration{0};
    std::atomic<float> fovScale{1.0f};
    std::atomic<bool> pauseInBackground{false};
    std::atomic<bool> waitForShaders{true};
};

WFRenderSettings& wf_render_settings();
