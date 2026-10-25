#include "hle/guest_printf.h"

#include <cstdio>

#include "core/guest.h"

namespace wf::hle {
namespace {

struct Source
{
    virtual uint32_t word() = 0;
    virtual double dbl() = 0;
    uint64_t dword()
    {
        alignPair();
        uint64_t hi = word(), lo = word();
        return hi << 32 | lo;
    }
    virtual void alignPair() {}
    virtual ~Source() = default;
};

struct CallSource : Source
{
    VarArgs a;
    explicit CallSource(VarArgs v) : a(v) {}
    uint32_t word() override
    {
        if (a.gpr <= 10) return a.ctx->r[a.gpr++];
        uint32_t v = rd32(a.stack);
        a.stack += 4;
        return v;
    }
    void alignPair() override
    {
        if (a.gpr <= 10) { if (!(a.gpr & 1)) a.gpr++; }
        else a.stack = (a.stack + 7) & ~7u;
    }
    double dbl() override
    {
        if (a.fpr <= 8) return a.ctx->f[a.fpr++].ps0;
        a.stack = (a.stack + 7) & ~7u;
        uint64_t v = rd64(a.stack);
        a.stack += 8;
        return std::bit_cast<double>(v);
    }
};

struct VaSource : Source
{
    uint32_t va;
    explicit VaSource(uint32_t a) : va(a) {}
    uint32_t word() override
    {
        uint8_t g = rd8(va);
        if (g < 8)
        {
            wr8(va, g + 1);
            return rd32(rd32(va + 8) + 4 * g);
        }
        uint32_t o = rd32(va + 4);
        wr32(va + 4, o + 4);
        return rd32(o);
    }
    void alignPair() override
    {
        uint8_t g = rd8(va);
        if (g < 8) { if (g & 1) wr8(va, g + 1); }
        else wr32(va + 4, (rd32(va + 4) + 7) & ~7u);
    }
    double dbl() override
    {
        uint8_t f = rd8(va + 1);
        if (f < 8)
        {
            wr8(va + 1, f + 1);
            return std::bit_cast<double>(rd64(rd32(va + 8) + 32 + 8 * f));
        }
        uint32_t o = (rd32(va + 4) + 7) & ~7u;
        wr32(va + 4, o + 8);
        return std::bit_cast<double>(rd64(o));
    }
};

std::string format(uint32_t fmtAddr, Source& src)
{
    std::string fmt = read_cstr(fmtAddr), out;
    for (size_t i = 0; i < fmt.size(); i++)
    {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        size_t start = i++;
        while (i < fmt.size() && strchr("-+ #0", fmt[i])) i++;
        std::string spec = fmt.substr(start, i - start);
        auto number = [&] {
            if (i < fmt.size() && fmt[i] == '*') { spec += std::to_string((int32_t)src.word()); i++; return; }
            while (i < fmt.size() && isdigit((unsigned char)fmt[i])) spec += fmt[i++];
        };
        number();
        if (i < fmt.size() && fmt[i] == '.') { spec += fmt[i++]; number(); }
        int longs = 0;
        bool shortArg = false;
        while (i < fmt.size() && strchr("hlLqjzt", fmt[i]))
        {
            if (fmt[i] == 'l' || fmt[i] == 'q' || fmt[i] == 'L' || fmt[i] == 'j') longs++;
            if (fmt[i] == 'h') shortArg = true;
            i++;
        }
        if (i >= fmt.size()) break;
        char conv = fmt[i];
        char buf[512];
        switch (conv)
        {
        case '%': out += '%'; continue;
        case 'd': case 'i':
            if (longs >= 2) snprintf(buf, sizeof buf, (spec + "lld").c_str(), (long long)(int64_t)src.dword());
            else snprintf(buf, sizeof buf, (spec + "d").c_str(), shortArg ? (int)(int16_t)src.word() : (int)(int32_t)src.word());
            break;
        case 'u': case 'x': case 'X': case 'o':
            if (longs >= 2) snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), (unsigned long long)src.dword());
            else snprintf(buf, sizeof buf, (spec + conv).c_str(), (unsigned)src.word());
            break;
        case 'c': snprintf(buf, sizeof buf, (spec + "c").c_str(), (int)src.word()); break;
        case 'p': snprintf(buf, sizeof buf, "0x%08X", src.word()); break;
        case 's':
        {
            std::string s = read_cstr(src.word());
            snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            snprintf(buf, sizeof buf, (spec + conv).c_str(), src.dbl());
            break;
        case 'n': src.word(); continue;
        default: out += spec; out += conv; continue;
        }
        out += buf;
    }
    return out;
}

}

std::string guest_format(uint32_t fmt, VarArgs args)
{
    CallSource s(args);
    return format(fmt, s);
}

std::string guest_vformat(uint32_t fmt, VaList va)
{
    VaSource s(va.addr);
    return format(fmt, s);
}

}
