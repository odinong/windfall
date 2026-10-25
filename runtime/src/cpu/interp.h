#pragma once
#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

#include "wf/ppc.h"

namespace wf::interp {

void setup(const std::vector<uint32_t>& patchedEntries, const std::vector<std::pair<uint32_t, uint32_t>>& ranges,
           const std::vector<std::pair<uint32_t, uint32_t>>& rel24);
bool interpreted(uint32_t addr);
bool entry(PPCContext& ctx, uint8_t* base, uint32_t addr);
void call(PPCContext& ctx, uint8_t* base, uint32_t addr);

}
