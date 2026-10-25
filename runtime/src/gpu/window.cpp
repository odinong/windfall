#include "Common/precompiled.h"
#include "WindowSystem.h"
#include "config/CemuConfig.h"

#include "gpu/window.h"
#include "gpu/gpu.h"

#include <SDL3/SDL.h>

#include "core/log.h"
#include "hle/swkbd.h"
#include "input/input.h"
#include "input/InputManager.h"
#include "debug/explorer.h"
#include "net/multiplayer.h"
#include "ui/menu.h"
#include "ui/settings.h"

namespace wf::window {

std::atomic<bool> g_focused{true};
bool focused() { return g_focused; }

int refresh_rate()
{
    SDL_DisplayID id = state().sdl ? SDL_GetDisplayForWindow(state().sdl) : SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* mode = id ? SDL_GetCurrentDisplayMode(id) : nullptr;
    if (!mode || mode->refresh_rate < 30.0f) return 60;
    return (int)std::lround(mode->refresh_rate);
}

State& state()
{
    static State s;
    return s;
}

namespace {

std::atomic<int> g_fullscreenRequest{-1};
Uint32 g_wakeEvent = 0;

void set_fullscreen(bool on)
{
    State& s = state();
    if (((SDL_GetWindowFlags(s.sdl) & SDL_WINDOW_FULLSCREEN) != 0) != on) SDL_SetWindowFullscreen(s.sdl, on);
}

void publish_mouse(float x, float y)
{
    float density = SDL_GetWindowPixelDensity(state().sdl);
    auto& im = InputManager::instance();
    im.m_mouseX = (sint32)(x * density);
    im.m_mouseY = (sint32)(y * density);
}

void publish_size()
{
    State& s = state();
    int w = 0, h = 0, lw = 0, lh = 0;
    SDL_GetWindowSizeInPixels(s.sdl, &w, &h);
    SDL_GetWindowSize(s.sdl, &lw, &lh);
    s.width = (uint32_t)w;
    s.height = (uint32_t)h;
    auto& info = WindowSystem::GetWindowInfo();
    info.width = lw;
    info.height = lh;
    info.phys_width = w;
    info.phys_height = h;
    info.dpi_scale = lw ? (double)w / lw : 1.0;
    info.is_fullscreen = (SDL_GetWindowFlags(s.sdl) & SDL_WINDOW_FULLSCREEN) != 0;
}

}

void create(const char* title)
{
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        fatal("SDL_Init failed: {}", SDL_GetError());
    State& s = state();
    int width = 1280, height = 720;
    if (const char* e = std::getenv("WF_WINDOW_SIZE")) std::sscanf(e, "%dx%d", &width, &height);  // tests and captures
    s.sdl = SDL_CreateWindow(title, width, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!s.sdl)
        fatal("SDL_CreateWindow failed: {}", SDL_GetError());
    void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(s.sdl), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    auto& info = WindowSystem::GetWindowInfo();
    info.window_main = {WindowSystem::WindowHandleInfo::Backend::Windows, nullptr, hwnd};
    info.canvas_main = info.window_main;
    publish_size();
    SDL_StartTextInput(s.sdl);
    g_wakeEvent = SDL_RegisterEvents(1);
    input::init();
}

void request_fullscreen(bool on)
{
    g_fullscreenRequest = on ? 1 : 0;
    if (g_wakeEvent && state().sdl)
    {
        SDL_Event e{};
        e.type = g_wakeEvent;
        SDL_PushEvent(&e);
    }
}

void run_loop()
{
    State& s = state();
    while (!s.quit)
    {
        if (int fs = g_fullscreenRequest.exchange(-1); fs >= 0) set_fullscreen(fs == 1);
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 4))
        {
            input::update();
            continue;
        }
        do
        {
            input::handle_event(e);
            ui::menu::on_sdl_event(e);
            explorer::on_sdl_event(e);
            bool chatting = mp::chat_open();
            if (!ui::menu::is_open()) mp::on_chat_event(&e);
            chatting = chatting || mp::chat_open();
            switch (e.type)
            {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                s.quit = true;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                publish_size();
                s.resized = true;
                break;
            case SDL_EVENT_WINDOW_MINIMIZED: s.minimized = true; break;
            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_MAXIMIZED: s.minimized = false; break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                WindowSystem::GetWindowInfo().app_active = e.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
                g_focused = e.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
                settings::apply_audio();
                break;
            case SDL_EVENT_MOUSE_MOTION: publish_mouse(e.motion.x, e.motion.y); break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                publish_mouse(e.button.x, e.button.y);
                if (e.button.button == SDL_BUTTON_LEFT) InputManager::instance().m_mouseLeftDown = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (!e.key.repeat && (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))))
                {
                    bool on = !(SDL_GetWindowFlags(s.sdl) & SDL_WINDOW_FULLSCREEN);
                    SDL_SetWindowFullscreen(s.sdl, on);
                    {
                        std::lock_guard lk(settings::lock());
                        settings::get().fullscreen = on;
                    }
                    settings::save();
                }
                if (e.key.key == SDLK_F12 && !e.key.repeat)
                {
                    bool on;
                    {
                        std::lock_guard lk(settings::lock());
                        on = settings::get().screenshotKey;
                    }
                    if (on) gpu::save_screenshot();
                }
                if (e.key.key == SDLK_F9 && !e.key.repeat) gpu::capture_frames();
                static const bool testKeys = std::getenv("WF_TESTKEYS") != nullptr;
                if (testKeys && e.key.key == SDLK_F10 && !e.key.repeat) gpu::toggle_frame_hold();
                if (e.key.key == SDLK_F2 && !e.key.repeat)
                {
                    bool on = !GetConfig().gamepad_overlay;
                    GetConfig().gamepad_overlay = on;
                    {
                        std::lock_guard lk(settings::lock());
                        settings::get().gamepadOverlay = on;
                    }
                    settings::save();
                }
                if (ui::menu::is_open() || chatting) break;
                WindowSystem::GetWindowInfo().set_keystate(e.key.key, true);
                if (e.key.key == SDLK_BACKSPACE) hle::swkbd_backspace();
                if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) hle::swkbd_enter();
                break;
            case SDL_EVENT_TEXT_INPUT:
                if (!ui::menu::is_open() && !chatting) hle::swkbd_text(e.text.text);
                break;
            case SDL_EVENT_KEY_UP:
                WindowSystem::GetWindowInfo().set_keystate(e.key.key, false);
                break;
            default: break;
            }
        } while (SDL_PollEvent(&e));
        input::update();
    }
}

}

