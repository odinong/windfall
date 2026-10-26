#include "Common/precompiled.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "imgui/imgui_extension.h"
#include "WindowSystem.h"

#include "ui/menu.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

#include "core/guest.h"
#include "core/log.h"
#include "enhancements/cheats.h"
#include "enhancements/graphic_packs.h"
#include "mods/mods.h"
#include "input/input.h"
#include "ui/game_art.h"
#include "ui/sfx.h"
#include "ui/settings.h"
#include "net/multiplayer.h"
#include "enhancements/randomizer.h"
#include <filesystem>

namespace wf::ui::menu {
namespace {

std::atomic<bool> g_open{false};
std::mutex g_eventLock;
std::vector<SDL_Event> g_events;

enum class Capture { None, Key, Pad };
Capture g_capture = Capture::None;
int g_captureAction = -1;

int g_tab = 0;
constexpr const char* kTabs[] = {"Graphics", "Video", "Input", "Audio", "Enhancements", "Multiplayer", "Randomizer"};
constexpr int kTabCount = (int)std::size(kTabs);
bool g_styled = false;

constexpr ImU32 kWindow = IM_COL32(16, 42, 86, 255);
constexpr ImU32 kWindowLight = IM_COL32(252, 250, 242, 255);
constexpr ImU32 kText = IM_COL32(246, 242, 228, 255);
constexpr ImU32 kTextDim = IM_COL32(150, 176, 210, 255);
constexpr ImU32 kFrame = IM_COL32(28, 66, 124, 255);
constexpr ImU32 kFrameHover = IM_COL32(40, 88, 156, 255);
constexpr ImU32 kGold = IM_COL32(244, 200, 84, 255);
constexpr ImU32 kGoldDark = IM_COL32(150, 104, 24, 255);
constexpr ImU32 kHeading = IM_COL32(252, 214, 110, 255);
ImVec4 col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

ImGuiKey sdl_key(SDL_Keycode k)
{
    switch (k)
    {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default:
        if (k >= SDLK_A && k <= SDLK_Z) return (ImGuiKey)(ImGuiKey_A + (k - SDLK_A));
        if (k >= SDLK_0 && k <= SDLK_9) return (ImGuiKey)(ImGuiKey_0 + (k - SDLK_0));
        return ImGuiKey_None;
    }
}

ImGuiKey pad_nav_key(SDL_Gamepad* pad, SDL_GamepadButton b)
{
    if (b == SDL_GAMEPAD_BUTTON_SOUTH || b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_WEST || b == SDL_GAMEPAD_BUTTON_NORTH)
    {
        SDL_GamepadButtonLabel l = pad ? SDL_GetGamepadButtonLabel(pad, b) : SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
        if (l == SDL_GAMEPAD_BUTTON_LABEL_A || l == SDL_GAMEPAD_BUTTON_LABEL_CROSS) return ImGuiKey_GamepadFaceDown;
        if (l == SDL_GAMEPAD_BUTTON_LABEL_B || l == SDL_GAMEPAD_BUTTON_LABEL_CIRCLE) return ImGuiKey_GamepadFaceRight;
        if (l == SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN)
            return b == SDL_GAMEPAD_BUTTON_SOUTH ? ImGuiKey_GamepadFaceDown : b == SDL_GAMEPAD_BUTTON_EAST ? ImGuiKey_GamepadFaceRight : ImGuiKey_None;
        return ImGuiKey_None;
    }
    switch (b)
    {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    default: return ImGuiKey_None;
    }
}

void changed()
{
    settings::apply();
    settings::save();
}

void process_events()
{
    std::vector<SDL_Event> events;
    {
        std::lock_guard lk(g_eventLock);
        events.swap(g_events);
    }
    ImGuiIO& io = ImGui::GetIO();
    for (const SDL_Event& e : events)
    {
        if (g_capture != Capture::None)
        {
            bool done = false;
            std::lock_guard lk(settings::lock());
            settings::Binding& b = settings::get().bindings[g_captureAction];
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat)
            {
                if (e.key.key == SDLK_ESCAPE) done = true;
                else if (e.key.key == SDLK_DELETE) { (g_capture == Capture::Key ? b.key : b.pad) = g_capture == Capture::Key ? 0 : settings::kPadNone; done = true; }
                else if (g_capture == Capture::Key) { b.key = e.key.scancode; done = true; }
            }
            else if (g_capture == Capture::Pad && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button != SDL_GAMEPAD_BUTTON_GUIDE)
            {
                b.pad = e.gbutton.button;
                done = true;
            }
            else if (g_capture == Capture::Pad && e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && std::abs(e.gaxis.value) > 24000)
            {
                bool trigger = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                b.pad = settings::kPadAxisBase + e.gaxis.axis * 2 + (trigger || e.gaxis.value > 0 ? 1 : 0);
                done = true;
            }
            if (done)
            {
                log_info("settings menu: {} binding for {}: {}", g_capture == Capture::Key ? "keyboard" : "controller",
                         settings::action_name(g_captureAction),
                         g_capture == Capture::Key ? (b.key ? SDL_GetScancodeName((SDL_Scancode)b.key) : "none") : settings::pad_input_name(b.pad));
                g_capture = Capture::None;
                g_captureAction = -1;
            }
            continue;
        }
        switch (e.type)
        {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (ImGuiKey k = sdl_key(e.key.key); k != ImGuiKey_None) io.AddKeyEvent(k, e.type == SDL_EVENT_KEY_DOWN);
            io.AddKeyEvent(ImGuiMod_Ctrl, (e.key.mod & SDL_KMOD_CTRL) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (e.key.mod & SDL_KMOD_SHIFT) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (e.key.mod & SDL_KMOD_ALT) != 0);
            break;
        case SDL_EVENT_TEXT_INPUT: io.AddInputCharactersUTF8(e.text.text); break;
        case SDL_EVENT_MOUSE_WHEEL: io.AddMouseWheelEvent(e.wheel.x, e.wheel.y); break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        {
            bool down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            SDL_Gamepad* pad = SDL_GetGamepadFromID(e.gbutton.which);
            if (down && e.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) { g_tab = (g_tab + kTabCount - 1) % kTabCount; sfx::play(sfx::kTab); }
            if (down && e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) { g_tab = (g_tab + 1) % kTabCount; sfx::play(sfx::kTab); }
            ImGuiKey k = pad_nav_key(pad, (SDL_GamepadButton)e.gbutton.button);
            if (k == ImGuiKey_GamepadFaceRight && down && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
                g_open = false;
            else if (k != ImGuiKey_None)
                io.AddKeyEvent(k, down);
            break;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        {
            float v = e.gaxis.value / 32767.0f;
            if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX)
            {
                io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, v < -0.5f, std::max(0.0f, -v));
                io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, v > 0.5f, std::max(0.0f, v));
            }
            else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)
            {
                io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, v < -0.5f, std::max(0.0f, -v));
                io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, v > 0.5f, std::max(0.0f, v));
            }
            break;
        }
        default: break;
        }
    }
}

void piece(ImDrawList* dl, void* tex, const char* name, ImVec2 a, ImVec2 b, bool flipX, bool flipY, ImVec2 tile = ImVec2(1, 1))
{
    const art::Image* img = art::get(name);
    float ix = (img && tile.x == 1) ? 0.5f / img->width : 0, iy = (img && tile.y == 1) ? 0.5f / img->height : 0;
    float u0 = ix, u1 = tile.x - ix, v0 = iy, v1 = tile.y - iy;
    ImVec2 uv0(flipX ? u1 : u0, flipY ? v1 : v0), uv1(flipX ? u0 : u1, flipY ? v0 : v1);
    dl->AddImage(tex, a, b, uv0, uv1);
}

void wave_window(ImDrawList* dl, ImVec2 a, ImVec2 b, float s, bool shadow)
{
    const char* lt = shadow ? "WaveWindowShLT_00" : "WaveWindowLT_00";
    const char* t = shadow ? "WaveWindowShT_00" : "WaveWindowT_00";
    const char* l = shadow ? "WaveWindowShL_00" : "WaveWindowL_00";
    ImU32 dark = shadow ? IM_COL32(0, 0, 0, 0) : kWindow, light = shadow ? IM_COL32(0, 0, 0, 120) : kWindowLight;
    void* tLT = art::texture_tinted(lt, dark, light);
    void* tT = art::texture_tinted(t, dark, light);
    void* tL = art::texture_tinted(l, dark, light);
    if (!tLT || !tT || !tL)
    {
        if (!shadow) dl->AddRectFilled(a, b, kWindow, 16 * s);
        return;
    }
    float c = 130 * s;
    float w = b.x - a.x, h = b.y - a.y;
    float tilesX = std::max(1.0f, std::round((w - 2 * c) / (140 * s))), tilesY = std::max(1.0f, std::round((h - 2 * c) / (140 * s)));
    if (!shadow) dl->AddRectFilled(ImVec2(a.x + c * 0.5f, a.y + c * 0.5f), ImVec2(b.x - c * 0.5f, b.y - c * 0.5f), kWindow);
    piece(dl, tT, t, ImVec2(a.x + c, a.y), ImVec2(b.x - c, a.y + c), false, false, ImVec2(tilesX, 1));
    piece(dl, tT, t, ImVec2(a.x + c, b.y - c), ImVec2(b.x - c, b.y), false, true, ImVec2(tilesX, 1));
    piece(dl, tL, l, ImVec2(a.x, a.y + c), ImVec2(a.x + c, b.y - c), false, false, ImVec2(1, tilesY));
    piece(dl, tL, l, ImVec2(b.x - c, a.y + c), ImVec2(b.x, b.y - c), true, false, ImVec2(1, tilesY));
    piece(dl, tLT, lt, a, ImVec2(a.x + c, a.y + c), false, false);
    piece(dl, tLT, lt, ImVec2(b.x - c, a.y), ImVec2(b.x, a.y + c), true, false);
    piece(dl, tLT, lt, ImVec2(a.x, b.y - c), ImVec2(a.x + c, b.y), false, true);
    piece(dl, tLT, lt, ImVec2(b.x - c, b.y - c), b, true, true);
}

