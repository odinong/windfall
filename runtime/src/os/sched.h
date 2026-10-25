#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "wf/ppc.h"

namespace wf::os {

inline constexpr int kNumCores = 3;
inline constexpr uint64_t kTimerClock = 62156250;

enum class ThreadState : uint8_t { None = 0, Ready = 1, Running = 2, Waiting = 4, Moribund = 8 };

struct Thread;

struct WaitQueue
{
    std::vector<Thread*> waiters;
};

enum class WakeReason : uint8_t { None, Signaled, Timeout };

struct Thread
{
    PPCContext ctx{};
    uint32_t guest = 0;
    void* fiber = nullptr;
    ThreadState state = ThreadState::None;
    int basePriority = 16;
    int priority = 16;
    uint8_t affinity = 0x7;
    int suspendCount = 1;
    bool detached = false;
    uint32_t entry = 0, argc = 0, argv = 0;
    uint32_t stackTop = 0, stackEnd = 0;
    uint32_t exitValue = 0;
    int core = -1;
    uint64_t readySeq = 0;
    std::function<void(Thread&)> hostEntry;

    WaitQueue* waitingOn = nullptr;
    uint64_t deadline = 0;
    WakeReason wake = WakeReason::None;
    WaitQueue joinQueue;
    uint32_t cleanupCallback = 0, deallocator = 0;
    std::string name;
    void* bridgeCurrent = nullptr;
};

void init(uint32_t sdaBase, uint32_t sda2Base);
void start_cores();
Thread* create_thread(uint32_t guestThread, uint32_t entry, uint32_t argc, uint32_t argv, uint32_t stackTop,
                      uint32_t stackSize, int priority, uint32_t attr);
Thread* create_host_thread(const char* name, int priority, uint8_t affinity, uint32_t stackSize,
                           std::function<void(Thread&)> body);
Thread* find_thread(uint32_t guestThread);
std::vector<Thread*> all_threads();

void lock();
void unlock();
struct LockGuard { LockGuard() { lock(); } ~LockGuard() { unlock(); } };

Thread& current(PPCContext& ctx);
WakeReason block(Thread& self, WaitQueue* q, uint64_t deadlineNs = 0);
[[noreturn]] void exit_current(Thread& self, uint32_t value);
void yield_current(Thread& self);

void make_ready(Thread& t);
bool wake_one(WaitQueue& q, WakeReason why = WakeReason::Signaled);
int wake_all(WaitQueue& q, WakeReason why = WakeReason::Signaled);
int resume(Thread& t);
void set_priority(Thread& t, int prio);

uint64_t now_ns();
uint64_t ticks_since_2000();
uint64_t ticks_since_boot();
inline uint64_t ticks_to_ns(uint64_t t) { return t / kTimerClock * 1000000000ull + t % kTimerClock * 1000000000ull / kTimerClock; }
inline uint64_t ns_to_ticks(uint64_t ns) { return ns / 1000000000ull * kTimerClock + ns % 1000000000ull * kTimerClock / 1000000000ull; }

void add_timer_hook(std::function<void(uint64_t nowNs)> hook);

bool set_interrupts(PPCContext& ctx, bool enabled);

void* core_host_handle(int core);
Thread* core_running(int core);
Thread* running_here();

}
