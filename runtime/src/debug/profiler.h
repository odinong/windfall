#pragma once

#include <cstdint>

namespace wf::debug {

void profiler_start();
uint32_t guest_function_at(uintptr_t hostAddress);

}
