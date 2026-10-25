#pragma once
#include "ppc_types.h"
#if !WF_FREESTANDING
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#endif

#include "ppc_fp_cemu.h"

namespace wf {

#if WF_FREESTANDING
struct AtomicU32
{
    uint32_t v;
    uint32_t load(int order = __ATOMIC_SEQ_CST) const { return __atomic_load_n(&v, order); }
    void store(uint32_t x, int order = __ATOMIC_SEQ_CST) { __atomic_store_n(&v, x, order); }
};
#define WF_RELAXED __ATOMIC_RELAXED
#define WF_ACQUIRE __ATOMIC_ACQUIRE
#else
using AtomicU32 = std::atomic<uint32_t>;
#define WF_RELAXED std::memory_order_relaxed
#define WF_ACQUIRE std::memory_order_acquire
#endif

union FPR
{
    struct { double ps0, ps1; };
    struct { uint64_t ps0u, ps1u; };
};

struct alignas(64) PPCContext
{
    uint32_t r[32];
    uint32_t lr;
    uint32_t ctr;
    uint8_t cr[32];
    uint8_t xer_so, xer_ov, xer_ca, xer_bc;
    uint32_t fpscr;
    uint32_t gqr[8];
    uint32_t reserve_addr;
    uint32_t reserve_val;
    uint8_t reserve_valid;
    alignas(16) FPR f[32];

