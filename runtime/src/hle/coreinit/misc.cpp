#include <array>
#include <atomic>
#include <cstdlib>
#include <map>
#include <utility>
#include <mutex>
#include <string>

#include "core/log.h"
#include "hle/coreinit/sync.h"
#include "hle/guest_printf.h"
#include "hle/hle.h"
#include "os/sched.h"

namespace wf::hle {
namespace {

std::string trim_nl(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

void OSReport(PPCContext& ctx, uint32_t fmt) { log_cat("guest", "{}", trim_nl(guest_format(fmt, VarArgs::from_call(ctx, 4)))); }
WF_EXPORT(coreinit, OSReport);
void OSVReport(uint32_t fmt, uint32_t va) { log_cat("guest", "{}", trim_nl(guest_vformat(fmt, VaList{va}))); }
WF_EXPORT(coreinit, OSVReport);
void OSConsoleWrite(uint32_t buf, uint32_t len)
{
    std::string s(gptr<const char>(buf), len);
    log_cat("guest", "{}", trim_nl(s));
}
WF_EXPORT(coreinit, OSConsoleWrite);

void OSPanic(PPCContext& ctx, uint32_t file, uint32_t line, uint32_t fmt)
{
    std::string msg = guest_format(fmt, VarArgs::from_call(ctx, 6));
    fatal("guest OSPanic at {}:{}: {}", read_cstr(file), line, trim_nl(msg));
}
WF_EXPORT(coreinit, OSPanic);

int32_t ENVGetEnvironmentVariable(uint32_t name, uint32_t buf, uint32_t size)
{
    log_cat("hle", "ENVGetEnvironmentVariable({})", read_cstr(name));
    if (buf && size) wr8(buf, 0);
    return 1;
}
WF_EXPORT(coreinit, ENVGetEnvironmentVariable);

bool OSIsDebuggerPresent() { return false; }
WF_EXPORT(coreinit, OSIsDebuggerPresent);
bool OSIsDebuggerInitialized() { return false; }
WF_EXPORT(coreinit, OSIsDebuggerInitialized);
bool OSEnableHomeButtonMenu(uint32_t) { return true; }
WF_EXPORT(coreinit, OSEnableHomeButtonMenu);
void OSSavesDone_ReadyToRelease() {}
WF_EXPORT(coreinit, OSSavesDone_ReadyToRelease);
uint32_t OSSetExceptionCallback(uint32_t type, uint32_t cb)
{
    log_cat("hle", "OSSetExceptionCallback(type {}, {:08X})", type, cb);
    return 0;
}
WF_EXPORT(coreinit, OSSetExceptionCallback);
bool OSGetSharedData(uint32_t type, uint32_t, uint32_t outPtr, uint32_t outSize)
{
    log_cat("hle", "OSGetSharedData(type {})", type);
    if (outPtr) wr32(outPtr, 0);
    if (outSize) wr32(outSize, 0);
    return false;
}
WF_EXPORT(coreinit, OSGetSharedData);

int32_t IMEnableDim() { return 0; }
WF_EXPORT(coreinit, IMEnableDim);
int32_t IMDisableDim() { return 0; }
WF_EXPORT(coreinit, IMDisableDim);
int32_t IMIsDimEnabled(uint32_t out) { if (out) wr32(out, 0); return 0; }
WF_EXPORT(coreinit, IMIsDimEnabled);

int32_t UCOpen() { return 1; }
WF_EXPORT(coreinit, UCOpen);
int32_t UCClose(int32_t) { return 0; }
WF_EXPORT(coreinit, UCClose);
int32_t UCReadSysConfig(int32_t handle, uint32_t count, uint32_t items)
{
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t it = items + i * 0x54;
        std::string name = read_cstr(it);
        uint32_t data = rd32(it + 0x50);
        uint32_t v = 0;
        int width = 4;
        bool known = true;
        if (name == "cafe.language") v = 1;
        else if (name == "cafe.cntry_reg") v = 49;
        else if (name == "cafe.initial_launch") { v = 2; width = 1; }
        else if (name == "cafe.eula_agree" || name == "cafe.eco" || name == "cafe.fast_boot" ||
                 name == "nn.act.account_repaired") width = 1;
        else if (name == "cafe.version") width = 2;
        else if (name == "cafe.eula_version" || name == "parent.enable") v = 0;
        else known = false;
        if (data && known)
        {
            if (width == 1) wr8(data, (uint8_t)v);
            else if (width == 2) wr16(data, (uint16_t)v);
            else wr32(data, v);
        }
        log_cat("hle", "UCReadSysConfig {} -> {}{}", name, v, known ? "" : " (unknown)");
    }
    (void)handle;
    return 0;
}
WF_EXPORT(coreinit, UCReadSysConfig);

[[noreturn]] void exit_(int32_t code)
{
    log_info("guest called exit({})", code);
    std::_Exit(code);
}
WF_EXPORT_AS(coreinit, exit, exit_);
WF_EXPORT_AS(coreinit, _Exit, exit_);

std::mutex g_dynLock;
std::map<std::string, uint32_t> g_dynHandles;
std::map<uint32_t, std::string> g_dynNames;
std::map<std::pair<std::string, std::string>, uint32_t> g_dynThunks;

int32_t OSDynLoad_Acquire(uint32_t name, uint32_t handleOut)
{
    std::lock_guard lk(g_dynLock);
    std::string n = read_cstr(name);
    if (n.ends_with(".rpl")) n.resize(n.size() - 4);
    auto [it, inserted] = g_dynHandles.try_emplace(n, 0x100 + (uint32_t)g_dynHandles.size());
    g_dynNames[it->second] = n;
    if (handleOut) wr32(handleOut, it->second);
    log_cat("hle", "OSDynLoad_Acquire({}) -> {:X}", n, it->second);
    return 0;
}
WF_EXPORT(coreinit, OSDynLoad_Acquire);

constexpr size_t kMaxDynStubs = 256;
std::string g_dynStubNames[kMaxDynStubs];
std::atomic<bool> g_dynStubLogged[kMaxDynStubs];
size_t g_dynStubCount = 0;

template<size_t I> void dyn_stub(PPCContext& ctx, uint8_t*)
{
    if (!g_dynStubLogged[I].exchange(true))
        log_cat("hle", "unimplemented dynamic export {} (lr={:08X}) returns 0", g_dynStubNames[I], ctx.lr);
    ctx.r[3] = 0;
}
template<size_t... I> constexpr auto make_dyn_stubs(std::index_sequence<I...>) { return std::array<PPCFunc*, sizeof...(I)>{&dyn_stub<I>...}; }
constexpr auto kDynStubs = make_dyn_stubs(std::make_index_sequence<kMaxDynStubs>{});

int32_t OSDynLoad_FindExport(uint32_t handle, int32_t isData, uint32_t name, uint32_t out)
{
    std::lock_guard lk(g_dynLock);
    std::string mod = g_dynNames.count(handle) ? g_dynNames[handle] : "?";
    std::string sym = read_cstr(name);
    PPCFunc* fn = isData ? nullptr : find_function(mod.c_str(), sym.c_str());
    if (!fn && !isData && g_dynStubCount < kMaxDynStubs)
    {
        g_dynStubNames[g_dynStubCount] = mod + "::" + sym;
        fn = kDynStubs[g_dynStubCount++];
    }
    if (!fn)
    {
        log_warn("OSDynLoad_FindExport({}::{}, data={}) not available", mod, sym, isData);
        if (out) wr32(out, 0);
        return (int32_t)0xBAD10001;
    }
    auto [it, inserted] = g_dynThunks.try_emplace({mod, sym}, 0);
    if (inserted) it->second = make_thunk(fn);
    if (out) wr32(out, it->second);
    return 0;
}
WF_EXPORT(coreinit, OSDynLoad_FindExport);

void OSDynLoad_Release(uint32_t) {}
WF_EXPORT(coreinit, OSDynLoad_Release);
int32_t OSDynLoad_SetAllocator(uint32_t, uint32_t) { return 0; }
WF_EXPORT(coreinit, OSDynLoad_SetAllocator);

constexpr uint32_t kFopenMax = 100;
uint32_t g_iob = 0, g_iobLocks = 0, g_ghsLockKey = 0;

void init_iob(uint32_t slot)
{
    g_iob = slot;
    wr32(slot + 0 * 16 + 12, 0x1);
    wr32(slot + 1 * 16 + 12, 0x2 | (1 << 18));
    wr32(slot + 2 * 16 + 12, 0x2 | (2 << 18));
    g_iobLocks = sys_alloc(kFopenMax * 4, 4);
}
WF_DATA(coreinit, _iob, init_iob);
void init_fopen_max(uint32_t slot) { wr16(slot, (uint16_t)kFopenMax); }
WF_DATA(coreinit, __gh_FOPEN_MAX, init_fopen_max);
void init_zero(uint32_t slot) { wr32(slot, 0); }
WF_DATA(coreinit, environ, init_zero);
WF_DATA(coreinit, __atexit_cleanup, init_zero);
WF_DATA(coreinit, __stdio_cleanup, init_zero);
WF_DATA(coreinit, __cpp_exception_init_ptr, init_zero);
WF_DATA(coreinit, __cpp_exception_cleanup_ptr, init_zero);

void __ghsLock(PPCContext& ctx)
{
    if (!g_ghsLockKey) { g_ghsLockKey = sys_alloc(4, 4); mutex_init(g_ghsLockKey); }
    mutex_lock(ctx, g_ghsLockKey);
}
WF_EXPORT(coreinit, __ghsLock);
void __ghsUnlock(PPCContext& ctx) { mutex_unlock(ctx, g_ghsLockKey); }
WF_EXPORT(coreinit, __ghsUnlock);

uint32_t __ghs_flock_ptr(uint32_t file)
{
    uint32_t idx = g_iob && file >= g_iob ? (file - g_iob) / 16 : 0;
    return g_iobLocks + 4 * (idx % kFopenMax);
}
WF_EXPORT(coreinit, __ghs_flock_ptr);
void __ghs_flock_file(PPCContext& ctx, uint32_t lockPtr) { mutex_lock(ctx, lockPtr); }
WF_EXPORT(coreinit, __ghs_flock_file);
void __ghs_funlock_file(PPCContext& ctx, uint32_t lockPtr) { mutex_unlock(ctx, lockPtr); }
WF_EXPORT(coreinit, __ghs_funlock_file);
void __ghs_flock_destroy(uint32_t) {}
WF_EXPORT(coreinit, __ghs_flock_destroy);

void __ghs_mtx_init(uint32_t mtx)
{
    uint32_t key = sys_alloc(4, 4);
    mutex_init(key);
    wr32(mtx, key);
}
WF_EXPORT(coreinit, __ghs_mtx_init);
void __ghs_mtx_dst(uint32_t) {}
WF_EXPORT(coreinit, __ghs_mtx_dst);
void __ghs_mtx_lock(PPCContext& ctx, uint32_t mtx) { mutex_lock(ctx, rd32(mtx)); }
WF_EXPORT(coreinit, __ghs_mtx_lock);
void __ghs_mtx_unlock(PPCContext& ctx, uint32_t mtx) { mutex_unlock(ctx, rd32(mtx)); }
WF_EXPORT(coreinit, __ghs_mtx_unlock);

}
}