void draw_panel(ImDrawList* dl, ImVec2 a, ImVec2 b, float s)
{
    float k = 0.75f * s;
    wave_window(dl, a, b, k, false);
    if (void* paper = art::texture_tinted("PaperPattern_00", IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 18)))
    {
        float m = 60 * k;
        ImVec2 p0(a.x + m, a.y + m), p1(b.x - m, b.y - m);
        dl->AddImage(paper, p0, p1, ImVec2(0, 0), ImVec2((p1.x - p0.x) / (128 * s), (p1.y - p0.y) / (128 * s)));
    }
}

void button_art(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 face, ImU32 ink)
{
    if (void* tex = art::texture_tinted("MenuBtnBase_00", ink, face))
        dl->AddImage(tex, a, b);
    else
        dl->AddRectFilled(a, b, face, 8);
}

void apply_style(float s)
{
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    if (!g_styled)
    {
        ImGui::StyleColorsDark(&st);
        st.ScaleAllSizes(s);
        st.Colors[ImGuiCol_WindowBg].w = 1.0f;
        return;
    }
    st.WindowPadding = ImVec2(70 * s, 56 * s);
    st.FramePadding = ImVec2(12 * s, 6 * s);
    st.ItemSpacing = ImVec2(12 * s, 10 * s);
    st.ItemInnerSpacing = ImVec2(10 * s, 6 * s);
    st.FrameRounding = 8 * s;
    st.GrabRounding = 8 * s;
    st.PopupRounding = 8 * s;
    st.ScrollbarRounding = 8 * s;
    st.ScrollbarSize = 14 * s;
    st.GrabMinSize = 10 * s;
    st.WindowBorderSize = 0;
    st.FrameBorderSize = 1.5f * s;
    st.PopupBorderSize = 1.5f * s;
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = col(kText);
    c[ImGuiCol_TextDisabled] = col(kTextDim);
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = col(IM_COL32(18, 46, 92, 250));
    c[ImGuiCol_Border] = col(IM_COL32(200, 214, 236, 160));
    c[ImGuiCol_FrameBg] = col(kFrame);
    c[ImGuiCol_FrameBgHovered] = col(kFrameHover);
    c[ImGuiCol_FrameBgActive] = col(IM_COL32(56, 108, 180, 255));
    c[ImGuiCol_CheckMark] = col(kGold);
    c[ImGuiCol_SliderGrab] = col(IM_COL32(244, 200, 84, 200));
    c[ImGuiCol_SliderGrabActive] = col(IM_COL32(255, 224, 130, 255));
    c[ImGuiCol_Button] = col(kFrame);
    c[ImGuiCol_ButtonHovered] = col(kFrameHover);
    c[ImGuiCol_ButtonActive] = col(kGoldDark);
    c[ImGuiCol_Header] = col(IM_COL32(48, 96, 164, 255));
    c[ImGuiCol_HeaderHovered] = col(kFrameHover);
    c[ImGuiCol_HeaderActive] = col(kGoldDark);
    c[ImGuiCol_Separator] = col(IM_COL32(200, 214, 236, 110));
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = col(IM_COL32(90, 130, 190, 255));
    c[ImGuiCol_ScrollbarGrabHovered] = col(IM_COL32(120, 160, 215, 255));
    c[ImGuiCol_ScrollbarGrabActive] = col(kGold);
    c[ImGuiCol_NavHighlight] = col(kGold);
    c[ImGuiCol_TableHeaderBg] = col(IM_COL32(26, 60, 116, 255));
    c[ImGuiCol_TableRowBgAlt] = col(IM_COL32(255, 255, 255, 12));
    c[ImGuiCol_TableBorderLight] = col(IM_COL32(200, 214, 236, 60));
    c[ImGuiCol_TableBorderStrong] = col(IM_COL32(200, 214, 236, 120));
}

float g_uiScale = 1.0f;

float wobble(uint32 seed, int i)
{
    uint32 h = seed * 2654435761u + (uint32)i * 40503u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (float)(h & 0xFFFF) / 65535.0f - 0.5f;
}

ImU32 faded(ImU32 c) { return ImGui::GetColorU32(ImGui::ColorConvertU32ToFloat4(c)); }

void ink_rect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c, float th, uint32 seed, float rounding)
{
    for (int pass = 0; pass < 2; pass++)
    {
        float j = 1.2f * g_uiScale;
        ImVec2 o(wobble(seed, pass * 4) * j, wobble(seed, pass * 4 + 1) * j);
        ImVec2 e(wobble(seed, pass * 4 + 2) * j, wobble(seed, pass * 4 + 3) * j);
        dl->AddRect(ImVec2(a.x + o.x, a.y + o.y), ImVec2(b.x + e.x, b.y + e.y), faded(c), rounding, 0, pass ? th * 0.6f : th);
    }
}

void ink_line(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 c, float th, uint32 seed)
{
    float len = std::hypot(p1.x - p0.x, p1.y - p0.y);
    ImVec2 n(-(p1.y - p0.y) / std::max(len, 1.0f), (p1.x - p0.x) / std::max(len, 1.0f));
    float amp = 1.6f * g_uiScale;
    ImVec2 c1(p0.x + (p1.x - p0.x) / 3 + n.x * wobble(seed, 1) * amp * 2, p0.y + (p1.y - p0.y) / 3 + n.y * wobble(seed, 1) * amp * 2);
    ImVec2 c2(p0.x + (p1.x - p0.x) * 2 / 3 + n.x * wobble(seed, 2) * amp * 2, p0.y + (p1.y - p0.y) * 2 / 3 + n.y * wobble(seed, 2) * amp * 2);
    dl->AddBezierCubic(p0, c1, c2, p1, faded(c), th);
}

std::string visible_label(const char* label)
{
    std::string l = label;
    if (size_t h = l.find("##"); h != std::string::npos) l = l.substr(0, h);
    return l;
}

void sound_throttled(sfx::Sound snd)
{
    static double s_last = 0;
    double now = ImGui::GetTime();
    if (now - s_last > 0.07)
    {
        sfx::play(snd);
        s_last = now;
    }
}

bool toggle(const char* label, bool* v)
{
    if (!g_styled) return ImGui::Checkbox(label, v);
    ImGuiStyle& st = ImGui::GetStyle();
    float h = ImGui::GetFrameHeight(), w = h * 1.9f;
    std::string text = visible_label(label);
    ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGuiID id = ImGui::GetID(label);
    bool pressed = ImGui::InvisibleButton(label, ImVec2(w + (ts.x > 0 ? st.ItemInnerSpacing.x + ts.x : 0), h));
    if (pressed)
    {
        *v = !*v;
        sfx::play(*v ? sfx::kSelect : sfx::kBack);
    }
    bool hovered = ImGui::IsItemHovered();
    ImGuiStorage* store = ImGui::GetStateStorage();
    float t = store->GetFloat(id, *v ? 1.0f : 0.0f);
    t += ((*v ? 1.0f : 0.0f) - t) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
    store->SetFloat(id, t);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a(p.x, p.y + h * 0.12f), b(p.x + w, p.y + h * 0.88f);
    ImVec4 off = col(hovered ? kFrameHover : kFrame), on = col(kGold);
    ImVec4 fill(off.x + (on.x - off.x) * t, off.y + (on.y - off.y) * t, off.z + (on.z - off.z) * t, 1.0f);
    float r = (b.y - a.y) * 0.5f;
    dl->AddRectFilled(a, b, ImGui::GetColorU32(fill), r);
    ink_rect(dl, a, b, kWindowLight, 2.0f * g_uiScale, id, r);
    float kx = a.x + r + t * (w - 2 * r);
    ImVec2 kc(kx, (a.y + b.y) * 0.5f);
    dl->AddCircleFilled(kc, r * 0.78f, faded(*v ? IM_COL32(255, 250, 235, 255) : IM_COL32(200, 214, 236, 255)));
    dl->AddCircle(kc, r * 0.78f, faded(*v ? kGoldDark : IM_COL32(16, 42, 86, 255)), 0, 2.0f * g_uiScale);
    if (ts.x > 0)
        dl->AddText(ImVec2(p.x + w + st.ItemInnerSpacing.x, p.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
    return pressed;
}

struct ClearFrame
{
    ClearFrame()
    {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    }
    ~ClearFrame() { ImGui::PopStyleColor(9); }
};

template<typename Fn>
bool slider_impl(const char* label, float frac, const std::string& value, Fn&& widget)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();
    float w = ImGui::CalcItemWidth(), h = ImGui::GetFrameHeight();
    ImGuiID id = ImGui::GetID(label);
    ImDrawListSplitter split;
    split.Split(dl, 2);
    split.SetCurrentChannel(dl, 1);
    bool changed;
    {
        ClearFrame cf;
        changed = widget();
    }
    bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
    split.SetCurrentChannel(dl, 0);
    float pad = h * 0.5f, cy = a.y + h * 0.5f;
    float x0 = a.x + pad, x1 = a.x + w - pad, kx = x0 + std::clamp(frac, 0.0f, 1.0f) * (x1 - x0);
    ink_line(dl, ImVec2(x0, cy), ImVec2(x1, cy), IM_COL32(200, 214, 236, 150), 4.0f * g_uiScale, id);
    if (kx > x0 + 1) ink_line(dl, ImVec2(x0, cy), ImVec2(kx, cy), kGold, 6.0f * g_uiScale, id + 1);
    float kr = h * (active ? 0.36f : hovered ? 0.33f : 0.3f);
    dl->AddCircleFilled(ImVec2(kx, cy), kr, faded(IM_COL32(255, 250, 235, 255)));
    dl->AddCircle(ImVec2(kx, cy), kr, faded(kGoldDark), 0, 2.5f * g_uiScale);
    split.SetCurrentChannel(dl, 1);
    ImVec2 ts = ImGui::CalcTextSize(value.c_str());
    ImVec2 tp(a.x + w + ImGui::GetStyle().ItemInnerSpacing.x * 0.0f - ts.x - pad * 0.2f, a.y - ts.y * 0.9f);
    tp = ImVec2(x1 - ts.x * 0.5f, a.y + (h - ts.y) * 0.5f);
    tp.x = std::min(tp.x, a.x + w - ts.x);
    tp = ImVec2(a.x + w - ts.x - pad * 1.6f, a.y + (h - ts.y) * 0.5f);
    if (std::abs((tp.x + ts.x * 0.5f) - kx) < ts.x * 0.5f + kr) tp.x = a.x + pad * 1.6f;
    for (int ox = -1; ox <= 1; ox++)
        for (int oy = -1; oy <= 1; oy++)
            if (ox || oy) dl->AddText(ImVec2(tp.x + ox * g_uiScale * 1.5f, tp.y + oy * g_uiScale * 1.5f), faded(IM_COL32(10, 28, 60, 255)), value.c_str());
    dl->AddText(tp, ImGui::GetColorU32(ImGuiCol_Text), value.c_str());
    split.Merge(dl);
    if (changed) sound_throttled(sfx::kMove);
    return changed;
}

template<typename T>
std::string printf_value(const char* fmt, T v)
{
    char buf[64];
    snprintf(buf, sizeof(buf), fmt, v);
    return buf;
}

int nav_step()
{
    int d = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft)) d--;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight)) d++;
    return d;
}

