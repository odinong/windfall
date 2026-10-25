#pragma once
#include "config/CemuConfig.h"

enum class AccurateShaderMulOption { False = 0, True = 1 };
enum class PositionInvariance { Auto = 0, False = 1, True = 2 };
enum class PrecompiledShaderOption { Auto = 0, Enable = 1, Disable = 2 };

class GameProfile
{
public:
    const AccurateShaderMulOption& GetAccurateShaderMul() const { return m_accurateShaderMul; }
    PositionInvariance GetPositionInvariance() const { return PositionInvariance::Auto; }
    const std::optional<PrecompiledShaderOption>& GetPrecompiledShadersState() const { return m_precompiledShaders; }
private:
    AccurateShaderMulOption m_accurateShaderMul = AccurateShaderMulOption::True;
    std::optional<PrecompiledShaderOption> m_precompiledShaders;
};

inline GameProfile* const g_current_game_profile = [] { static GameProfile p; return &p; }();
