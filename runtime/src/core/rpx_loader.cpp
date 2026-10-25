#include "core/rpx_loader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <windows.h>
#include <bcrypt.h>

#include "miniz.h"

#include "core/guest.h"
#include "core/log.h"
#include "wf/recomp_api.h"
#include "cpu/interp.h"

namespace wf {

namespace {

constexpr uint32_t SHF_RPL_ZLIB = 0x08000000, SHF_ALLOC = 0x2;
constexpr uint32_t SHT_SYMTAB = 2, SHT_RELA = 4, SHT_NOBITS = 8, SHT_RPL_IMPORTS = 0x80000002;

uint32_t be32at(const std::vector<uint8_t>& f, size_t o) { return (uint32_t)f[o] << 24 | f[o + 1] << 16 | f[o + 2] << 8 | f[o + 3]; }
uint16_t be16at(const std::vector<uint8_t>& f, size_t o) { return (uint16_t)(f[o] << 8 | f[o + 1]); }

struct Section
{
    std::string name;
    uint32_t type, flags, addr, size, link, info;
    std::vector<uint8_t> data;
};

std::string sha1_hex(const std::vector<uint8_t>& d)
{
    uint8_t h[20];
    BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, (PUCHAR)d.data(), (ULONG)d.size(), h, sizeof(h));
    std::string s;
    for (uint8_t b : h) s += std::format("{:02x}", b);
    return s;
}

}

std::string file_sha1(const std::filesystem::path& p)
{
    std::vector<uint8_t> d;
    if (FILE* f = _wfopen(p.c_str(), L"rb"))
    {
        std::fseek(f, 0, SEEK_END);
        d.resize(std::ftell(f));
        std::fseek(f, 0, SEEK_SET);
        std::fread(d.data(), 1, d.size(), f);
        std::fclose(f);
    }
    return sha1_hex(d);
}

namespace {

std::vector<uint8_t> read_all(const std::filesystem::path& path)
{
    std::vector<uint8_t> f;
    FILE* fp = _wfopen(path.c_str(), L"rb");
    if (!fp) fatal("cannot open {}", path.string());
    std::fseek(fp, 0, SEEK_END);
    f.resize(std::ftell(fp));
    std::fseek(fp, 0, SEEK_SET);
    std::fread(f.data(), 1, f.size(), fp);
    std::fclose(fp);
    return f;
}

std::vector<Section> parse(const std::vector<uint8_t>& f)
{
    uint32_t shoff = be32at(f, 0x20);
    uint16_t shentsize = be16at(f, 0x2E), shnum = be16at(f, 0x30), shstrndx = be16at(f, 0x32);
    std::vector<Section> secs(shnum);
    std::vector<uint32_t> nameOff(shnum);
    for (uint32_t i = 0; i < shnum; i++)
    {
        size_t h = shoff + (size_t)i * shentsize;
        Section& s = secs[i];
        nameOff[i] = be32at(f, h);
        s.type = be32at(f, h + 4);
        s.flags = be32at(f, h + 8);
        s.addr = be32at(f, h + 12);
        uint32_t off = be32at(f, h + 16);
        s.size = be32at(f, h + 20);
        s.link = be32at(f, h + 24);
        s.info = be32at(f, h + 28);
        if (s.type == SHT_NOBITS || s.size == 0)
            continue;
        if (s.flags & SHF_RPL_ZLIB)
        {
            uint32_t outSize = be32at(f, off);
            s.data.resize(outSize);
            mz_ulong dl = outSize;
            if (mz_uncompress(s.data.data(), &dl, f.data() + off + 4, s.size - 4) != MZ_OK || dl != outSize)
                fatal("RPX section {} failed to decompress", i);
        }
        else
            s.data.assign(f.begin() + off, f.begin() + off + s.size);
    }
    for (uint32_t i = 0; i < shnum; i++)
        secs[i].name = (const char*)secs[shstrndx].data.data() + nameOff[i];
    return secs;
}

RpxLoadResult place(const std::vector<Section>& secs, std::vector<std::pair<uint32_t, uint32_t>>* rel24)
{
    RpxLoadResult res;
    for (const Section& s : secs)
    {
        if (!(s.flags & SHF_ALLOC) || s.type == SHT_RPL_IMPORTS || s.addr >= 0xC0000000)
            continue;
        uint32_t size = s.type == SHT_NOBITS ? s.size : (uint32_t)s.data.size();
        if (!is_mapped(s.addr, size))
            fatal("RPX section {} ({:08X}+{:X}) lies outside guest memory", s.name, s.addr, size);
        if (s.type == SHT_NOBITS)
            memset(gptr(s.addr), 0, size);
        else
            memcpy(gptr(s.addr), s.data.data(), size);
        if (s.addr >= gmem::kMem2Base)
            res.dataEnd = std::max(res.dataEnd, s.addr + size);
    }

    std::map<std::pair<std::string, std::string>, uint32_t> assigned;
    for (uint32_t i = 0; i < g_importCount; i++)
        assigned[{g_imports[i].module, g_imports[i].name}] = g_imports[i].guest;

    const Section& symtab = *std::find_if(secs.begin(), secs.end(), [](const Section& s) { return s.type == SHT_SYMTAB; });
    const Section& strtab = secs[symtab.link];
    std::vector<uint32_t> symValue(symtab.data.size() / 16);
    for (size_t k = 0; k < symValue.size(); k++)
    {
        const uint8_t* e = symtab.data.data() + k * 16;
        uint32_t nm = (uint32_t)e[0] << 24 | e[1] << 16 | e[2] << 8 | e[3];
        uint32_t val = (uint32_t)e[4] << 24 | e[5] << 16 | e[6] << 8 | e[7];
        uint16_t shndx = (uint16_t)(e[14] << 8 | e[15]);
        symValue[k] = val;
        if (shndx && shndx < secs.size() && secs[shndx].type == SHT_RPL_IMPORTS)
        {
            std::string mod = secs[shndx].name.substr(secs[shndx].name.find('_') + 1);
            if (mod.size() > 4 && mod.ends_with(".rpl"))
                mod.resize(mod.size() - 4);
            std::string sym = (const char*)strtab.data.data() + nm;
            auto it = assigned.find({mod, sym});
            if (it != assigned.end())
                symValue[k] = it->second;
        }
    }

    for (const Section& rs : secs)
    {
        if (rs.type != SHT_RELA)
            continue;
        const Section& target = secs[rs.info];
        if (!(target.flags & SHF_ALLOC))
            continue;
        for (size_t o = 0; o + 12 <= rs.data.size(); o += 12)
        {
            const uint8_t* e = rs.data.data() + o;
            uint32_t off = (uint32_t)e[0] << 24 | e[1] << 16 | e[2] << 8 | e[3];
            uint32_t info = (uint32_t)e[4] << 24 | e[5] << 16 | e[6] << 8 | e[7];
            int32_t addend = (int32_t)((uint32_t)e[8] << 24 | e[9] << 16 | e[10] << 8 | e[11]);
            uint32_t v = symValue[info >> 8] + (uint32_t)addend;
            switch (info & 0xFF)
            {
            case 1: wr32(off, v); break;
            case 4: wr16(off, (uint16_t)v); break;
            case 5: wr16(off, (uint16_t)(v >> 16)); break;
            case 6: wr16(off, (uint16_t)((v + 0x8000) >> 16)); break;
            case 0: continue;
            case 10:
                if (rel24) rel24->push_back({off, v});
                continue;
            default: fatal("unsupported relocation type {} at {:08X}", info & 0xFF, off);
            }
            res.relocations++;
        }
    }
    return res;
}

}

