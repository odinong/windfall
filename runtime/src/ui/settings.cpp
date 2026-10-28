#include "Common/precompiled.h"
#include "audio/IAudioAPI.h"
#include "config/CemuConfig.h"
#include "wf_render_settings.h"

#include "ui/settings.h"

#include <SDL3/SDL.h>

#include <fstream>
#include <map>

#include "core/config.h"
#include "core/log.h"
#include "gpu/window.h"

namespace wf::settings {
namespace {

Settings g_settings;
Settings g_startup;

constexpr const char* kActionNames[kActionCount] = {
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Plus", "Minus",
    "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Left Stick Press", "Right Stick Press",
    "Move Up", "Move Down", "Move Left", "Move Right",
    "Camera Up", "Camera Down", "Camera Left", "Camera Right",
    "Fast Forward (hold)",
};
constexpr const char* kActionKeys[kActionCount] = {
    "a", "b", "x", "y", "l", "r", "zl", "zr", "plus", "minus",
    "up", "down", "left", "right", "ls", "rs",
    "move_up", "move_down", "move_left", "move_right",
    "camera_up", "camera_down", "camera_left", "camera_right",
    "fast_forward",
};

std::filesystem::path file_path()
{
    if (const wchar_t* e = _wgetenv(L"WF_SETTINGS_FILE")) return e;
    return config().dataDir / "settings.ini";
}

const char* label_text(int label)
{
    switch (label)
    {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return "a";
    case SDL_GAMEPAD_BUTTON_LABEL_B: return "b";
    case SDL_GAMEPAD_BUTTON_LABEL_X: return "x";
    case SDL_GAMEPAD_BUTTON_LABEL_Y: return "y";
    default: return "?";
    }
}

std::string pad_to_text(int code)
{
    if (code == kPadNone) return "none";
    if (code >= kPadLabelBase) return std::string("label:") + label_text(code - kPadLabelBase);
    if (code >= kPadAxisBase)
    {
        int axis = (code - kPadAxisBase) / 2;
        const char* n = SDL_GetGamepadStringForAxis((SDL_GamepadAxis)axis);
        return std::string("axis:") + (n ? n : "?") + ((code - kPadAxisBase) % 2 ? "+" : "-");
    }
    const char* n = SDL_GetGamepadStringForButton((SDL_GamepadButton)code);
    return std::string("button:") + (n ? n : "?");
}

int pad_from_text(const std::string& t)
{
    if (t.starts_with("label:"))
    {
        std::string l = t.substr(6);
        if (l == "a") return kPadLabelBase + SDL_GAMEPAD_BUTTON_LABEL_A;
        if (l == "b") return kPadLabelBase + SDL_GAMEPAD_BUTTON_LABEL_B;
        if (l == "x") return kPadLabelBase + SDL_GAMEPAD_BUTTON_LABEL_X;
        if (l == "y") return kPadLabelBase + SDL_GAMEPAD_BUTTON_LABEL_Y;
    }
    else if (t.starts_with("axis:") && t.size() > 6)
    {
        std::string n = t.substr(5, t.size() - 6);
        SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(n.c_str());
        if (a != SDL_GAMEPAD_AXIS_INVALID) return kPadAxisBase + a * 2 + (t.back() == '+' ? 1 : 0);
    }
    else if (t.starts_with("button:"))
    {
        SDL_GamepadButton b = SDL_GetGamepadButtonFromString(t.substr(7).c_str());
        if (b != SDL_GAMEPAD_BUTTON_INVALID) return (int)b;
    }
    return kPadNone;
}

template<typename F>
void for_each_value(Settings& s, F&& f)
{
    f("graphics.fxaa", s.fxaa);
    f("graphics.wait_for_shaders", s.waitForShaders);
    f("video.smooth_ui", s.smoothUi);
    f("graphics.sharpening", s.sharpening);
    f("graphics.anisotropy", s.anisotropy);
    f("graphics.texture_filter", s.textureFilter);
    f("graphics.upscale_filter", s.upscaleFilter);
    f("graphics.downscale_filter", s.downscaleFilter);
    f("video.fullscreen", s.fullscreen);
    f("video.aspect", s.aspect);
    f("video.vsync", s.vsync);
    f("video.frame_rate", s.frameRate);
    f("video.fps", s.fps);
    f("video.brightness", s.brightness);
    f("video.contrast", s.contrast);
    f("video.saturation", s.saturation);
    f("video.gamma", s.gamma);
    f("video.overlay_position", s.overlayPosition);
    f("video.overlay_fps", s.overlayFps);
    f("video.overlay_drawcalls", s.overlayDrawcalls);
    f("video.overlay_cpu", s.overlayCpu);
    f("video.overlay_ram", s.overlayRam);
    f("video.overlay_vram", s.overlayVram);
    f("video.gamepad_overlay", s.gamepadOverlay);
    f("input.deadzone", s.deadzone);
    f("input.invert_camera_x", s.invertCameraX);
    f("input.invert_camera_y", s.invertCameraY);
    f("audio.volume", s.volume);
    f("audio.mute_in_background", s.muteInBackground);
    f("game.field_of_view", s.fieldOfView);
    f("game.pause_in_background", s.pauseInBackground);
    f("game.discord_presence", s.discordPresence);
    f("game.screenshot_key", s.screenshotKey);
    f("menu.scale", s.menuScale);
    f("menu.styled", s.styledMenu);
    f("paths.game", s.gameDir);
    f("randomizer.enabled", s.randoEnabled);
    f("randomizer.seed", s.randoSeed);
    f("randomizer.seed_text", s.randoSeedText);
    f("multiplayer.server", s.mpServer);
    f("multiplayer.room", s.mpRoom);
    f("multiplayer.name", s.mpName);
    f("multiplayer.auto_connect", s.mpAutoConnect);
    f("multiplayer.show_names", s.mpShowNames);
    f("multiplayer.share_items", s.mpShareItems);
    f("multiplayer.share_story", s.mpShareStory);
    f("multiplayer.share_enemies", s.mpShareEnemies);
    f("multiplayer.share_cutscenes", s.mpShareCutscenes);
    f("multiplayer.chat_seconds", s.mpChatSeconds);
}

std::string to_text(bool v) { return v ? "true" : "false"; }
std::string to_text(int v) { return std::to_string(v); }
std::string to_text(float v) { return fmt::format("{:.3f}", v); }
std::string to_text(const std::string& v) { return v; }
std::string to_text(uint32_t v) { return std::to_string(v); }
void from_text(const std::string& t, bool& v) { v = t == "true" || t == "1"; }
void from_text(const std::string& t, int& v) { v = std::atoi(t.c_str()); }
void from_text(const std::string& t, float& v) { v = (float)std::atof(t.c_str()); }
void from_text(const std::string& t, std::string& v) { v = t; }
void from_text(const std::string& t, uint32_t& v) { v = (uint32_t)std::strtoul(t.c_str(), nullptr, 10); }

void clamp_values(Settings& s)
{
    s.sharpening = std::clamp(s.sharpening, 0, 100);
    s.textureFilter = std::clamp(s.textureFilter, 0, 2);
    s.upscaleFilter = std::clamp(s.upscaleFilter, 0, 3);
    s.downscaleFilter = std::clamp(s.downscaleFilter, 0, 3);
    s.aspect = std::clamp(s.aspect, 0, 1);
    s.vsync = std::clamp(s.vsync, 0, 2);
    s.frameRate = std::clamp(s.frameRate, 0, 1);
    if (s.fps < 0) s.fps = s.frameRate == kFps30 ? 30 : 60;
    if (s.fps != 0) s.fps = std::clamp(s.fps, 30, 360);
    s.brightness = std::clamp(s.brightness, -0.5f, 0.5f);
    s.contrast = std::clamp(s.contrast, 0.5f, 1.5f);
    s.saturation = std::clamp(s.saturation, 0.0f, 2.0f);
    s.gamma = std::clamp(s.gamma, 0.5f, 2.0f);
    s.overlayPosition = std::clamp(s.overlayPosition, 0, 6);
    s.deadzone = std::clamp(s.deadzone, 0.0f, 0.9f);
    s.volume = std::clamp(s.volume, 0, 100);
    s.fieldOfView = std::clamp(s.fieldOfView, 40, 100);
    s.menuScale = std::clamp(s.menuScale, 0.75f, 2.0f);
    s.mpChatSeconds = std::clamp(s.mpChatSeconds, 3, 60);
}

}

const char* action_name(int action) { return action >= 0 && action < kActionCount ? kActionNames[action] : "?"; }

std::string pad_input_name(int code)
{
    if (code == kPadNone) return "-";
    if (code >= kPadLabelBase) return std::string("Button ") + (char)std::toupper(label_text(code - kPadLabelBase)[0]);
    if (code >= kPadAxisBase)
    {
        static const char* kAxes[][2] = {{"Left Stick Left", "Left Stick Right"},   {"Left Stick Up", "Left Stick Down"},
                                         {"Right Stick Left", "Right Stick Right"}, {"Right Stick Up", "Right Stick Down"},
                                         {"Left Trigger", "Left Trigger"},          {"Right Trigger", "Right Trigger"}};
        int axis = (code - kPadAxisBase) / 2;
        bool pos = (code - kPadAxisBase) % 2;
        if (axis < 6) return kAxes[axis][pos ? 1 : 0];
        return fmt::format("Axis {} {}", axis, pos ? "+" : "-");
    }
    static const char* kButtons[] = {"South", "East", "West", "North", "Back", "Guide", "Start", "Left Stick", "Right Stick",
                                     "Left Shoulder", "Right Shoulder", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right"};
    return code < (int)std::size(kButtons) ? kButtons[code] : fmt::format("Button {}", code);
}

std::mutex& lock()
{
    static std::mutex m;
    return m;
}

Settings& get() { return g_settings; }

void set_default_bindings(Settings& s)
{
    auto bind = [&](int action, SDL_Scancode key, int pad) { s.bindings[action] = {key, pad}; };
    auto axis = [](SDL_GamepadAxis a, bool positive) { return kPadAxisBase + a * 2 + (positive ? 1 : 0); };
    bind(kActA, SDL_SCANCODE_SPACE, kPadLabelBase + SDL_GAMEPAD_BUTTON_LABEL_A);
    bind(kActB, SDL_SCANCODE_LSHIFT, kPadLabelBase + SDL_GAMEPAD_BUTTON_LABEL_B);
    bind(kActX, SDL_SCANCODE_E, kPadLabelBase + SDL_GAMEPAD_BUTTON_LABEL_Y);
    bind(kActY, SDL_SCANCODE_Q, kPadLabelBase + SDL_GAMEPAD_BUTTON_LABEL_X);
    bind(kActL, SDL_SCANCODE_F, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    bind(kActR, SDL_SCANCODE_R, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    bind(kActZL, SDL_SCANCODE_LCTRL, axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, true));
    bind(kActZR, SDL_SCANCODE_C, axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, true));
    bind(kActPlus, SDL_SCANCODE_RETURN, SDL_GAMEPAD_BUTTON_START);
    bind(kActMinus, SDL_SCANCODE_BACKSPACE, SDL_GAMEPAD_BUTTON_BACK);
    bind(kActUp, SDL_SCANCODE_1, SDL_GAMEPAD_BUTTON_DPAD_UP);
    bind(kActDown, SDL_SCANCODE_2, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    bind(kActLeft, SDL_SCANCODE_3, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    bind(kActRight, SDL_SCANCODE_4, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    bind(kActStickL, SDL_SCANCODE_V, SDL_GAMEPAD_BUTTON_LEFT_STICK);
    bind(kActStickR, SDL_SCANCODE_B, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    bind(kActMoveUp, SDL_SCANCODE_W, axis(SDL_GAMEPAD_AXIS_LEFTY, false));
    bind(kActMoveDown, SDL_SCANCODE_S, axis(SDL_GAMEPAD_AXIS_LEFTY, true));
    bind(kActMoveLeft, SDL_SCANCODE_A, axis(SDL_GAMEPAD_AXIS_LEFTX, false));
    bind(kActMoveRight, SDL_SCANCODE_D, axis(SDL_GAMEPAD_AXIS_LEFTX, true));
    bind(kActCameraUp, SDL_SCANCODE_UP, axis(SDL_GAMEPAD_AXIS_RIGHTY, false));
    bind(kActCameraDown, SDL_SCANCODE_DOWN, axis(SDL_GAMEPAD_AXIS_RIGHTY, true));
    bind(kActCameraLeft, SDL_SCANCODE_LEFT, axis(SDL_GAMEPAD_AXIS_RIGHTX, false));
    bind(kActCameraRight, SDL_SCANCODE_RIGHT, axis(SDL_GAMEPAD_AXIS_RIGHTX, true));
    bind(kActFastForward, SDL_SCANCODE_GRAVE, kPadNone);
}

Settings defaults()
{
    Settings s;
    set_default_bindings(s);
    return s;
}

void load() { load_from(file_path()); }

void load_from(const std::filesystem::path& path)
{
    std::lock_guard lk(lock());
    g_settings = defaults();
    std::ifstream f(path);
    std::map<std::string, std::string> kv;
    for (std::string line; std::getline(f, line);)
    {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    for_each_value(g_settings, [&](const char* key, auto& v) {
        if (auto it = kv.find(key); it != kv.end()) from_text(it->second, v);
    });
    for (const auto& [k, v] : kv)
    {
        if (k.starts_with("pack[")) g_settings.packs[k] = v;
        else if (k.starts_with("cheat[") && k.ends_with("]")) g_settings.cheats[k.substr(6, k.size() - 7)] = v == "true";
        else if (k.starts_with("mod[") && k.ends_with("]")) g_settings.mods[k.substr(4, k.size() - 5)] = v == "true";
        else if (k.starts_with("rando[") && k.ends_with("]")) g_settings.randoOptions[k.substr(6, k.size() - 7)] = v;
    }
    for (int a = 0; a < kActionCount; a++)
    {
        if (auto it = kv.find(std::string("key.") + kActionKeys[a]); it != kv.end())
            g_settings.bindings[a].key = it->second == "none" ? 0 : (int)SDL_GetScancodeFromName(it->second.c_str());
        if (auto it = kv.find(std::string("pad.") + kActionKeys[a]); it != kv.end())
            g_settings.bindings[a].pad = pad_from_text(it->second);
    }
    clamp_values(g_settings);
    g_startup = g_settings;
    log_info("settings: {} ({})", file_path().string(), f ? "loaded" : "defaults");
}

void save()
{
    Settings s;
    {
        std::lock_guard lk(lock());
        s = g_settings;
    }
    std::string out = "# Windfall settings (written by the settings menu, F1)\n";
    for_each_value(s, [&](const char* key, auto& v) { out += fmt::format("{} = {}\n", key, to_text(v)); });
    for (int a = 0; a < kActionCount; a++)
    {
        const char* name = s.bindings[a].key ? SDL_GetScancodeName((SDL_Scancode)s.bindings[a].key) : "";
        out += fmt::format("key.{} = {}\n", kActionKeys[a], *name ? name : "none");
        out += fmt::format("pad.{} = {}\n", kActionKeys[a], pad_to_text(s.bindings[a].pad));
    }
    for (const auto& [k, v] : s.packs) out += fmt::format("{} = {}\n", k, v);
    for (const auto& [k, v] : s.cheats) out += fmt::format("cheat[{}] = {}\n", k, v ? "true" : "false");
    for (const auto& [k, v] : s.mods) out += fmt::format("mod[{}] = {}\n", k, v ? "true" : "false");
    for (const auto& [k, v] : s.randoOptions) out += fmt::format("rando[{}] = {}\n", k, v);
    std::error_code ec;
    std::filesystem::create_directories(file_path().parent_path(), ec);
    std::filesystem::path tmp = std::filesystem::path(file_path()) += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << out;
        if (!f)
        {
            log_warn("settings: could not write {}", file_path().string());
            return;
        }
    }
    std::filesystem::rename(tmp, file_path(), ec);
    if (ec) log_warn("settings: could not write {}: {}", file_path().string(), ec.message());
}

void apply_audio()
{
    Settings s;
    {
        std::lock_guard lk(lock());
        s = g_settings;
    }
    int volume = s.muteInBackground && !window::focused() ? 0 : s.volume;
    GetConfig().tv_volume = volume;
    if (g_tvAudio) g_tvAudio->SetVolume(volume);
}

void apply()
{
    Settings s;
    {
        std::lock_guard lk(lock());
        s = g_settings;
    }
    CemuConfig& c = GetConfig();
    c.vsync = s.vsync == 0 ? 0 : s.vsync == 2 ? 2 : 1;
    c.upscale_filter = s.upscaleFilter;
    c.downscale_filter = s.downscaleFilter;
    c.fullscreen_scaling = s.aspect;
    c.overlay.position = (ScreenPosition)s.overlayPosition;
    c.overlay.fps = s.overlayFps;
    c.overlay.drawcalls = s.overlayDrawcalls;
    c.overlay.cpu_usage = s.overlayCpu;
    c.overlay.ram_usage = s.overlayRam;
    c.overlay.vram_usage = s.overlayVram;
    c.gamepad_overlay = s.gamepadOverlay;
    apply_audio();
    WFRenderSettings& r = wf_render_settings();
    r.fxaa = s.fxaa ? 1.0f : 0.0f;
    r.waitForShaders = s.waitForShaders;
    r.smoothUi = s.smoothUi;
    r.sharpen = s.sharpening / 100.0f;
    r.brightness = s.brightness;
    r.contrast = s.contrast;
    r.saturation = s.saturation;
    r.gamma = s.gamma;
    r.fovScale = (float)(std::tan(60.0 * 3.14159265358979 / 360.0) / std::tan(s.fieldOfView * 3.14159265358979 / 360.0));
    r.pauseInBackground = s.pauseInBackground;
    if (r.anisotropy != s.anisotropy || r.textureFilter != s.textureFilter)
    {
        r.anisotropy = s.anisotropy;
        r.textureFilter = s.textureFilter;
        r.samplerGeneration++;
    }
    window::request_fullscreen(s.fullscreen);
}

}

WFRenderSettings& wf_render_settings()
{
    static WFRenderSettings s;
    return s;
}

namespace wf::settings {

bool restart_pending()
{
    std::lock_guard lk(lock());
    auto resolution = [](const Settings& s) {
        std::string v;
        for (const auto& [k, x] : s.packs)
            if (k.find("/Graphics/Resolution]") != std::string::npos) v += k + "=" + x + ";";
        return v;
    };
    return g_settings.randoEnabled != g_startup.randoEnabled || resolution(g_settings) != resolution(g_startup);
}

bool pack_enabled(const Settings& s, const std::string& id)
{
    auto it = s.packs.find("pack[" + id + "].enabled");
    return it != s.packs.end() && it->second == "true";
}

void set_pack_enabled(Settings& s, const std::string& id, bool on) { s.packs["pack[" + id + "].enabled"] = on ? "true" : "false"; }

std::string pack_preset(const Settings& s, const std::string& id, const std::string& category)
{
    auto it = s.packs.find("pack[" + id + "].preset[" + category + "]");
    return it == s.packs.end() ? std::string() : it->second;
}

void set_pack_preset(Settings& s, const std::string& id, const std::string& category, const std::string& preset)
{
    s.packs["pack[" + id + "].preset[" + category + "]"] = preset;
}

}
