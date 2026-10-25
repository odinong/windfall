#include "cpu/interp.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <unordered_map>
#include <vector>

#include "core/game_module.h"
#include "core/log.h"
#include "wf/recomp_api.h"

namespace wf::interp {

bool g_active = false;

namespace {

std::vector<uint8_t> g_kind;
std::unordered_map<uint32_t, uint32_t> g_rel24;

inline uint8_t kind_of(uint32_t a)
{
    uint32_t off = a - kCodeBase;
    if (off >= kCodeEnd - kCodeBase || (off & 3) || g_kind.empty()) return 0;
    return g_kind[off >> 2];
}

inline uint32_t mask(uint32_t mb, uint32_t me)
{
    uint32_t m1 = 0xFFFFFFFFu >> mb, m2 = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? (m1 & m2) : (m1 | m2);
}
inline int32_t sext16(uint32_t v) { return (int32_t)(int16_t)(v & 0xFFFF); }
inline int32_t sext12(uint32_t v) { return ((int32_t)(v << 20)) >> 20; }

[[noreturn]] void unsupported(uint32_t pc, uint32_t w, const char* what)
{
    fatal("interpreter: unsupported instruction {:08X} at {:08X} ({})", w, pc, what);
}

void call_any(PPCContext& ctx, uint8_t* base, uint32_t t, uint32_t from);

inline bool bc_taken(PPCContext& ctx, uint32_t bo, uint32_t bi)
{
    bool ok = true;
    if (!(bo & 4))
    {
        ctx.ctr--;
        ok = (bo & 2) ? ctx.ctr == 0 : ctx.ctr != 0;
    }
    if (!(bo & 0x10)) ok = ok && ((bo & 8) ? ctx.cr[bi] != 0 : ctx.cr[bi] == 0);
    return ok;
}

inline void cr0(PPCContext& ctx, uint32_t v) { cmp_s(ctx, 0, (int32_t)v, 0); }

void run_at(PPCContext& ctx, uint8_t* base, uint32_t pc)
{
    auto& R = ctx.r;
    auto& Fr = ctx.f;
    for (;;)
    {
        uint32_t w = ld32(base, pc);
        uint32_t op = w >> 26;
        uint32_t d = (w >> 21) & 31, a = (w >> 16) & 31, b = (w >> 11) & 31, c = (w >> 6) & 31;
        uint32_t ra0 = a ? R[a] : 0;
        uint32_t next = pc + 4;
        auto jump = [&](uint32_t t) -> bool {
            if (kind_of(t))
            {
                if (t <= pc && ctx.yield_request.load(std::memory_order_relaxed)) [[unlikely]] yield(ctx, base);
                pc = t;
                return true;
            }
            call_any(ctx, base, t, pc);
            return false;
        };
        switch (op)
        {
        case 18:
        {
            uint32_t t;
            if (auto it = g_rel24.find(pc); it != g_rel24.end()) t = it->second;
            else
            {
                int32_t disp = ((int32_t)((w & 0x03FFFFFC) << 6)) >> 6;
                t = (w & 2) ? (uint32_t)disp : pc + (uint32_t)disp;
            }
            if (w & 1)
            {
                ctx.lr = next;
                call_any(ctx, base, t, pc);
                pc = next;
                continue;
            }
            if (jump(t)) continue;
            return;
        }
        case 16:
        {
            int32_t disp = sext16(w & 0xFFFC);
            uint32_t t = (w & 2) ? (uint32_t)disp : pc + (uint32_t)disp;
            if (!bc_taken(ctx, d, a)) { pc = next; continue; }
            if (w & 1)
            {
                ctx.lr = next;
                call_any(ctx, base, t, pc);
                pc = next;
                continue;
            }
            if (jump(t)) continue;
            return;
        }
        case 19:
        {
            uint32_t xo = (w >> 1) & 0x3FF;
            switch (xo)
            {
            case 16:
                if (!bc_taken(ctx, d, a)) { pc = next; continue; }
                if (w & 1)
                {
                    uint32_t t = ctx.lr;
                    ctx.lr = next;
                    call_any(ctx, base, t, pc);
                    pc = next;
                    continue;
                }
                return;
            case 528:
                if (!bc_taken(ctx, d | 4, a)) { pc = next; continue; }
                if (w & 1)
                {
                    ctx.lr = next;
                    call_any(ctx, base, ctx.ctr, pc);
                    pc = next;
                    continue;
                }
                if (jump(ctx.ctr)) continue;
                return;
            case 0: std::memcpy(&ctx.cr[((w >> 23) & 7) * 4], &ctx.cr[((w >> 18) & 7) * 4], 4); break;
            case 257: ctx.cr[d] = (uint8_t)(ctx.cr[a] & ctx.cr[b]); break;
            case 449: ctx.cr[d] = (uint8_t)(ctx.cr[a] | ctx.cr[b]); break;
            case 193: ctx.cr[d] = (uint8_t)(ctx.cr[a] ^ ctx.cr[b]); break;
            case 225: ctx.cr[d] = (uint8_t)!(ctx.cr[a] & ctx.cr[b]); break;
            case 33: ctx.cr[d] = (uint8_t)!(ctx.cr[a] | ctx.cr[b]); break;
            case 289: ctx.cr[d] = (uint8_t)!(ctx.cr[a] ^ ctx.cr[b]); break;
            case 129: ctx.cr[d] = (uint8_t)(ctx.cr[a] & !ctx.cr[b]); break;
            case 417: ctx.cr[d] = (uint8_t)(ctx.cr[a] | !ctx.cr[b]); break;
            case 150: std::atomic_signal_fence(std::memory_order_seq_cst); break;
            default: unsupported(pc, w, "op19");
            }
            pc = next;
            continue;
        }
        case 14: R[d] = ra0 + (uint32_t)sext16(w); break;
        case 15: R[d] = ra0 + ((w & 0xFFFF) << 16); break;
        case 12: case 13:
        {
            uint64_t t = (uint64_t)R[a] + (uint32_t)sext16(w);
            ctx.xer_ca = (uint8_t)(t >> 32);
            R[d] = (uint32_t)t;
            if (op == 13) cr0(ctx, R[d]);
            break;
        }
        case 8:
        {
            uint64_t t = (uint64_t)(uint32_t)~R[a] + (uint32_t)sext16(w) + 1;
            ctx.xer_ca = (uint8_t)(t >> 32);
            R[d] = (uint32_t)t;
            break;
        }
        case 7: R[d] = (uint32_t)((int64_t)(int32_t)R[a] * sext16(w)); break;
        case 24: R[a] = R[d] | (w & 0xFFFF); break;
        case 25: R[a] = R[d] | ((w & 0xFFFF) << 16); break;
        case 26: R[a] = R[d] ^ (w & 0xFFFF); break;
        case 27: R[a] = R[d] ^ ((w & 0xFFFF) << 16); break;
        case 28: R[a] = R[d] & (w & 0xFFFF); cr0(ctx, R[a]); break;
        case 29: R[a] = R[d] & ((w & 0xFFFF) << 16); cr0(ctx, R[a]); break;
        case 20: case 21: case 23:
        {
            uint32_t m = mask(c, (w >> 1) & 31);
            uint32_t rot = op == 23 ? rotl(R[d], R[b]) : (b ? rotl(R[d], b) : R[d]);
            R[a] = op == 20 ? ((rot & m) | (R[a] & ~m)) : (rot & m);
            if (w & 1) cr0(ctx, R[a]);
            break;
        }
        case 11:
            if ((w >> 21) & 1) unsupported(pc, w, "64-bit compare");
            cmp_s(ctx, (w >> 23) & 7, (int32_t)R[a], sext16(w));
            break;
        case 10:
            if ((w >> 21) & 1) unsupported(pc, w, "64-bit compare");
            cmp_u(ctx, (w >> 23) & 7, R[a], w & 0xFFFF);
            break;
        case 3:
        {
            int32_t s = sext16(w);
            bool t = d == 31 || ((d & 16) && (int32_t)R[a] < s) || ((d & 8) && (int32_t)R[a] > s) || ((d & 4) && R[a] == (uint32_t)s) ||
                     ((d & 2) && R[a] < (uint32_t)s) || ((d & 1) && R[a] > (uint32_t)s);
            if (t) trap(ctx, base, pc);
            break;
        }
        case 32: R[d] = ld32(base, ra0 + (uint32_t)sext16(w)); break;
        case 33: { uint32_t ea = R[a] + (uint32_t)sext16(w); R[d] = ld32(base, ea); R[a] = ea; break; }
        case 34: R[d] = ld8(base, ra0 + (uint32_t)sext16(w)); break;
        case 35: { uint32_t ea = R[a] + (uint32_t)sext16(w); R[d] = ld8(base, ea); R[a] = ea; break; }
        case 40: R[d] = ld16(base, ra0 + (uint32_t)sext16(w)); break;
        case 41: { uint32_t ea = R[a] + (uint32_t)sext16(w); R[d] = ld16(base, ea); R[a] = ea; break; }
        case 42: R[d] = (uint32_t)(int32_t)(int16_t)ld16(base, ra0 + (uint32_t)sext16(w)); break;
        case 43: { uint32_t ea = R[a] + (uint32_t)sext16(w); R[d] = (uint32_t)(int32_t)(int16_t)ld16(base, ea); R[a] = ea; break; }
        case 36: st32(base, ra0 + (uint32_t)sext16(w), R[d]); break;
        case 37: { uint32_t ea = R[a] + (uint32_t)sext16(w); st32(base, ea, R[d]); R[a] = ea; break; }
        case 38: st8(base, ra0 + (uint32_t)sext16(w), (uint8_t)R[d]); break;
        case 39: { uint32_t ea = R[a] + (uint32_t)sext16(w); st8(base, ea, (uint8_t)R[d]); R[a] = ea; break; }
        case 44: st16(base, ra0 + (uint32_t)sext16(w), (uint16_t)R[d]); break;
        case 45: { uint32_t ea = R[a] + (uint32_t)sext16(w); st16(base, ea, (uint16_t)R[d]); R[a] = ea; break; }
        case 46:
        {
            uint32_t ea = ra0 + (uint32_t)sext16(w);
            for (uint32_t k = 0, r = d; r < 32; r++, k++) R[r] = ld32(base, ea + 4 * k);
            break;
        }
        case 47:
        {
            uint32_t ea = ra0 + (uint32_t)sext16(w);
            for (uint32_t k = 0, r = d; r < 32; r++, k++) st32(base, ea + 4 * k, R[r]);
            break;
        }
        case 48: lfs(ctx, d, ld32(base, ra0 + (uint32_t)sext16(w))); break;
        case 49: { uint32_t ea = R[a] + (uint32_t)sext16(w); lfs(ctx, d, ld32(base, ea)); R[a] = ea; break; }
        case 50: Fr[d].ps0u = ld64(base, ra0 + (uint32_t)sext16(w)); break;
        case 51: { uint32_t ea = R[a] + (uint32_t)sext16(w); Fr[d].ps0u = ld64(base, ea); R[a] = ea; break; }
        case 52: st32(base, ra0 + (uint32_t)sext16(w), stfs_bits(ctx, d)); break;
        case 53: { uint32_t ea = R[a] + (uint32_t)sext16(w); st32(base, ea, stfs_bits(ctx, d)); R[a] = ea; break; }
        case 54: st64(base, ra0 + (uint32_t)sext16(w), Fr[d].ps0u); break;
        case 55: { uint32_t ea = R[a] + (uint32_t)sext16(w); st64(base, ea, Fr[d].ps0u); R[a] = ea; break; }
        case 56: case 57: case 60: case 61:
        {
            uint32_t pw = (w >> 15) & 1, pi = (w >> 12) & 7;
            uint32_t ea = (op == 57 || op == 61 ? R[a] : ra0) + (uint32_t)sext12(w);
            if (op < 60) psq_load(ctx, base, d, ea, pw, pi);
            else psq_store(ctx, base, d, ea, pw, pi);
            if (op == 57 || op == 61) R[a] = ea;
            break;
        }
        case 31:
        {
            uint32_t xo = (w >> 1) & 0x3FF;
            bool rc = w & 1;
            uint32_t eax = ra0 + R[b];
            switch (xo)
            {
            case 266: case 266 | 512: R[d] = R[a] + R[b]; if (rc) cr0(ctx, R[d]); break;
            case 40: case 40 | 512: R[d] = R[b] - R[a]; if (rc) cr0(ctx, R[d]); break;
            case 104: case 104 | 512: R[d] = 0u - R[a]; if (rc) cr0(ctx, R[d]); break;
            case 235: case 235 | 512: R[d] = (uint32_t)((int64_t)(int32_t)R[a] * (int32_t)R[b]); if (rc) cr0(ctx, R[d]); break;
            case 75: R[d] = (uint32_t)(((int64_t)(int32_t)R[a] * (int32_t)R[b]) >> 32); if (rc) cr0(ctx, R[d]); break;
            case 11: R[d] = (uint32_t)(((uint64_t)R[a] * R[b]) >> 32); if (rc) cr0(ctx, R[d]); break;
            case 491: case 491 | 512: R[d] = (uint32_t)divw((int32_t)R[a], (int32_t)R[b]); if (rc) cr0(ctx, R[d]); break;
            case 459: case 459 | 512: R[d] = divwu(R[a], R[b]); if (rc) cr0(ctx, R[d]); break;
            case 10: case 10 | 512: case 138: case 138 | 512: case 202: case 202 | 512: case 234: case 234 | 512:
            case 8: case 8 | 512: case 136: case 136 | 512: case 200: case 200 | 512: case 232: case 232 | 512:
            {
                uint64_t t;
                switch (xo & 0x1FF)
                {
                case 10: t = (uint64_t)R[a] + R[b]; break;
                case 138: t = (uint64_t)R[a] + R[b] + ctx.xer_ca; break;
                case 202: t = (uint64_t)R[a] + ctx.xer_ca; break;
                case 234: t = (uint64_t)R[a] + ctx.xer_ca + 0xFFFFFFFFu; break;
                case 8: t = (uint64_t)(uint32_t)~R[a] + R[b] + 1; break;
                case 136: t = (uint64_t)(uint32_t)~R[a] + R[b] + ctx.xer_ca; break;
                case 200: t = (uint64_t)(uint32_t)~R[a] + ctx.xer_ca; break;
                default: t = (uint64_t)(uint32_t)~R[a] + ctx.xer_ca + 0xFFFFFFFFu; break;
                }
                ctx.xer_ca = (uint8_t)(t >> 32);
                R[d] = (uint32_t)t;
                if (rc) cr0(ctx, R[d]);
                break;
            }
            case 28: R[a] = R[d] & R[b]; if (rc) cr0(ctx, R[a]); break;
            case 60: R[a] = R[d] & ~R[b]; if (rc) cr0(ctx, R[a]); break;
            case 444: R[a] = R[d] | R[b]; if (rc) cr0(ctx, R[a]); break;
            case 412: R[a] = R[d] | ~R[b]; if (rc) cr0(ctx, R[a]); break;
            case 316: R[a] = R[d] ^ R[b]; if (rc) cr0(ctx, R[a]); break;
            case 124: R[a] = ~(R[d] | R[b]); if (rc) cr0(ctx, R[a]); break;
            case 476: R[a] = ~(R[d] & R[b]); if (rc) cr0(ctx, R[a]); break;
            case 284: R[a] = ~(R[d] ^ R[b]); if (rc) cr0(ctx, R[a]); break;
            case 24: R[a] = (R[b] & 0x20) ? 0u : (R[d] << (R[b] & 31)); if (rc) cr0(ctx, R[a]); break;
            case 536: R[a] = (R[b] & 0x20) ? 0u : (R[d] >> (R[b] & 31)); if (rc) cr0(ctx, R[a]); break;
            case 954: R[a] = (uint32_t)(int32_t)(int8_t)R[d]; if (rc) cr0(ctx, R[a]); break;
            case 922: R[a] = (uint32_t)(int32_t)(int16_t)R[d]; if (rc) cr0(ctx, R[a]); break;
            case 26: R[a] = R[d] ? (uint32_t)__builtin_clz(R[d]) : 32u; if (rc) cr0(ctx, R[a]); break;
            case 792: R[a] = sraw(ctx, R[d], R[b] & 0x3F); if (rc) cr0(ctx, R[a]); break;
            case 824:
                if (b == 0) { R[a] = R[d]; ctx.xer_ca = 0; }
                else
                {
                    ctx.xer_ca = (int32_t)R[d] < 0 && (R[d] & ((1u << b) - 1)) != 0;
                    R[a] = (uint32_t)((int32_t)R[d] >> b);
                }
                if (rc) cr0(ctx, R[a]);
                break;
            case 0:
                if ((w >> 21) & 1) unsupported(pc, w, "64-bit compare");
                cmp_s(ctx, (w >> 23) & 7, (int32_t)R[a], (int32_t)R[b]);
                break;
            case 32:
                if ((w >> 21) & 1) unsupported(pc, w, "64-bit compare");
                cmp_u(ctx, (w >> 23) & 7, R[a], R[b]);
                break;
            case 19: R[d] = get_cr(ctx); break;
            case 144:
            {
                uint32_t crm = (w >> 12) & 0xFF;
                for (int fld = 0; fld < 8; fld++)
                    if (crm & (0x80 >> fld)) set_cr_field(ctx, fld, (R[d] >> (28 - 4 * fld)) & 0xF);
                break;
            }
            case 339: case 467:
            {
                uint32_t spr = a | (b << 5);
                uint32_t* reg = nullptr;
                if (spr == 8) reg = &ctx.lr;
                else if (spr == 9) reg = &ctx.ctr;
                else if (spr >= 912 && spr <= 919) reg = &ctx.gqr[spr - 912];
                else if (spr >= 896 && spr <= 903) reg = &ctx.gqr[spr - 896];
                else if (spr != 1) unsupported(pc, w, "spr");
                if (xo == 339) R[d] = reg ? *reg : get_xer(ctx);
                else if (reg) *reg = R[d];
                else set_xer(ctx, R[d]);
                break;
            }
            case 23: R[d] = ld32(base, eax); break;
            case 55: { uint32_t ea = R[a] + R[b]; R[d] = ld32(base, ea); R[a] = ea; break; }
            case 87: R[d] = ld8(base, eax); break;
            case 119: { uint32_t ea = R[a] + R[b]; R[d] = ld8(base, ea); R[a] = ea; break; }
            case 279: R[d] = ld16(base, eax); break;
            case 311: { uint32_t ea = R[a] + R[b]; R[d] = ld16(base, ea); R[a] = ea; break; }
            case 343: R[d] = (uint32_t)(int32_t)(int16_t)ld16(base, eax); break;
            case 375: { uint32_t ea = R[a] + R[b]; R[d] = (uint32_t)(int32_t)(int16_t)ld16(base, ea); R[a] = ea; break; }
            case 151: st32(base, eax, R[d]); break;
            case 183: { uint32_t ea = R[a] + R[b]; st32(base, ea, R[d]); R[a] = ea; break; }
            case 215: st8(base, eax, (uint8_t)R[d]); break;
            case 247: { uint32_t ea = R[a] + R[b]; st8(base, ea, (uint8_t)R[d]); R[a] = ea; break; }
            case 407: st16(base, eax, (uint16_t)R[d]); break;
            case 439: { uint32_t ea = R[a] + R[b]; st16(base, ea, (uint16_t)R[d]); R[a] = ea; break; }
            case 535: lfs(ctx, d, ld32(base, eax)); break;
            case 567: { uint32_t ea = R[a] + R[b]; lfs(ctx, d, ld32(base, ea)); R[a] = ea; break; }
            case 599: Fr[d].ps0u = ld64(base, eax); break;
            case 631: { uint32_t ea = R[a] + R[b]; Fr[d].ps0u = ld64(base, ea); R[a] = ea; break; }
            case 663: st32(base, eax, stfs_bits(ctx, d)); break;
            case 695: { uint32_t ea = R[a] + R[b]; st32(base, ea, stfs_bits(ctx, d)); R[a] = ea; break; }
            case 727: st64(base, eax, Fr[d].ps0u); break;
            case 759: { uint32_t ea = R[a] + R[b]; st64(base, ea, Fr[d].ps0u); R[a] = ea; break; }
            case 983: st32(base, eax, (uint32_t)Fr[d].ps0u); break;
            case 534: R[d] = __builtin_bswap32(ld32(base, eax)); break;
            case 662: st32(base, eax, __builtin_bswap32(R[d])); break;
            case 790: R[d] = __builtin_bswap16(ld16(base, eax)); break;
            case 918: st16(base, eax, __builtin_bswap16((uint16_t)R[d])); break;
            case 20: R[d] = lwarx(ctx, base, eax); break;
            case 150: stwcx(ctx, base, eax, R[d]); break;
            case 597: case 725:
            {
                uint32_t ea = a ? R[a] : 0, n = b ? b : 32, reg = d - 1, k = 0;
                for (uint32_t byte = 0; byte < n; byte++)
                {
                    if (k == 0)
                    {
                        reg = (reg + 1) % 32;
                        if (xo == 597) R[reg] = 0;
                    }
                    uint32_t shift = 24 - k;
                    if (xo == 597) R[reg] |= (uint32_t)ld8(base, ea + byte) << shift;
                    else st8(base, ea + byte, (uint8_t)(R[reg] >> shift));
                    k = (k + 8) % 32;
                }
                break;
            }
            case 54: case 86: case 278: case 246: case 470: case 982: break;
            case 1014: std::memset(base + (eax & ~31u), 0, 32); break;
            case 598: std::atomic_thread_fence(std::memory_order_seq_cst); break;
            case 854: std::atomic_signal_fence(std::memory_order_seq_cst); break;
            case 4:
            {
                int32_t sb = (int32_t)R[b];
                bool t = d == 31 || ((d & 16) && (int32_t)R[a] < sb) || ((d & 8) && (int32_t)R[a] > sb) || ((d & 4) && R[a] == R[b]) ||
                         ((d & 2) && R[a] < R[b]) || ((d & 1) && R[a] > R[b]);
                if (t) trap(ctx, base, pc);
                break;
            }
            default: unsupported(pc, w, "op31");
            }
            break;
        }
        case 59:
        {
            if (w & 1) unsupported(pc, w, "Rc=1 float");
            uint32_t xo = (w >> 1) & 0x1F;
            double fa = Fr[a].ps0, fb = Fr[b].ps0, c25 = fp::round25(Fr[c].ps0);
            switch (xo)
            {
            case 21: set_single(ctx, d, fa + fb); break;
            case 20: set_single(ctx, d, fa - fb); break;
            case 25: set_single(ctx, d, fa * c25); break;
            case 18: set_single(ctx, d, fa / fb); break;
            case 29: set_single(ctx, d, __builtin_fma(fa, c25, fb)); break;
            case 28: set_single(ctx, d, __builtin_fma(fa, c25, -fb)); break;
            case 31: set_single(ctx, d, -__builtin_fma(fa, c25, fb)); break;
            case 30: set_single(ctx, d, -__builtin_fma(fa, c25, -fb)); break;
            case 24: { double t = fp::fres(fb); Fr[d].ps0 = t; Fr[d].ps1 = t; break; }
            case 22: set_single(ctx, d, std::sqrt(fb)); break;
            default: unsupported(pc, w, "op59");
            }
            break;
        }
        case 63:
        {
            uint32_t xox = (w >> 1) & 0x3FF, xoa = (w >> 1) & 0x1F;
            switch (xox)
            {
            case 0: case 32: cmp_f(ctx, (w >> 23) & 7, Fr[a].ps0, Fr[b].ps0); pc = next; continue;
            case 72: Fr[d].ps0u = Fr[b].ps0u; pc = next; continue;
            case 40: Fr[d].ps0u = Fr[b].ps0u ^ 0x8000000000000000ull; pc = next; continue;
            case 264: Fr[d].ps0u = Fr[b].ps0u & 0x7FFFFFFFFFFFFFFFull; pc = next; continue;
            case 136: Fr[d].ps0u = Fr[b].ps0u | 0x8000000000000000ull; pc = next; continue;
            case 12: set_single(ctx, d, Fr[b].ps0); pc = next; continue;
            case 15: Fr[d].ps0u = fp::fctiwz(Fr[b].ps0); pc = next; continue;
            case 14:
            {
                double r = std::nearbyint(Fr[b].ps0);
                Fr[d].ps0u = fp::fctiwz(r);
                pc = next;
                continue;
            }
            case 583: Fr[d].ps0u = ctx.fpscr; pc = next; continue;
            case 711: ctx.fpscr = (uint32_t)Fr[b].ps0u; pc = next; continue;
            case 38: case 70: case 134: pc = next; continue;
            default: break;
            }
            if (w & 1) unsupported(pc, w, "Rc=1 float");
            double fa = Fr[a].ps0, fb = Fr[b].ps0, fc = Fr[c].ps0;
            switch (xoa)
            {
            case 21: Fr[d].ps0 = fa + fb; break;
            case 20: Fr[d].ps0 = fa - fb; break;
            case 25: Fr[d].ps0 = fa * fc; break;
            case 18: Fr[d].ps0 = fa / fb; break;
            case 29: Fr[d].ps0 = __builtin_fma(fa, fc, fb); break;
            case 28: Fr[d].ps0 = __builtin_fma(fa, fc, -fb); break;
            case 31: Fr[d].ps0 = -__builtin_fma(fa, fc, fb); break;
            case 30: Fr[d].ps0 = -__builtin_fma(fa, fc, -fb); break;
            case 26: Fr[d].ps0 = fp::frsqrte(fb); break;
            case 23: { FPR t = (fa >= -0.0) ? Fr[c] : Fr[b]; Fr[d] = t; break; }
            case 22: Fr[d].ps0 = std::sqrt(fb); break;
            default: unsupported(pc, w, "op63");
            }
            break;
        }
        case 4:
        {
            uint32_t xox = (w >> 1) & 0x3FF, xoq = (w >> 1) & 0x3F, xoa = (w >> 1) & 0x1F;
            auto fl = [](double v) { return (double)(float)v; };
            auto pair = [&](double t0, double t1) { Fr[d].ps0 = t0; Fr[d].ps1 = t1; };
            const FPR A = Fr[a], B = Fr[b], C = Fr[c];
            bool done = true;
            switch (xox)
            {
            case 0: case 32: cmp_f(ctx, (w >> 23) & 7, A.ps0, B.ps0); break;
            case 64: case 96: cmp_f(ctx, (w >> 23) & 7, A.ps1, B.ps1); break;
            case 40: Fr[d].ps0u = B.ps0u ^ 0x8000000000000000ull; Fr[d].ps1u = B.ps1u ^ 0x8000000000000000ull; break;
            case 136: Fr[d].ps0u = B.ps0u | 0x8000000000000000ull; Fr[d].ps1u = B.ps1u | 0x8000000000000000ull; break;
            case 264: Fr[d].ps0u = B.ps0u & 0x7FFFFFFFFFFFFFFFull; Fr[d].ps1u = B.ps1u & 0x7FFFFFFFFFFFFFFFull; break;
            case 72: Fr[d] = B; break;
            case 528: pair(A.ps0, B.ps0); break;
            case 560: pair(A.ps0, B.ps1); break;
            case 592: pair(A.ps1, B.ps0); break;
            case 624: pair(A.ps1, B.ps1); break;
            case 1014: std::memset(base + ((ra0 + R[b]) & ~31u), 0, 32); break;
            default: done = false;
            }
            if (done) break;
            if (xoq == 6 || xoq == 7 || xoq == 38 || xoq == 39)
            {
                uint32_t pw = (w >> 10) & 1, pi = (w >> 7) & 7;
                bool upd = xoq >= 38;
                uint32_t ea = (upd ? R[a] : ra0) + R[b];
                if (xoq == 6 || xoq == 38) psq_load(ctx, base, d, ea, pw, pi);
                else psq_store(ctx, base, d, ea, pw, pi);
                if (upd) R[a] = ea;
                break;
            }
            switch (xoa)
            {
            case 21: pair(fl(A.ps0 + B.ps0), fl(A.ps1 + B.ps1)); break;
            case 20: pair(fl(A.ps0 - B.ps0), fl(A.ps1 - B.ps1)); break;
            case 18: pair(fl(A.ps0 / B.ps0), fl(A.ps1 / B.ps1)); break;
            case 25: pair(fl(A.ps0 * fp::round25(C.ps0)), fl(A.ps1 * fp::round25(C.ps1))); break;
            case 29: pair(fl(__builtin_fma(A.ps0, fp::round25(C.ps0), B.ps0)), fl(__builtin_fma(A.ps1, fp::round25(C.ps1), B.ps1))); break;
            case 28: pair(fl(__builtin_fma(A.ps0, fp::round25(C.ps0), -B.ps0)), fl(__builtin_fma(A.ps1, fp::round25(C.ps1), -B.ps1))); break;
            case 31: pair(fl(-__builtin_fma(A.ps0, fp::round25(C.ps0), B.ps0)), fl(-__builtin_fma(A.ps1, fp::round25(C.ps1), B.ps1))); break;
            case 30: pair(fl(-__builtin_fma(A.ps0, fp::round25(C.ps0), -B.ps0)), fl(-__builtin_fma(A.ps1, fp::round25(C.ps1), -B.ps1))); break;
            case 14: case 15:
            {
                double cs = fp::round25(xoa == 14 ? C.ps0 : C.ps1);
                pair(fl(__builtin_fma(A.ps0, cs, B.ps0)), fl(__builtin_fma(A.ps1, cs, B.ps1)));
                break;
            }
            case 12: case 13:
            {
                double cs = fp::round25(xoa == 12 ? C.ps0 : C.ps1);
                pair(fl(A.ps0 * cs), fl(A.ps1 * cs));
                break;
            }
            case 10: pair(fl(A.ps0 + B.ps1), fl(C.ps1)); break;
            case 11: pair(fl(C.ps0), fl(A.ps0 + B.ps1)); break;
            case 23: pair((A.ps0 >= -0.0) ? C.ps0 : B.ps0, (A.ps1 >= -0.0) ? C.ps1 : B.ps1); break;
            case 24: pair(fl(fp::fres(B.ps0)), fl(fp::fres(B.ps1))); break;
            case 26: pair(fl(fp::frsqrte(B.ps0)), fl(fp::frsqrte(B.ps1))); break;
            default: unsupported(pc, w, "op4");
            }
            break;
        }
        case 17:
            unsupported(pc, w, "sc");
        default:
            unsupported(pc, w, "primary");
        }
        pc = next;
    }
}

void call_any(PPCContext& ctx, uint8_t* base, uint32_t t, uint32_t from)
{
    if (kind_of(t))
    {
        run_at(ctx, base, t);
        return;
    }
    uint32_t off = t - kCodeBase;
    PPCFunc* fn = (off < kCodeEnd - kCodeBase && !(off & 3)) ? g_funcTable[off >> 2] : nullptr;
    if (!fn) bad_indirect_call(ctx, t, from);
    fn(ctx, base);
}

}

void setup(const std::vector<uint32_t>& patchedEntries, const std::vector<std::pair<uint32_t, uint32_t>>& ranges,
           const std::vector<std::pair<uint32_t, uint32_t>>& rel24)
{
    g_kind.assign((kCodeEnd - kCodeBase) / 4, 0);
    for (auto [lo, hi] : ranges)
        for (uint32_t a = lo; a < hi && a < kCodeEnd; a += 4)
            if (a >= kCodeBase) g_kind[(a - kCodeBase) >> 2] = 1;
    for (uint32_t e : patchedEntries)
        if (e >= kCodeBase && e < kCodeEnd) g_kind[(e - kCodeBase) >> 2] = 2;
    for (auto [site, target] : rel24) g_rel24[site] = target;
    __atomic_store_n(&g_active, !ranges.empty(), __ATOMIC_RELAXED);
    game_module::set_interp_active(!ranges.empty());
    log_info("interpreter: {} patched functions, {} interpreted code ranges", patchedEntries.size(), ranges.size());
}

bool interpreted(uint32_t addr) { return __atomic_load_n(&g_active, __ATOMIC_RELAXED) && kind_of(addr) != 0; }

bool entry(PPCContext& ctx, uint8_t* base, uint32_t addr)
{
    if (kind_of(addr) != 2) return false;
    run_at(ctx, base, addr);
    return true;
}

void call(PPCContext& ctx, uint8_t* base, uint32_t addr) { run_at(ctx, base, addr); }

}

namespace wf {
void indirect_miss(PPCContext& ctx, uint8_t* base, uint32_t target, uint32_t from)
{
    if (interp::interpreted(target))
    {
        interp::call(ctx, base, target);
        return;
    }
    bad_indirect_call(ctx, target, from);
}
}