    AtomicU32 yield_request;
    uint32_t core;
    void* thread;
};

using PPCFunc = void(PPCContext& __restrict ctx, uint8_t* __restrict base);

struct FuncMapping { uint32_t guest; PPCFunc* host; };

inline constexpr uint32_t kCodeBase = 0x02000000;
inline constexpr uint32_t kCodeEnd = 0x02930000;

[[noreturn]] void bad_indirect_call(PPCContext& ctx, uint32_t target, uint32_t from);
void yield(PPCContext& ctx, uint8_t* base);
void trap(PPCContext& ctx, uint8_t* base, uint32_t addr);
void call_import(PPCContext& ctx, uint8_t* base, uint32_t id);
void probe(PPCContext& ctx, uint8_t* base, uint32_t addr, const char* name);
extern PPCFunc** g_funcTable;

void indirect_miss(PPCContext& ctx, uint8_t* base, uint32_t target, uint32_t from);
inline void call_indirect(PPCContext& ctx, uint8_t* base, uint32_t target, uint32_t from)
{
    uint32_t off = target - kCodeBase;
    PPCFunc* fn = (off < kCodeEnd - kCodeBase && !(off & 3)) ? g_funcTable[off >> 2] : nullptr;
    if (!fn) [[unlikely]]
    {
        indirect_miss(ctx, base, target, from);
        return;
    }
    fn(ctx, base);
}

namespace interp {
extern bool g_active;
bool entry(PPCContext& ctx, uint8_t* base, uint32_t addr);
}

template<class T> inline T ld_raw(uint8_t* b, uint32_t a) { T v; __builtin_memcpy(&v, b + a, sizeof v); return v; }
template<class T> inline void st_raw(uint8_t* b, uint32_t a, T v) { __builtin_memcpy(b + a, &v, sizeof v); }
inline uint8_t ld8(uint8_t* b, uint32_t a) { return ld_raw<uint8_t>(b, a); }
inline uint16_t ld16(uint8_t* b, uint32_t a) { return __builtin_bswap16(ld_raw<uint16_t>(b, a)); }
inline uint32_t ld32(uint8_t* b, uint32_t a) { return __builtin_bswap32(ld_raw<uint32_t>(b, a)); }
inline uint64_t ld64(uint8_t* b, uint32_t a) { return __builtin_bswap64(ld_raw<uint64_t>(b, a)); }
inline void st8(uint8_t* b, uint32_t a, uint8_t v) { st_raw<uint8_t>(b, a, v); }
inline void st16(uint8_t* b, uint32_t a, uint16_t v) { st_raw<uint16_t>(b, a, __builtin_bswap16(v)); }
inline void st32(uint8_t* b, uint32_t a, uint32_t v) { st_raw<uint32_t>(b, a, __builtin_bswap32(v)); }
inline void st64(uint8_t* b, uint32_t a, uint64_t v) { st_raw<uint64_t>(b, a, __builtin_bswap64(v)); }

inline void cmp_s(PPCContext& c, int f, int32_t a, int32_t b)
{
    uint8_t* p = &c.cr[f * 4];
    p[0] = a < b; p[1] = a > b; p[2] = a == b; p[3] = c.xer_so;
}
inline void cmp_u(PPCContext& c, int f, uint32_t a, uint32_t b)
{
    uint8_t* p = &c.cr[f * 4];
    p[0] = a < b; p[1] = a > b; p[2] = a == b; p[3] = c.xer_so;
}
inline void cmp_f(PPCContext& c, int f, double a, double b)
{
    uint8_t* p = &c.cr[f * 4];
    bool un = __builtin_isnan(a) || __builtin_isnan(b);
    p[0] = !un && a < b; p[1] = !un && a > b; p[2] = !un && a == b; p[3] = un;
}
inline uint32_t get_cr(const PPCContext& c)
{
    uint32_t v = 0;
    for (int i = 0; i < 32; i++) v |= (uint32_t)c.cr[i] << (31 - i);
    return v;
}
inline void set_cr_field(PPCContext& c, int f, uint32_t nibble)
{
    for (int i = 0; i < 4; i++) c.cr[f * 4 + i] = (nibble >> (3 - i)) & 1;
}
inline uint32_t get_xer(const PPCContext& c)
{
    return ((uint32_t)c.xer_so << 31) | ((uint32_t)c.xer_ov << 30) | ((uint32_t)c.xer_ca << 29) | c.xer_bc;
}
inline void set_xer(PPCContext& c, uint32_t v)
{
    c.xer_so = v >> 31; c.xer_ov = (v >> 30) & 1; c.xer_ca = (v >> 29) & 1; c.xer_bc = v & 0x7F;
}

inline uint32_t rotl(uint32_t v, uint32_t n) { return __builtin_rotateleft32(v, n & 31); }
inline int32_t divw(int32_t a, int32_t b)
{
    if (b == 0) return a < 0 ? -1 : 0;
    if (a == (int32_t)0x80000000 && b == -1) return -1;
    return a / b;
}
inline uint32_t divwu(uint32_t a, uint32_t b) { return b ? a / b : 0; }
inline uint32_t sraw(PPCContext& c, uint32_t s, uint32_t n)
{
    int32_t v = (int32_t)s;
    if (n & 0x20)
    {
        c.xer_ca = v < 0;
        return (uint32_t)(v >> 31);
    }
    n &= 31;
    c.xer_ca = v < 0 && n && (s & ((1u << n) - 1)) != 0;
    return (uint32_t)(v >> n);
}

inline uint32_t lwarx(PPCContext& c, uint8_t* b, uint32_t ea)
{
    uint32_t raw = __atomic_load_n((uint32_t*)(b + ea), __ATOMIC_SEQ_CST);
    c.reserve_addr = ea;
    c.reserve_val = raw;
    c.reserve_valid = 1;
    return __builtin_bswap32(raw);
}
inline void stwcx(PPCContext& c, uint8_t* b, uint32_t ea, uint32_t v)
{
    bool ok = false;
    if (c.reserve_valid && c.reserve_addr == ea)
    {
        uint32_t expected = c.reserve_val;
        ok = __atomic_compare_exchange_n((uint32_t*)(b + ea), &expected, __builtin_bswap32(v), false,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    c.reserve_valid = 0;
    c.cr[0] = 0; c.cr[1] = 0; c.cr[2] = ok; c.cr[3] = c.xer_so;
}

inline void lfs(PPCContext& c, int d, uint32_t bits)
{
    uint64_t v = fp::single_to_double_bits(bits);
    c.f[d].ps0u = v;
    c.f[d].ps1u = v;
}
inline uint32_t stfs_bits(const PPCContext& c, int s) { return fp::double_to_single_bits(c.f[s].ps0u); }
inline void set_single(PPCContext& c, int d, double v)
{
    double r = (double)(float)v;
    c.f[d].ps0 = r;
    c.f[d].ps1 = r;
}

inline void psq_load(PPCContext& c, uint8_t* b, int d, uint32_t ea, int w, int i)
{
    uint32_t gqr = c.gqr[i];
    uint32_t type = (gqr >> 16) & 7, scale = (gqr >> 24) & 63;
    uint32_t v0, v1 = 0;
    switch (type)
    {
    case 4: case 6: v0 = ld8(b, ea); if (!w) v1 = ld8(b, ea + 1); break;
    case 5: case 7: v0 = ld16(b, ea); if (!w) v1 = ld16(b, ea + 2); break;
    default: v0 = ld32(b, ea); if (!w) v1 = ld32(b, ea + 4); break;
    }
    c.f[d].ps0 = fp::dequantize(v0, type, scale);
    c.f[d].ps1 = w ? 1.0 : (double)fp::dequantize(v1, type, scale);
}
inline void psq_store(PPCContext& c, uint8_t* b, int s, uint32_t ea, int w, int i)
{
    uint32_t gqr = c.gqr[i];
    uint32_t type = gqr & 7, scale = (gqr >> 8) & 63;
    uint32_t v0 = fp::quantize((float)c.f[s].ps0, type, scale);
    uint32_t v1 = w ? 0 : fp::quantize((float)c.f[s].ps1, type, scale);
    switch (type)
    {
    case 4: case 6: st8(b, ea, (uint8_t)v0); if (!w) st8(b, ea + 1, (uint8_t)v1); break;
    case 5: case 7: st16(b, ea, (uint16_t)v0); if (!w) st16(b, ea + 2, (uint16_t)v1); break;
    default: st32(b, ea, v0); if (!w) st32(b, ea + 4, v1); break;
    }
}

}

#define PPC_FUNC(name) void name(wf::PPCContext& __restrict ctx, uint8_t* __restrict base)
#define PPC_INTERP_ENTRY(addr) do { if (__atomic_load_n(&wf::interp::g_active, __ATOMIC_RELAXED)) [[unlikely]] if (wf::interp::entry(ctx, base, addr)) return; } while (0)
#define PPC_YIELD_CHECK() do { if (ctx.yield_request.load(WF_ACQUIRE)) [[unlikely]] wf::yield(ctx, base); } while (0)