bool slider_float(const char* label, float* v, float mn, float mx, const char* fmt)
{
    if (!g_styled) return ImGui::SliderFloat(label, v, mn, mx, fmt);
    bool stepped = false;
    if (ImGui::GetFocusID() == ImGui::GetID(label) && ImGui::GetActiveID() != ImGui::GetID(label))
        if (int d = nav_step())
        {
            *v = std::clamp(*v + d * (mx - mn) / 40.0f, mn, mx);
            stepped = true;
            sound_throttled(sfx::kMove);
        }
    return stepped | slider_impl(label, (*v - mn) / (mx - mn), printf_value(fmt, *v), [&] { return ImGui::SliderFloat(label, v, mn, mx, ""); });
}

bool slider_int(const char* label, int* v, int mn, int mx, const char* fmt)
{
    if (!g_styled) return ImGui::SliderInt(label, v, mn, mx, fmt);
    bool stepped = false;
    if (ImGui::GetFocusID() == ImGui::GetID(label) && ImGui::GetActiveID() != ImGui::GetID(label))
        if (int d = nav_step())
        {
            *v = std::clamp(*v + d * std::max(1, (mx - mn) / 20), mn, mx);
            stepped = true;
            sound_throttled(sfx::kMove);
        }
    return stepped | slider_impl(label, (float)(*v - mn) / (float)(mx - mn), printf_value(fmt, *v), [&] { return ImGui::SliderInt(label, v, mn, mx, ""); });
}

bool button(const char* label, ImVec2 size = ImVec2(0, 0))
{
    if (!g_styled) return ImGui::Button(label, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImDrawListSplitter split;
    split.Split(dl, 2);
    split.SetCurrentChannel(dl, 1);
    bool pressed;
    {
        ClearFrame cf;
        pressed = ImGui::Button(label, size);
    }
    bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
    split.SetCurrentChannel(dl, 0);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    button_art(dl, a, b, faded(active ? kGoldDark : hovered ? kFrameHover : kFrame), faded(IM_COL32(220, 230, 245, 255)));
    split.Merge(dl);
    if (pressed) sfx::play(sfx::kConfirm);
    return pressed;
}

bool begin_combo(const char* label, const char* preview)
{
    if (!g_styled) return ImGui::BeginCombo(label, preview);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();
    float w = ImGui::CalcItemWidth(), h = ImGui::GetFrameHeight();
    ImVec2 b(a.x + w, a.y + h);
    ImGuiID id = ImGui::GetID(label);
    bool wasOpen = ImGui::IsPopupOpen(id, ImGuiPopupFlags_None);
    bool hovered = ImGui::IsMouseHoveringRect(a, b) && !ImGui::GetCurrentContext()->DisabledStackSize;
    ImDrawListSplitter split;
    split.Split(dl, 2);
    split.SetCurrentChannel(dl, 1);
    bool open;
    {
        ClearFrame cf;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x + 14 * g_uiScale, ImGui::GetStyle().FramePadding.y));
        open = ImGui::BeginCombo(label, preview, ImGuiComboFlags_NoArrowButton);
        ImGui::PopStyleVar();
    }
    split.SetCurrentChannel(dl, 0);
    button_art(dl, a, b, faded(open ? IM_COL32(48, 96, 164, 255) : hovered ? kFrameHover : kFrame), faded(IM_COL32(220, 230, 245, 255)));
    if (void* arrow = art::texture_tinted("OptionArrow_00", IM_COL32(244, 200, 84, 0), IM_COL32(244, 200, 84, 255)))
    {
        float ah = h * 0.78f, aw = ah * 64.0f / 96.0f;
        ImVec2 p0(b.x - aw - h * 0.3f, a.y + (h - ah) * 0.5f);
        dl->AddImage(arrow, p0, ImVec2(p0.x + aw, p0.y + ah), ImVec2(0, 1), ImVec2(1, 0), faded(IM_COL32(255, 255, 255, 255)));
    }
    split.Merge(dl);
    if (open && !wasOpen) sfx::play(sfx::kConfirm);
    return open;
}

bool combo(const char* label, int& v, std::initializer_list<const char*> items)
{
    std::vector<const char*> list(items);
    bool changed = false;
    if (begin_combo(label, v >= 0 && v < (int)list.size() ? list[(size_t)v] : ""))
    {
        for (int i = 0; i < (int)list.size(); i++)
            if (ImGui::Selectable(list[(size_t)i], i == v))
            {
                v = i;
                changed = true;
            }
        ImGui::EndCombo();
    }
    return changed;
}

void hint(const char* text)
{
    bool focused = ImGui::IsItemFocused() && !GImGui->NavDisableHighlight;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered() || focused) ImGui::SetTooltip("%s", text);
}

void section(const char* title)
{
    if (!g_styled)
    {
        ImGui::Spacing();
        ImGui::SeparatorText(title);
        return;
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, col(kHeading));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ink_line(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + 1), ImVec2(p.x + w, p.y + 2), IM_COL32(200, 214, 236, 120), 2.0f * g_uiScale, ImGui::GetID(title));
    ImGui::Dummy(ImVec2(w, 6 * g_uiScale));
}

void resolution_combo(settings::Settings& s, bool& dirty)
{
    const gp::Pack* res = gp::resolution_pack();
    if (!res)
    {
        ImGui::BeginDisabled();
        int native = 0;
        combo("Internal resolution", native, {"1920x1080 (native)"});
        ImGui::EndDisabled();
        hint("Other resolutions use Cemu's Wind Waker HD Resolution graphic pack, which wasn't found. Install Cemu with its "
             "community graphic packs, or put the pack in %APPDATA%\\WWHDRecomp\\graphicPacks.");
        return;
    }
    std::vector<std::string> items;
    std::string category = res->categories.empty() ? std::string() : res->categories[0];
    std::string defaultName;
    for (const gp::Preset& p : res->presets)
        if (p.category == category)
        {
            items.push_back(p.name);
            if (p.isDefault && defaultName.empty()) defaultName = p.name;
        }
    if (defaultName.empty() && !items.empty()) defaultName = items[0];
    items.push_back(gp::kMatchWindow);
    std::string current = settings::pack_enabled(s, res->id) ? settings::pack_preset(s, res->id, category) : defaultName;
    if (current.empty()) current = defaultName;
    if (begin_combo("Internal resolution", current.c_str()))
    {
        for (const std::string& item : items)
        {
            bool header = item.starts_with("----");
            if (header)
            {
                ImGui::TextDisabled("%s", item.c_str());
                continue;
            }
            if (ImGui::Selectable(item.c_str(), item == current))
            {
                bool native = item == defaultName;
                settings::set_pack_enabled(s, res->id, !native);
                settings::set_pack_preset(s, res->id, category, item);
                dirty = true;
            }
        }
        ImGui::EndCombo();
    }
    hint("The resolution the game renders at (from Cemu's Resolution graphic pack). Higher values look sharper and smooth "
         "edges like supersampling; Match window renders at your window's size. Applies after restart.");
}

void tab_graphics(settings::Settings& s, bool& dirty)
{
    section("Rendering");
    resolution_combo(s, dirty);
    ImGui::BeginDisabled();
    int msaa = 0;
    combo("MSAA", msaa, {"Not available"});
    ImGui::EndDisabled();
    hint("The game's renderer draws to its own buffers, so hardware MSAA can't be applied. Use a higher internal resolution or FXAA.");
    dirty |= toggle("FXAA", &s.fxaa);
    dirty |= toggle("Wait for new shaders", &s.waitForShaders);
    hint("The first time something is drawn, its shaders are built for your GPU. On: the game pauses briefly until they're ready, "
         "so nothing pops in. Off: no pauses, but new objects can appear a moment late. Either way it only happens once.");
    dirty |= slider_int("Sharpening", &s.sharpening, 0, 100, s.sharpening ? "%d%%" : "Off");
    section("Textures");
    int aniso = s.anisotropy == 0 ? 0 : s.anisotropy <= 2 ? 1 : s.anisotropy <= 4 ? 2 : s.anisotropy <= 8 ? 3 : 4;
    if (combo("Anisotropic filtering", aniso, {"As the game sets it", "2x", "4x", "8x", "16x"}))
    {
        s.anisotropy = aniso == 0 ? 0 : 1 << aniso;
        dirty = true;
    }
    dirty |= combo("Texture filtering", s.textureFilter, {"As the game sets it", "Smooth (trilinear)", "Sharp pixels (nearest)"});
    section("Scaling to the window");
    dirty |= combo("Upscale filter", s.upscaleFilter, {"Bilinear", "Bicubic", "Hermite", "Nearest neighbor"});
    dirty |= combo("Downscale filter", s.downscaleFilter, {"Bilinear", "Bicubic", "Hermite", "Nearest neighbor"});
}

