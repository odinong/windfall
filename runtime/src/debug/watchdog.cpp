#include "debug/watchdog.h"

#include <cstdlib>
#include <functional>
#include <thread>
#include <chrono>
#include <vector>
#include <algorithm>
#include <mutex>
#include <map>

#include <windows.h>

#include "core/guest.h"
#include "core/log.h"
#include "gpu/gpu.h"
#include "os/sched.h"

namespace wf::debug {

bool g_traceImports = false;

namespace {

const char* state_name(os::ThreadState s)
{
    switch (s)
    {
    case os::ThreadState::None: return "suspended";
    case os::ThreadState::Ready: return "ready";
    case os::ThreadState::Running: return "running";
    case os::ThreadState::Waiting: return "waiting";
    case os::ThreadState::Moribund: return "exited";
    }
    return "?";
}

os::Thread* thread_for_current_fiber()
{
    void* fiber = GetCurrentFiber();
    for (os::Thread* t : os::all_threads())
        if (t->fiber == fiber) return t;
    return nullptr;
}

uint32_t g_watchAddr = 0;
uint32_t g_watchLast = 0;
void* g_watchPage = nullptr;
thread_local bool t_rearm = false;

void arm_watch()
{
    DWORD old;
    VirtualProtect(g_watchPage, 1, PAGE_READWRITE | PAGE_GUARD, &old);
}

}

void watch_bulk_write(const PPCContext& ctx, uint32_t dst, uint32_t size, const char* what)
{
    static const bool stats = std::getenv("WF_MEMCPY_STATS") != nullptr;
    if (stats)
    {
        static std::mutex lock;
        static std::map<std::pair<uint32_t, uint32_t>, uint32_t> counts;
        static uint64_t last = 0;
        std::lock_guard lk(lock);
        counts[{ctx.lr, size}]++;
        uint64_t now = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
        if (!last) last = now;
        if (now - last > 5000000000ull)
        {
            std::vector<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> v;
            for (auto& [k, c] : counts) v.push_back({c, k});
            std::sort(v.rbegin(), v.rend());
            std::string out = "bulk copies in the last 5 s (calls, lr, size):";
            for (size_t i = 0; i < std::min<size_t>(25, v.size()); i++)
                out += std::format("\n  {:7} {:08X} {:X}", v[i].first, v[i].second.first, v[i].second.second);
            log_info("{}", out);
            counts.clear();
            last = now;
        }
    }
    if (g_watchAddr && g_watchAddr >= dst && g_watchAddr < dst + size)
        log_info("watch: {} {:08X}+{:X} covers {:08X}, from lr={:08X}\n{}", what, dst, size, g_watchAddr, ctx.lr, backtrace(ctx));
}

namespace {

LONG CALLBACK watch_handler(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == STATUS_GUARD_PAGE_VIOLATION && g_watchPage)
    {
        uintptr_t a = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        bool write = ep->ExceptionRecord->ExceptionInformation[0] == 1;
        if (write && a >= (uintptr_t)g_base + g_watchAddr && a < (uintptr_t)g_base + g_watchAddr + 4)
        {
            os::Thread* t = thread_for_current_fiber();
            log_info("watch: write to {:08X} (was {:08X}) by \"{}\"\n{}", g_watchAddr, rd32(g_watchAddr), t ? t->name : "?",
                     t ? backtrace(t->ctx) : "");
        }
        ep->ContextRecord->EFlags |= 0x100;
        t_rearm = true;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && t_rearm)
    {
        t_rearm = false;
        if (rd32(g_watchAddr) != g_watchLast)
        {
            log_info("watch: {:08X} = {:08X}", g_watchAddr, rd32(g_watchAddr));
            g_watchLast = rd32(g_watchAddr);
        }
        arm_watch();
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void start_watch(const char* spec)
{
    std::string s = spec;
    bool indirect = s[0] == '*';
    if (indirect) s.erase(0, 1);
    size_t plus = s.find('+');
    uint32_t ptr = (uint32_t)std::stoul(s.substr(0, plus), nullptr, 16);
    uint32_t off = plus == std::string::npos ? 0 : (uint32_t)std::stoul(s.substr(plus + 1), nullptr, 16);
    AddVectoredExceptionHandler(1, watch_handler);
    std::thread([=] {
        uint32_t target = ptr + off;
        if (indirect)
        {
            while (!rd32(ptr)) std::this_thread::sleep_for(std::chrono::microseconds(50));
            target = rd32(ptr) + off;
        }
        g_watchAddr = target;
        g_watchLast = rd32(target);
        g_watchPage = gptr(target & ~0xFFFu);
        log_info("watch: armed on {:08X} (currently {:08X})", target, g_watchLast);
        arm_watch();
    }).detach();
}

LONG CALLBACK crash_handler(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;
    std::string msg = std::format("host exception {:08X} at {}", code, ep->ExceptionRecord->ExceptionAddress);
    if (code == EXCEPTION_ACCESS_VIOLATION)
    {
        uintptr_t a = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        bool write = ep->ExceptionRecord->ExceptionInformation[0] == 1;
        if (a >= (uintptr_t)g_base && a < (uintptr_t)g_base + 0x100000000ull)
            msg += std::format(", guest {} of unmapped address {:08X}", write ? "write" : "read", (uint32_t)(a - (uintptr_t)g_base));
        else
            msg += std::format(", {} of host address {:X}", write ? "write" : "read", a);
    }
    {
        void* frames[24];
        USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
        HMODULE exe = GetModuleHandleW(nullptr);
        msg += "\nhost stack (exe+offset):";
        for (USHORT i = 0; i < n; i++)
            msg += std::format(" {:X}", (uintptr_t)frames[i] - (uintptr_t)exe);
        msg += std::format("\nfault rip exe+{:X}", (uintptr_t)ep->ContextRecord->Rip - (uintptr_t)exe);
    }
    if (os::Thread* t = thread_for_current_fiber())
        msg += std::format("\nguest thread \"{}\" ({:08X}) on core {}\n{}", t->name, t->guest, t->core, backtrace(t->ctx));
    log_error("{}", msg);
    log_info("{}", dump_threads());
    fatal_message(msg);
}

}

std::string backtrace(const PPCContext& ctx)
{
    std::string s = std::format("  lr={:08X} ctr={:08X} r1={:08X} r3={:08X} r4={:08X} r5={:08X} r31={:08X}\n  frames:", ctx.lr,
                                ctx.ctr, ctx.r[1], ctx.r[3], ctx.r[4], ctx.r[5], ctx.r[31]);
    uint32_t sp = ctx.r[1];
    for (int i = 0; i < 24 && sp && is_mapped(sp, 8); i++)
    {
        uint32_t back = rd32(sp);
        if (!back || !is_mapped(back, 8) || back <= sp) break;
        s += std::format(" {:08X}", rd32(back + 4));
        sp = back;
    }
    return s;
}

std::string dump_threads()
{
    std::string s = "guest threads:\n";
    for (os::Thread* t : os::all_threads())
        s += std::format("- \"{}\" {:08X} {} prio {} affinity {:X} core {}\n{}\n", t->name, t->guest, state_name(t->state),
                         t->priority, t->affinity, t->core, backtrace(t->ctx));
    return s;
}

void start_memdumps(const char* spec)
{
    struct Dump { uint32_t frame, addr, size; };
    std::vector<Dump> dumps;
    std::string s = spec;
    for (size_t p = 0; p < s.size();)
    {
        size_t c = s.find(',', p);
        std::string item = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
        unsigned f = 0, a = 0, n = 0;
        if (sscanf(item.c_str(), "%u:%x:%x", &f, &a, &n) == 3) dumps.push_back({f, a, n});
        if (c == std::string::npos) break;
        p = c + 1;
    }
    std::thread([dumps] {
        for (const Dump& d : dumps)
        {
            while (gpu::frame_count() < d.frame) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!is_mapped(d.addr, d.size)) continue;
            std::string name = std::format("memdump_{:08X}_{}.bin", d.addr, d.frame);
            if (FILE* f = fopen(name.c_str(), "wb"))
            {
                fwrite(gptr(d.addr), 1, d.size, f);
                fclose(f);
                log_info("memory dump: {} ({:X} bytes)", name, d.size);
            }
        }
    }).detach();
}

void start_pokes(const char* spec)
{
    struct Poke { uint32_t frame, addr; std::vector<uint8_t> bytes; };
    std::vector<Poke> pokes;
    std::string s = spec;
    for (size_t p = 0; p < s.size();)
    {
        size_t c = s.find(',', p);
        std::string item = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
        unsigned f = 0, a = 0;
        char hex[512] = {};
        if (sscanf(item.c_str(), "%u:%x:%511s", &f, &a, hex) == 3)
        {
            Poke pk{f, a, {}};
            for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) pk.bytes.push_back((uint8_t)strtoul(std::string(hex + i, 2).c_str(), nullptr, 16));
            pokes.push_back(std::move(pk));
        }
        if (c == std::string::npos) break;
        p = c + 1;
    }
    std::thread([pokes] {
        for (const Poke& pk : pokes)
        {
            while (gpu::frame_count() < pk.frame) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (!is_mapped(pk.addr, (uint32_t)pk.bytes.size())) continue;
            memcpy(gptr(pk.addr), pk.bytes.data(), pk.bytes.size());
            log_info("poke: {} bytes at {:08X} (frame {})", pk.bytes.size(), pk.addr, pk.frame);
        }
    }).detach();
}

