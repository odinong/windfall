#include "debug/profiler.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <dbghelp.h>

#include "core/guest.h"
#include "core/log.h"
#include "os/sched.h"
#include "wf/recomp_api.h"

namespace wf::debug {

namespace {

struct Range { uintptr_t start; uint32_t guest; };
std::vector<Range> g_ranges;
std::vector<uint32_t> g_guestStarts;

struct ExtraThread { std::string name; HANDLE handle; };
std::mutex g_extraLock;
std::vector<ExtraThread> g_extra;

void build_ranges()
{
    for (const FuncMapping* m = g_funcMappings; m->host; m++)
    {
        g_ranges.push_back({(uintptr_t)m->host, m->guest});
        g_guestStarts.push_back(m->guest);
    }
    std::sort(g_guestStarts.begin(), g_guestStarts.end());
    std::sort(g_ranges.begin(), g_ranges.end(), [](auto& a, auto& b) { return a.start < b.start; });
}

uint32_t guest_for(uintptr_t rip)
{
    auto it = std::upper_bound(g_ranges.begin(), g_ranges.end(), rip, [](uintptr_t v, const Range& r) { return v < r.start; });
    if (it == g_ranges.begin()) return 0;
    --it;
    if (rip - it->start > 0x100000) return 0;
    return it->guest;
}

std::once_flag g_rangesOnce;

}  // namespace

uint32_t guest_function_at(uintptr_t hostAddress)
{
    std::call_once(g_rangesOnce, build_ranges);
    return guest_for(hostAddress);
}

namespace {

uint32_t guest_func_of(uint32_t addr)
{
    auto it = std::upper_bound(g_guestStarts.begin(), g_guestStarts.end(), addr);
    return it == g_guestStarts.begin() ? 0 : *--it;
}

void stack_functions(const PPCContext& ctx, std::vector<uint32_t>& out)
{
    out.clear();
    uint32_t sp = ctx.r[1];
    for (int depth = 0; depth < 48 && sp >= 0x10000000 && sp < 0x50000000; depth++)
    {
        uint32_t next = rd32(sp);
        if (next <= sp || next >= 0x50000000) break;
        uint32_t lr = rd32(next + 4);
        if (uint32_t f = guest_func_of(lr - 4)) out.push_back(f);
        sp = next;
    }
}

std::pair<uintptr_t, std::string> host_symbol(uintptr_t rip)
{
    static std::map<uintptr_t, std::pair<uintptr_t, std::string>> cache;
    if (auto it = cache.find(rip); it != cache.end()) return it->second;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = (SYMBOL_INFO*)buf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    std::pair<uintptr_t, std::string> r{rip, std::format("{:X}", rip)};
    if (SymFromAddr(GetCurrentProcess(), rip, &disp, sym))
    {
        r = {(uintptr_t)(rip - disp), sym->Name};
        IMAGEHLP_LINE64 line{sizeof line};
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(GetCurrentProcess(), rip - disp, &lineDisp, &line) && line.FileName)
        {
            std::string file = line.FileName;
            file = file.substr(file.find_last_of("\\/") + 1);
            r.second = std::format("{} ({}:{})", r.second.substr(0, 60), file, line.LineNumber);
        }
    }
    return cache[rip] = r;
}

bool sample(HANDLE h, uintptr_t& rip)
{
    if (SuspendThread(h) == (DWORD)-1) return false;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL;
    BOOL ok = GetThreadContext(h, &ctx);
    ResumeThread(h);
    rip = ctx.Rip;
    return ok;
}

void sampler()
{
    SetThreadDescription(GetCurrentThread(), L"Windfall profiler");
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    std::map<uint32_t, uint32_t> hits;
    std::map<std::string, uint32_t> runtimeHits;
    std::map<std::string, uint32_t> coreHits[os::kNumCores];
    uint32_t coreBusy[os::kNumCores] = {}, coreTotal[os::kNumCores] = {};
    std::map<std::string, std::map<std::string, uint32_t>> extraHits;
    std::map<std::string, uint32_t> extraTotal;
    uint32_t idle = 0, runtime = 0, total = 0;
    std::map<uint32_t, uint32_t> mainInclusive;
    uint32_t mainSamples = 0;
    std::vector<uint32_t> stack;
    uint64_t windowStart = os::now_ns();
    for (;;)
    {
        Sleep(1);
        for (int c = 0; c < os::kNumCores; c++)
        {
            HANDLE h = (HANDLE)os::core_host_handle(c);
            bool busy = os::core_running(c) != nullptr;
            uintptr_t rip;
            if (!sample(h, rip)) continue;
            total++;
            coreTotal[c]++;
            if (!busy) { idle++; continue; }
            coreBusy[c]++;
            if (os::Thread* t = os::core_running(c); t && t->name == "Main thread")
            {
                mainSamples++;
                stack_functions(t->ctx, stack);
                if (uint32_t g = guest_for(rip)) stack.push_back(g);
                std::sort(stack.begin(), stack.end());
                stack.erase(std::unique(stack.begin(), stack.end()), stack.end());
                for (uint32_t f : stack) mainInclusive[f]++;
            }
            uint32_t g = guest_for(rip);
            if (g)
            {
                hits[g]++;
                coreHits[c][std::format("guest {:08X}", g)]++;
            }
            else
            {
                runtime++;
                std::string name = host_symbol(rip).second;
                runtimeHits[name]++;
                coreHits[c][name.substr(0, 90)]++;
            }
        }
        {
            std::lock_guard lk(g_extraLock);
            for (auto& t : g_extra)
            {
                uintptr_t rip;
                if (!sample(t.handle, rip)) continue;
                extraTotal[t.name]++;
                extraHits[t.name][host_symbol(rip).second]++;
            }
        }
        if (os::now_ns() - windowStart > 2000000000ull && total)
        {
            auto top_of = [](const auto& m, size_t n) {
                std::vector<std::pair<typename std::decay_t<decltype(m)>::key_type, uint32_t>> v(m.begin(), m.end());
                std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
                if (v.size() > n) v.resize(n);
                return v;
            };
            std::string s = std::format("CPU samples {} (3 cores): idle {:.0f}%, runtime/HLE {:.0f}%, guest {:.0f}%. Top guest functions:", total,
                                        100.0 * idle / total, 100.0 * runtime / total, 100.0 * (total - idle - runtime) / total);
            for (auto& [g, n] : top_of(hits, 12)) s += std::format("\n    {:08X} {:5.1f}%", g, 100.0 * n / total);
            s += "\n  Top runtime functions (on the cores):";
            for (auto& [name, n] : top_of(runtimeHits, 15)) s += std::format("\n    {:5.1f}% {}", 100.0 * n / total, name);
            if (mainSamples)
            {
                s += std::format("\n  Main thread, inclusive ({} samples):", mainSamples);
                for (auto& [g, n] : top_of(mainInclusive, 40)) s += std::format("\n    {:5.1f}% {:08X}", 100.0 * n / mainSamples, g);
                mainInclusive.clear();
                mainSamples = 0;
            }
            for (int c = 0; c < os::kNumCores; c++)
            {
                if (!coreTotal[c]) continue;
                uint32_t guestN = 0;
                for (auto& [name, n] : coreHits[c])
                    if (name.starts_with("guest ")) guestN += n;
                s += std::format("\n  core {}: busy {:.0f}% (guest {:.0f}%, runtime {:.0f}%)", c, 100.0 * coreBusy[c] / coreTotal[c],
                                 100.0 * guestN / coreTotal[c], 100.0 * (coreBusy[c] - guestN) / coreTotal[c]);
                for (auto& [name, n] : top_of(coreHits[c], 60)) s += std::format("\n    {:5.1f}% {}", 100.0 * n / coreTotal[c], name);
                coreHits[c].clear();
                coreBusy[c] = coreTotal[c] = 0;
            }
            for (auto& [thread, m] : extraHits)
            {
                s += std::format("\n  {} thread ({} samples):", thread, extraTotal[thread]);
                for (auto& [name, n] : top_of(m, 15)) s += std::format("\n    {:5.1f}% {}", 100.0 * n / extraTotal[thread], name);
            }
            log_cat("profile", "{}", s);
            hits.clear();
            runtimeHits.clear();
            extraHits.clear();
            extraTotal.clear();
            idle = runtime = total = 0;
            windowStart = os::now_ns();
        }
    }
}

}

void profiler_start()
{
    if (!std::getenv("WF_PROFILE")) return;
    std::call_once(g_rangesOnce, build_ranges);
    std::thread(sampler).detach();
}

}

void wf_profile_this_thread(const char* name)
{
    if (!std::getenv("WF_PROFILE")) return;
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    std::lock_guard lk(wf::debug::g_extraLock);
    wf::debug::g_extra.push_back({name, h});
}
