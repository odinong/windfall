#include "input/input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "enhancements/playtest.h"
#include "core/log.h"
#include "gpu/gpu.h"
#include "ui/menu.h"
#include "ui/settings.h"

namespace wf::input {
namespace {

std::mutex g_lock;
ProState g_state;
SDL_Gamepad* g_pad = nullptr;
bool g_rumbling = false;
std::atomic<bool> g_textCapture{false};
std::atomic<bool> g_fastForward{false};
std::array<float, settings::kActionCount> g_actionValues{};
std::string g_controllerName;

constexpr uint32_t kActionButtons[settings::kActStickR + 1] = {
    kA, kB, kX, kY, kL, kR, kZL, kZR, kPlus, kMinus, kUp, kDown, kLeft, kRight, kStickL, kStickR,
};

float pad_value(SDL_Gamepad* pad, int code, float deadzone)
{
    if (!pad || code == settings::kPadNone) return 0.0f;
    if (code >= settings::kPadLabelBase)
    {
        for (SDL_GamepadButton face : {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH})
        {
            SDL_GamepadButtonLabel l = SDL_GetGamepadButtonLabel(pad, face);
            if (l == SDL_GAMEPAD_BUTTON_LABEL_CROSS) l = SDL_GAMEPAD_BUTTON_LABEL_A;
            else if (l == SDL_GAMEPAD_BUTTON_LABEL_CIRCLE) l = SDL_GAMEPAD_BUTTON_LABEL_B;
            else if (l == SDL_GAMEPAD_BUTTON_LABEL_SQUARE) l = SDL_GAMEPAD_BUTTON_LABEL_X;
            else if (l == SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE) l = SDL_GAMEPAD_BUTTON_LABEL_Y;
            if ((int)l == code - settings::kPadLabelBase) return SDL_GetGamepadButton(pad, face) ? 1.0f : 0.0f;
        }
        return 0.0f;
    }
    if (code >= settings::kPadAxisBase)
    {
        int axis = (code - settings::kPadAxisBase) / 2;
        bool positive = (code - settings::kPadAxisBase) % 2;
        float v = std::clamp(SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)axis) / 32767.0f, -1.0f, 1.0f);
        if (!positive) v = -v;
        bool trigger = axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        float dz = trigger ? 0.0f : deadzone;
        return v <= dz ? 0.0f : (v - dz) / (1.0f - dz);
    }
    return SDL_GetGamepadButton(pad, (SDL_GamepadButton)code) ? 1.0f : 0.0f;
}

struct ScriptStep { uint32_t start, end; uint32_t buttons; float lx, ly, rx, ry; bool fastForward = false; };
std::vector<ScriptStep> g_script, g_preScript;  // g_preScript: WF_INPUT_PRE, counted from launch

void parse_script(const char* s, std::vector<ScriptStep>& g_script)
{
    static constexpr std::pair<std::string_view, uint32_t> kNames[] = {
        {"A", kA}, {"B", kB}, {"X", kX}, {"Y", kY}, {"L", kL}, {"R", kR}, {"ZL", kZL}, {"ZR", kZR},
        {"PLUS", kPlus}, {"MINUS", kMinus}, {"HOME", kHome}, {"UP", kUp}, {"DOWN", kDown}, {"LEFT", kLeft},
        {"RIGHT", kRight}, {"LS", kStickL}, {"RS", kStickR},
    };
    std::string_view rest(s);
    while (!rest.empty())
    {
        size_t comma = rest.find(',');
        std::string_view item = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        size_t c1 = item.find(':'), c2 = item.rfind(':');
        if (c1 == std::string_view::npos || c1 == c2)
        {
            log_warn("WF_INPUT: ignoring '{}' (expected frame:buttons:frames)", item);
            continue;
        }
        ScriptStep step{};
        step.start = (uint32_t)std::strtoul(std::string(item.substr(0, c1)).c_str(), nullptr, 10);
        step.end = step.start + (uint32_t)std::strtoul(std::string(item.substr(c2 + 1)).c_str(), nullptr, 10);
        std::string_view names = item.substr(c1 + 1, c2 - c1 - 1);
        while (!names.empty())
        {
            size_t plus = names.find('+', 1);
            if (names.size() >= 3 && (names[0] == 'L' || names[0] == 'R') && (names[1] == 'X' || names[1] == 'Y'))
                plus = 3;
            std::string_view name = names.substr(0, plus);
            names = plus >= names.size() ? std::string_view{} : names.substr(plus + (names[plus] == '+' ? 1 : 0));
            if (name.size() == 3 && (name[1] == 'X' || name[1] == 'Y'))
            {
                float v = name[2] == '+' ? 1.0f : -1.0f;
                float& axis = name[0] == 'L' ? (name[1] == 'X' ? step.lx : step.ly) : (name[1] == 'X' ? step.rx : step.ry);
                axis = v;
                continue;
            }
            if (name == "FF")
            {
                step.fastForward = true;
                continue;
            }
            bool found = false;
            for (auto [n, bit] : kNames)
                if (n == name) { step.buttons |= bit; found = true; }
            if (!found) log_warn("WF_INPUT: unknown button '{}'", name);
        }
        g_script.push_back(step);
    }
    log_info("WF_INPUT: {} scripted steps", g_script.size());
}

}

