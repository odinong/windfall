#pragma once
#include "Cafe/HW/Latte/Core/LatteTextureView.h"

class RendererOutputShader;
const std::string* wf_find_custom_shader(uint64 baseHash, uint64 auxHash, int type);

class GraphicPack2
{
public:
    enum class GP_SHADER_TYPE : uint8 { VERTEX = 0, PIXEL = 1, GEOMETRY = 2 };

    struct TextureRule
    {
        struct FILTER_SETTINGS
        {
            std::vector<sint32> format_whitelist, format_blacklist, tilemode_whitelist, tilemode_blacklist;
            sint32 width = -1, height = -1, depth = -1;
            enum class MEM1_FILTER { BOTH = 0, INSIDE = 1, OUTSIDE = 2 } inMEM1 = MEM1_FILTER::BOTH;
        } filter_settings;
        struct OVERWRITE_SETTINGS
        {
            sint32 width = -1, height = -1, depth = -1, format = -1;
            sint32 lod_bias = -1, relative_lod_bias = -1, anistropic_value = -1;
        } overwrite_settings;
    };

    const std::string& GetName() const { return m_name; }
    const std::vector<TextureRule>& GetTextureRules() const { return m_textureRules; }
    bool AllowRendertargetSizeOptimization() const { return m_allowRendertargetSizeOptimization; }
    RendererOutputShader* GetUpscalingShader(bool render_upside_down) { return nullptr; }
    RendererOutputShader* GetDownscalingShader(bool render_upside_down) { return nullptr; }
    RendererOutputShader* GetOuputShader(bool render_upside_down) { return nullptr; }
    LatteTextureView::MagFilter GetUpscalingMagFilter() const { return LatteTextureView::MagFilter::kLinear; }
    LatteTextureView::MagFilter GetDownscalingMagFilter() const { return LatteTextureView::MagFilter::kLinear; }

    static const std::vector<std::shared_ptr<GraphicPack2>>& GetActiveGraphicPacks() { return s_active; }
    static void WaitUntilReady() {}
    static const std::string* FindCustomShaderSource(uint64 shaderBaseHash, uint64 shaderAuxHash, GP_SHADER_TYPE type, bool isVulkanRenderer, bool isMetalRenderer)
    {
        return wf_find_custom_shader(shaderBaseHash, shaderAuxHash, (int)type);
    }

    std::string m_name;
    std::vector<TextureRule> m_textureRules;
    bool m_allowRendertargetSizeOptimization = false;
    static inline std::vector<std::shared_ptr<GraphicPack2>> s_active;
};
