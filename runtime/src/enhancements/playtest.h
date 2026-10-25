#pragma once

#include "input/input.h"

namespace wf::playtest {

bool enabled();
void set_enabled();
bool driving();

void apply(input::ProState& s, bool& fastForward);

void tick();

}