void tab_video(settings::Settings& s, bool& dirty)
{
    section("Display");
    dirty |= toggle("Fullscreen  (F11, Alt+Enter)", &s.fullscreen);
    dirty |= combo("Aspect ratio", s.aspect, {"16:9 (keep aspect ratio)", "Stretch to window"});
    dirty |= combo("V-Sync", s.vsync, {"Off", "On", "Fast (mailbox, no tearing)"});
    {
        static constexpr int kRates[] = {30, 60, 75, 90, 100, 120, 144, 165, 180, 240, 0};
        static constexpr const char* kNames[] = {"30 fps (original)", "60 fps", "75 fps", "90 fps", "100 fps", "120 fps",
                                                 "144 fps", "165 fps", "180 fps", "240 fps", "Match the display"};
        int idx = 1;
        for (int i = 0; i < (int)std::size(kRates); i++)
            if (kRates[i] == s.fps) idx = i;
        if (begin_combo("Frame rate", kNames[idx]))
        {
            for (int i = 0; i < (int)std::size(kRates); i++)
                if (ImGui::Selectable(kNames[i], i == idx))
                {
                    s.fps = kRates[i];
                    dirty = true;
                }
            ImGui::EndCombo();
        }
        hint("The game's logic stays at 30 ticks per second (so it plays the same); every frame in between is "
             "interpolated. Pick your monitor's refresh rate with V-Sync on.");
    }
    dirty |= toggle("Smooth menus and HUD (experimental)", &s.smoothUi);
    hint("Animates menus and the HUD at your full frame rate instead of the game's 30. Some HUD pieces can briefly "
         "jump or overlap at very high frame rates.");
    dirty |= slider_int("Field of view", &s.fieldOfView, 40, 100, s.fieldOfView == 60 ? "60 (original)" : "%d");
    hint("The camera's vertical angle of view in degrees. Wider shows more around Link; the game's own is 60.");
    section("Color");
    dirty |= slider_float("Brightness", &s.brightness, -0.5f, 0.5f, "%+.2f");
    dirty |= slider_float("Contrast", &s.contrast, 0.5f, 1.5f, "%.2f");
    dirty |= slider_float("Saturation", &s.saturation, 0.0f, 2.0f, "%.2f");
    dirty |= slider_float("Gamma", &s.gamma, 0.5f, 2.0f, "%.2f");
    if (button("Reset color"))
    {
        s.brightness = 0;
        s.contrast = s.saturation = s.gamma = 1;
        dirty = true;
    }
    section("Overlays");
    dirty |= combo("Performance overlay", s.overlayPosition, {"Off", "Top left", "Top center", "Top right", "Bottom left", "Bottom center", "Bottom right"});
    if (s.overlayPosition)
    {
        ImGui::Indent();
        dirty |= toggle("FPS and frame time", &s.overlayFps);
        dirty |= toggle("Draw calls", &s.overlayDrawcalls);
        dirty |= toggle("CPU usage", &s.overlayCpu);
        dirty |= toggle("RAM", &s.overlayRam);
        dirty |= toggle("VRAM", &s.overlayVram);
        ImGui::Unindent();
    }
    dirty |= toggle("GamePad screen overlay  (F2)", &s.gamepadOverlay);
}

struct BindingGroup { const char* title; std::initializer_list<int> actions; };
const BindingGroup kBindingGroups[] = {
    {"Movement", {settings::kActMoveUp, settings::kActMoveDown, settings::kActMoveLeft, settings::kActMoveRight}},
    {"Camera", {settings::kActCameraUp, settings::kActCameraDown, settings::kActCameraLeft, settings::kActCameraRight}},
    {"Buttons", {settings::kActA, settings::kActB, settings::kActX, settings::kActY, settings::kActL, settings::kActR, settings::kActZL,
                 settings::kActZR}},
    {"D-pad and menus", {settings::kActUp, settings::kActDown, settings::kActLeft, settings::kActRight, settings::kActPlus, settings::kActMinus,
                         settings::kActStickL, settings::kActStickR}},
    {"Windfall", {settings::kActFastForward}},
};
constexpr double kCaptureSeconds = 6.0;
double g_captureStart = 0;

std::string shared_with(const settings::Settings& s, int action, bool key)
{
    std::string out;
    const settings::Binding& b = s.bindings[(size_t)action];
    if (key ? b.key == 0 : b.pad == settings::kPadNone) return out;
    for (int a = 0; a < settings::kActionCount; a++)
        if (a != action && (key ? s.bindings[(size_t)a].key == b.key : s.bindings[(size_t)a].pad == b.pad))
            out += (out.empty() ? "" : ", ") + std::string(settings::action_name(a));
    return out;
}

void stick_view(const char* label, int up, int down, int left, int right, float deadzone)
{
    float size = ImGui::GetFrameHeight() * 3.2f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size + ImGui::GetTextLineHeightWithSpacing()));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    float r = size * 0.45f;
    float x = input::action_value(right) - input::action_value(left), y = input::action_value(down) - input::action_value(up);
    float len = std::sqrt(x * x + y * y);
    if (len > 1.0f)
    {
        x /= len;
        y /= len;
    }
    dl->AddCircleFilled(c, r, kFrame, 48);
    dl->AddCircle(c, r, IM_COL32(200, 214, 236, 160), 48, 2.0f * g_uiScale);
    if (deadzone > 0) dl->AddCircle(c, r * deadzone, IM_COL32(200, 214, 236, 70), 32, 1.5f * g_uiScale);
    bool moved = x != 0 || y != 0;
    ImVec2 knob(c.x + x * r, c.y + y * r);
    if (moved) dl->AddLine(c, knob, IM_COL32(244, 200, 84, 160), 2.0f * g_uiScale);
    dl->AddCircleFilled(knob, r * 0.16f, moved ? kGold : IM_COL32(150, 176, 210, 255), 24);
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, p.y + size + 2 * g_uiScale), ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
}

