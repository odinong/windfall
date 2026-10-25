#include <chrono>
#include "Common/precompiled.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"

#include <cstdio>

#include "core/log.h"

namespace {

FILE* g_dump = nullptr;
uint32 g_dumpFrame = 0xFFFFFFFF, g_dumpCount = 1;
uint32 g_drawIndex = 0;
bool g_done = false;
std::string g_dumpDir;

float be_float(const uint32* p) { return std::bit_cast<float>(_swapEndianU32(*p)); }

void dump_vs_uniforms(LatteDecompilerShader* vs)
{
    const uint32* regs = LatteGPUState.contextRegister;
    if (vs->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK)
    {
        for (const auto& buf : vs->list_quickBufferList)
        {
            uint32 base = mmSQ_VTX_UNIFORM_BLOCK_START + buf.index * 7;
            MPTR addr = regs[base];
            uint32 size = regs[base + 1] + 1;
            fmt::print(g_dump, " ub{}@{:08X}+{:X}[", buf.index, addr, size);
            const uint32* data = (const uint32*)memory_getPointerFromPhysicalOffset(addr);
            for (uint32 i = 0; i < std::min<uint32>(16, size / 4); i++)
                fmt::print(g_dump, "{}{:.4g}", i ? " " : "", be_float(data + i));
            fmt::print(g_dump, "]");
        }
    }
    else if (vs->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_REMAPPED)
    {
        fmt::print(g_dump, " remapped regs={} groups=", vs->list_remappedUniformEntries_register.size());
        for (const auto& g : vs->list_remappedUniformEntries_bufferGroups)
        {
            uint32 base = mmSQ_VTX_UNIFORM_BLOCK_START + g.bufferId * 7;
            fmt::print(g_dump, "b{}@{:08X}x{}(", g.bufferId, regs[base], g.entries.size());
            for (const auto& e : g.entries) fmt::print(g_dump, " r{}=+{:X}", e.mappedIndexOffset, e.indexOffset * 16);
            fmt::print(g_dump, ") ");
        }
    }
    else if (vs->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CFILE)
        fmt::print(g_dump, " cfile n={}", vs->uniform.count_uniformRegister);
    else
        fmt::print(g_dump, " no-uniforms");
}

void dump_vertex_buffers()
{
    LatteFetchShader* fs = LatteSHRC_GetActiveFetchShader();
    if (!fs) return;
    const uint32* regs = LatteGPUState.contextRegister + mmSQ_VTX_ATTRIBUTE_BLOCK_START;
    for (auto& g : fs->bufferGroups)
    {
        const uint32* b = regs + g.attributeBufferIndex * 7;
        MPTR addr = b[0];
        uint32 size = b[1] + 1;
        uint64 h = 1469598103934665603ull;
        if (addr)
        {
            const uint8* p = (const uint8*)memory_getPointerFromPhysicalOffset(addr);
            for (uint32 i = 0; i < std::min<uint32>(size, 0x40000); i++) h = (h ^ p[i]) * 1099511628211ull;
        }
        fmt::print(g_dump, " vb{}@{:08X}+{:X}#{:08x}", g.attributeBufferIndex, addr, size, (uint32)(h ^ (h >> 32)));
    }
}

void dump_uniform_hashes()
{
    const uint32* regs = LatteGPUState.contextRegister;
    for (int stage = 0; stage < 2; stage++)
    {
        uint32 start = stage == 0 ? mmSQ_VTX_UNIFORM_BLOCK_START : mmSQ_PS_UNIFORM_BLOCK_START;
        for (uint32 b = 0; b < 16; b++)
        {
            MPTR addr = regs[start + b * 7];
            uint32 size = regs[start + b * 7 + 1] + 1;
            if (!addr || size > 0x10000) continue;
            const uint8* p = (const uint8*)memory_getPointerFromPhysicalOffset(addr);
            uint64 h = 1469598103934665603ull;
            for (uint32 i = 0; i < size; i++) h = (h ^ p[i]) * 1099511628211ull;
            fmt::print(g_dump, " {}u{}@{:08X}+{:X}#{:08x}", stage ? 'p' : 'v', b, addr, size, (uint32)(h ^ (h >> 32)));
            static const bool s_vu3 = std::getenv("WF_GPUDUMP_VU3") != nullptr;
            if (stage == 0 && (b == 1 || (b == 3 && s_vu3)) && size >= 0x30)
            {
                const float* w = (const float*)p;
                fmt::print(g_dump, "(t={:.1f},{:.1f},{:.1f})", w[3], w[7], w[11]);
            }
            static const char* s_vs = std::getenv("WF_GPUDUMP_VS");
            static uint32 s_vsFrame = 0xFFFFFFFF;
            static uint32 s_vsDraw = 0xFFFFFFFF;
            static const bool s_vsAll = std::getenv("WF_GPUDUMP_VS_ALL") != nullptr;
            LatteDecompilerShader* vsh = LatteSHRC_GetActiveVertexShader();
            auto vs_matches = [&] {
                std::string h = fmt::format("{:016x}", vsh->baseHash);
                for (std::string_view l = s_vs; !l.empty();)
                {
                    size_t c = l.find(',');
                    if (h.starts_with(l.substr(0, c))) return true;
                    l = c == std::string_view::npos ? std::string_view() : l.substr(c + 1);
                }
                return false;
            };
            if (s_vs && vsh && vs_matches() && (s_vsAll || s_vsFrame != LatteGPUState.frameCounter || s_vsDraw == g_drawIndex))
            {
                s_vsFrame = LatteGPUState.frameCounter;
                s_vsDraw = g_drawIndex;
                fmt::print(g_dump, "\nUB {}{}:", stage ? 'p' : 'v', b);
                for (uint32 i = 0; i < size / 4; i++)
                    fmt::print(g_dump, "{}{:.6g}", i % 8 ? " " : "\n  ", std::bit_cast<float>(((const uint32*)p)[i]));
                fmt::print(g_dump, "\n");
            }
        }
    }
}

void dump_texture_hashes()
{
    const uint32* regs = LatteGPUState.contextRegister;
    LatteDecompilerShader* shaders[2] = {LatteSHRC_GetActiveVertexShader(), LatteSHRC_GetActivePixelShader()};
    uint32 bases[2] = {Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS, Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS};
    for (int stage = 0; stage < 2; stage++)
    {
        LatteDecompilerShader* sh = shaders[stage];
        if (!sh) continue;
        for (uint8 i = 0; i < sh->textureUnitListCount; i++)
        {
            uint8 t = sh->textureUnitList[i];
            MPTR addr = regs[bases[stage] + t * 7 + 2] << 8;
            if (!addr) continue;
            const uint8* p = (const uint8*)memory_getPointerFromPhysicalOffset(addr);
            uint64 h = 1469598103934665603ull;
            for (uint32 k = 0; k < 0x10000; k += 4) h = (h ^ *(const uint32*)(p + k)) * 1099511628211ull;
            fmt::print(g_dump, " {}t{}@{:08X}#{:08x}", stage ? 'p' : 'v', t, addr, (uint32)(h ^ (h >> 32)));
        }
    }
}

}

