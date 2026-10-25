#include "Common/precompiled.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "Cafe/HW/Latte/Core/LatteTextureLoader.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"

#include "ui/game_art.h"

#include <fstream>
#include <map>
#include <mutex>

#include "core/config.h"
#include "core/log.h"

namespace wf::ui::art {
namespace {

std::mutex g_lock;
bool g_loaded = false, g_ok = false;
std::map<std::string, Image> g_images;
std::map<std::string, void*> g_textures;

uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

std::vector<uint8_t> read_file(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

std::vector<uint8_t> yaz0(const std::vector<uint8_t>& d)
{
    if (d.size() < 16 || memcmp(d.data(), "Yaz0", 4) != 0) return d;
    uint32_t size = be32(&d[4]);
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
                size_t dist = ((b1 & 0xF) << 8 | b2) + 1;
                size_t n = b1 >> 4;
                if (n == 0) n = (i < d.size() ? d[i++] : 0) + 0x12;
                else n += 2;
                if (dist > out.size()) return out;
                for (size_t k = 0; k < n; k++) out.push_back(out[out.size() - dist]);
            }
        }
    }
    return out;
}

std::map<std::string, std::vector<uint8_t>> sarc(const std::vector<uint8_t>& d)
{
    std::map<std::string, std::vector<uint8_t>> files;
    if (d.size() < 0x14 || memcmp(d.data(), "SARC", 4) != 0) return files;
    bool big = d[6] == 0xFE;
    auto r16 = [&](size_t o) { return big ? be16(&d[o]) : (uint16_t)(d[o] | d[o + 1] << 8); };
    auto r32 = [&](size_t o) { return big ? be32(&d[o]) : (uint32_t)(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | d[o + 3] << 24); };
    size_t p = r16(4);
    uint32_t dataOffset = r32(0xC);
    if (p + 12 > d.size() || memcmp(&d[p], "SFAT", 4) != 0) return files;
    uint16_t count = r16(p + 6);
    size_t nodes = p + 12;
    size_t sfnt = nodes + 16 * (size_t)count;
    if (sfnt + 8 > d.size() || memcmp(&d[sfnt], "SFNT", 4) != 0) return files;
    size_t names = sfnt + 8;
    for (uint16_t k = 0; k < count; k++)
    {
        size_t n = nodes + 16 * (size_t)k;
        uint32_t attr = r32(n + 4), start = r32(n + 8), end = r32(n + 12);
        if (!(attr & 0x01000000)) continue;
        size_t nameAt = names + (attr & 0xFFFF) * 4;
        if (nameAt >= d.size() || dataOffset + end > d.size() || start > end) continue;
        std::string name((const char*)&d[nameAt], strnlen((const char*)&d[nameAt], d.size() - nameAt));
        files[name].assign(d.begin() + dataOffset + start, d.begin() + dataOffset + end);
    }
    return files;
}

