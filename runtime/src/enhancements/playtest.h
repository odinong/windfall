#pragma once

#include "input/input.h"

namespace wf::playtest {

bool enabled();
void set_enabled();
bool driving();

void apply(input::ProState& s, bool& fastForward);

void tick();
// the presented frame when the playtest reached its spawn point (0 until then)
uint32_t arrival_frame();

}