void wf_gpu_dump_draw_impl(uint32 count, uint32 instanceCount);

static bool skip_draw()
{
    static const std::vector<std::string> s_skip = [] {
        std::vector<std::string> v;
        if (const char* e = std::getenv("WF_SKIP_VS"))
        {
            std::string all = e;
            for (size_t p = 0; p < all.size();)
            {
                size_t c = all.find(',', p);
                if (c == std::string::npos) c = all.size();
                v.push_back(all.substr(p, c - p));
                p = c + 1;
            }
        }
        return v;
    }();
    if (s_skip.empty()) return false;
    LatteDecompilerShader* vs = LatteSHRC_GetActiveVertexShader();
    if (!vs) return false;
    std::string h = fmt::format("{:016x}", vs->baseHash);
    for (const auto& p : s_skip)
        if (h.starts_with(p)) return true;
    return false;
}

void wf_gpu_dump_request(const std::string& dir, uint32 first, uint32 count)
{
    if (g_dump) return;
    g_dumpDir = dir + "/";
    g_dumpCount = count;
    g_drawIndex = 0;
    g_dumpFrame = first;
    g_done = false;
}

bool wf_gpu_dump_draw(uint32 count, uint32 instanceCount)
{
    static const long long slowNs = std::getenv("WF_GPU_SLOW") ? std::atoll(std::getenv("WF_GPU_SLOW")) : 0;
    if (slowNs)
    {
        auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(slowNs);
        while (std::chrono::steady_clock::now() < until) {}
    }
    static uint32 s_vbLo = 0, s_vbHi = 0;
    static bool s_vbInit = false;
    if (!s_vbInit)
    {
        s_vbInit = true;
        if (const char* e = std::getenv("WF_LOG_VB")) std::sscanf(e, "%x-%x", &s_vbLo, &s_vbHi);
    }
    if (s_vbHi)
    {
        const uint32* b = LatteGPUState.contextRegister + mmSQ_VTX_ATTRIBUTE_BLOCK_START;
        MPTR addr = b[0];
        uint32 size = b[1] + 1;
        if (addr >= s_vbLo && addr < s_vbHi)
        {
            const uint32* p = (const uint32*)memory_getPointerFromPhysicalOffset(addr);
            float worst = 0;
            uint32 bad = 0, n = std::min<uint32>(size / 4, 0x4000);
            for (uint32 i = 0; i < n; i++)
            {
                float v = be_float(p + i);
                if (!std::isfinite(v) || std::abs(v) > 1e6f) bad++;
                else worst = std::max(worst, std::abs(v));
            }
            static uint32 s_logged = 0;
            if (bad && s_logged++ < 400)
                wf::log_info("vb {:08X}+{:X} frame {} n {}: {} garbage words (largest sane {:.0f})", addr, size, LatteGPUState.frameCounter, count, bad, worst);
        }
    }
    bool skip = skip_draw();
    wf_gpu_dump_draw_impl(count, instanceCount);
    return skip;
}

