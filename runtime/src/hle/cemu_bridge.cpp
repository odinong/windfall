#include "Common/precompiled.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/OS/RPL/COSModule.h"
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/TCL/TCL.h"
#include "Cafe/OS/libs/coreinit/coreinit.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"

#include <array>
#include <utility>

#include "core/log.h"
#include "hle/cemu_bridge.h"
#include "hle/coreinit/sync.h"
#include "hle/hle.h"
#include "os/sched.h"

namespace wf::hle {
uint32_t default_heap_alloc(uint32_t size, int32_t align);
void default_heap_free(uint32_t p);
}

namespace {

PPCInterpreter_t* current_get()
{
    wf::os::Thread* t = wf::os::running_here();
    return t ? (PPCInterpreter_t*)t->bridgeCurrent : nullptr;
}

struct CurrentProxy
{
    operator PPCInterpreter_t*() const { return current_get(); }
    PPCInterpreter_t* operator->() const { return current_get(); }
    CurrentProxy& operator=(PPCInterpreter_t* p)
    {
        if (wf::os::Thread* t = wf::os::running_here()) t->bridgeCurrent = p;
        return *this;
    }
};
CurrentProxy t_current;

void load(PPCInterpreter_t& h, wf::PPCContext& c)
{
    h.wfContext = &c;
    memcpy(h.gpr, c.r, sizeof(h.gpr));
    for (int i = 0; i < 14; i++) { h.fpr[i].fp0 = c.f[i].ps0; h.fpr[i].fp1 = c.f[i].ps1; }
    memcpy(h.cr, c.cr, 32);
    h.xer_ca = c.xer_ca; h.xer_so = c.xer_so; h.xer_ov = c.xer_ov;
    h.spr.LR = c.lr;
    h.spr.CTR = c.ctr;
    h.spr.UPIR = c.core;
    memcpy(h.spr.UGQR, c.gqr, sizeof(h.spr.UGQR));
    h.fpscr = c.fpscr;
    h.instructionPointer = 0;
}

void store(const PPCInterpreter_t& h, wf::PPCContext& c)
{
    memcpy(c.r, h.gpr, sizeof(c.r));
    for (int i = 0; i < 32; i++) { c.f[i].ps0 = h.fpr[i].fp0; c.f[i].ps1 = h.fpr[i].fp1; }
    memcpy(c.cr, h.cr, 32);
    c.xer_ca = h.xer_ca;
}

constexpr size_t kMaxExports = 512;
void (*g_exports[kMaxExports])(PPCInterpreter_t*);
size_t g_exportCount = 0;

template<size_t I> void trampoline(wf::PPCContext& ctx, uint8_t*)
{
    PPCInterpreter_t h;
    load(h, ctx);
    PPCInterpreter_t* prev = t_current;
    t_current = &h;
    g_exports[I](&h);
    t_current = prev;
    ctx.r[3] = h.gpr[3];
    ctx.r[4] = h.gpr[4];
    ctx.f[1].ps0 = h.fpr[1].fp0;
    ctx.f[1].ps1 = h.fpr[1].fp1;
}
template<size_t... I> constexpr auto make_trampolines(std::index_sequence<I...>) { return std::array<wf::PPCFunc*, sizeof...(I)>{&trampoline<I>...}; }
constexpr auto kTrampolines = make_trampolines(std::make_index_sequence<kMaxExports>{});

wf::PPCFunc* trampoline_for(void (*fn)(PPCInterpreter_t*))
{
    for (size_t i = 0; i < g_exportCount; i++)
        if (g_exports[i] == fn) return kTrampolines[i];
    if (g_exportCount >= kMaxExports) wf::fatal("cemu_bridge: out of export trampolines");
    g_exports[g_exportCount] = fn;
    return kTrampolines[g_exportCount++];
}

}

void osLib_addFunctionInternal(const char* libraryName, const char* functionName, void (*osFunction)(PPCInterpreter_t* hCPU))
{
    wf::hle::register_function(libraryName, functionName, trampoline_for(osFunction));
}

void osLib_addVirtualPointer(const char* libraryName, const char* functionName, uint32 vPtr)
{
    wf::log_cat("hle", "osLib_addVirtualPointer({}::{}) ignored", libraryName, functionName);
}

void osLib_returnFromFunction(PPCInterpreter_t* hCPU, uint32 returnValue) { hCPU->gpr[3] = returnValue; }
void osLib_returnFromFunction64(PPCInterpreter_t* hCPU, uint64 returnValue64)
{
    hCPU->gpr[3] = (uint32)(returnValue64 >> 32);
    hCPU->gpr[4] = (uint32)returnValue64;
}

PPCInterpreter_t* PPCInterpreter_getCurrentInstance() { return t_current; }
uint32 PPCInterpreter_getCurrentCoreIndex() { return t_current ? t_current->spr.UPIR : 1; }
uint64 PPCInterpreter_getMainCoreCycleCounter() { return wf::os::ticks_since_boot() * 20; }

PPCInterpreter_t* PPCCore_executeCallbackInternal(uint32 functionMPTR)
{
    PPCInterpreter_t* h = t_current;
    wf::PPCContext& c = *(wf::PPCContext*)h->wfContext;
    uint32 savedR1 = c.r[1], savedLr = c.lr;
    for (int i = 3; i <= 10; i++) c.r[i] = h->gpr[i];
    for (int i = 1; i <= 8; i++) c.f[i].ps0 = h->fpr[i].fp0;
    c.r[1] = h->gpr[1] - 16 * 4;
    c.lr = 0;
    wf::call_indirect(c, wf::g_base, functionMPTR, 0);
    h->gpr[3] = c.r[3];
    h->gpr[4] = c.r[4];
    h->fpr[1].fp0 = c.f[1].ps0;
    c.r[1] = savedR1;
    c.lr = savedLr;
    return h;
}

