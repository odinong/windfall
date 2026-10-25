#pragma once

#define MEMORY_TILINGAPERTURE_AREA_ADDR (0xE8000000)
#define MEMORY_TILINGAPERTURE_AREA_SIZE (0x02000000)

inline uint64 memory_readU64(uint32 a) { return _swapEndianU64(*(uint64*)(memory_base + a)); }
inline void memory_writeU64(uint32 a, uint64 v) { *(uint64*)(memory_base + a) = _swapEndianU64(v); }
inline void memory_writeU16(uint32 a, uint16 v) { *(uint16*)(memory_base + a) = _swapEndianU16(v); }
inline void memory_writeU8(uint32 a, uint8 v) { memory_base[a] = v; }
template<typename T> inline T memory_read(uint32 a)
{
    T v;
    memcpy(&v, memory_base + a, sizeof(T));
    if constexpr (sizeof(T) == 2) return (T)_swapEndianU16((uint16)v);
    else if constexpr (sizeof(T) == 4) return (T)_swapEndianU32((uint32)v);
    else if constexpr (sizeof(T) == 8) return (T)_swapEndianU64((uint64)v);
    else return v;
}

struct MMURange
{
    uint32 baseAddress, size;
    bool containsAddress(uint32 addr) const { return addr >= baseAddress && addr < baseAddress + size; }
    uint32 getBase() const { return baseAddress; }
    uint32 getEnd() const { return baseAddress + size; }
};
inline MMURange mmuRange_MEM1{0xF4000000, 0x02000000};