void wf_gpu_dump_draw_impl(uint32 count, uint32 instanceCount)
{
    if (g_done) return;
    if (g_dumpFrame == 0xFFFFFFFF)
    {
        const char* f = std::getenv("WF_GPUDUMP");
        char* end = nullptr;
        g_dumpFrame = f ? (uint32)std::strtoul(f, &end, 10) : 0xFFFFFFFE;
        if (f && *end == 'x') g_dumpCount = std::max<uint32>(1, (uint32)std::strtoul(end + 1, nullptr, 10));
    }
    static uint32 s_fileFrame = 0;
    uint32 frame = LatteGPUState.frameCounter;
    if (frame < g_dumpFrame) return;
    if (g_dump && frame != s_fileFrame)
    {
        std::fclose(g_dump);
        g_dump = nullptr;
        wf::log_info("GPU dump: frame {} ({} draws) written", s_fileFrame, g_drawIndex);
        g_drawIndex = 0;
    }
    if (frame >= g_dumpFrame + g_dumpCount)
    {
        g_done = true;
        return;
    }
    if (!g_dump)
    {
        s_fileFrame = frame;
        g_dump = std::fopen(fmt::format("{}gpudump_{}.txt", g_dumpDir, frame).c_str(), "w");
    }
    if (!g_dump) { g_done = true; return; }
    LatteDecompilerShader* vs = LatteSHRC_GetActiveVertexShader();
    LatteDecompilerShader* ps = LatteSHRC_GetActivePixelShader();
    fmt::print(g_dump, "#{} n={} inst={} rt0={:08X} vs={:016x} ps={:016x}", g_drawIndex++, count, instanceCount,
               LatteGPUState.contextRegister[mmCB_COLOR0_BASE] << 8, vs ? vs->baseHash : 0, ps ? ps->baseHash : 0);
    dump_vertex_buffers();
    dump_uniform_hashes();
    if (static const bool remap = std::getenv("WF_GPUDUMP_REMAP") != nullptr; remap)
        if (LatteDecompilerShader* vsh = LatteSHRC_GetActiveVertexShader()) dump_vs_uniforms(vsh);
    dump_texture_hashes();
    const uint32* r = LatteGPUState.contextRegister;
    fmt::print(g_dump, " depth={:08X} stencil={:08X}/{:08X} cmask={:08X} blend0={:08X} blend1={:08X} cctl={:08X} rt1={:08X}", r[0xA200], r[0xA10C],
               r[0xA10D], r[0xA08E], r[0xA1E0], r[0xA1E1], r[0xA202], r[mmCB_COLOR0_BASE + 1] << 8);
    fmt::print(g_dump, "\n");
}
