#pragma once
#include <atomic>
#include <cstdint>

struct SDL_Window;

namespace wf::window {

struct State
{
    SDL_Window* sdl = nullptr;
    std::atomic<uint32_t> width{1280}, height{720};
    std::atomic<bool> resized{false};
    std::atomic<bool> minimized{false};
    std::atomic<bool> quit{false};
};

State& state();

void create(const char* title);
void run_loop();
void request_fullscreen(bool on);
bool focused();
int refresh_rate();

}
