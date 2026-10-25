#pragma once
#include <cstdint>
#include <string>

#include "wf/ppc.h"

namespace wf::hle {

struct VarArgs
{
    PPCContext* ctx = nullptr;
    int gpr = 4, fpr = 1;
    uint32_t stack = 0;
    static VarArgs from_call(PPCContext& c, int firstGpr) { return {&c, firstGpr, 1, c.r[1] + 8}; }
};

struct VaList { uint32_t addr; };

std::string guest_format(uint32_t fmt, VarArgs args);
std::string guest_vformat(uint32_t fmt, VaList va);

}
