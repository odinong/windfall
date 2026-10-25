#include <map>
#include <mutex>
#include <unordered_map>

#include "core/log.h"
#include "debug/watchdog.h"
#include "hle/hle.h"

namespace wf::hle {
namespace {

constexpr uint32_t kExpMagic = 0x45585048;
constexpr uint32_t kFrmMagic = 0x46524D48;
constexpr uint32_t kHeaderSize = 0x80;
constexpr uint32_t OPT_CLEAR = 1, OPT_FILL = 2;

uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

struct ExpHeap
{
    uint32_t start, end, flags;
    std::map<uint32_t, uint32_t> free;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> used;
};
struct FrmHeap { uint32_t start, end, head, tail, flags; };

std::mutex g_lock;
std::unordered_map<uint32_t, ExpHeap> g_exp;
std::unordered_map<uint32_t, FrmHeap> g_frm;
uint32_t g_baseHeaps[9]{};
uint32_t g_defaultHeap = 0;

uint32_t exp_alloc(ExpHeap& h, uint32_t size, int32_t alignment)
{
    if (alignment == 0) return 0;
    size = align_up(size ? size : 1, 4);
    bool fromTail = alignment < 0;
    uint32_t align = (uint32_t)(fromTail ? -alignment : alignment);
    if (align < 4) align = 4;
    auto take = [&](std::map<uint32_t, uint32_t>::iterator it, uint32_t user) {
        uint32_t bStart = it->first, bEnd = it->first + it->second;
        h.free.erase(it);
        if (user > bStart) h.free[bStart] = user - bStart;
        if (user + size < bEnd) h.free[user + size] = bEnd - (user + size);
        h.used[user] = {user, size};
        if (h.flags & OPT_CLEAR) memset(gptr(user), 0, size);
        return user;
    };
    if (!fromTail)
    {
        for (auto it = h.free.begin(); it != h.free.end(); ++it)
        {
            uint32_t user = align_up(it->first, align);
            if ((uint64_t)user + size <= (uint64_t)it->first + it->second)
                return take(it, user);
        }
    }
    else
    {
        for (auto it = h.free.rbegin(); it != h.free.rend(); ++it)
        {
            uint64_t endAddr = (uint64_t)it->first + it->second;
            if (endAddr < size) continue;
            uint32_t user = (uint32_t)((endAddr - size) & ~(uint64_t)(align - 1));
            if (user >= it->first)
                return take(std::prev(it.base()), user);
        }
    }
    return 0;
}

void exp_free(ExpHeap& h, uint32_t p)
{
    auto u = h.used.find(p);
    if (u == h.used.end())
    {
        if (p) log_warn("MEMFreeToExpHeap: {:08X} was not allocated from this heap", p);
        return;
    }
    uint32_t start = u->second.first, size = u->second.second;
    h.used.erase(u);
    auto next = h.free.lower_bound(start);
    if (next != h.free.end() && start + size == next->first)
    {
        size += next->second;
        next = h.free.erase(next);
    }
    if (next != h.free.begin())
    {
        auto prev = std::prev(next);
        if (prev->first + prev->second == start)
        {
            prev->second += size;
            return;
        }
    }
    h.free[start] = size;
}

uint32_t create_exp(uint32_t start, uint32_t size, uint32_t flags)
{
    uint32_t handle = align_up(start, 4);
    if (size < kHeaderSize + 16) return 0;
    uint32_t end = start + size;
    memset(gptr(handle), 0, kHeaderSize);
    wr32(handle, kExpMagic);
    ExpHeap h{handle + kHeaderSize, end, flags};
    h.free[h.start] = end - h.start;
    g_exp[handle] = std::move(h);
    return handle;
}

uint32_t create_frm(uint32_t start, uint32_t size, uint32_t flags)
{
    uint32_t handle = align_up(start, 4);
    if (size < kHeaderSize) return 0;
    memset(gptr(handle), 0, kHeaderSize);
    wr32(handle, kFrmMagic);
    g_frm[handle] = FrmHeap{handle + kHeaderSize, start + size, handle + kHeaderSize, start + size, flags};
    return handle;
}

uint32_t MEMCreateExpHeapEx(uint32_t start, uint32_t size, uint32_t flags)
{
    std::lock_guard lk(g_lock);
    return create_exp(start, size, flags);
}
WF_EXPORT(coreinit, MEMCreateExpHeapEx);

uint32_t MEMDestroyExpHeap(uint32_t heap)
{
    std::lock_guard lk(g_lock);
    g_exp.erase(heap);
    return heap;
}
WF_EXPORT(coreinit, MEMDestroyExpHeap);

uint32_t MEMAllocFromExpHeapEx(uint32_t heap, uint32_t size, int32_t alignment)
{
    std::lock_guard lk(g_lock);
    auto it = g_exp.find(heap);
    if (it == g_exp.end()) { log_warn("MEMAllocFromExpHeapEx: unknown heap {:08X}", heap); return 0; }
    uint32_t p = exp_alloc(it->second, size, alignment);
    if (!p) log_cat("heap", "exp heap {:08X}: allocation of {:X} (align {}) failed", heap, size, alignment);
    return p;
}
WF_EXPORT(coreinit, MEMAllocFromExpHeapEx);

void MEMFreeToExpHeap(uint32_t heap, uint32_t p)
{
    std::lock_guard lk(g_lock);
    auto it = g_exp.find(heap);
    if (it != g_exp.end()) exp_free(it->second, p);
}
WF_EXPORT(coreinit, MEMFreeToExpHeap);

uint32_t MEMGetAllocatableSizeForExpHeapEx(uint32_t heap, int32_t alignment)
{
    std::lock_guard lk(g_lock);
    auto it = g_exp.find(heap);
    if (it == g_exp.end()) return 0;
    uint32_t align = (uint32_t)(alignment < 0 ? -alignment : alignment);
    if (align < 4) align = 4;
    uint32_t best = 0;
    for (auto& [a, s] : it->second.free)
    {
        uint32_t user = align_up(a, align);
        if (user < a + s) best = std::max(best, a + s - user);
    }
    return best & ~3u;
}
WF_EXPORT(coreinit, MEMGetAllocatableSizeForExpHeapEx);

uint32_t MEMGetTotalFreeSizeForExpHeap(uint32_t heap)
{
    std::lock_guard lk(g_lock);
    auto it = g_exp.find(heap);
    if (it == g_exp.end()) return 0;
    uint32_t total = 0;
    for (auto& [a, s] : it->second.free) total += s;
    return total;
}
WF_EXPORT(coreinit, MEMGetTotalFreeSizeForExpHeap);

uint32_t MEMCreateFrmHeapEx(uint32_t start, uint32_t size, uint32_t flags)
{
    std::lock_guard lk(g_lock);
    return create_frm(start, size, flags);
}
WF_EXPORT(coreinit, MEMCreateFrmHeapEx);

uint32_t MEMDestroyFrmHeap(uint32_t heap)
{
    std::lock_guard lk(g_lock);
    g_frm.erase(heap);
    return heap;
}
WF_EXPORT(coreinit, MEMDestroyFrmHeap);

uint32_t MEMAllocFromFrmHeapEx(uint32_t heap, uint32_t size, int32_t alignment)
{
    std::lock_guard lk(g_lock);
    auto it = g_frm.find(heap);
    if (it == g_frm.end() || alignment == 0) return 0;
    FrmHeap& h = it->second;
    size = align_up(size ? size : 1, 4);
    uint32_t p;
    if (alignment > 0)
    {
        p = align_up(h.head, (uint32_t)alignment);
        if ((uint64_t)p + size > h.tail) return 0;
        h.head = p + size;
    }
    else
    {
        if (h.tail - h.head < size) return 0;
        p = (h.tail - size) & ~((uint32_t)-alignment - 1);
        if (p < h.head) return 0;
        h.tail = p;
    }
    if (h.flags & OPT_CLEAR) memset(gptr(p), 0, size);
    return p;
}
WF_EXPORT(coreinit, MEMAllocFromFrmHeapEx);

uint32_t MEMGetAllocatableSizeForFrmHeapEx(uint32_t heap, int32_t alignment)
{
    std::lock_guard lk(g_lock);
    auto it = g_frm.find(heap);
    if (it == g_frm.end()) return 0;
    FrmHeap& h = it->second;
    uint32_t a = align_up(h.head, (uint32_t)(alignment < 0 ? -alignment : (alignment ? alignment : 4)));
    return a < h.tail ? (h.tail - a) & ~3u : 0;
}
WF_EXPORT(coreinit, MEMGetAllocatableSizeForFrmHeapEx);

uint32_t MEMGetBaseHeapHandle(uint32_t arena) { return arena < 9 ? g_baseHeaps[arena] : 0; }
WF_EXPORT(coreinit, MEMGetBaseHeapHandle);

uint32_t default_alloc(uint32_t size) { return MEMAllocFromExpHeapEx(g_defaultHeap, size, 0x40); }
uint32_t default_alloc_ex(uint32_t size, int32_t align) { return MEMAllocFromExpHeapEx(g_defaultHeap, size, align); }
void default_free(uint32_t p) { MEMFreeToExpHeap(g_defaultHeap, p); }

void init_default_alloc(uint32_t slot) { wr32(slot, make_thunk(wrap<&default_alloc>())); }
void init_default_alloc_ex(uint32_t slot) { wr32(slot, make_thunk(wrap<&default_alloc_ex>())); }
void init_default_free(uint32_t slot) { wr32(slot, make_thunk(wrap<&default_free>())); }
WF_DATA(coreinit, MEMAllocFromDefaultHeap, init_default_alloc);
WF_DATA(coreinit, MEMAllocFromDefaultHeapEx, init_default_alloc_ex);
WF_DATA(coreinit, MEMFreeToDefaultHeap, init_default_free);

uint32_t OSBlockMove(PPCContext& ctx, uint32_t dst, uint32_t src, uint32_t size, uint32_t)
{
    debug::watch_bulk_write(ctx, dst, size, "OSBlockMove");
    memmove(gptr(dst), gptr(src), size);
    return dst;
}
WF_EXPORT(coreinit, OSBlockMove);
uint32_t OSBlockSet(PPCContext& ctx, uint32_t dst, uint32_t v, uint32_t size)
{
    debug::watch_bulk_write(ctx, dst, size, "OSBlockSet");
    memset(gptr(dst), (int)v, size);
    return dst;
}
WF_EXPORT(coreinit, OSBlockSet);
uint32_t memcpy_(PPCContext& ctx, uint32_t dst, uint32_t src, uint32_t size)
{
    debug::watch_bulk_write(ctx, dst, size, "memcpy");
    memcpy(gptr(dst), gptr(src), size);
    return dst;
}
WF_EXPORT_AS(coreinit, memcpy, memcpy_);
uint32_t memmove_(PPCContext& ctx, uint32_t dst, uint32_t src, uint32_t size)
{
    debug::watch_bulk_write(ctx, dst, size, "memmove");
    memmove(gptr(dst), gptr(src), size);
    return dst;
}
WF_EXPORT_AS(coreinit, memmove, memmove_);
uint32_t memset_(uint32_t dst, uint32_t v, uint32_t size) { memset(gptr(dst), (int)v, size); return dst; }
WF_EXPORT_AS(coreinit, memset, memset_);

void DCFlushRange(uint32_t, uint32_t) {}
WF_EXPORT(coreinit, DCFlushRange);
void DCFlushRangeNoSync(uint32_t, uint32_t) {}
WF_EXPORT(coreinit, DCFlushRangeNoSync);
void DCInvalidateRange(uint32_t, uint32_t) {}
WF_EXPORT(coreinit, DCInvalidateRange);
void DCStoreRange(uint32_t, uint32_t) {}
WF_EXPORT(coreinit, DCStoreRange);
void DCStoreRangeNoSync(uint32_t, uint32_t) {}
WF_EXPORT(coreinit, DCStoreRangeNoSync);
void DCZeroRange(uint32_t a, uint32_t size) { memset(gptr(a & ~31u), 0, align_up(size + (a & 31), 32)); }
WF_EXPORT(coreinit, DCZeroRange);
bool OSIsAddressRangeDCValid(uint32_t a, uint32_t size) { return is_mapped(a, size); }
WF_EXPORT(coreinit, OSIsAddressRangeDCValid);
void OSMemoryBarrier() { std::atomic_thread_fence(std::memory_order_seq_cst); }
WF_EXPORT(coreinit, OSMemoryBarrier);

}

uint32_t default_heap_alloc(uint32_t size, int32_t align) { return MEMAllocFromExpHeapEx(g_defaultHeap, size, align); }
void default_heap_free(uint32_t p) { MEMFreeToExpHeap(g_defaultHeap, p); }

void heaps_init(uint32_t mem2Start, uint32_t mem2End)
{
    std::lock_guard lk(g_lock);
    g_baseHeaps[0] = create_frm(gmem::kMem1Base, gmem::kMem1Size, 0);
    g_baseHeaps[8] = create_frm(gmem::kFgBucketBase, 0x2800000, 0);
    g_defaultHeap = g_baseHeaps[1] = create_exp(mem2Start, mem2End - mem2Start, 0);
}

}
