#include "core/archive.h"

#include <algorithm>
#include <cstring>

namespace wf::archive {
namespace {

uint16_t rd16(const uint8_t* p, bool be) { return be ? (uint16_t)(p[0] << 8 | p[1]) : (uint16_t)(p[1] << 8 | p[0]); }
uint32_t rd32(const uint8_t* p, bool be)
{
    return be ? (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3] : (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0];
}
void wr16(std::vector<uint8_t>& v, size_t o, uint16_t x, bool be)
{
    v[o] = be ? (uint8_t)(x >> 8) : (uint8_t)x;
    v[o + 1] = be ? (uint8_t)x : (uint8_t)(x >> 8);
}
void wr32(std::vector<uint8_t>& v, size_t o, uint32_t x, bool be)
{
    for (int i = 0; i < 4; i++) v[o + i] = (uint8_t)(x >> (be ? 24 - 8 * i : 8 * i));
}

uint32_t name_hash(const std::string& name, uint32_t key)
{
    uint32_t h = 0;
    for (char c : name) h = h * key + (uint32_t)(int32_t)(signed char)c;
    return h;
}

size_t align_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

}

bool is_yaz0(const std::vector<uint8_t>& d) { return d.size() >= 16 && memcmp(d.data(), "Yaz0", 4) == 0; }

std::vector<uint8_t> yaz0_decompress(const std::vector<uint8_t>& d)
{
    if (!is_yaz0(d)) return d;
    uint32_t size = rd32(&d[4], true);
    std::vector<uint8_t> out;
    out.reserve(size);
    size_t i = 16;
    while (out.size() < size && i < d.size())
    {
        uint8_t code = d[i++];
        for (int b = 0; b < 8 && out.size() < size && i < d.size(); b++)
        {
            if (code & (0x80 >> b))
                out.push_back(d[i++]);
            else
            {
                if (i + 1 >= d.size()) return out;
                uint8_t b1 = d[i], b2 = d[i + 1];
                i += 2;
                size_t dist = ((b1 & 0xF) << 8 | b2) + 1, n = b1 >> 4;
                if (n == 0) n = (i < d.size() ? d[i++] : 0) + 0x12;
                else n += 2;
                if (dist > out.size()) return out;
                for (size_t k = 0; k < n; k++) out.push_back(out[out.size() - dist]);
            }
        }
    }
    return out;
}

std::vector<uint8_t> yaz0_store(const std::vector<uint8_t>& d)
{
    std::vector<uint8_t> out(16, 0);
    memcpy(out.data(), "Yaz0", 4);
    wr32(out, 4, (uint32_t)d.size(), true);
    out.reserve(16 + d.size() + d.size() / 8 + 1);
    for (size_t i = 0; i < d.size(); i += 8)
    {
        size_t n = std::min<size_t>(8, d.size() - i);
        out.push_back((uint8_t)(0xFF << (8 - n)));
        out.insert(out.end(), d.begin() + i, d.begin() + i + n);
    }
    return out;
}

std::vector<uint8_t> yaz0_compress(const std::vector<uint8_t>& d)
{
    std::vector<uint8_t> out(16, 0);
    memcpy(out.data(), "Yaz0", 4);
    wr32(out, 4, (uint32_t)d.size(), true);
    out.reserve(16 + d.size() / 2);
    constexpr size_t kWindow = 0x1000, kMax = 0x111, kHash = 1 << 16, kDepth = 48;
    std::vector<int32_t> head(kHash, -1), prev(d.size(), -1);
    auto hash3 = [&](size_t i) { return ((d[i] << 16 | d[i + 1] << 8 | d[i + 2]) * 2654435761u) >> 16; };
    auto insert = [&](size_t i) {
        if (i + 2 < d.size())
        {
            uint32_t h = hash3(i) & (kHash - 1);
            prev[i] = head[h];
            head[h] = (int32_t)i;
        }
    };
    size_t i = 0;
    while (i < d.size())
    {
        size_t codePos = out.size();
        out.push_back(0);
        uint8_t code = 0;
        for (int b = 0; b < 8 && i < d.size(); b++)
        {
            size_t bestLen = 0, bestDist = 0;
            if (i + 2 < d.size())
            {
                int32_t cand = head[hash3(i) & (kHash - 1)];
                size_t limit = std::min(kMax, d.size() - i);
                for (size_t depth = 0; cand >= 0 && depth < kDepth; depth++, cand = prev[(size_t)cand])
                {
                    size_t dist = i - (size_t)cand;
                    if (dist > kWindow) break;
                    size_t len = 0;
                    while (len < limit && d[(size_t)cand + len] == d[i + len]) len++;
                    if (len > bestLen)
                    {
                        bestLen = len;
                        bestDist = dist;
                        if (len == limit) break;
                    }
                }
            }
            if (bestLen >= 3)
            {
                size_t dist = bestDist - 1;
                if (bestLen >= 0x12)
                {
                    out.push_back((uint8_t)(dist >> 8));
                    out.push_back((uint8_t)dist);
                    out.push_back((uint8_t)(bestLen - 0x12));
                }
                else
                {
                    out.push_back((uint8_t)(((bestLen - 2) << 4) | (dist >> 8)));
                    out.push_back((uint8_t)dist);
                }
                for (size_t k = 0; k < bestLen; k++) insert(i + k);
                i += bestLen;
            }
            else
            {
                code |= (uint8_t)(0x80 >> b);
                out.push_back(d[i]);
                insert(i);
                i++;
            }
        }
        out[codePos] = code;
    }
    return out;
}

SarcFile* Sarc::find(const std::string& name)
{
    for (SarcFile& f : files)
        if (f.name == name) return &f;
    return nullptr;
}

bool sarc_read(const std::vector<uint8_t>& d, Sarc& out)
{
    if (d.size() < 0x14 || memcmp(d.data(), "SARC", 4) != 0) return false;
    bool be = d[6] == 0xFE;
    out.bigEndian = be;
    size_t p = rd16(&d[4], be);
    uint32_t dataOffset = rd32(&d[0xC], be);
    if (p + 12 > d.size() || memcmp(&d[p], "SFAT", 4) != 0) return false;
    uint16_t count = rd16(&d[p + 6], be);
    out.hashKey = rd32(&d[p + 8], be);
    size_t nodes = p + 12, sfnt = nodes + 16 * (size_t)count;
    if (sfnt + 8 > d.size() || memcmp(&d[sfnt], "SFNT", 4) != 0) return false;
    size_t names = sfnt + 8;
    for (uint16_t k = 0; k < count; k++)
    {
        size_t n = nodes + 16 * (size_t)k;
        uint32_t attr = rd32(&d[n + 4], be), start = rd32(&d[n + 8], be), end = rd32(&d[n + 12], be);
        if (!(attr & 0x01000000) || dataOffset + (size_t)end > d.size() || start > end) continue;
        size_t at = names + (attr & 0xFFFF) * 4;
        if (at >= d.size()) continue;
        SarcFile f;
        f.name.assign((const char*)&d[at], strnlen((const char*)&d[at], d.size() - at));
        f.data.assign(d.begin() + dataOffset + start, d.begin() + dataOffset + end);
        uint32_t abs = dataOffset + start;
        f.alignment = 4;
        while (f.alignment < 0x2000 && abs % (f.alignment * 2) == 0) f.alignment *= 2;
        out.files.push_back(std::move(f));
    }
    return true;
}

std::vector<uint8_t> sarc_write(const Sarc& s)
{
    bool be = s.bigEndian;
    std::vector<const SarcFile*> order;
    for (const SarcFile& f : s.files) order.push_back(&f);
    std::sort(order.begin(), order.end(), [&](const SarcFile* a, const SarcFile* b) { return name_hash(a->name, s.hashKey) < name_hash(b->name, s.hashKey); });

    size_t sfat = 0x14, nodes = sfat + 0xC, sfnt = nodes + 16 * order.size();
    std::vector<uint32_t> nameOffsets;
    size_t namesSize = 0;
    for (const SarcFile* f : order)
    {
        nameOffsets.push_back((uint32_t)namesSize);
        namesSize = align_up(namesSize + f->name.size() + 1, 4);
    }
    size_t maxAlign = 4;
    for (const SarcFile* f : order) maxAlign = std::max<size_t>(maxAlign, f->alignment);
    size_t dataOffset = align_up(sfnt + 8 + namesSize, maxAlign);
    std::vector<uint32_t> starts;
    size_t cur = 0;
    for (const SarcFile* f : order)
    {
        cur = align_up(dataOffset + cur, f->alignment) - dataOffset;
        starts.push_back((uint32_t)cur);
        cur += f->data.size();
    }
    std::vector<uint8_t> out(dataOffset + cur, 0);
    memcpy(&out[0], "SARC", 4);
    wr16(out, 4, 0x14, be);
    out[6] = be ? 0xFE : 0xFF;
    out[7] = be ? 0xFF : 0xFE;
    wr32(out, 8, (uint32_t)out.size(), be);
    wr32(out, 0xC, (uint32_t)dataOffset, be);
    wr16(out, 0x10, 0x0100, be);
    memcpy(&out[sfat], "SFAT", 4);
    wr16(out, sfat + 4, 0xC, be);
    wr16(out, sfat + 6, (uint16_t)order.size(), be);
    wr32(out, sfat + 8, s.hashKey, be);
    for (size_t i = 0; i < order.size(); i++)
    {
        size_t n = nodes + 16 * i;
        wr32(out, n, name_hash(order[i]->name, s.hashKey), be);
        wr32(out, n + 4, 0x01000000 | (nameOffsets[i] / 4), be);
        wr32(out, n + 8, starts[i], be);
        wr32(out, n + 12, starts[i] + (uint32_t)order[i]->data.size(), be);
    }
    memcpy(&out[sfnt], "SFNT", 4);
    wr16(out, sfnt + 4, 8, be);
    for (size_t i = 0; i < order.size(); i++) memcpy(&out[sfnt + 8 + nameOffsets[i]], order[i]->name.c_str(), order[i]->name.size());
    for (size_t i = 0; i < order.size(); i++)
        if (!order[i]->data.empty()) memcpy(&out[dataOffset + starts[i]], order[i]->data.data(), order[i]->data.size());
    return out;
}

}