bool decode_bflim(const std::vector<uint8_t>& f, Image& out)
{
    if (f.size() < 0x28) return false;
    const uint8_t* h = &f[f.size() - 0x28];
    if (memcmp(h, "FLIM", 4) != 0 || memcmp(h + 0x14, "imag", 4) != 0 || h[4] != 0xFE) return false;
    const uint8_t* im = h + 0x14;
    uint32_t width = be16(im + 8), height = be16(im + 10);
    uint8_t format = im[14], flags = im[15];
    uint32_t imageSize = be32(im + 16);
    if (imageSize > f.size() || !width || !height) return false;

    Latte::E_GX2SURFFMT gx2fmt;
    int kind;
    switch (format)
    {
    case 17: gx2fmt = Latte::E_GX2SURFFMT::BC5_UNORM; kind = 0; break;
    case 16: gx2fmt = Latte::E_GX2SURFFMT::BC4_UNORM; kind = 1; break;
    case 15: gx2fmt = Latte::E_GX2SURFFMT::BC4_UNORM; kind = 2; break;
    case 12: case 21: gx2fmt = Latte::E_GX2SURFFMT::BC1_UNORM; kind = 3; break;
    case 14: case 23: gx2fmt = Latte::E_GX2SURFFMT::BC3_UNORM; kind = 3; break;
    case 9: case 20: gx2fmt = Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM; kind = 3; break;
    case 0: gx2fmt = Latte::E_GX2SURFFMT::R8_UNORM; kind = 4; break;
    case 1: gx2fmt = Latte::E_GX2SURFFMT::R8_UNORM; kind = 5; break;
    case 3: gx2fmt = Latte::E_GX2SURFFMT::R8_G8_UNORM; kind = 6; break;
    default: return false;
    }
    auto tileMode = (Latte::E_GX2TILEMODE)(flags & 0x1F);
    uint32_t swizzle = (uint32_t)(flags >> 5) << 8;
    LatteAddrLib::AddrSurfaceInfo_OUT info{};
    LatteAddrLib::GX2CalculateSurfaceInfo(gx2fmt, width, height, 1, Latte::E_DIM::DIM_2D, tileMode, 0, 0, &info);
    bool compressed = kind <= 3 && gx2fmt != Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM;
    uint32_t step = compressed ? 4 : 1;
    uint32_t bpp = info.bpp;
    uint32_t pipeSwizzle = (swizzle >> 8) & 1, bankSwizzle = (swizzle >> 9) & 3;
    uint32_t blocksW = (width + step - 1) / step, blocksH = (height + step - 1) / step;

    out.width = (int)width;
    out.height = (int)height;
    out.rgba.assign((size_t)width * height * 4, 0);
    for (uint32_t by = 0; by < blocksH; by++)
        for (uint32_t bx = 0; bx < blocksW; bx++)
        {
            uint32_t offset;
            if (Latte::TM_IsMacroTiled(info.hwTileMode))
                offset = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiled(bx, by, 0, 0, bpp, info.pitch, info.height, 1, info.hwTileMode, false, pipeSwizzle, bankSwizzle);
            else if (info.hwTileMode == Latte::E_HWTILEMODE::TM_1D_TILED_THIN1 || info.hwTileMode == Latte::E_HWTILEMODE::TM_1D_TILED_THICK)
                offset = LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(bx, by, 0, bpp, info.pitch, info.height, info.hwTileMode, false);
            else
                offset = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(bx, by, 0, 0, bpp, info.pitch, info.height, 1);
            if (offset + bpp / 8 > imageSize) continue;
            uint8_t* block = const_cast<uint8_t*>(&f[offset]);
            float px[16 * 4];
            if (compressed)
            {
                if (kind == 0)
                {
                    float rg[16 * 2];
                    decodeBC5Block_UNORM(block, rg);
                    for (int i = 0; i < 16; i++) { px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = rg[i * 2]; px[i * 4 + 3] = rg[i * 2 + 1]; }
                }
                else if (kind == 1 || kind == 2)
                {
                    float r[16];
                    decodeBC4Block_UNORM(block, r);
                    for (int i = 0; i < 16; i++)
                    {
                        px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = kind == 1 ? 1.0f : r[i];
                        px[i * 4 + 3] = kind == 1 ? r[i] : 1.0f;
                    }
                }
                else if (gx2fmt == Latte::E_GX2SURFFMT::BC1_UNORM)
                    decodeBC1Block(block, px);
                else
                    decodeBC3Block_UNORM(block, px);
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++)
                    {
                        uint32_t X = bx * 4 + x, Y = by * 4 + y;
                        if (X >= width || Y >= height) continue;
                        for (int c = 0; c < 4; c++)
                            out.rgba[((size_t)Y * width + X) * 4 + c] = (uint8_t)std::clamp(px[(y * 4 + x) * 4 + c] * 255.0f + 0.5f, 0.0f, 255.0f);
                    }
            }
            else
            {
                uint8_t* dst = &out.rgba[((size_t)by * width + bx) * 4];
                switch (kind)
                {
                case 3: memcpy(dst, block, 4); break;
                case 4: dst[0] = dst[1] = dst[2] = block[0]; dst[3] = 255; break;
                case 5: dst[0] = dst[1] = dst[2] = 255; dst[3] = block[0]; break;
                case 6: dst[0] = dst[1] = dst[2] = block[0]; dst[3] = block[1]; break;
                }
            }
        }
    return true;
}

