#include "Common/precompiled.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/OS/libs/gx2/GX2_Command.h"
#include "Cafe/OS/libs/gx2/GX2_Event.h"
#include "Cafe/OS/libs/gx2/GX2_Memory.h"
#include "Cafe/OS/libs/gx2/GX2_Resource.h"

#include <deque>
#include <thread>

#include "core/log.h"
#include "hle/hle.h"
#include "os/sched.h"
#include "debug/watchdog.h"

namespace wf::hle {
uint32_t default_heap_alloc(uint32_t size, int32_t align);
void default_heap_free(uint32_t p);
}

namespace {

wf::os::WaitQueue g_vsyncWaiters, g_flipWaiters, g_eventWaiters;
std::deque<GX2::GX2CallbackEventType> g_eventQueue;

struct EventCallback { MPTR func = 0; MPTR userData = 0; };
EventCallback g_callbacks[GX2::GX2CallbackEventTypeCount];

wf::PPCContext& cur_ctx() { return *(wf::PPCContext*)PPCInterpreter_getCurrentInstance()->wfContext; }

std::atomic<int> g_vsyncRate{60};
bool vsync_callback_due()
{
    static int acc = 0;
    int rate = g_vsyncRate;
    if (rate == 60) return true;
    acc += 60;
    if (acc < rate) return false;
    acc -= rate;
    return true;
}

void notify_locked(GX2::GX2CallbackEventType type)
{
    if (type == GX2::GX2CallbackEventType::VSYNC && !vsync_callback_due())
    {
        wf::os::wake_all(g_vsyncWaiters);
        return;
    }
    if (g_callbacks[(size_t)type].func)
    {
        g_eventQueue.push_back(type);
        wf::os::wake_one(g_eventWaiters);
    }
    if (type == GX2::GX2CallbackEventType::VSYNC) wf::os::wake_all(g_vsyncWaiters);
    if (type == GX2::GX2CallbackEventType::FLIP) wf::os::wake_all(g_flipWaiters);
}

}

void wf_gx2_set_vsync_rate(int rate) { g_vsyncRate = rate; }

MPTR gx2Mem_defaultAlloc = MPTR_NULL;
MPTR gx2Mem_defaultFree = MPTR_NULL;

void gx2Memory_GX2SetDefaultAllocator(MPTR defaultAllocFunc, MPTR defaulFreeFunc)
{
    gx2Mem_defaultAlloc = defaultAllocFunc;
    gx2Mem_defaultFree = defaulFreeFunc;
}

static void _GX2DefaultAlloc_Alloc(PPCInterpreter_t* hCPU)
{
    hCPU->gpr[3] = wf::hle::default_heap_alloc(hCPU->gpr[4], (sint32)hCPU->gpr[5]);
}
static void _GX2DefaultAlloc_Free(PPCInterpreter_t* hCPU)
{
    wf::hle::default_heap_free(hCPU->gpr[4]);
}
static void gx2Export_GX2SetDefaultAllocator(PPCInterpreter_t* hCPU)
{
    gx2Mem_defaultAlloc = hCPU->gpr[3];
    gx2Mem_defaultFree = hCPU->gpr[4];
    osLib_returnFromFunction(hCPU, 0);
}
static void _GX2DefaultAllocR_Alloc(PPCInterpreter_t* hCPU)
{
    hCPU->gpr[3] = PPCCoreCallback(gx2Mem_defaultAlloc, hCPU->gpr[3], hCPU->gpr[4], hCPU->gpr[5]);
}
static void _GX2DefaultAllocR_Free(PPCInterpreter_t* hCPU)
{
    PPCCoreCallback(gx2Mem_defaultFree, hCPU->gpr[3], hCPU->gpr[4]);
}

namespace TCL { void TCLPublish(int why); }

namespace GX2 {

void GX2MEMAllocatorsInit()
{
    gx2Mem_defaultAlloc = PPCInterpreter_makeCallableExportDepr(_GX2DefaultAlloc_Alloc);
    gx2Mem_defaultFree = PPCInterpreter_makeCallableExportDepr(_GX2DefaultAlloc_Free);
    GX2::GX2RSetAllocator(PPCInterpreter_makeCallableExportDepr(_GX2DefaultAllocR_Alloc), PPCInterpreter_makeCallableExportDepr(_GX2DefaultAllocR_Free));
}

void GX2MemInit() { osLib_addFunction("gx2", "GX2SetDefaultAllocator", gx2Export_GX2SetDefaultAllocator); }

void GX2SetGPUFence(uint32be* fencePtr, uint32 mask, uint32 compareOp, uint32 compareValue)
{
    GX2ReserveCmdSpace(7);
    uint8 compareOpTable[] = {0x7, 0x1, 0x3, 0x2, 0x6, 0x4, 0x5, 0x0};
    gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_WAIT_REG_MEM, 6));
    gx2WriteGather_submitU32AsBE((uint32)(compareOpTable[compareOp & 7]) | 0x10);
    gx2WriteGather_submitU32AsBE(memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(fencePtr)) | 2);
    gx2WriteGather_submitU32AsBE(0);
    gx2WriteGather_submitU32AsBE(compareValue);
    gx2WriteGather_submitU32AsBE(mask);
    gx2WriteGather_submitU32AsBE(0xA);
}

