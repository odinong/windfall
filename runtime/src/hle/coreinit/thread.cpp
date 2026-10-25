#include "core/log.h"
#include "hle/hle.h"
#include "os/sched.h"

namespace wf::hle {

namespace {

constexpr uint32_t T_SPECIFIC = 0x57C, T_NAME = 0x5C0, T_CTX_ERRNO = 0x300;
constexpr int kAppPriorityBase = 0x40;

bool OSCreateThread(PPCContext& ctx, uint32_t thread, uint32_t entry, uint32_t argc, uint32_t argv,
                    uint32_t stackTop, uint32_t stackSize, int32_t priority, uint32_t attr)
{
    if (priority < 0 || priority >= 32 || attr >= 0x20 || !stackTop || !stackSize)
    {
        log_warn("OSCreateThread: invalid arguments (prio {}, attr {:X})", priority, attr);
        return false;
    }
    os::LockGuard g;
    os::Thread* t = os::create_thread(thread, entry, argc, argv, stackTop, stackSize, priority, attr);
    (void)ctx;
    log_cat("thread", "OSCreateThread {:08X} entry {:08X} prio {} attr {:X} stack {:08X}-{:08X}", thread, entry, priority, attr,
            stackTop - stackSize, stackTop);
    (void)t;
    return true;
}
WF_EXPORT(coreinit, OSCreateThread);

int32_t OSResumeThread(uint32_t thread)
{
    os::LockGuard g;
    os::Thread* t = os::find_thread(thread);
    if (!t) return 0;
    return os::resume(*t);
}
WF_EXPORT(coreinit, OSResumeThread);

void OSExitThread(PPCContext& ctx, uint32_t value)
{
    os::exit_current(os::current(ctx), value);
}
WF_EXPORT(coreinit, OSExitThread);

bool OSJoinThread(PPCContext& ctx, uint32_t thread, uint32_t exitValueOut)
{
    os::LockGuard g;
    os::Thread* t = os::find_thread(thread);
    if (!t || t->detached) return false;
    while (t->state != os::ThreadState::Moribund)
        os::block(os::current(ctx), &t->joinQueue);
    if (exitValueOut) wr32(exitValueOut, t->exitValue);
    return true;
}
WF_EXPORT(coreinit, OSJoinThread);

uint32_t OSGetCurrentThread(PPCContext& ctx) { return os::current(ctx).guest; }
WF_EXPORT(coreinit, OSGetCurrentThread);

uint32_t OSGetCoreId(PPCContext& ctx) { return ctx.core; }
WF_EXPORT(coreinit, OSGetCoreId);

void OSYieldThread(PPCContext& ctx)
{
    os::LockGuard g;
    os::yield_current(os::current(ctx));
}
WF_EXPORT(coreinit, OSYieldThread);

void OSSleepTicks(PPCContext& ctx, uint64_t ticks)
{
    os::LockGuard g;
    os::WaitQueue none;
    os::block(os::current(ctx), &none, os::now_ns() + os::ticks_to_ns(ticks));
}
WF_EXPORT(coreinit, OSSleepTicks);

void OSSetThreadName(uint32_t thread, uint32_t name)
{
    wr32(thread + T_NAME, name);
    os::LockGuard g;
    if (os::Thread* t = os::find_thread(thread))
    {
        t->name = read_cstr(name);
    }
    log_cat("thread", "OSSetThreadName {:08X} \"{}\"", thread, read_cstr(name));
}
WF_EXPORT(coreinit, OSSetThreadName);

bool OSSetThreadAffinity(PPCContext& ctx, uint32_t thread, uint32_t affinity)
{
    os::LockGuard g;
    os::Thread* t = os::find_thread(thread);
    if (!t || !(affinity & 7)) return false;
    t->affinity = affinity & 7;
    if (t->state == os::ThreadState::Running && !(t->affinity & (1 << t->core)))
        t->ctx.yield_request.store(1);
    (void)ctx;
    return true;
}
WF_EXPORT(coreinit, OSSetThreadAffinity);

int32_t OSGetThreadPriority(uint32_t thread)
{
    os::LockGuard g;
    os::Thread* t = os::find_thread(thread);
    return t ? t->basePriority - kAppPriorityBase : 16;
}
WF_EXPORT(coreinit, OSGetThreadPriority);

uint32_t OSGetThreadSpecific(PPCContext& ctx, uint32_t index)
{
    return index < 16 ? rd32(os::current(ctx).guest + T_SPECIFIC + 4 * index) : 0;
}
WF_EXPORT(coreinit, OSGetThreadSpecific);

void OSSetThreadSpecific(PPCContext& ctx, uint32_t index, uint32_t value)
{
    if (index < 16) wr32(os::current(ctx).guest + T_SPECIFIC + 4 * index, value);
}
WF_EXPORT(coreinit, OSSetThreadSpecific);

uint32_t OSDisableInterrupts(PPCContext& ctx) { return os::set_interrupts(ctx, false) ? 1 : 0; }
WF_EXPORT(coreinit, OSDisableInterrupts);

uint32_t OSRestoreInterrupts(PPCContext& ctx, uint32_t state)
{
    bool prev = os::set_interrupts(ctx, state != 0);
    if (state && ctx.yield_request.load(std::memory_order_relaxed))
        wf::yield(ctx, g_base);
    return prev ? 1 : 0;
}
WF_EXPORT(coreinit, OSRestoreInterrupts);

void OSInitThreadQueue(uint32_t queue) { memset(gptr(queue), 0, 0x10); }
WF_EXPORT(coreinit, OSInitThreadQueue);

void OSBlockThreadsOnExit() {}
WF_EXPORT(coreinit, OSBlockThreadsOnExit);

void __gh_set_errno(PPCContext& ctx, int32_t err) { wr32(os::current(ctx).guest + T_CTX_ERRNO, (uint32_t)err); }
WF_EXPORT(coreinit, __gh_set_errno);

}
}