void start_memscans(const char* spec)
{
    struct Scan { uint32_t frame; float x, y, z, tol; };
    std::vector<Scan> scans;
    std::string s = spec;
    for (size_t p = 0; p < s.size();)
    {
        size_t c = s.find(';', p);
        std::string item = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
        Scan sc{};
        if (sscanf(item.c_str(), "%u:%f,%f,%f:%f", &sc.frame, &sc.x, &sc.y, &sc.z, &sc.tol) == 5) scans.push_back(sc);
        if (c == std::string::npos) break;
        p = c + 1;
    }
    std::thread([scans] {
        for (const Scan& sc : scans)
        {
            while (gpu::frame_count() < sc.frame) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::string hits;
            int n = 0;
            for (uint32_t page = gmem::kMem2Base; page < gmem::kMem2End && n < 64; page += 0x10000)
            {
                if (!is_mapped(page, 0x10000)) continue;
                for (uint32_t a = page; a < page + 0x10000 - 12 && n < 64; a += 4)
                {
                    float x = std::bit_cast<float>(rd32(a));
                    if (!(std::abs(x - sc.x) <= sc.tol)) continue;
                    float y = std::bit_cast<float>(rd32(a + 4)), z = std::bit_cast<float>(rd32(a + 8));
                    if (!(std::abs(y - sc.y) <= sc.tol) || !(std::abs(z - sc.z) <= sc.tol)) continue;
                    hits += std::format(" {:08X}({:.1f},{:.1f},{:.1f})", a, x, y, z);
                    n++;
                }
            }
            log_info("memscan frame {}: {} hits:{}", sc.frame, n, hits);
        }
    }).detach();
}