enum class GX2PipeEventType : uint32 { TOP = 0, BOTTOM = 1, BOTTOM_AFTER_FLUSH = 2 };

void GX2SubmitUserTimeStamp(uint64* timestampOut, uint64 value, GX2PipeEventType eventType, uint32 triggerInterrupt)
{
    GX2ReserveCmdSpace(11);
    MPTR phys = memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(timestampOut));
    gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_MEM_WRITE, 4));
    gx2WriteGather_submitU32AsBE(phys | 0x2);
    gx2WriteGather_submitU32AsBE(0);
    gx2WriteGather_submitU32AsBE((uint32)value);
    gx2WriteGather_submitU32AsBE((uint32)(value >> 32));
}

void GX2SetEventCallback(GX2CallbackEventType eventType, void* callbackFuncPtr, void* userData)
{
    if ((size_t)eventType >= GX2CallbackEventTypeCount) return;
    g_callbacks[(size_t)eventType] = {memory_getVirtualOffsetFromPointer(callbackFuncPtr), memory_getVirtualOffsetFromPointer(userData)};
    wf::log_info("GX2SetEventCallback(type {}, func {:08X}, user {:08X})", (uint32)eventType, g_callbacks[(size_t)eventType].func,
                 g_callbacks[(size_t)eventType].userData);
}

void GX2GetEventCallback(GX2CallbackEventType eventType, MEMPTR<void>* callbackFuncPtrOut, MEMPTR<void>* userDataOut)
{
    if ((size_t)eventType >= GX2CallbackEventTypeCount) return;
    if (callbackFuncPtrOut) *callbackFuncPtrOut = MEMPTR<void>(g_callbacks[(size_t)eventType].func);
    if (userDataOut) *userDataOut = MEMPTR<void>(g_callbacks[(size_t)eventType].userData);
}

void __GX2NotifyEvent(GX2CallbackEventType eventType)
{
    wf::os::LockGuard g;
    notify_locked(eventType);
}

void __GX2NotifyNewRetirementTimestamp(uint64) {}

void GX2WaitForVsync()
{
    wf::PPCContext& c = cur_ctx();
    wf::os::LockGuard g;
    wf::os::block(wf::os::current(c), &g_vsyncWaiters);
}

void GX2WaitForFlip()
{
    TCL::TCLPublish(7);
    wf::PPCContext& c = cur_ctx();
    wf::os::LockGuard g;
    if (_swapEndianU32(LatteGPUState.sharedArea->flipRequestCountBE) == _swapEndianU32(LatteGPUState.sharedArea->flipExecuteCountBE))
        return;
    wf::os::block(wf::os::current(c), &g_flipWaiters);
}

bool GX2DrawDone()
{
    GX2Command_Flush(0x100, true);
    return GX2WaitTimeStamp(GX2GetLastSubmittedTimeStamp());
}

void GX2Init_event()
{
    static bool launched = false;
    if (launched) return;
    launched = true;
    wf::os::create_host_thread("GX2 event callback", 0x20 + 16, 0x7, 0x4000, [](wf::os::Thread& self) {
        for (;;)
        {
            wf::os::lock();
            while (g_eventQueue.empty())
                wf::os::block(self, &g_eventWaiters);
            GX2CallbackEventType type = g_eventQueue.front();
            g_eventQueue.pop_front();
            EventCallback cb = g_callbacks[(size_t)type];
            wf::os::unlock();
            if (cb.func)
                wf::hle::call_guest(self.ctx, cb.func, {(uint32)type, cb.userData});
        }
    });
}

void GX2EventResetToDefaultState()
{
    for (auto& cb : g_callbacks) cb = {};
}

void GX2EventInit()
{
    cafeExportRegister("gx2", GX2SetGPUFence, LogType::GX2);
    cafeExportRegister("gx2", GX2SubmitUserTimeStamp, LogType::GX2);
    cafeExportRegister("gx2", GX2SetEventCallback, LogType::GX2);
    cafeExportRegister("gx2", GX2GetEventCallback, LogType::GX2);
    cafeExportRegister("gx2", GX2WaitForVsync, LogType::GX2);
    cafeExportRegister("gx2", GX2WaitForFlip, LogType::GX2);
    cafeExportRegister("gx2", GX2DrawDone, LogType::GX2);
}

}

void gx2Export_GX2AllocateTilingApertureEx(PPCInterpreter_t* hCPU)
{
    wf::log_warn("GX2AllocateTilingApertureEx is not supported");
    osLib_returnFromFunction(hCPU, 0);
}
void gx2Export_GX2FreeTilingAperture(PPCInterpreter_t* hCPU) { osLib_returnFromFunction(hCPU, 0); }

void wf_trace_attrib_buffer(void* data)
{
    static uint32 lo = 0, hi = 0, hits = 0;
    static const bool on = [] {
        const char* e = std::getenv("WF_TRACE_ATTRIB");
        if (!e) return false;
        char* end = nullptr;
        lo = (uint32)std::strtoul(e, &end, 16);
        hi = *end == '-' ? (uint32)std::strtoul(end + 1, nullptr, 16) : lo + 1;
        return true;
    }();
    if (!on || hits >= 6) return;
    uint32 a = memory_getVirtualOffsetFromPointer(data);
    if (a < lo || a >= hi) return;
    hits++;
    wf::log_info("attrib buffer {:08X}\n{}", a, wf::debug::backtrace(cur_ctx()));
}