void tab_input(settings::Settings& s, bool& dirty)
{
    section("Controller");
    std::string pad = input::controller_name();
    if (pad.empty()) ImGui::TextDisabled("No controller connected. You can plug one in at any time.");
    else ImGui::Text("Connected: %s", pad.c_str());
    stick_view("Move", settings::kActMoveUp, settings::kActMoveDown, settings::kActMoveLeft, settings::kActMoveRight, s.deadzone);
    ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 3);
    stick_view("Camera", settings::kActCameraUp, settings::kActCameraDown, settings::kActCameraLeft, settings::kActCameraRight, s.deadzone);
    dirty |= slider_float("Dead zone", &s.deadzone, 0.0f, 0.5f, "%.2f");
    hint("How far a stick has to move before it counts. Raise it if Link or the camera drift by themselves.");
    dirty |= toggle("Invert camera X", &s.invertCameraX);
    dirty |= toggle("Invert camera Y", &s.invertCameraY);

    section("Bindings");
    ImGui::TextDisabled("Pick a binding, then press the key or button for it. Delete clears it, Escape cancels.");
    ImGui::TextDisabled("Actions light up while you hold them.");
    if (g_capture != Capture::None && ImGui::GetTime() - g_captureStart > kCaptureSeconds)
    {
        g_capture = Capture::None;
        g_captureAction = -1;
    }
    int secondsLeft = (int)std::ceil(kCaptureSeconds - (ImGui::GetTime() - g_captureStart));
    for (const BindingGroup& group : kBindingGroups)
    {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, col(kHeading));
        ImGui::TextUnformatted(group.title);
        ImGui::PopStyleColor();
        if (!ImGui::BeginTable(group.title, 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
            continue;
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        for (int a : group.actions)
        {
            ImGui::PushID(a);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            bool held = input::action_value(a) > 0.5f;
            ImGui::PushStyleColor(ImGuiCol_Text, held ? col(kGold) : ImGui::GetStyleColorVec4(ImGuiCol_Text));
            ImGui::TextUnformatted(settings::action_name(a));
            ImGui::PopStyleColor();
            for (bool key : {true, false})
            {
                ImGui::TableNextColumn();
                bool capturing = g_capture == (key ? Capture::Key : Capture::Pad) && g_captureAction == a;
                std::string name;
                if (capturing) name = std::format("{} ({})", key ? "Press a key..." : "Press a button...", secondsLeft);
                else if (key)
                {
                    const char* k = s.bindings[(size_t)a].key ? SDL_GetScancodeName((SDL_Scancode)s.bindings[(size_t)a].key) : "";
                    name = *k ? k : "-";
                }
                else name = settings::pad_input_name(s.bindings[(size_t)a].pad);
                std::string shared = capturing ? std::string() : shared_with(s, a, key);
                if (!shared.empty())
                {
                    name += "  (!)";
                    ImGui::PushStyleColor(ImGuiCol_Text, col(IM_COL32(255, 170, 120, 255)));
                }
                bool pressed = button((name + (key ? "##key" : "##pad")).c_str(), ImVec2(-FLT_MIN, 0));
                if (!shared.empty())
                {
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered() || (ImGui::IsItemFocused() && !GImGui->NavDisableHighlight))
                        ImGui::SetTooltip("Also used for %s", shared.c_str());
                }
                if (pressed)
                {
                    g_capture = key ? Capture::Key : Capture::Pad;
                    g_captureAction = a;
                    g_captureStart = ImGui::GetTime();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    settings::Settings defaults;
    settings::set_default_bindings(defaults);
    if (button("Reset keyboard"))
    {
        for (int a = 0; a < settings::kActionCount; a++) s.bindings[(size_t)a].key = defaults.bindings[(size_t)a].key;
        dirty = true;
    }
    ImGui::SameLine();
    if (button("Reset controller"))
    {
        for (int a = 0; a < settings::kActionCount; a++) s.bindings[(size_t)a].pad = defaults.bindings[(size_t)a].pad;
        dirty = true;
    }
}

void tab_audio(settings::Settings& s, bool& dirty)
{
    section("Volume");
    dirty |= slider_int("Master volume", &s.volume, 0, 100, "%d%%");
    dirty |= toggle("Mute while in the background", &s.muteInBackground);
    hint("Silent while another window has the focus.");
}

bool text_field(const char* label, std::string& value, size_t maxLen, float width = 0)
{
    char buf[256] = {};
    std::snprintf(buf, sizeof buf, "%s", value.c_str());
    ImGui::SetNextItemWidth(width > 0 ? width : ImGui::GetContentRegionAvail().x * 0.6f);
    if (!ImGui::InputText(label, buf, std::min(maxLen + 1, sizeof buf))) return false;
    value = buf;
    return true;
}

constexpr const char* kPublicServer = "wss://windfall.bigdick.app";

struct Card
{
    ImDrawList* dl = nullptr;
    ImDrawListSplitter split;
    ImVec2 start;
    float width = 0, inner = 0;
    const char* title = "";
};
void begin_card(Card& c, const char* title)
{
    c.dl = ImGui::GetWindowDrawList();
    c.title = title;
    c.split.Split(c.dl, 2);
    c.split.SetCurrentChannel(c.dl, 1);
    c.start = ImGui::GetCursorScreenPos();
    c.width = ImGui::GetContentRegionAvail().x;
    float pad = 18 * g_uiScale;
    c.inner = c.width - 2 * pad;
    ImGui::SetCursorScreenPos(ImVec2(c.start.x + pad, c.start.y + pad * 0.7f));
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, col(kHeading));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::PushItemWidth(c.inner);
}
void end_card(Card& c)
{
    float pad = 18 * g_uiScale;
    ImGui::PopItemWidth();
    ImGui::EndGroup();
    ImVec2 end(c.start.x + c.width, ImGui::GetItemRectMax().y + pad * 0.7f);
    c.split.SetCurrentChannel(c.dl, 0);
    if (g_styled)
    {
        c.dl->AddRectFilled(c.start, end, faded(IM_COL32(8, 26, 58, 150)), 14 * g_uiScale);
        ink_rect(c.dl, c.start, end, IM_COL32(200, 214, 236, 150), 2.0f * g_uiScale, ImGui::GetID(c.title), 14 * g_uiScale);
    }
    else
        c.dl->AddRect(c.start, end, ImGui::GetColorU32(ImGuiCol_Border), ImGui::GetStyle().FrameRounding);
    c.split.Merge(c.dl);
    ImGui::SetCursorScreenPos(ImVec2(c.start.x, end.y));
    ImGui::Dummy(ImVec2(c.width, ImGui::GetStyle().ItemSpacing.y));
}

std::string random_room_code()
{
    static constexpr char kChars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // no 0/O or 1/I
    uint32 h = (uint32)(ImGui::GetTime() * 1000003.0) ^ (uint32)SDL_GetTicksNS();
    std::string code;
    for (int i = 0; i < 6; i++)
    {
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        code += kChars[h % (sizeof kChars - 1)];
    }
    return code;
}

// what a player plays, for the player list: the original game or a randomizer seed
std::string world_label(const std::string& world)
{
    if (world.empty()) return "Original game";
    std::string seed = rando::parse_world(world).seed;
    return "Randomizer: " + (seed.empty() ? std::string("?") : seed);
}

void tab_multiplayer(settings::Settings& s, bool& dirty)
{
    bool joined = mp::connected();
    std::string status = mp::status();
    std::vector<mp::PlayerInfo> players = mp::room_info();

    section("Connection");
    {
        bool connecting = !joined && status.find("onnecting") != std::string::npos;
        ImU32 c = joined ? IM_COL32(110, 220, 130, 255) : connecting ? kGold : IM_COL32(150, 176, 210, 255);
        std::string line = joined ? std::format("In room {} ({} of 8 players)", mp::room_code(), players.size()) : status;
        ImGui::PushStyleColor(ImGuiCol_Text, col(c));
        ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::BeginDisabled(joined);
    float field = ImGui::GetContentRegionAvail().x * 0.5f;
    if (text_field("Name", s.mpName, 15, field)) dirty = true;
    hint("Shown over your head to the others in the room.");
    {
        const char* roll = "Random";
        float bw = ImGui::CalcTextSize(roll).x + ImGui::GetStyle().FramePadding.x * 4;
        char buf[32] = {};
        std::snprintf(buf, sizeof buf, "%s", joined ? mp::room_code().c_str() : s.mpRoom.c_str());
        ImGui::SetNextItemWidth(field - bw - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::InputText("##room", buf, 16, ImGuiInputTextFlags_CharsUppercase))
        {
            s.mpRoom = buf;
            dirty = true;
        }
        ImGui::SameLine();
        if (button(roll, ImVec2(bw, 0)))
        {
            s.mpRoom = random_room_code();
            dirty = true;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Room code");
        hint("Everyone who enters the same code plays together (up to 8 players).");
    }
    int server = s.mpServer == kPublicServer ? 0 : 1;
    ImGui::SetNextItemWidth(field);
    if (combo("Server", server, {"Windfall public server", "Your own server"}))
    {
        if (server == 0) s.mpServer = kPublicServer;
        else if (s.mpServer == kPublicServer) s.mpServer = "ws://";
        dirty = true;
    }
    if (server == 1)
    {
        if (text_field("Address", s.mpServer, 120, field)) dirty = true;
        hint("host:port for UDP, or a ws:// or wss:// address. The server is tools/mp_server/windfall_server.py.");
    }
    ImGui::EndDisabled();
    if (!joined)
    {
        if (button("Join room"))
        {
            if (s.mpRoom.empty()) s.mpRoom = random_room_code();
            mp::connect(s.mpServer, s.mpRoom, s.mpName);
            dirty = true;
        }
    }
    else if (button("Leave room"))
        mp::disconnect();

    section("Players");
    if (!joined) ImGui::TextDisabled("Join a room to see who's playing.");
    else if (ImGui::BeginTable("##players", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (const char* h : {"Name", "Where", "Game"})
        {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", h);
        }
        for (const mp::PlayerInfo& pl : players)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // a small square in their tunic color, then the name
            float sz = ImGui::GetTextLineHeight() * 0.7f;
            ImVec2 p = ImGui::GetCursorScreenPos();
            float y = p.y + (ImGui::GetTextLineHeight() - sz) * 0.5f;
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + sz, y + sz), mp::tunic_color(pl.color), 2.0f * g_uiScale);
            ImGui::Dummy(ImVec2(sz, sz));
            ImGui::SameLine();
            ImGui::Text("%s%s", pl.name.c_str(), pl.self ? " (you)" : "");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(pl.place.empty() ? "Title screen" : pl.place.c_str());
            ImGui::TableNextColumn();
            if (pl.sameWorld) ImGui::TextUnformatted(world_label(pl.world).c_str());
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Text, col(IM_COL32(255, 170, 120, 255)));
                ImGui::TextUnformatted(world_label(pl.world).c_str());
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Not the same game as yours, so items aren't shared with them. The Randomizer tab can make their seed.");
            }
        }
        ImGui::EndTable();
    }

    section("Chat");
    {
        std::vector<mp::ChatEntry> chat = mp::recent_chat(6);
        if (chat.empty()) ImGui::TextDisabled("%s", joined ? "No messages yet. Press Enter while playing to chat." : "Join a room to chat.");
        for (const mp::ChatEntry& e : chat)
        {
            ImU32 nc = e.color < 0 ? IM_COL32(255, 255, 255, 255) : mp::tunic_color(e.color);
            ImGui::PushStyleColor(ImGuiCol_Text, col(nc));
            ImGui::TextUnformatted((e.name + ":").c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextWrapped("%s", e.text.c_str());
        }
    }

    section("Options");
    if (toggle("Join this room when the game starts", &s.mpAutoConnect)) dirty = true;
    if (toggle("Show names over the other players", &s.mpShowNames)) dirty = true;
    if (toggle("Share items with the room", &s.mpShareItems)) dirty = true;
    hint("Items, upgrades, equipment, songs, pearls, Triforce pieces and heart containers anyone gets, everyone gets. "
         "Rupees, ammo and what's in bottles stay yours. Only between players of the same game: the original game, or "
         "the same randomizer seed.");
    if (toggle("Share story progress with the room", &s.mpShareStory)) dirty = true;
    hint("Everyone plays the first player's story: joining takes on their progress (your save file only changes if you "
         "save) and reloads your area. After that, events, opened chests, switches, keys and bosses carry over.");
    if (slider_int("Chat stays on screen", &s.mpChatSeconds, 3, 60, "%d s")) dirty = true;
}

struct RandoOption
{
    const char* key;
    const char* label;
    char type;
    const char* def;
    std::initializer_list<const char*> choices;
    int lo = 0, hi = 0;
    const char* help = nullptr;
};
const RandoOption kRandoLocations[] = {
    {"progression_dungeons", "Dungeons", 'c', "Standard", {"Disabled", "Standard", "Race Mode"}, 0, 0,
     "Race Mode: only the required dungeons hold progress items (see Required dungeons)."},
    {"progression_great_fairies", "Great Fairies", 'b', "true", {}},
    {"progression_puzzle_secret_caves", "Puzzle secret caves", 'b', "true", {}},
    {"progression_combat_secret_caves", "Combat secret caves", 'b', "false", {}},
    {"progression_short_sidequests", "Short sidequests", 'b', "false", {}},
    {"progression_long_sidequests", "Long sidequests", 'b', "false", {}},
    {"progression_spoils_trading", "Spoils trading", 'b', "false", {}},
    {"progression_minigames", "Minigames", 'b', "false", {}},
    {"progression_free_gifts", "Free gifts", 'b', "true", {}},
    {"progression_mail", "Mail", 'b', "false", {}},
    {"progression_platforms_rafts", "Platforms and rafts", 'b', "false", {}},
    {"progression_submarines", "Submarines", 'b', "false", {}},
    {"progression_eye_reef_chests", "Eye reef chests", 'b', "false", {}},
    {"progression_big_octos_gunboats", "Big Octos and gunboats", 'b', "false", {}},
    {"progression_triforce_charts", "Sunken Triforce", 'b', "false", {}},
    {"progression_treasure_charts", "Sunken treasure", 'b', "false", {}},
    {"progression_expensive_purchases", "Expensive purchases", 'b', "true", {}},
    {"progression_misc", "Miscellaneous", 'b', "true", {}},
    {"progression_tingle_chests", "Tingle chests", 'b', "false", {}},
    {"progression_battlesquid", "Battlesquid", 'b', "false", {}},
    {"progression_savage_labyrinth", "Savage Labyrinth", 'b', "false", {}},
    {"progression_island_puzzles", "Island puzzles", 'b', "false", {}},
    {"progression_dungeon_secrets", "Dungeon secrets", 'b', "false", {}},
    {"progression_obscure", "Obscure", 'b', "false", {}},
};
const RandoOption kRandoItems[] = {
    {"dungeon_small_keys", "Small keys", 'c', "Own Dungeon", {"Vanilla", "Own Dungeon", "Any Dungeon", "Overworld", "Keysanity"}},
    {"dungeon_big_keys", "Big keys", 'c', "Own Dungeon", {"Vanilla", "Own Dungeon", "Any Dungeon", "Overworld", "Keysanity"}},
    {"dungeon_maps_compasses", "Maps and compasses", 'c', "Own Dungeon", {"Vanilla", "Own Dungeon", "Any Dungeon", "Overworld", "Keysanity"}},
    {"num_required_dungeons", "Required dungeons", 'i', "0", {}, 0, 6, "Dungeons whose boss must be beaten to enter Ganon's Tower."},
    {"randomize_charts", "Shuffle the sea charts", 'b', "false", {}},
    {"remove_swords", "Swordless", 'b', "false", {}},
    {"chest_type_matches_contents", "Chests show what's inside", 'b', "false", {}},
    {"start_with_random_item", "Start with a random item", 'b', "false", {}},
    {"starting_hcs", "Starting hearts", 'i', "3", {}, 1, 20},
};
const RandoOption kRandoEntrances[] = {
    {"randomize_starting_island", "Random starting island", 'b', "false", {}},
    {"randomize_dungeon_entrances", "Dungeon entrances", 'b', "false", {}},
    {"randomize_boss_entrances", "Boss entrances", 'b', "false", {}},
    {"randomize_miniboss_entrances", "Miniboss entrances", 'b', "false", {}},
    {"randomize_cave_entrances", "Caves", 'c', "Disabled", {"Disabled", "Caves", "Caves and Fairies"}},
    {"randomize_door_entrances", "Doors", 'b', "false", {}},
    {"randomize_misc_entrances", "Other entrances", 'b', "false", {}},
    {"mix_dungeons", "Mix dungeons with other entrances", 'b', "false", {}},
    {"decouple_entrances", "Decoupled (one-way) entrances", 'b', "false", {}},
};
const RandoOption kRandoHints[] = {
    {"path_hints", "Path hints", 'i', "0", {}, 0, 7},
    {"barren_hints", "Barren hints", 'i', "0", {}, 0, 7},
    {"item_hints", "Item hints", 'i', "0", {}, 0, 7},
    {"location_hints", "Location hints", 'i', "0", {}, 0, 7},
    {"korl_hints", "King of Red Lions gives hints", 'b', "false", {}},
    {"ho_ho_hints", "Old Man Ho Ho gives hints", 'b', "false", {}},
    {"clearer_hints", "Clearer hint text", 'b', "false", {}},
};
const RandoOption kRandoComfort[] = {
    {"instant_text_boxes", "Instant text", 'b', "true", {}},
    {"reveal_full_sea_chart", "Reveal the whole sea chart", 'b', "true", {}},
    {"skip_rematch_bosses", "Skip boss rematches", 'b', "true", {}},
    {"add_shortcut_warps_between_dungeons", "Warps between dungeons", 'b', "false", {}},
    {"quiet_swift_sail", "Quiet swift sail", 'b', "false", {}},
    {"damage_multiplier", "Damage taken", 'i', "2", {}, 1, 80, "How many times the normal damage Link takes."},
    {"remove_music", "No music", 'b', "false", {}},
};

std::string rando_value(const settings::Settings& s, const RandoOption& o)
{
    auto it = s.randoOptions.find(o.key);
    return it != s.randoOptions.end() ? it->second : o.def;
}

bool rando_option(settings::Settings& s, const RandoOption& o)
{
    std::string v = rando_value(s, o);
    std::string id = std::string(o.label) + "##" + o.key;
    bool changed = false;
    if (o.type == 'b')
    {
        bool on = v == "true";
        if (toggle(id.c_str(), &on))
        {
            s.randoOptions[o.key] = on ? "true" : "false";
            changed = true;
        }
    }
    else if (o.type == 'i')
    {
        int n = std::atoi(v.c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);  // half the column: room for the label
        if (slider_int(id.c_str(), &n, o.lo, o.hi, "%d"))
        {
            s.randoOptions[o.key] = std::to_string(n);
            changed = true;
        }
    }
    else
    {
        std::vector<const char*> list(o.choices);
        int idx = 0;
        for (int i = 0; i < (int)list.size(); i++)
            if (v == list[(size_t)i]) idx = i;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        if (combo(id.c_str(), idx, o.choices))
        {
            s.randoOptions[o.key] = list[(size_t)idx];
            changed = true;
        }
    }
    if (o.help) hint(o.help);
    return changed;
}

template<size_t N>
bool rando_options_card(settings::Settings& s, const char* title, const RandoOption (&opts)[N], int columns)
{
    Card c;
    begin_card(c, title);
    bool changed = false;
    if (columns > 1 && ImGui::BeginTable(title, columns))
    {
        for (const RandoOption& o : opts)
        {
            ImGui::TableNextColumn();
            changed |= rando_option(s, o);
        }
        ImGui::EndTable();
    }
    else
        for (const RandoOption& o : opts) changed |= rando_option(s, o);
    end_card(c);
    return changed;
}

std::string random_seed_text()
{
    static constexpr const char* kA[] = {"Swift", "Sunken", "Golden", "Cursed", "Windy", "Stormy", "Hidden", "Ancient", "Brave", "Lost"};
    static constexpr const char* kB[] = {"Pirate", "Rito", "Korok", "Moblin", "Seagull", "Octo", "Tingle", "Pig", "Fairy", "Sage"};
    static constexpr const char* kC[] = {"Bay", "Reef", "Tower", "Sail", "Song", "Chart", "Isle", "Crest", "Bomb", "Wind"};
    uint32 h = (uint32)(ImGui::GetTime() * 7919.0) ^ (uint32)SDL_GetTicksNS();
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return std::string(kA[h % 10]) + kB[(h / 10) % 10] + kC[(h / 100) % 10] + std::to_string((h >> 12) % 100);
}

void tab_randomizer(settings::Settings& s, bool& dirty)
{
    rando::Status st = rando::status();
    bool playing = rando::playing();
    {
        Card c;
        begin_card(c, "TWWHD Randomizer");
        ImGui::TextWrapped("Every item in a new place, with logic so the seed can always be finished; the world opens from the start "
                           "and most cutscenes are skipped. By SuperDude88 and contributors (MIT).");
        if (!rando::generator_available())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, col(IM_COL32(240, 140, 90, 255)));
            ImGui::TextWrapped("The randomizer isn't installed next to windfall.exe (randomizer\\wwhd_rando.exe).");
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();
        if (toggle("Play the randomized game", &s.randoEnabled)) dirty = true;
        hint("Uses the last generated seed, with its own save files (your normal save isn't touched). Applies after restart.");
        if (playing) ImGui::TextDisabled("Now playing seed: %s", st.seed.empty() ? "?" : st.seed.c_str());
        else if (!st.seed.empty()) ImGui::TextDisabled("Generated seed: %s", st.seed.c_str());
        ImGui::Spacing();
        float bw = ImGui::CalcTextSize("Random seed").x + ImGui::GetStyle().FramePadding.x * 4;
        if (text_field("##seed", s.randoSeedText, 60, c.inner - bw - ImGui::GetStyle().ItemSpacing.x)) dirty = true;
        ImGui::SameLine();
        if (button("Random seed", ImVec2(bw, 0)))
        {
            s.randoSeedText = random_seed_text();
            dirty = true;
        }
        ImGui::TextDisabled("Seed: the same seed and options give the same game");
        ImGui::BeginDisabled(st.running || !rando::generator_available());
        if (button(st.running ? "Generating..." : "Generate seed", ImVec2(c.inner, ImGui::GetFrameHeight() * 1.5f)))
        {
            if (s.randoSeedText.empty()) s.randoSeedText = random_seed_text();
            rando::generate(s.randoSeedText, s.randoOptions);
            s.randoEnabled = true;
            dirty = true;
        }
        ImGui::EndDisabled();
        if (!st.message.empty())
        {
            ImU32 mc = st.running ? kGold : st.ok ? IM_COL32(110, 220, 130, 255) : IM_COL32(240, 140, 90, 255);
            ImGui::PushStyleColor(ImGuiCol_Text, col(mc));
            ImGui::TextWrapped("%s", st.message.c_str());
            ImGui::PopStyleColor();
        }
        float half = (c.inner - ImGui::GetStyle().ItemSpacing.x) / 2;
        std::filesystem::path log = rando::spoiler_log();
        ImGui::BeginDisabled(log.empty());
        if (button("Open spoiler log", ImVec2(half, 0))) rando::open_spoiler_log();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (button("Restart now", ImVec2(half, 0)))
        {
            {
                std::lock_guard lk(settings::lock());
                settings::get() = s;
            }
            settings::save();
            rando::restart_game();
        }
        end_card(c);
    }
    // co-op: players in the room on another randomizer seed, whose world this player can make too
    if (mp::connected())
    {
        std::vector<mp::PlayerInfo> players = mp::room_info();
        std::vector<const mp::PlayerInfo*> others;
        for (const mp::PlayerInfo& pl : players)
            if (!pl.self && !pl.world.empty() && !pl.sameWorld &&
                std::none_of(others.begin(), others.end(), [&](const mp::PlayerInfo* o) { return o->world == pl.world; }))
                others.push_back(&pl);
        bool together = players.size() > 1 && playing &&
                        std::all_of(players.begin(), players.end(), [](const mp::PlayerInfo& pl) { return pl.sameWorld; });
        if (!others.empty() || together)
        {
            Card c;
            begin_card(c, "Co-op room");
            if (together) ImGui::TextWrapped("Everyone in the room plays this seed, so items are shared.");
            for (const mp::PlayerInfo* pl : others)
            {
                rando::World w = rando::parse_world(pl->world);
                ImGui::PushID(pl->world.c_str());
                ImGui::TextWrapped("%s plays seed %s.", pl->name.c_str(), w.seed.c_str());
                ImGui::BeginDisabled(st.running || !rando::generator_available());
                if (button(std::format("Make {}'s seed", pl->name).c_str()))
                {
                    s.randoSeedText = w.seed;
                    s.randoOptions = w.options;
                    s.randoEnabled = true;
                    rando::generate(w.seed, w.options);
                    dirty = true;
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            if (!others.empty())
                ImGui::TextDisabled("Generates the same world with their seed and options. Restart when it's ready to play it together.");
            end_card(c);
        }
    }
    dirty |= rando_options_card(s, "Where progress items can be", kRandoLocations, 2);
    dirty |= rando_options_card(s, "Items", kRandoItems, 1);
    dirty |= rando_options_card(s, "Entrances", kRandoEntrances, 2);
    dirty |= rando_options_card(s, "Hints", kRandoHints, 1);
    dirty |= rando_options_card(s, "Comfort and difficulty", kRandoComfort, 1);
    ImGui::TextDisabled("Options apply to the next seed you generate.");
}

void tab_enhancements(settings::Settings& s, bool& dirty)
{
    section("Game");
    dirty |= toggle("Pause while in the background", &s.pauseInBackground);
    hint("The game stops while another window has the focus.");
    dirty |= toggle("F12 takes a screenshot", &s.screenshotKey);
    hint("Saved as PNG in your Pictures\\Windfall folder.");
    dirty |= toggle("Discord status", &s.discordPresence);
    hint("Shows where you are and your co-op room on your Discord profile.");
    section("Graphic packs");
    const auto& packs = gp::packs();
    const gp::Pack* res = gp::resolution_pack();
    if (packs.empty() || (packs.size() == 1 && res))
        ImGui::TextWrapped("No Wind Waker HD graphic packs found. They come with Cemu (its community graphic packs); Windfall uses "
                           "the ones in your Cemu folder, or in %%APPDATA%%\\WWHDRecomp\\graphicPacks.");
    for (const gp::Pack& pk : packs)
    {
        if (&pk == res) continue;
        ImGui::PushID(pk.id.c_str());
        std::string group = pk.id;
        if (size_t slash = group.find('/'); slash != std::string::npos) group = group.substr(slash + 1);
        if (size_t slash = group.find('/'); slash != std::string::npos) group = group.substr(0, slash);
        std::string label = group.empty() || group == pk.id ? pk.name : group + ": " + pk.name;
        bool on = settings::pack_enabled(s, pk.id);
        if (!pk.supported()) ImGui::BeginDisabled();
        if (toggle(label.c_str(), &on))
        {
            settings::set_pack_enabled(s, pk.id, on);
            dirty = true;
        }
        if (!pk.supported())
        {
            ImGui::EndDisabled();
            hint("This pack patches the game's code. Windfall runs the game as compiled native code, so code patches can't be applied.");
        }
        else if (!pk.description.empty())
            hint(pk.description.c_str());
        if (on && pk.supported())
        {
            ImGui::Indent();
            for (const std::string& cat : pk.categories)
            {
                std::vector<const gp::Preset*> list;
                for (const gp::Preset& p : pk.presets)
                    if (p.category == cat) list.push_back(&p);
                if (list.size() < 2) continue;
                std::string current = settings::pack_preset(s, pk.id, cat);
                if (current.empty())
                {
                    for (const gp::Preset* p : list)
                        if (p->isDefault && current.empty()) current = p->name;
                    if (current.empty()) current = list[0]->name;
                }
                if (begin_combo(cat.empty() ? "Preset" : cat.c_str(), current.c_str()))
                {
                    for (const gp::Preset* p : list)
                        if (ImGui::Selectable(p->name.c_str(), p->name == current))
                        {
                            settings::set_pack_preset(s, pk.id, cat, p->name);
                            dirty = true;
                        }
                    ImGui::EndCombo();
                }
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }

    section("Mods");
    const auto& mods = mods::list();
    if (mods.empty()) ImGui::TextDisabled("No mods. Put them in %%APPDATA%%\\WWHDRecomp\\mods (see the README).");
    for (const mods::Mod& m : mods)
    {
        bool on = s.mods.count(m.folder) && s.mods[m.folder];
        std::string label = m.name + (m.version.empty() ? "" : " " + m.version) + (m.author.empty() ? "" : "  (" + m.author + ")") + "##mod" + m.folder;
        if (toggle(label.c_str(), &on))
        {
            s.mods[m.folder] = on;
            dirty = true;
        }
        std::string tip = m.description.empty() ? m.folder : m.description;
        tip += std::string("\n") + (m.hasContent ? "Replaces game content (from the next area you enter). " : "") +
               (m.hasPlugin ? "Has a plugin: turning it off pauses it, but changes it already made stay until you restart." : "");
        hint(tip.c_str());
    }

    section("Cheats");
    const auto& cheats = cheats::list();
    if (cheats.empty()) ImGui::TextDisabled("No cheats.");
    for (const cheats::Cheat& c : cheats)
    {
        bool on = s.cheats.count(c.name) && s.cheats[c.name];
        if (toggle(c.name.c_str(), &on))
        {
            s.cheats[c.name] = on;
            dirty = true;
        }
        std::string tip = c.description.empty() ? c.source : c.description + "\n(" + c.source + ")";
        hint(tip.c_str());
    }
    ImGui::TextDisabled("Your own cheats: %%APPDATA%%\\WWHDRecomp\\cheats\\*.txt (format in the README).");

    section("Menu");
    dirty |= toggle("Styled menu", &s.styledMenu);
    hint("Draws this menu with the game's own windows, buttons and colors instead of plain controls.");
    float scale = s.menuScale;
    if (slider_float("Menu size", &scale, 0.75f, 2.0f, "%.2fx"))
    {
        s.menuScale = scale;
        dirty = true;
    }
}

}

bool is_open() { return g_open; }
void open_tab(int tab)
{
    g_tab = std::clamp(tab, 0, kTabCount - 1);
    g_open = true;
}

void toggle()
{
    g_open = !g_open;
    g_capture = Capture::None;
}

void on_sdl_event(const SDL_Event& e)
{
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F1) { toggle(); return; }
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) { toggle(); return; }
    if (!g_open) return;
    switch (e.type)
    {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    {
        std::lock_guard lk(g_eventLock);
        if (g_events.size() < 256) g_events.push_back(e);
        break;
    }
    default: break;
    }
}

void inject_test_input()
{
    struct Press { uint32 frame; std::string name; };
    static std::vector<Press> s_presses = [] {
        std::vector<Press> v;
        if (const char* e = std::getenv("WF_MENU_KEYS"))
        {
            std::string all = e;
            for (size_t p = 0; p < all.size();)
            {
                size_t c = all.find(',', p);
                std::string item = all.substr(p, c == std::string::npos ? std::string::npos : c - p);
                size_t colon = item.find(':');
                if (colon != std::string::npos) v.push_back({(uint32)std::atoi(item.substr(0, colon).c_str()), item.substr(colon + 1)});
                if (c == std::string::npos) break;
                p = c + 1;
            }
        }
        return v;
    }();
    static std::vector<SDL_Event> s_release;
    for (SDL_Event& e : s_release)
    {
        e.type = e.type == SDL_EVENT_KEY_DOWN ? SDL_EVENT_KEY_UP : SDL_EVENT_GAMEPAD_BUTTON_UP;
        if (e.type == SDL_EVENT_KEY_UP) e.key.down = false; else e.gbutton.down = false;
        SDL_PushEvent(&e);
    }
    s_release.clear();
    uint32 frame = LatteGPUState.frameCounter;
    for (const Press& p : s_presses)
    {
        if (p.frame != frame) continue;
        SDL_Event e{};
        if (p.name.starts_with("pad:"))
        {
            e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            e.gbutton.button = (Uint8)SDL_GetGamepadButtonFromString(p.name.substr(4).c_str());
            e.gbutton.down = true;
        }
        else
        {
            e.type = SDL_EVENT_KEY_DOWN;
            e.key.key = SDL_GetKeyFromName(p.name.c_str());
            e.key.scancode = SDL_GetScancodeFromName(p.name.c_str());
            e.key.down = true;
        }
        SDL_PushEvent(&e);
        s_release.push_back(e);
    }
}

void render()
{
    static bool s_wasOpen = false;
    inject_test_input();
    // WF_SETTINGS_AT=<frame>:<file>: the settings in that file take over at that presented frame, as if changed in this
    // menu (tests of settings applied while playing)
    static const char* s_settingsAt = std::getenv("WF_SETTINGS_AT");  // several: <frame>:<file>;<frame>:<file>
    for (const char* p = s_settingsAt; p && *p;)
    {
        const char* end = std::strchr(p, ';');
        std::string item(p, end ? end : p + std::strlen(p));
        if (LatteGPUState.frameCounter == (uint32)std::atoi(item.c_str()))
        {
            settings::load_from(item.substr(item.find(':') + 1));
            settings::apply();
            log_info("settings: test settings {} applied", item);
        }
        p = end ? end + 1 : nullptr;
    }
    static const uint32 s_openAt = std::getenv("WF_MENU_OPEN") ? (uint32)std::atoi(std::getenv("WF_MENU_OPEN")) : 0;
    if (s_openAt && LatteGPUState.frameCounter == s_openAt)
    {
        g_open = true;
        if (const char* t = std::getenv("WF_MENU_TAB")) g_tab = std::clamp(std::atoi(t), 0, kTabCount - 1);
    }
    bool open = g_open;
    if (!open)
    {
        if (s_wasOpen)
        {
            settings::save();
            sfx::play(sfx::kClose);
        }
        s_wasOpen = false;
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.IniFilename = nullptr;
    if (!s_wasOpen)
    {
        sfx::play(sfx::kOpen);
        art::load();
        s_wasOpen = true;
        ImGui::SetWindowFocus("##windfall_settings");
    }
    process_events();

    settings::Settings s;
    {
        std::lock_guard lk(settings::lock());
        s = settings::get();
    }
    float scale = io.DisplaySize.y / 1080.0f * s.menuScale;
    g_styled = s.styledMenu;
    ImFont* body = ImGui_GetFont(std::round(24.0f * scale));
    ImFont* title = ImGui_GetFont(std::round(36.0f * scale));
    if (!body || !title) return;

    apply_style(scale);
    g_uiScale = scale;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(4, 16, 36, 140));

    ImVec2 size(std::min(io.DisplaySize.x - 40 * scale, 1180 * scale), std::min(io.DisplaySize.y - 40 * scale, 860 * scale));
    ImVec2 pos((io.DisplaySize.x - size.x) / 2, (io.DisplaySize.y - size.y) / 2);
    if (g_styled) draw_panel(bg, pos, ImVec2(pos.x + size.x, pos.y + size.y), scale);

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushFont(body);
    bool dirty = false;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse;
    if (g_styled) flags |= ImGuiWindowFlags_NoBackground;
    if (ImGui::Begin("##windfall_settings", nullptr, flags))
    {
        void* link = g_styled ? art::texture("MapLink_00") : nullptr;
        if (link)
        {
            float h = ImGui::GetTextLineHeight() * 1.9f;
            ImGui::Image(link, ImVec2(h * 80.0f / 70.0f, h));
            ImGui::SameLine();
        }
        ImGui::PushFont(title);
        ImGui::TextUnformatted("Settings");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetCursorPosX(size.x - ImGui::GetStyle().WindowPadding.x - ImGui::CalcTextSize("F1 / HOME  close").x);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10 * scale);
        ImGui::TextDisabled("F1 / HOME  close");

        if (!g_styled)
        {
            static int s_shownTab = -1;
            bool force = g_tab != s_shownTab;
            if (ImGui::BeginTabBar("##tabs"))
            {
                for (int t = 0; t < kTabCount; t++)
                    if (ImGui::BeginTabItem(kTabs[t], nullptr, force && t == g_tab ? ImGuiTabItemFlags_SetSelected : 0))
                    {
                        if (!force) g_tab = t;
                        ImGui::EndTabItem();
                    }
                ImGui::EndTabBar();
            }
            s_shownTab = g_tab;
        }
        else
        {
            float gap = ImGui::GetStyle().ItemSpacing.x;
            float w = (size.x - 2 * ImGui::GetStyle().WindowPadding.x - (kTabCount - 1) * gap) / kTabCount;
            float h = ImGui::GetFrameHeight() * 1.5f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (int t = 0; t < kTabCount; t++)
            {
                if (t) ImGui::SameLine(0, gap);
                ImGui::PushID(t);
                ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("tab", ImVec2(w, h)) && g_tab != t)
                {
                    g_tab = t;
                    sfx::play(sfx::kTab);
                }
                bool active = t == g_tab, hovered = ImGui::IsItemHovered();
                button_art(dl, p, ImVec2(p.x + w, p.y + h), active ? kGold : hovered ? kFrameHover : kFrame,
                           active ? kGoldDark : IM_COL32(220, 230, 245, 255));
                ImVec2 ts = ImGui::CalcTextSize(kTabs[t]);
                dl->AddText(ImVec2(p.x + (w - ts.x) / 2, p.y + (h - ts.y) / 2), active ? IM_COL32(40, 26, 6, 255) : kText, kTabs[t]);
                ImGui::PopID();
            }
        }
        ImGui::Spacing();
        bool restart = settings::restart_pending();
        float footer = ImGui::GetFrameHeightWithSpacing() * (restart ? 1.2f : 0.2f);
        if (ImGui::BeginChild("##page", ImVec2(0, -footer), false, ImGuiWindowFlags_NavFlattened))
        {
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            static const char* s_scroll = std::getenv("WF_MENU_SCROLL");  // tests: scroll the page down this far on opening
            if (s_scroll && s_openAt && LatteGPUState.frameCounter == s_openAt + 10) ImGui::SetScrollY((float)std::atof(s_scroll));
            switch (g_tab)
            {
            case 0: tab_graphics(s, dirty); break;
            case 1: tab_video(s, dirty); break;
            case 2: tab_input(s, dirty); break;
            case 3: tab_audio(s, dirty); break;
            case 4: tab_enhancements(s, dirty); break;
            case 5: tab_multiplayer(s, dirty); break;
            case 6: tab_randomizer(s, dirty); break;
            }
            ImGui::PopItemWidth();
        }
        ImGui::EndChild();
        if (restart)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, col(kHeading));
            ImGui::TextUnformatted("Some changes apply after you restart the game.");
            ImGui::PopStyleColor();
        }
        if (g_capture == Capture::None && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
            g_open = false;
    }
    ImGui::End();
    ImGui::PopFont();

    {
        ImGuiContext& g = *GImGui;
        static ImGuiID s_nav = 0, s_hover = 0;
        if (g.NavId != s_nav && g.NavId && !g.NavDisableHighlight && s_nav) sound_throttled(sfx::kMove);
        if (g.HoveredId != s_hover && g.HoveredId && g.HoveredId != g.ActiveId) sound_throttled(sfx::kMove);
        s_nav = g.NavId;
        s_hover = g.HoveredId;
    }

    if (dirty)
    {
        {
            std::lock_guard lk(settings::lock());
            auto bindings = settings::get().bindings;
            settings::get() = s;
            if (g_tab != 2) settings::get().bindings = bindings;
        }
        changed();
    }
    else if (g_capture == Capture::None && g_captureAction == -1)
    {
        static std::array<settings::Binding, settings::kActionCount> s_last{};
        std::array<settings::Binding, settings::kActionCount> now;
        {
            std::lock_guard lk(settings::lock());
            now = settings::get().bindings;
        }
        if (memcmp(&now, &s_last, sizeof(now)) != 0)
        {
            bool first = s_last[0].key == 0 && s_last[0].pad == 0;
            s_last = now;
            if (!first) settings::save();
        }
    }
}

}