void init()
{
    if (const char* s = std::getenv("WF_INPUT"))
        parse_script(s, g_script);
    if (const char* s = std::getenv("WF_INPUT_PRE"))
        parse_script(s, g_preScript);
}

void handle_event(const SDL_Event& e)
{
    if (e.type == SDL_EVENT_GAMEPAD_ADDED && !g_pad)
    {
        g_pad = SDL_OpenGamepad(e.gdevice.which);
        if (g_pad)
        {
            const char* name = SDL_GetGamepadName(g_pad);
            log_info("input: using gamepad \"{}\"", name ? name : "?");
            std::lock_guard lk(g_lock);
            g_controllerName = name ? name : "Controller";
        }
    }
    else if (e.type == SDL_EVENT_GAMEPAD_REMOVED && g_pad && SDL_GetGamepadID(g_pad) == e.gdevice.which)
    {
        SDL_CloseGamepad(g_pad);
        g_pad = nullptr;
        std::lock_guard lk(g_lock);
        g_controllerName.clear();
        log_info("input: gamepad disconnected");
    }
}

void update()
{
    ProState s;
    settings::Settings cfg;
    {
        std::lock_guard lk(settings::lock());
        cfg.bindings = settings::get().bindings;
        cfg.deadzone = settings::get().deadzone;
        cfg.invertCameraX = settings::get().invertCameraX;
        cfg.invertCameraY = settings::get().invertCameraY;
    }
    const bool* keys = SDL_GetKeyboardState(nullptr);
    bool keyboard = !g_textCapture;
    auto value = [&](int action) {
        const settings::Binding& b = cfg.bindings[action];
        float v = (keyboard && b.key > 0 && b.key < SDL_SCANCODE_COUNT && keys[b.key]) ? 1.0f : 0.0f;
        return std::max(v, pad_value(g_pad, b.pad, cfg.deadzone));
    };
    std::array<float, settings::kActionCount> values;
    for (int a = 0; a < settings::kActionCount; a++) values[a] = value(a);
    for (int a = 0; a <= settings::kActStickR; a++)
        if (values[a] > 0.5f) s.buttons |= kActionButtons[a];
    s.lx = std::clamp(values[settings::kActMoveRight] - values[settings::kActMoveLeft], -1.0f, 1.0f);
    s.ly = std::clamp(values[settings::kActMoveUp] - values[settings::kActMoveDown], -1.0f, 1.0f);
    s.rx = std::clamp(values[settings::kActCameraRight] - values[settings::kActCameraLeft], -1.0f, 1.0f);
    s.ry = std::clamp(values[settings::kActCameraUp] - values[settings::kActCameraDown], -1.0f, 1.0f);
    {
        std::lock_guard lk(g_lock);
        g_actionValues = values;
    }
    if (cfg.invertCameraX) s.rx = -s.rx;
    if (cfg.invertCameraY) s.ry = -s.ry;
    g_fastForward = values[settings::kActFastForward] > 0.5f && !ui::menu::is_open() && !std::getenv("WF_NO_FAST_FORWARD");
    if (std::getenv("WF_FAST_FORWARD")) g_fastForward = true;
    if (ui::menu::is_open())
        s = ProState{};
    for (const ScriptStep& step : g_preScript)
    {
        uint32_t frame = gpu::frame_count();
        if (frame < step.start || frame >= step.end) continue;
        s.buttons |= step.buttons;
        if (step.lx) s.lx = step.lx;
        if (step.ly) s.ly = step.ly;
        if (step.rx) s.rx = step.rx;
        if (step.ry) s.ry = step.ry;
    }
    static const bool fromArrival = std::getenv("WF_INPUT_ARRIVAL") != nullptr;  // script frames count from the playtest's arrival
    if (!g_script.empty() && (!fromArrival || playtest::arrival_frame()))
    {
        uint32_t frame = gpu::frame_count() - (fromArrival ? playtest::arrival_frame() : 0);
        for (const ScriptStep& step : g_script)
        {
            if (frame < step.start || frame >= step.end) continue;
            s.buttons |= step.buttons;
            if (step.lx) s.lx = step.lx;
            if (step.ly) s.ly = step.ly;
            if (step.rx) s.rx = step.rx;
            if (step.ry) s.ry = step.ry;
            if (step.fastForward) g_fastForward = true;
        }
    }
    bool ff = g_fastForward;
    playtest::apply(s, ff);
    g_fastForward = ff;
    std::lock_guard lk(g_lock);
    g_state = s;
}

ProState read()
{
    std::lock_guard lk(g_lock);
    return g_state;
}

void set_text_capture(bool on) { g_textCapture = on; }

float action_value(int action)
{
    std::lock_guard lk(g_lock);
    return action >= 0 && action < settings::kActionCount ? g_actionValues[(size_t)action] : 0.0f;
}

std::string controller_name()
{
    std::lock_guard lk(g_lock);
    return g_controllerName;
}

bool fast_forward() { return g_fastForward; }

void rumble(bool on)
{
    std::lock_guard lk(g_lock);
    if (on == g_rumbling || !g_pad) return;
    g_rumbling = on;
    SDL_RumbleGamepad(g_pad, on ? 0x6000 : 0, on ? 0x6000 : 0, on ? 1000 : 0);
}

}