void start_memfinds(const char* spec)
{
    struct Find { uint32_t frame; std::vector<uint8_t> bytes; };
    std::vector<Find> finds;
    std::string s = spec;
    for (size_t p = 0; p < s.size();)
    {
        size_t c = s.find(';', p);
        std::string item = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
        size_t colon = item.find(':');
        if (colon != std::string::npos)
        {
            Find f{(uint32_t)std::strtoul(item.c_str(), nullptr, 10), {}};
            std::string hex = item.substr(colon + 1);
            for (size_t i = 0; i + 1 < hex.size(); i += 2) f.bytes.push_back((uint8_t)std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
            if (!f.bytes.empty()) finds.push_back(std::move(f));
        }
        if (c == std::string::npos) break;
        p = c + 1;
    }
    std::thread([finds] {
        for (const Find& f : finds)
        {
            while (gpu::frame_count() < f.frame) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::string hits;
            int n = 0;
            for (uint32_t page = gmem::kMem2Base; page < gmem::kMem2End && n < 32; page += 0x10000)
            {
                if (!is_mapped(page, 0x10000)) continue;
                const uint8_t* b = gptr<uint8_t>(page);
                const uint8_t* end = b + 0x10000;
                std::boyer_moore_horspool_searcher searcher(f.bytes.begin(), f.bytes.end());
                for (const uint8_t* at = b; n < 32;)
                {
                    at = std::search(at, end, searcher);
                    if (at == end) break;
                    hits += std::format(" {:08X}", page + (uint32_t)(at - b));
                    n++;
                    at++;
                }
            }
            log_info("memfind frame {}: {} hits:{}", f.frame, n, hits);
        }
    }).detach();
}

void watchdog_start()
{
    AddVectoredExceptionHandler(0, crash_handler);
    if (const char* m = std::getenv("WF_MEMSCAN")) start_memscans(m);
    if (const char* m = std::getenv("WF_MEMDUMP")) start_memdumps(m);
    if (const char* m = std::getenv("WF_POKE")) start_pokes(m);
    if (const char* m = std::getenv("WF_MEMFIND")) start_memfinds(m);
    if (const char* tr = std::getenv("WF_TRACE_IMPORTS")) g_traceImports = *tr == '1';
    if (const char* w = std::getenv("WF_WATCH")) start_watch(w);
    const char* to = std::getenv("WF_TIMEOUT");
    if (!to) return;
    int secs = std::atoi(to);
    std::thread([secs] {
        std::this_thread::sleep_for(std::chrono::seconds(secs));
        log_info("watchdog: {} s elapsed\n{}\n{}", secs, gpu::status(), dump_threads());
        std::_Exit(2);
    }).detach();
}

}