void PPCCore_switchToScheduler()
{
    auto& c = *(wf::PPCContext*)t_current->wfContext;
    wf::os::LockGuard g;
    wf::os::yield_current(wf::os::current(c));
}

namespace wf::cemu {
void with_context(PPCContext& ctx, const std::function<void()>& fn)
{
    PPCInterpreter_t h;
    load(h, ctx);
    PPCInterpreter_t* prev = t_current;
    t_current = &h;
    fn();
    t_current = prev;
}
}

MPTR PPCInterpreter_makeCallableExportDepr(void (*fn)(PPCInterpreter_t*))
{
    return wf::hle::make_thunk(trampoline_for(fn));
}

uint8* PPCInterpreterGetStackPointer() { return memory_base + t_current->gpr[1]; }
uint8* PPCInterpreter_PushAndReturnStackPointer(sint32 offset)
{
    t_current->gpr[1] -= (uint32)offset;
    return memory_base + t_current->gpr[1];
}
void PPCInterpreterModifyStackPointer(sint32 offset) { t_current->gpr[1] -= (uint32)offset; }

MPTR coreinit_allocFromSysArea(uint32 size, uint32 alignment) { return wf::sys_alloc(size, std::max(alignment, 4u)); }

namespace coreinit {
uint32 OSGetCoreId() { return t_current ? t_current->spr.UPIR : 1; }
OSThread_t* OSGetCurrentThread()
{
    if (!t_current) return nullptr;
    auto& c = *(wf::PPCContext*)t_current->wfContext;
    return (OSThread_t*)memory_getPointerFromVirtualOffset(wf::os::current(c).guest);
}
uint32 __OSGetProcessSDKVersion() { return 20911; }
void* _weak_MEMAllocFromDefaultHeapEx(uint32 size, sint32 alignment) { return memory_getPointerFromVirtualOffsetAllowNull(wf::hle::default_heap_alloc(size, alignment)); }
void* _weak_MEMAllocFromDefaultHeap(uint32 size) { return _weak_MEMAllocFromDefaultHeapEx(size, 0x40); }
void _weak_MEMFreeToDefaultHeap(void* ptr) { wf::hle::default_heap_free(memory_getVirtualOffsetFromPointer(ptr)); }
uint64 OSGetSystemTime() { return wf::os::ticks_since_boot(); }
uint64 OSGetTime() { return wf::os::ticks_since_2000(); }

void OSSleepTicks(uint64 ticks)
{
    auto& c = *(wf::PPCContext*)t_current->wfContext;
    wf::os::LockGuard g;
    wf::os::WaitQueue none;
    wf::os::block(wf::os::current(c), &none, wf::os::now_ns() + wf::os::ticks_to_ns(ticks));
}
void OSInitEvent(OSEvent* event, OSEvent::EVENT_STATE initialState, OSEvent::EVENT_MODE mode)
{
    wf::hle::event_init(memory_getVirtualOffsetFromPointer(event), initialState == OSEvent::EVENT_STATE::STATE_SIGNALED,
                        mode == OSEvent::EVENT_MODE::MODE_AUTO);
}
void OSWaitEvent(OSEvent* event) { wf::hle::event_wait(*(wf::PPCContext*)t_current->wfContext, memory_getVirtualOffsetFromPointer(event)); }
void OSSignalEvent(OSEvent* event) { wf::hle::event_signal(memory_getVirtualOffsetFromPointer(event)); }
void OSSignalEventAllInternal(OSEvent* event) { wf::hle::event_signal_all(memory_getVirtualOffsetFromPointer(event)); }

void OSInitMutexEx(OSMutex* mutex, void*) { wf::hle::mutex_init(memory_getVirtualOffsetFromPointer(mutex)); }
void OSLockMutex(OSMutex* mutex) { wf::hle::mutex_lock(*(wf::PPCContext*)t_current->wfContext, memory_getVirtualOffsetFromPointer(mutex)); }
void OSUnlockMutex(OSMutex* mutex) { wf::hle::mutex_unlock(*(wf::PPCContext*)t_current->wfContext, memory_getVirtualOffsetFromPointer(mutex)); }
void OSExitThread(sint32 exitValue)
{
    auto& c = *(wf::PPCContext*)t_current->wfContext;
    wf::os::lock();
    wf::os::exit_current(wf::os::current(c), (uint32)exitValue);
}
bool OSIsThreadTerminated(OSThread_t* thread)
{
    wf::os::LockGuard g;
    wf::os::Thread* t = wf::os::find_thread(memory_getVirtualOffsetFromPointer(thread));
    return !t || t->state == wf::os::ThreadState::Moribund;
}
}

namespace GX2 {
void _test_AddrLib() {}
COSModule* GetModule();
}
namespace TCL { COSModule* GetModule(); }
namespace snd_core {
COSModule* GetModuleSndCore1();
COSModule* GetModuleSndCore2();
void AXOut_init();
}

void wf::cemu::register_modules()
{
    SysAllocatorContainer::GetInstance().Initialize();
    for (COSModule* m : {GX2::GetModule(), TCL::GetModule(), snd_core::GetModuleSndCore1(), snd_core::GetModuleSndCore2()})
    {
        m->RPLMapped();
        m->rpl_entry(0, coreinit::RplEntryReason::Loaded);
    }
    snd_core::AXOut_init();
}
