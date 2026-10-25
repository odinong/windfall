#pragma once
#include <functional>

#include "wf/ppc.h"

namespace wf::cemu {

void register_modules();

void with_context(PPCContext& ctx, const std::function<void()>& fn);

}
