#pragma once
#include <filesystem>
#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace wf::settings {

enum Action : int
{
    kActA, kActB, kActX, kActY, kActL, kActR, kActZL, kActZR, kActPlus, kActMinus,
    kActUp, kActDown, kActLeft, kActRight, kActStickL, kActStickR,
    kActMoveUp, kActMoveDown, kActMoveLeft, kActMoveRight,
    kActCameraUp, kActCameraDown, kActCameraLeft, kActCameraRight,
    kActFastForward,
    kActionCount
};
const char* action_name(int action);

constexpr int kPadNone = -1;
constexpr int kPadLabelBase = 200;
constexpr int kPadAxisBase = 100;
std::string pad_input_name(int code);

struct Binding
{
    int key = 0;
    int pad = kPadNone;
};

enum FrameRate : int { kFps30, kFps60 };

struct Settings
{
    bool fxaa = false;
    bool smoothUi = false;  // experimental: menus and HUD blended between ticks at high frame rates
    bool waitForShaders = true;  // draw new objects only once their shaders are built (a pause) rather than late (pop-in)
    int sharpening = 0;
    int anisotropy = 0;
    int textureFilter = 0;
    int upscaleFilter = 1;
    int downscaleFilter = 0;
    bool fullscreen = false;
    int aspect = 0;
    int vsync = 1;
    int frameRate = kFps60;
    int fps = -1;
    float brightness = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float gamma = 1.0f;
    int overlayPosition = 0;
    bool overlayFps = true, overlayDrawcalls = false, overlayCpu = false, overlayRam = false, overlayVram = false;
    bool gamepadOverlay = false;
    std::array<Binding, kActionCount> bindings{};
    float deadzone = 0.12f;
    bool invertCameraX = false, invertCameraY = false;
    int volume = 100;
    bool muteInBackground = false;
    int fieldOfView = 60;
    bool pauseInBackground = false;
    bool discordPresence = true;
    bool screenshotKey = true;
    float menuScale = 1.0f;
    bool styledMenu = false;
    std::string gameDir;
    std::map<std::string, std::string> packs;
    std::map<std::string, bool> cheats;
    std::map<std::string, bool> mods;
    bool randoEnabled = false;
    uint32_t randoSeed = 1;
    std::string randoSeedText;
    std::map<std::string, std::string> randoOptions;
    std::string mpServer = "wss://windfall.bigdick.app";
    std::string mpRoom = "PUBLIC";
    std::string mpName = "Link";
    bool mpAutoConnect = false;
    bool mpShowNames = true;
    bool mpShareItems = true;
    int mpChatSeconds = 12;
};

std::mutex& lock();
Settings& get();
Settings defaults();
void set_default_bindings(Settings& s);

void load();
void load_from(const std::filesystem::path& path);
void save();
void apply();
void apply_audio();
bool restart_pending();

bool pack_enabled(const Settings& s, const std::string& id);
void set_pack_enabled(Settings& s, const std::string& id, bool on);
std::string pack_preset(const Settings& s, const std::string& id, const std::string& category);
void set_pack_preset(Settings& s, const std::string& id, const std::string& category, const std::string& preset);

}
