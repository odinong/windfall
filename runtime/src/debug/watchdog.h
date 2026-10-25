#pragma once
#include <string>

#include "wf/ppc.h"

namespace wf::debug {

void watchdog_start();
std::string dump_threads();
std::string backtrace(const PPCContext& ctx);
void watch_bulk_write(const PPCContext& ctx, uint32_t dst, uint32_t size, const char* what);
extern bool g_traceImports;

}