RpxLoadResult load_rpx(const std::filesystem::path& path)
{
    std::vector<uint8_t> f = read_all(path);
    std::string sha = sha1_hex(f);
    if (sha != g_image.sha1)
        fatal("{} does not match the version this build was recompiled from.\nExpected SHA-1 {}, got {}.\n"
              "Windfall supports The Wind Waker HD (USA, WUP-P-BCZE) v0 only.", path.string(), g_image.sha1, sha);
    RpxLoadResult res = place(parse(f), nullptr);
    log_info("loaded {} (data ends at {:08X}, {} relocations applied)", path.filename().string(), res.dataEnd, res.relocations);
    return res;
}

RpxLoadResult load_rpx_patched(const std::filesystem::path& original, const std::filesystem::path& patched)
{
    std::vector<uint8_t> fo = read_all(original);
    if (sha1_hex(fo) != g_image.sha1)
        fatal("{} does not match the version this build was recompiled from.\nWindfall supports The Wind Waker HD (USA, WUP-P-BCZE) v0 only.",
              original.string());
    std::vector<Section> so = parse(fo), sp = parse(read_all(patched));
    std::vector<std::pair<uint32_t, uint32_t>> rel24;
    RpxLoadResult res = place(sp, &rel24);

    auto text = [](const std::vector<Section>& v) -> const Section& {
        return *std::find_if(v.begin(), v.end(), [](const Section& s) { return s.name == ".text"; });
    };
    const Section& to = text(so);
    const Section& tp = text(sp);
    std::vector<uint32_t> starts;
    for (const FuncMapping* m = g_funcMappings; m->host; m++) starts.push_back(m->guest);
    std::sort(starts.begin(), starts.end());
    std::vector<uint32_t> entries;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    size_t common = std::min(to.data.size(), tp.data.size());
    for (size_t o = 0; o + 4 <= common; o += 4)
    {
        if (std::memcmp(&to.data[o], &tp.data[o], 4) == 0) continue;
        uint32_t a = to.addr + (uint32_t)o;
        auto it = std::upper_bound(starts.begin(), starts.end(), a);
        if (it == starts.begin()) continue;
        uint32_t fn = *(it - 1), end = it == starts.end() ? to.addr + (uint32_t)to.data.size() : *it;
        if (entries.empty() || entries.back() != fn)
        {
            entries.push_back(fn);
            ranges.push_back({fn, end});
        }
    }
    size_t added = tp.data.size() > to.data.size() ? tp.data.size() - to.data.size() : 0;
    if (added) ranges.push_back({tp.addr + (uint32_t)to.data.size(), tp.addr + (uint32_t)tp.data.size()});
    interp::setup(entries, ranges, rel24);
    log_info("loaded {} over the original (data ends at {:08X}, {} relocations, {} changed functions, {} bytes of added code)",
             patched.string(), res.dataEnd, res.relocations, entries.size(), added);
    return res;
}

}