namespace WindowSystem {

WindowInfo& GetWindowInfo()
{
    static WindowInfo info;
    return info;
}
void UpdateWindowTitles(bool, bool, double) {}
void GetWindowSize(int& w, int& h) { w = GetWindowInfo().width; h = GetWindowInfo().height; }
void GetPadWindowSize(int& w, int& h) { w = h = 0; }
void GetWindowPhysSize(int& w, int& h) { w = GetWindowInfo().phys_width; h = GetWindowInfo().phys_height; }
void GetPadWindowPhysSize(int& w, int& h) { w = h = 0; }
double GetWindowDPIScale() { return GetWindowInfo().dpi_scale; }
double GetPadDPIScale() { return 1.0; }
bool IsPadWindowOpen() { return false; }
bool IsKeyDown(uint32 key) { return GetWindowInfo().get_keystate(key); }
bool IsKeyDown(PlatformKeyCodes key)
{
    switch (key)
    {
    case PlatformKeyCodes::LCONTROL: return IsKeyDown(SDLK_LCTRL);
    case PlatformKeyCodes::RCONTROL: return IsKeyDown(SDLK_RCTRL);
    case PlatformKeyCodes::TAB: return IsKeyDown(SDLK_TAB);
    case PlatformKeyCodes::ESCAPE: return IsKeyDown(SDLK_ESCAPE);
    }
    return false;
}
bool IsFullScreen() { return GetWindowInfo().is_fullscreen; }

}
