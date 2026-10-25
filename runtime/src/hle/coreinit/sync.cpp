#include <algorithm>
#include <map>
#include <unordered_map>

#include "core/log.h"
#include "hle/coreinit/sync.h"
#include "hle/hle.h"
#include "os/sched.h"

namespace wf::hle {

using os::Thread;
using os::WaitQueue;

namespace {

struct Mutex { Thread* owner = nullptr; int count = 0; WaitQueue q; };
struct Event { bool signaled = false; bool autoReset = false; WaitQueue q; };
struct MsgQueue { uint32_t array = 0, count = 0, first = 0, used = 0; WaitQueue sendQ, recvQ; };
struct Rendezvous { WaitQueue q; };
struct Alarm { uint64_t fireNs = 0, periodNs = 0; uint32_t handler = 0; bool armed = false; };

std::unordered_map<uint32_t, Mutex> g_mutexes;
std::unordered_map<uint32_t, Event> g_events;
std::unordered_map<uint32_t, MsgQueue> g_queues;
std::unordered_map<uint32_t, Rendezvous> g_rendezvous;
std::unordered_map<uint32_t, Alarm> g_alarms;
std::vector<uint32_t> g_dueAlarms;
WaitQueue g_alarmThreadQueue;

template<class T> T& lookup(std::unordered_map<uint32_t, T>& m, uint32_t key)
{
    struct Slot { uint32_t key; T* p; };
    static Slot cache[256];
    Slot& s = cache[(key >> 3) & 255];
    if (s.p && s.key == key) return *s.p;
    T& r = m[key];
    s = {key, &r};
    return r;
}

constexpr uint32_t kMutexMagic = 0x6D557458;
constexpr uint32_t kEventMagic = 0x65566E54;
constexpr uint32_t kQueueMagic = 0x6D536751;
constexpr uint32_t kAlarmMagic = 0x614C724D;

}

void mutex_init(uint32_t m)
{
    os::LockGuard g;
    g_mutexes[m] = Mutex{};
    if (m) wr32(m, kMutexMagic);
}

void mutex_lock(PPCContext& ctx, uint32_t m)
{
    os::LockGuard g;
    Thread& self = os::current(ctx);
    Mutex& mx = lookup(g_mutexes, m);
    while (mx.owner && mx.owner != &self)
        os::block(self, &mx.q);
    mx.owner = &self;
    mx.count++;
}

bool mutex_trylock(PPCContext& ctx, uint32_t m)
{
    os::LockGuard g;
    Thread& self = os::current(ctx);
    Mutex& mx = lookup(g_mutexes, m);
    if (mx.owner && mx.owner != &self) return false;
    mx.owner = &self;
    mx.count++;
    return true;
}

void mutex_unlock(PPCContext& ctx, uint32_t m)
{
    os::LockGuard g;
    Mutex& mx = lookup(g_mutexes, m);
    if (mx.owner != &os::current(ctx))
    {
        log_warn("unlock of mutex {:08X} not owned by the calling thread", m);
        return;
    }
    if (--mx.count == 0)
    {
        mx.owner = nullptr;
        os::wake_one(mx.q);
    }
}

void event_init(uint32_t e, bool signaled, bool autoReset)
{
    os::LockGuard g;
    g_events[e] = Event{signaled, autoReset};
    wr32(e, kEventMagic);
    wr32(e + 0x0C, signaled);
    wr32(e + 0x20, autoReset);
}

void event_signal(uint32_t e)
{
    os::LockGuard g;
    Event& ev = lookup(g_events, e);
    if (ev.autoReset)
    {
        if (!os::wake_one(ev.q)) ev.signaled = true;
    }
    else
    {
        ev.signaled = true;
        os::wake_all(ev.q);
    }
    wr32(e + 0x0C, ev.signaled);
}

void event_signal_all(uint32_t e)
{
    os::LockGuard g;
    Event& ev = lookup(g_events, e);
    if (os::wake_all(ev.q) == 0 || !ev.autoReset)
        ev.signaled = true;
    wr32(e + 0x0C, ev.signaled);
}

bool event_wait(PPCContext& ctx, uint32_t e, uint64_t deadline)
{
    os::LockGuard g;
    Event& ev = lookup(g_events, e);
    if (ev.signaled)
    {
        if (ev.autoReset) { ev.signaled = false; wr32(e + 0x0C, 0); }
        return true;
    }
    return os::block(os::current(ctx), &ev.q, deadline) == os::WakeReason::Signaled;
}

namespace {

void OSInitMutex(uint32_t m) { mutex_init(m); }
WF_EXPORT(coreinit, OSInitMutex);
void OSLockMutex(PPCContext& ctx, uint32_t m) { mutex_lock(ctx, m); }
WF_EXPORT(coreinit, OSLockMutex);
bool OSTryLockMutex(PPCContext& ctx, uint32_t m) { return mutex_trylock(ctx, m); }
WF_EXPORT(coreinit, OSTryLockMutex);
void OSUnlockMutex(PPCContext& ctx, uint32_t m) { mutex_unlock(ctx, m); }
WF_EXPORT(coreinit, OSUnlockMutex);

void OSInitEvent(uint32_t e, uint32_t signaled, uint32_t mode) { event_init(e, signaled != 0, mode == 1); }
WF_EXPORT(coreinit, OSInitEvent);

void OSSignalEvent(uint32_t e) { event_signal(e); }
WF_EXPORT(coreinit, OSSignalEvent);

void OSSignalEventAll(uint32_t e) { event_signal_all(e); }
WF_EXPORT(coreinit, OSSignalEventAll);

void OSResetEvent(uint32_t e)
{
    os::LockGuard g;
    g_events[e].signaled = false;
    wr32(e + 0x0C, 0);
}
WF_EXPORT(coreinit, OSResetEvent);

void OSWaitEvent(PPCContext& ctx, uint32_t e) { event_wait(ctx, e, 0); }
WF_EXPORT(coreinit, OSWaitEvent);

bool OSWaitEventWithTimeout(PPCContext& ctx, uint32_t e, uint64_t timeoutTicks)
{
    return event_wait(ctx, e, os::now_ns() + os::ticks_to_ns(timeoutTicks));
}
WF_EXPORT(coreinit, OSWaitEventWithTimeout);

void OSInitMessageQueue(uint32_t q, uint32_t array, uint32_t count)
{
    os::LockGuard g;
    g_queues[q] = MsgQueue{array, count};
    wr32(q, kQueueMagic);
    wr32(q + 0x24, array);
    wr32(q + 0x28, count);
}
WF_EXPORT(coreinit, OSInitMessageQueue);

bool OSSendMessage(PPCContext& ctx, uint32_t q, uint32_t msg, uint32_t flags)
{
    os::LockGuard g;
    MsgQueue& mq = lookup(g_queues, q);
    while (mq.used >= mq.count)
    {
        if (!(flags & 1)) return false;
        os::block(os::current(ctx), &mq.sendQ);
    }
    memcpy(gptr(mq.array + 16 * ((mq.first + mq.used) % mq.count)), gptr(msg), 16);
    mq.used++;
    os::wake_one(mq.recvQ);
    return true;
}
WF_EXPORT(coreinit, OSSendMessage);

bool OSReceiveMessage(PPCContext& ctx, uint32_t q, uint32_t msg, uint32_t flags)
{
    os::LockGuard g;
    MsgQueue& mq = lookup(g_queues, q);
    while (mq.used == 0)
    {
        if (!(flags & 1)) return false;
        os::block(os::current(ctx), &mq.recvQ);
    }
    memcpy(gptr(msg), gptr(mq.array + 16 * mq.first), 16);
    mq.first = (mq.first + 1) % mq.count;
    mq.used--;
    os::wake_one(mq.sendQ);
    return true;
}
WF_EXPORT(coreinit, OSReceiveMessage);

void OSInitRendezvous(uint32_t rv)
{
    os::LockGuard g;
    memset(gptr(rv), 0, 0x10);
    g_rendezvous[rv] = Rendezvous{};
}
WF_EXPORT(coreinit, OSInitRendezvous);

bool OSWaitRendezvous(PPCContext& ctx, uint32_t rv, uint32_t coreMask)
{
    os::LockGuard g;
    Rendezvous& r = g_rendezvous[rv];
    wr32(rv + 4 * ctx.core, 1);
    auto done = [&] {
        for (int c = 0; c < 3; c++)
            if ((coreMask & (1 << c)) && !rd32(rv + 4 * c)) return false;
        return true;
    };
    if (done())
    {
        os::wake_all(r.q);
        return true;
    }
    while (!done())
        os::block(os::current(ctx), &r.q);
    return true;
}
WF_EXPORT(coreinit, OSWaitRendezvous);

void OSCreateAlarm(uint32_t a)
{
    os::LockGuard g;
    memset(gptr(a), 0, 0x58);
    wr32(a, kAlarmMagic);
    g_alarms[a] = Alarm{};
}
WF_EXPORT(coreinit, OSCreateAlarm);

void arm(uint32_t a, uint64_t delayTicks, uint64_t periodTicks, uint32_t handler)
{
    os::LockGuard g;
    Alarm& al = g_alarms[a];
    al.fireNs = os::now_ns() + os::ticks_to_ns(delayTicks);
    al.periodNs = os::ticks_to_ns(periodTicks);
    al.handler = handler;
    al.armed = true;
    wr32(a + 0x0C, handler);
}

bool OSSetAlarm(uint32_t a, uint64_t delayTicks, uint32_t handler)
{
    arm(a, delayTicks, 0, handler);
    return true;
}
WF_EXPORT(coreinit, OSSetAlarm);

bool OSSetPeriodicAlarm(uint32_t a, uint64_t startTicks, uint64_t periodTicks, uint32_t handler)
{
    uint64_t now = os::ticks_since_2000();
    arm(a, startTicks > now ? startTicks - now : 0, periodTicks, handler);
    return true;
}
WF_EXPORT(coreinit, OSSetPeriodicAlarm);

bool OSCancelAlarm(uint32_t a)
{
    os::LockGuard g;
    auto it = g_alarms.find(a);
    if (it == g_alarms.end() || !it->second.armed) return false;
    it->second.armed = false;
    std::erase(g_dueAlarms, a);
    return true;
}
WF_EXPORT(coreinit, OSCancelAlarm);

void OSSetAlarmUserData(uint32_t a, uint32_t data) { wr32(a + 0x38, data); }
WF_EXPORT(coreinit, OSSetAlarmUserData);
uint32_t OSGetAlarmUserData(uint32_t a) { return rd32(a + 0x38); }
WF_EXPORT(coreinit, OSGetAlarmUserData);

}

void start_alarm_thread()
{
    uint32_t ctxArea = sys_alloc(0x320, 8);
    os::create_host_thread("Alarm Thread", 0x20, 0x7, 128 * 1024, [ctxArea](Thread& self) {
        for (;;)
        {
            os::lock();
            while (g_dueAlarms.empty())
                os::block(self, &g_alarmThreadQueue);
            uint32_t a = g_dueAlarms.front();
            g_dueAlarms.erase(g_dueAlarms.begin());
            uint32_t handler = g_alarms[a].handler;
            os::unlock();
            wr32(a + 0x54, ctxArea);
            call_guest(self.ctx, handler, {a, ctxArea});
        }
    });
    os::add_timer_hook([](uint64_t now) {
        for (auto& [addr, al] : g_alarms)
        {
            if (!al.armed || al.fireNs > now) continue;
            if (std::find(g_dueAlarms.begin(), g_dueAlarms.end(), addr) == g_dueAlarms.end())
                g_dueAlarms.push_back(addr);
            if (al.periodNs)
                while (al.fireNs <= now) al.fireNs += al.periodNs;
            else
                al.armed = false;
        }
        if (!g_dueAlarms.empty())
            os::wake_one(g_alarmThreadQueue);
    });
}

}
