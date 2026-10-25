#pragma once

#include <cstdint>

union SDL_Event;

namespace wf::explorer {

bool is_open();
void toggle();

void on_sdl_event(const SDL_Event& e);

void render();

}

bool wf_explorer_draw(const uint32_t* regs, uint32_t count, uint32_t instances, int copies);
