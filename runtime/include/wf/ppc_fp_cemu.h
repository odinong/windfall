// Espresso floating-point helpers adapted from Cemu
// Original code: Copyright (c) Cemu project contributors, licensed under the Mozilla Public License 2.0.
// This file remains under MPL-2.0: https://mozilla.org/MPL/2.0/
#pragma once
#include "ppc_types.h"

namespace wf::fp {

inline double round25(double d)
{
    uint64_t v = __builtin_bit_cast(uint64_t, d);
    v = (v & 0xFFFFFFFFF8000000ull) + (v & 0x8000000ull);
    return __builtin_bit_cast(double, v);
}

inline uint64_t single_to_double_bits(uint32_t value)
{
    uint64_t x = value;
    uint64_t exp = (x >> 23) & 0xff;
    uint64_t frac = x & 0x007fffff;
    if (exp > 0 && exp < 255)
    {
        uint64_t y = !(exp >> 7);
        uint64_t z = y << 61 | y << 60 | y << 59;
        return ((x & 0xc0000000) << 32) | z | ((x & 0x3fffffff) << 29);
    }
    else if (exp == 0 && frac != 0)
    {
        exp = 1023 - 126;
        do
        {
            frac <<= 1;
            exp -= 1;
        } while ((frac & 0x00800000) == 0);
        return ((x & 0x80000000) << 32) | (exp << 52) | ((frac & 0x007fffff) << 29);
    }
    else
    {
        uint64_t y = exp >> 7;
        uint64_t z = y << 61 | y << 60 | y << 59;
        return ((x & 0xc0000000) << 32) | z | ((x & 0x3fffffff) << 29);
    }
}

inline uint32_t double_to_single_bits(uint64_t x)
{
    uint32_t exp = (x >> 52) & 0x7ff;
    if (exp > 896 || (x & ~0x8000000000000000ULL) == 0)
        return ((x >> 32) & 0xc0000000) | ((x >> 29) & 0x3fffffff);
    else if (exp >= 874)
    {
        uint32_t t = (uint32_t)(0x80000000 | ((x & 0x000FFFFFFFFFFFFFULL) >> 21));
        t = t >> (905 - exp);
        t |= (x >> 32) & 0x80000000;
        return t;
    }
    return ((x >> 32) & 0xc0000000) | ((x >> 29) & 0x3fffffff);
}

struct LutEntry { uint32_t offset, step; };

inline constexpr LutEntry kFrsqrteLut[32] = {
    {0x1a7e800, 0x568},{0x17cb800, 0x4f3},{0x1552800, 0x48d},{0x130c000, 0x435},
    {0x10f2000, 0x3e7},{0xeff000, 0x3a2},{0xd2e000, 0x365},{0xb7c000, 0x32e},
    {0x9e5000, 0x2fc},{0x867000, 0x2d0},{0x6ff000, 0x2a8},{0x5ab800, 0x283},
    {0x46a000, 0x261},{0x339800, 0x243},{0x218800, 0x226},{0x105800, 0x20b},
    {0x3ffa000, 0x7a4},{0x3c29000, 0x700},{0x38aa000, 0x670},{0x3572000, 0x5f2},
    {0x3279000, 0x584},{0x2fb7000, 0x524},{0x2d26000, 0x4cc},{0x2ac0000, 0x47e},
    {0x2881000, 0x43a},{0x2665000, 0x3fa},{0x2468000, 0x3c2},{0x2287000, 0x38e},
    {0x20c1000, 0x35e},{0x1f12000, 0x332},{0x1d79000, 0x30a},{0x1bf4000, 0x2e6},
};

inline double frsqrte(double input)
{
    uint64_t x = __builtin_bit_cast(uint64_t, input);
    if ((x << 1) == 0)
    {
        x &= ~0x7FFFFFFFFFFFFFFFull;
        x |= 0x7FF0000000000000ull;
        return __builtin_bit_cast(double, x);
    }
    uint32_t e = (x >> 52) & 0x7FF;
    if (e == 0x7FF)
    {
        if ((x & ((1ull << 52) - 1)) == 0)
            return (int64_t)x < 0 ? __builtin_bit_cast(double, 0x7FF8000000000000ull) : 0.0;
        return __builtin_bit_cast(double, x);
    }
    if ((int64_t)x < 0)
        return __builtin_bit_cast(double, 0x7FF8000000000000ull);
    uint32_t idx = (x >> (52 - 5 + 1)) & 31;
    uint32_t stepMul = (x >> (52 - 5 + 1 - 11)) & ((1 << 11) - 1);
    int32_t sum = kFrsqrteLut[idx].offset - kFrsqrteLut[idx].step * stepMul;
    e = 1023 - ((e - 1021) >> 1);
    x &= ~(0x7FFull << 52);
    x |= (uint64_t)e << 52;
    x &= ~((1ull << 52) - 1ull);
    x += (uint64_t)sum << 26;
    return __builtin_bit_cast(double, x);
}

inline constexpr LutEntry kFresLut[32] = {
    {0x7ff800, 0x3e1},{0x783800, 0x3a7},{0x70ea00, 0x371},{0x6a0800, 0x340},
    {0x638800, 0x313},{0x5d6200, 0x2ea},{0x579000, 0x2c4},{0x520800, 0x2a0},
    {0x4cc800, 0x27f},{0x47ca00, 0x261},{0x430800, 0x245},{0x3e8000, 0x22a},
    {0x3a2c00, 0x212},{0x360800, 0x1fb},{0x321400, 0x1e5},{0x2e4a00, 0x1d1},
    {0x2aa800, 0x1be},{0x272c00, 0x1ac},{0x23d600, 0x19b},{0x209e00, 0x18b},
    {0x1d8800, 0x17c},{0x1a9000, 0x16e},{0x17ae00, 0x15b},{0x14f800, 0x15b},
    {0x124400, 0x143},{0xfbe00, 0x143},{0xd3800, 0x12d},{0xade00, 0x12d},
    {0x88400, 0x11a},{0x65000, 0x11a},{0x41c00, 0x108},{0x20c00, 0x106},
};

inline double fres(double input)
{
    uint64_t x = __builtin_bit_cast(uint64_t, input);
    uint32_t idx = (x >> (52 - 5)) & 31;
    uint32_t stepMul = (x >> (52 - 5 - 10)) & ((1 << 10) - 1);
    uint32_t sum = kFresLut[idx].offset - (kFresLut[idx].step * stepMul + 1) / 2;
    uint32_t e = (x >> 52) & 0x7FF;
    if (e == 0)
    {
        x |= 0x7FF0000000000000ull;
        return __builtin_bit_cast(double, x);
    }
    if (e == 0x7ff)
    {
        if ((x & ((1ull << 52) - 1)) == 0)
            return (int64_t)x < 0 ? __builtin_bit_cast(double, 0x8000000000000000ull) : 0.0;
        return __builtin_bit_cast(double, x);
    }
    e = 2045 - e;
    x &= ~(0x7FFull << 52);
    x |= (uint64_t)e << 52;
    x &= ~((1ull << 52) - 1ull);
    x += (uint64_t)sum << 29;
    return __builtin_bit_cast(double, x);
}

inline uint64_t fctiwz(double b)
{
    uint64_t v;
    if (b > (double)0x7FFFFFFF)
        v = 0x7FFFFFFF;
    else if (b < -(double)0x80000000)
        v = 0x80000000;
    else
        v = (uint64_t)(uint32_t)(int32_t)b;
    uint64_t r = 0xFFF8000000000000ULL | v;
    if (v == 0 && (__builtin_bit_cast(uint64_t, b) >> 63))
        r |= 0x100000000ull;
    return r;
}

constexpr float pow2f(int e)
{
    return __builtin_bit_cast(float, (uint32_t)(127 + e) << 23);
}
inline constexpr auto kLdScale = [] {
    struct { float v[64]; } t{};
    for (int s = 0; s < 64; s++) t.v[s] = pow2f(s < 32 ? -s : 64 - s);
    return t;
}();
inline constexpr auto kStScale = [] {
    struct { float v[64]; } t{};
    for (int s = 0; s < 64; s++) t.v[s] = pow2f(s < 32 ? s : -(64 - s));
    return t;
}();
inline float ld_scale(uint32_t s) { return kLdScale.v[s & 63]; }
inline float st_scale(uint32_t s) { return kStScale.v[s & 63]; }

inline float dequantize(uint32_t data, uint32_t type, uint32_t scale)
{
    switch (type)
    {
    case 4: return (float)(uint8_t)data * ld_scale(scale);
    case 5: return (float)(uint16_t)data * ld_scale(scale);
    case 6: return (float)(int8_t)data * ld_scale(scale);
    case 7: return (float)(int16_t)data * ld_scale(scale);
    default: return __builtin_bit_cast(float, data);
    }
}

inline uint32_t quantize(float data, uint32_t type, uint32_t scale)
{
    switch (type)
    {
    case 4: data *= st_scale(scale); if (data < 0) data = 0; if (data > 255) data = 255; return (uint8_t)(uint32_t)data;
    case 5: data *= st_scale(scale); if (data < 0) data = 0; if (data > 65535) data = 65535; return (uint16_t)(uint32_t)data;
    case 6: data *= st_scale(scale); if (data < -128) data = -128; if (data > 127) data = 127; return (uint8_t)(int32_t)data;
    case 7: data *= st_scale(scale); if (data < -32768) data = -32768; if (data > 32767) data = 32767; return (uint16_t)(int32_t)data;
    default: return __builtin_bit_cast(uint32_t, data);
    }
}

}