namespace wf::ui::menu {
void render_title_button()
{
    constexpr uint32_t kCurrentStage = 0x104741E4;
    char stage[9] = {};
    for (int i = 0; i < 8; i++) stage[i] = (char)rd8(kCurrentStage + i);
    bool menus = !stage[0] || std::strcmp(stage, "Name") == 0 || std::strncmp(stage, "sea_T", 5) == 0;
    if (g_open || !menus || LatteGPUState.frameCounter < 600) return;
    ImGuiIO& io = ImGui::GetIO();
    float scale = io.DisplaySize.y / 1080.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 32 * scale, io.DisplaySize.y - 32 * scale), ImGuiCond_Always, ImVec2(1, 1));
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (ImGui::Begin("##mp_title_button", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
    {
        ImGui::PushFont(ImGui_GetFont(std::round(26.0f * scale)));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(16, 42, 86, 210));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(40, 80, 150, 230));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(210, 170, 60, 240));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10 * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18 * scale, 10 * scale));
        std::string label = mp::connected() ? "Multiplayer: " + mp::room_code() : std::string("Multiplayer  (F1)");
        static bool s_logged = false;
        if (!s_logged) log_info("menu: Multiplayer button shown on {}", stage[0] ? stage : "the title screen");
        s_logged = true;
        if (ImGui::Button(label.c_str())) open_tab(5);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
        ImGui::PopFont();
    }
    ImGui::End();
}
}

