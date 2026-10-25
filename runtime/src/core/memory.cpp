#include <atomic>
#include <mutex>

#include <windows.h>

#include "core/guest.h"
#include "core/log.h"

uint8_t* memory_base = nullptr;

namespace wf {

uint8_t* g_base = nullptr;

namespace {

struct Region { uint32_t base, size; const char* name; };

constexpr Region kRegions[] = {
    {gmem::kCodeBase, gmem::kCodeSize, "code"},
    {gmem::kMem2Base, gmem::kMem2End - gmem::kMem2Base, "MEM2"},
    {gmem::kFgBucketBase, gmem::kFgBucketSize, "FG bucket"},
    {gmem::kMem1Base, gmem::kMem1Size, "MEM1"},
    {gmem::kPerCorePage, 0x1000, "per-core"},
};

std::mutex s_sysLock;
uint32_t s_sysNext = gmem::kSysAreaBase;

}

void memory_init()
{
    g_base = (uint8_t*)VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS);
    if (!g_base)
        fatal("cannot reserve 4 GiB of address space for guest memory");
    memory_base = g_base;
    for (const Region& r : kRegions)
    {
        if (!VirtualAlloc(g_base + r.base, r.size, MEM_COMMIT, PAGE_READWRITE))
            fatal("cannot commit guest region {} ({:08X}+{:X})", r.name, r.base, r.size);
    }
    log_info("guest memory at host {}", (void*)g_base);
}

bool is_mapped(uint32_t addr, uint32_t size)
{
    uint64_t end = (uint64_t)addr + size;
    for (const Region& r : kRegions)
        if (addr >= r.base && end <= (uint64_t)r.base + r.size)
            return true;
    return false;
}

uint32_t sys_alloc(uint32_t size, uint32_t align)
{
    std::lock_guard lk(s_sysLock);
    uint32_t a = (s_sysNext + align - 1) & ~(align - 1);
    if (a + size > gmem::kSysAreaEnd)
        fatal("system area exhausted ({} bytes requested)", size);
    s_sysNext = a + size;
    memset(g_base + a, 0, size);
    return a;
}

}
