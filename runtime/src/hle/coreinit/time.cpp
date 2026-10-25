#include <chrono>

#include "hle/hle.h"
#include "os/sched.h"

namespace wf::hle {
namespace {

constexpr uint32_t kBusClock = 248625000, kCoreClock = 1243125000;

uint64_t OSGetTime() { return os::ticks_since_2000(); }
WF_EXPORT(coreinit, OSGetTime);
uint64_t OSGetSystemTime() { return os::ticks_since_boot(); }
WF_EXPORT(coreinit, OSGetSystemTime);
uint32_t OSGetTick() { return (uint32_t)os::ticks_since_2000(); }
WF_EXPORT(coreinit, OSGetTick);

void OSTicksToCalendarTime(uint64_t ticks, uint32_t out)
{
    using namespace std::chrono;
    uint64_t secs = ticks / os::kTimerClock, sub = ticks % os::kTimerClock;
    sys_days days2000 = sys_days{year{2000} / January / 1} + days{(int64_t)(secs / 86400)};
    year_month_day ymd{days2000};
    uint32_t sod = (uint32_t)(secs % 86400);
    weekday wd{days2000};
    int dayOfYear = (days2000 - sys_days{ymd.year() / January / 1}).count();
    wr32(out + 0x00, sod % 60);
    wr32(out + 0x04, sod / 60 % 60);
    wr32(out + 0x08, sod / 3600);
    wr32(out + 0x0C, (unsigned)ymd.day());
    wr32(out + 0x10, (unsigned)ymd.month() - 1);
    wr32(out + 0x14, (uint32_t)(int)ymd.year());
    wr32(out + 0x18, wd.c_encoding());
    wr32(out + 0x1C, (uint32_t)dayOfYear);
    wr32(out + 0x20, (uint32_t)(sub * 1000 / os::kTimerClock));
    wr32(out + 0x24, (uint32_t)(sub * 1000000 / os::kTimerClock % 1000));
}
WF_EXPORT(coreinit, OSTicksToCalendarTime);

uint32_t OSGetSystemInfo()
{
    static uint32_t info = [] {
        uint32_t a = sys_alloc(0x20, 8);
        wr32(a + 0x00, kBusClock);
        wr32(a + 0x04, kCoreClock);
        wr64(a + 0x08, os::ticks_since_2000());
        wr32(a + 0x10, 512 * 1024);
        wr32(a + 0x14, 2 * 1024 * 1024);
        wr32(a + 0x18, 512 * 1024);
        wr32(a + 0x1C, 5);
        return a;
    }();
    return info;
}
WF_EXPORT(coreinit, OSGetSystemInfo);

}
}