void load_locked()
{
    if (g_loaded) return;
    g_loaded = true;
    auto pack = sarc(yaz0(read_file(config().gameDir / "content" / "Common" / "Pack" / "permanent_2d_UsEnglish.pack")));
    if (pack.empty())
    {
        log_warn("menu art: content/Common/Pack/permanent_2d_UsEnglish.pack not readable; using plain styling");
        return;
    }
    for (const char* layout : {"SequenceOption_00.szs", "MenuBtns_00.szs", "BtnOption_00.szs"})
    {
        auto it = pack.find(layout);
        if (it == pack.end()) continue;
        for (auto& [name, data] : sarc(yaz0(it->second)))
        {
            if (!name.ends_with(".bflim")) continue;
            std::string key = name.substr(name.rfind('/') + 1);
            key = key.substr(0, key.find_first_of("^."));
            Image img;
            if (decode_bflim(data, img)) g_images[key] = std::move(img);
        }
    }
    g_ok = !g_images.empty();
    log_info("menu art: {} textures from the game's options screen", g_images.size());

    if (const char* dir = std::getenv("WF_DUMP_UI_ART"))
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        for (auto& [key, img] : g_images)
        {
            std::vector<uint8_t> tga(18 + img.rgba.size());
            tga[2] = 2;
            tga[12] = (uint8_t)img.width; tga[13] = (uint8_t)(img.width >> 8);
            tga[14] = (uint8_t)img.height; tga[15] = (uint8_t)(img.height >> 8);
            tga[16] = 32; tga[17] = 0x28;
            for (size_t i = 0; i < img.rgba.size(); i += 4)
            {
                tga[18 + i] = img.rgba[i + 2]; tga[18 + i + 1] = img.rgba[i + 1];
                tga[18 + i + 2] = img.rgba[i]; tga[18 + i + 3] = img.rgba[i + 3];
            }
            std::ofstream(std::filesystem::path(dir) / (key + ".tga"), std::ios::binary).write((const char*)tga.data(), tga.size());
        }
    }
}

}

bool load()
{
    std::lock_guard lk(g_lock);
    load_locked();
    return g_ok;
}

const Image* get(const std::string& name)
{
    std::lock_guard lk(g_lock);
    load_locked();
    auto it = g_images.find(name);
    return it == g_images.end() ? nullptr : &it->second;
}

void* texture(const std::string& name)
{
    std::lock_guard lk(g_lock);
    load_locked();
    if (auto t = g_textures.find(name); t != g_textures.end()) return t->second;
    auto it = g_images.find(name);
    void* tex = nullptr;
    if (it != g_images.end())
        tex = VulkanRenderer::GetInstance()->GenerateTextureRGBA(it->second.rgba, {it->second.width, it->second.height});
    g_textures[name] = tex;
    return tex;
}

void* texture_tinted(const std::string& name, uint32_t dark, uint32_t light)
{
    std::string key = fmt::format("{}#{:08x}{:08x}", name, dark, light);
    std::lock_guard lk(g_lock);
    load_locked();
    if (auto t = g_textures.find(key); t != g_textures.end()) return t->second;
    auto it = g_images.find(name);
    void* tex = nullptr;
    if (it != g_images.end())
    {
        std::vector<uint8_t> rgba = it->second.rgba;
        for (size_t i = 0; i < rgba.size(); i += 4)
        {
            float l = rgba[i] / 255.0f;
            for (int c = 0; c < 3; c++)
            {
                float d = (float)((dark >> (8 * c)) & 0xFF), w = (float)((light >> (8 * c)) & 0xFF);
                rgba[i + c] = (uint8_t)(d + (w - d) * l + 0.5f);
            }
            float ad = (float)(dark >> 24), aw = (float)(light >> 24);
            rgba[i + 3] = (uint8_t)(rgba[i + 3] * (ad + (aw - ad) * l) / 255.0f + 0.5f);
        }
        tex = VulkanRenderer::GetInstance()->GenerateTextureRGBA(rgba, {it->second.width, it->second.height});
    }
    g_textures[key] = tex;
    return tex;
}

}