namespace wf::ui::menu {
namespace {
std::mutex g_toastLock;
std::string g_toast;
double g_toastUntil = 0;
}

void toast(const std::string& text)
{
    std::lock_guard lk(g_toastLock);
    g_toast = text;
    g_toastUntil = -1;
}

void render_toast()
{
    std::string text;
    double now = ImGui::GetTime(), left;
    {
        std::lock_guard lk(g_toastLock);
        if (g_toast.empty()) return;
        if (g_toastUntil < 0) g_toastUntil = now + 3.0;
        left = g_toastUntil - now;
        if (left <= 0)
        {
            g_toast.clear();
            return;
        }
        text = g_toast;
    }
    ImGuiIO& io = ImGui::GetIO();
    float s = io.DisplaySize.y / 1080.0f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    float fs = 28 * s;
    ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, text.c_str());
    int a = (int)(std::clamp(left / 0.4, 0.0, 1.0) * 255);
    ImVec2 p((io.DisplaySize.x - ts.x) * 0.5f, 40 * s);
    dl->AddRectFilled(ImVec2(p.x - 22 * s, p.y - 12 * s), ImVec2(p.x + ts.x + 22 * s, p.y + ts.y + 12 * s), IM_COL32(16, 42, 86, a * 230 / 255), 14 * s);
    dl->AddRect(ImVec2(p.x - 22 * s, p.y - 12 * s), ImVec2(p.x + ts.x + 22 * s, p.y + ts.y + 12 * s), IM_COL32(244, 200, 84, a), 14 * s, 0, 2.5f * s);
    dl->AddText(font, fs, p, IM_COL32(246, 242, 228, a), text.c_str());
}
}

void wf_menu_render()
{
    wf::ui::menu::render_toast();
    wf::ui::menu::render_title_button();
    wf::ui::menu::render();
}
