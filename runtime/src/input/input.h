#pragma once
#include <cstdint>
#include <string>

union SDL_Event;

namespace wf::input {

enum ProButton : uint32_t
{
    kUp = 0x1, kLeft = 0x2, kZR = 0x4, kX = 0x8, kA = 0x10, kY = 0x20, kB = 0x40, kZL = 0x80,
    kR = 0x200, kPlus = 0x400, kHome = 0x800, kMinus = 0x1000, kL = 0x2000, kDown = 0x4000,
    kRight = 0x8000, kStickR = 0x10000, kStickL = 0x20000,
};

struct ProState
{
    uint32_t buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;
    bool connected = true;
};

void init();
void handle_event(const SDL_Event& e);
void update();

ProState read();
float action_value(int action);
std::string controller_name();
bool fast_forward();
void rumble(bool on);
void set_text_capture(bool on);

}
