#include "os/sched.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <thread>
#include <unordered_map>

#include <windows.h>
#include <timeapi.h>

#include "core/guest.h"
#include "core/log.h"

namespace wf {
void yield(PPCContext& ctx, uint8_t* base);
}

namespace wf::os {

namespace {

struct Core
{
    int id = 0;
    Thread* running = nullptr;
    void* idleFiber = nullptr;
    HANDLE wake = nullptr;
    std::atomic<bool> hint{false}, sleeping{false};
    bool interruptsDisabled = false;
    std::thread host;
};

std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
Core g_cores[kNumCores];
thread_local Core* t_core = nullptr;
std::vector<Thread*> g_ready;
std::vector<Thread*> g_threads;
std::unordered_map<uint32_t, Thread*> g_byGuest;
std::vector<Thread*> g_timed;
std::vector<void*> g_deadFibers;
std::vector<std::function<void(uint64_t)>> g_timerHooks;
uint64_t g_seq = 0;
uint32_t g_sda = 0, g_sda2 = 0;
uint16_t g_nextId = 1;
uint64_t g_bootNs = 0;
uint64_t g_ticks2000AtBoot = 0;

constexpr size_t kFiberStackCommit = 256 * 1024;
constexpr size_t kFiberStackReserve = 16 * 1024 * 1024;

constexpr uint32_t kThreadMagic = 0x74487244;
enum : uint32_t {
    T_CTX_GPR = 0x008, T_CTX_UPIR = 0x1DC, T_CTX_ERRNO = 0x300, T_CTX_AFFINITY = 0x304,
    T_MAGIC = 0x320, T_STATE = 0x324, T_ATTR = 0x325, T_ID = 0x326, T_SUSPEND = 0x328,
    T_PRIO_EFF = 0x32C, T_PRIO_BASE = 0x330, T_EXITVAL = 0x334, T_STACKBASE = 0x394,
    T_STACKEND = 0x398, T_ENTRY = 0x39C, T_TYPE = 0x5BC, T_NAME = 0x5C0,
};
constexpr int kAppPriorityBase = 0x40;

void sync_guest(const Thread& t)
{
    if (!t.guest) return;
    wr8(t.guest + T_STATE, (uint8_t)t.state);
    wr32(t.guest + T_SUSPEND, (uint32_t)t.suspendCount);
    wr32(t.guest + T_PRIO_EFF, (uint32_t)t.priority);
    wr32(t.guest + T_PRIO_BASE, (uint32_t)t.basePriority);
    wr32(t.guest + T_CTX_AFFINITY, t.affinity);
}

void notify(Thread& t)
{
    for (int c = 0; c < kNumCores; c++)
    {
        Core& core = g_cores[c];
        if (!(t.affinity & (1 << c)) || core.running) continue;
        core.hint.store(true, std::memory_order_seq_cst);
        if (core.sleeping.load(std::memory_order_seq_cst)) SetEvent(core.wake);
        return;
    }
    for (int c = 0; c < kNumCores; c++)
    {
        Core& core = g_cores[c];
        if ((t.affinity & (1 << c)) && core.running && core.running->priority > t.priority)
            core.running->ctx.yield_request.store(1, std::memory_order_relaxed);
    }
}

Thread* pick(int core)
{
    Thread* best = nullptr;
    size_t bestIdx = 0;
    for (size_t i = 0; i < g_ready.size(); i++)
    {
        Thread* t = g_ready[i];
        if (!(t->affinity & (1 << core))) continue;
        if (!best || t->priority < best->priority || (t->priority == best->priority && t->readySeq < best->readySeq))
        {
            best = t;
            bestIdx = i;
        }
    }
    if (best)
        g_ready.erase(g_ready.begin() + bestIdx);
    return best;
}

bool better_ready(int core, int priority)
{
    for (Thread* t : g_ready)
        if ((t->affinity & (1 << core)) && t->priority <= priority)
            return true;
    return false;
}

void run_on(Core& core, Thread* t)
{
    t->state = ThreadState::Running;
    t->core = core.id;
    t->ctx.core = (uint32_t)core.id;
    t->ctx.yield_request.store(0, std::memory_order_relaxed);
    core.running = t;
    if (t->guest) wr32(t->guest + T_CTX_UPIR, (uint32_t)core.id);
    sync_guest(*t);
}

void switch_away(Thread& self)
{
    Core& core = *t_core;
    Thread* next = pick(core.id);
    if (next == &self)
    {
        run_on(core, &self);
        return;
    }
    sync_guest(self);
    if (next)
    {
        run_on(core, next);
        SwitchToFiber(next->fiber);
    }
    else
    {
        core.running = nullptr;
        SwitchToFiber(core.idleFiber);
    }
}

void remove_from(std::vector<Thread*>& v, Thread* t)
{
    auto it = std::find(v.begin(), v.end(), t);
    if (it != v.end()) v.erase(it);
}

void wake_thread(Thread& t, WakeReason why)
{
    if (t.waitingOn) remove_from(t.waitingOn->waiters, &t);
    if (t.deadline) remove_from(g_timed, &t);
    t.waitingOn = nullptr;
    t.deadline = 0;
    t.wake = why;
    make_ready(t);
}

void WINAPI thread_fiber_entry(void* p)
{
    Thread& t = *(Thread*)p;
    unlock();
    uint32_t value = 0;
    if (t.hostEntry)
        t.hostEntry(t);
    else
    {
        t.ctx.r[3] = t.argc;
        t.ctx.r[4] = t.argv;
        t.ctx.lr = 0;
        call_indirect(t.ctx, g_base, t.entry, 0);
        value = t.ctx.r[3];
    }
    exit_current(t, value);
}

void WINAPI idle_fiber_entry(void* p)
{
    Core& core = *(Core*)p;
    for (;;)
    {
        lock();
        for (void* f : g_deadFibers)
            DeleteFiber(f);
        g_deadFibers.clear();
        if (Thread* t = pick(core.id))
        {
            run_on(core, t);
            SwitchToFiber(t->fiber);
            unlock();
            continue;
        }
        unlock();
        auto spinUntil = std::chrono::steady_clock::now() + std::chrono::microseconds(100);
        bool work = false;
        while (!(work = core.hint.exchange(false, std::memory_order_acq_rel)) && std::chrono::steady_clock::now() < spinUntil)
            for (int i = 0; i < 32; i++) _mm_pause();
        if (work) continue;
        core.sleeping.store(true, std::memory_order_seq_cst);
        if (!core.hint.exchange(false, std::memory_order_seq_cst)) WaitForSingleObject(core.wake, 2);
        core.sleeping.store(false, std::memory_order_seq_cst);
    }
}

void core_thread(int id)
{
    Core& core = g_cores[id];
    t_core = &core;
    SetThreadDescription(GetCurrentThread(), std::format(L"Espresso core {}", id).c_str());
    ConvertThreadToFiber(nullptr);
    core.idleFiber = CreateFiberEx(64 * 1024, 1024 * 1024, 0, idle_fiber_entry, &core);
    SwitchToFiber(core.idleFiber);
}

void timer_thread()
{
    SetThreadDescription(GetCurrentThread(), L"Windfall timer");
    timeBeginPeriod(1);
    for (;;)
    {
        Sleep(1);
        uint64_t now = now_ns();
        lock();
        for (size_t i = 0; i < g_timed.size();)
        {
            Thread* t = g_timed[i];
            if (t->deadline && t->deadline <= now)
                wake_thread(*t, WakeReason::Timeout);
            else
                i++;
        }
        for (auto& h : g_timerHooks)
            h(now);
        for (Core& c : g_cores)
            if (c.running && !c.interruptsDisabled && better_ready(c.id, c.running->priority))
                c.running->ctx.yield_request.store(1, std::memory_order_relaxed);
        unlock();
    }
}

}

void lock()
{
    for (int spins = 0; g_lock.test_and_set(std::memory_order_acquire); spins++)
    {
        if (spins < 64) _mm_pause();
        else SwitchToThread();
    }
}

void unlock() { g_lock.clear(std::memory_order_release); }

uint64_t now_ns()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t ticks_since_boot() { return ns_to_ticks(now_ns() - g_bootNs); }
uint64_t ticks_since_2000() { return g_ticks2000AtBoot + ticks_since_boot(); }

void init(uint32_t sdaBase, uint32_t sda2Base)
{
    g_sda = sdaBase;
    g_sda2 = sda2Base;
    g_bootNs = now_ns();
    std::time_t t = std::time(nullptr);
    std::tm lt{};
    localtime_s(&lt, &t);
    int64_t localUnix = (int64_t)t + (int64_t)(_mkgmtime(&lt) - t);
    g_ticks2000AtBoot = (uint64_t)(localUnix - 946684800) * kTimerClock;
    for (int i = 0; i < kNumCores; i++)
    {
        g_cores[i].id = i;
        g_cores[i].wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
}

void start_cores()
{
    for (int i = 0; i < kNumCores; i++)
        g_cores[i].host = std::thread(core_thread, i);
    std::thread(timer_thread).detach();
}

static Thread* new_thread(uint32_t guest, int priority, uint8_t affinity, uint32_t stackTop, uint32_t stackSize)
{
    Thread* t = new Thread();
    t->guest = guest;
    t->basePriority = t->priority = priority;
    t->affinity = affinity;
    t->stackTop = stackTop;
    t->stackEnd = stackTop - stackSize;
    t->ctx.r[1] = (stackTop - 8) & ~0xFu;
    wr32(t->ctx.r[1], 0);
    t->ctx.r[2] = g_sda2;
    t->ctx.r[13] = g_sda;
    t->ctx.gqr[2] = 0x00040004;
    t->ctx.gqr[3] = 0x00050005;
    t->ctx.gqr[4] = 0x00060006;
    t->ctx.gqr[5] = 0x00070007;
    t->ctx.thread = t;
    t->fiber = CreateFiberEx(kFiberStackCommit, kFiberStackReserve, FIBER_FLAG_FLOAT_SWITCH, thread_fiber_entry, t);
    if (!t->fiber)
        fatal("CreateFiberEx failed ({})", GetLastError());
    if (guest)
    {
        wr32(guest + T_MAGIC, kThreadMagic);
        wr16(guest + T_ID, g_nextId++);
        wr32(guest + T_STACKBASE, stackTop);
        wr32(guest + T_STACKEND, stackTop - stackSize);
        wr32(guest + T_TYPE, 2);
        g_byGuest[guest] = t;
    }
    g_threads.push_back(t);
    return t;
}

Thread* create_thread(uint32_t guest, uint32_t entry, uint32_t argc, uint32_t argv, uint32_t stackTop,
                      uint32_t stackSize, int priority, uint32_t attr)
{
    if (auto it = g_byGuest.find(guest); it != g_byGuest.end())
    {
        Thread* old = it->second;
        if (old->state != ThreadState::Moribund && old->state != ThreadState::None)
            log_warn("OSCreateThread on live thread {:08X}", guest);
        remove_from(g_threads, old);
        g_byGuest.erase(it);
    }
    uint8_t affinity = attr & 7;
    if (!affinity)
        affinity = t_core ? (uint8_t)(1 << t_core->id) : 2;
    memset(gptr(guest), 0, 0x6A0);
    Thread* t = new_thread(guest, priority + kAppPriorityBase, affinity, stackTop, stackSize);
    t->entry = entry;
    t->argc = argc;
    t->argv = argv;
    t->detached = (attr & 8) != 0;
    t->state = ThreadState::None;
    t->suspendCount = 1;
    wr8(guest + T_ATTR, (uint8_t)attr);
    wr32(guest + T_ENTRY, entry);
    sync_guest(*t);
    return t;
}

Thread* create_host_thread(const char* name, int priority, uint8_t affinity, uint32_t stackSize,
                           std::function<void(Thread&)> body)
{
    uint32_t guest = sys_alloc(0x6A0, 8);
    uint32_t stack = sys_alloc(stackSize, 16);
    LockGuard g;
    Thread* t = new_thread(guest, priority, affinity, stack + stackSize, stackSize);
    t->hostEntry = std::move(body);
    t->name = name;
    t->suspendCount = 0;
    make_ready(*t);
    return t;
}

Thread* find_thread(uint32_t guest)
{
    auto it = g_byGuest.find(guest);
    return it == g_byGuest.end() ? nullptr : it->second;
}

std::vector<Thread*> all_threads() { return g_threads; }

Thread& current(PPCContext& ctx) { return *(Thread*)ctx.thread; }

void make_ready(Thread& t)
{
    t.state = ThreadState::Ready;
    t.readySeq = ++g_seq;
    g_ready.push_back(&t);
    notify(t);
}

WakeReason block(Thread& self, WaitQueue* q, uint64_t deadlineNs)
{
    self.state = ThreadState::Waiting;
    self.wake = WakeReason::None;
    self.waitingOn = q;
    if (q) q->waiters.push_back(&self);
    self.deadline = deadlineNs;
    if (deadlineNs) g_timed.push_back(&self);
    switch_away(self);
    return self.wake;
}

bool wake_one(WaitQueue& q, WakeReason why)
{
    if (q.waiters.empty()) return false;
    Thread* best = q.waiters[0];
    for (Thread* t : q.waiters)
        if (t->priority < best->priority) best = t;
    wake_thread(*best, why);
    return true;
}

int wake_all(WaitQueue& q, WakeReason why)
{
    int n = 0;
    while (!q.waiters.empty())
    {
        wake_thread(*q.waiters.front(), why);
        n++;
    }
    return n;
}

int resume(Thread& t)
{
    int prev = t.suspendCount;
    if (t.suspendCount > 0 && --t.suspendCount == 0 && t.state == ThreadState::None)
        make_ready(t);
    sync_guest(t);
    return prev;
}

void set_priority(Thread& t, int prio)
{
    t.basePriority = t.priority = prio;
    sync_guest(t);
    if (t.state == ThreadState::Ready) notify(t);
}

void yield_current(Thread& self)
{
    self.ctx.yield_request.store(0, std::memory_order_relaxed);
    self.state = ThreadState::Ready;
    self.readySeq = ++g_seq;
    g_ready.push_back(&self);
    switch_away(self);
}

void exit_current(Thread& self, uint32_t value)
{
    lock();
    self.exitValue = value;
    if (self.guest) wr32(self.guest + T_EXITVAL, value);
    self.state = ThreadState::Moribund;
    wake_all(self.joinQueue);
    if (self.detached)
    {
        g_byGuest.erase(self.guest);
        remove_from(g_threads, &self);
    }
    g_deadFibers.push_back(self.fiber);
    switch_away(self);
    fatal("exited thread was rescheduled");
}

bool set_interrupts(PPCContext& ctx, bool enabled)
{
    Core& core = g_cores[ctx.core];
    bool prev = !core.interruptsDisabled;
    core.interruptsDisabled = !enabled;
    return prev;
}

void* core_host_handle(int core) { return g_cores[core].host.native_handle(); }
Thread* core_running(int core) { return g_cores[core].running; }
Thread* running_here() { return t_core ? t_core->running : nullptr; }

void add_timer_hook(std::function<void(uint64_t)> hook)
{
    LockGuard g;
    g_timerHooks.push_back(std::move(hook));
}

}

namespace wf {

void yield(PPCContext& ctx, uint8_t*)
{
    ctx.yield_request.store(0, std::memory_order_relaxed);
    os::lock();
    os::Thread& self = os::current(ctx);
    if (!os::g_cores[ctx.core].interruptsDisabled && os::better_ready(ctx.core, self.priority))
        os::yield_current(self);
    os::unlock();
}

}
