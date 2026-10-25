#include <mutex>
#include <unordered_map>
#include <atomic>
#include <cstdlib>
#include <vector>

#include "core/game_module.h"
#include "core/log.h"
#include "debug/watchdog.h"
#include "os/sched.h"
#include "wf/recomp_api.h"

namespace wf {

PPCFunc** g_funcTable = nullptr;

static std::vector<PPCFunc*> s_imports;
static std::vector<std::atomic<uint32_t>> s_missingCalls;

void init_function_table()
{
    static std::vector<PPCFunc*> table((kCodeEnd - kCodeBase) / 4, nullptr);
    for (const FuncMapping* m = g_funcMappings; m->host; m++)
        table[(m->guest - kCodeBase) / 4] = m->host;
    g_funcTable = table.data();
    game_module::set_func_table(g_funcTable);
    s_imports.assign(g_importCount, nullptr);
    s_missingCalls = std::vector<std::atomic<uint32_t>>(g_importCount);
}

void register_import(uint32_t id, PPCFunc* fn)
{
    s_imports[id] = fn;
}

void call_import(PPCContext& ctx, uint8_t* base, uint32_t id)
{
    const ImportInfo& imp = g_imports[id];
    if (debug::g_traceImports)
        log_cat("trace", "{}::{}({:08X}, {:08X}, {:08X}) lr={:08X}", imp.module, imp.name, ctx.r[3], ctx.r[4], ctx.r[5], ctx.lr);
    if (PPCFunc* fn = s_imports[id]) [[likely]]
    {
        fn(ctx, base);
        return;
    }
    if (s_missingCalls[id]++ < 3)
        log_cat("hle", "unimplemented {}::{} (lr={:08X}) returns 0", imp.module, imp.name, ctx.lr);
    ctx.r[3] = 0;
}

void probe(PPCContext& ctx, uint8_t*, uint32_t addr, const char* name)
{
    static const bool all = std::getenv("WF_PROBE_ALL") != nullptr;
    static std::mutex lock;
    static std::unordered_map<uint32_t, uint32_t> hits;
    {
        std::lock_guard lk(lock);
        if (!all && ++hits[addr] > 3) return;
    }
    os::Thread& t = os::current(ctx);
    log_cat("probe", "{} @{:08X} [{}] r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X} r8={:08X} r29={:08X} r30={:08X} r31={:08X} f1={}\n{}",
            name, addr, t.name, ctx.r[3], ctx.r[4], ctx.r[5], ctx.r[6], ctx.r[7], ctx.r[8], ctx.r[29], ctx.r[30], ctx.r[31],
            ctx.f[1].ps0, debug::backtrace(ctx));
}

void bad_indirect_call(PPCContext& ctx, uint32_t target, uint32_t from)
{
    fatal("indirect call to {:08X} from {:08X} has no recompiled function\n{}", target, from, debug::backtrace(ctx));
}

void trap(PPCContext& ctx, uint8_t*, uint32_t addr)
{
    fatal("guest trap instruction at {:08X}\n{}", addr, debug::backtrace(ctx));
}

}
