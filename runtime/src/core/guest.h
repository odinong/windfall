#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

namespace wf {

extern uint8_t* g_base;

namespace gmem {
inline constexpr uint32_t kCodeBase = 0x02000000, kCodeSize = 0x00930000;
inline constexpr uint32_t kRuntimeThunkBase = 0x02918000;
inline constexpr uint32_t kRuntimeThunkEnd = 0x02920000;
inline constexpr uint32_t kMem2Base = 0x10000000, kMem2End = 0x50000000;
inline constexpr uint32_t kSysAreaBase = 0x10500000, kSysAreaEnd = 0x10800000;
inline constexpr uint32_t kMem2HeapBase = 0x10800000;
inline constexpr uint32_t kFgBucketBase = 0xE0000000, kFgBucketSize = 0x04000000;
inline constexpr uint32_t kMem1Base = 0xF4000000, kMem1Size = 0x02000000;
inline constexpr uint32_t kPerCorePage = 0xFFFFF000;
}

template<class T> inline T bswap(T v)
{
    if constexpr (sizeof(T) == 1) return v;
    else if constexpr (sizeof(T) == 2) { uint16_t x; memcpy(&x, &v, 2); x = __builtin_bswap16(x); memcpy(&v, &x, 2); return v; }
    else if constexpr (sizeof(T) == 4) { uint32_t x; memcpy(&x, &v, 4); x = __builtin_bswap32(x); memcpy(&v, &x, 4); return v; }
    else { static_assert(sizeof(T) == 8); uint64_t x; memcpy(&x, &v, 8); x = __builtin_bswap64(x); memcpy(&v, &x, 8); return v; }
}

template<class T> struct be
{
    T raw;
    be() = default;
    be(T v) : raw(bswap(v)) {}
    operator T() const { return bswap(raw); }
    be& operator=(T v) { raw = bswap(v); return *this; }
    T get() const { return bswap(raw); }
};
using be16 = be<uint16_t>;
using be32 = be<uint32_t>;
using bes32 = be<int32_t>;
using be64 = be<uint64_t>;

template<class T = uint8_t> inline T* gptr(uint32_t addr) { return reinterpret_cast<T*>(g_base + addr); }
inline uint32_t gaddr(const void* p) { return (uint32_t)((const uint8_t*)p - g_base); }

template<class T> struct GuestPtr
{
    uint32_t addr = 0;
    GuestPtr() = default;
    explicit GuestPtr(uint32_t a) : addr(a) {}
    T* get() const { return addr ? gptr<T>(addr) : nullptr; }
    T* operator->() const { return gptr<T>(addr); }
    T& operator*() const { return *gptr<T>(addr); }
    explicit operator bool() const { return addr != 0; }
};

inline uint32_t rd32(uint32_t a) { return bswap(*gptr<uint32_t>(a)); }
inline uint16_t rd16(uint32_t a) { return bswap(*gptr<uint16_t>(a)); }
inline uint8_t rd8(uint32_t a) { return *gptr<uint8_t>(a); }
inline uint64_t rd64(uint32_t a) { return bswap(*gptr<uint64_t>(a)); }
inline void wr32(uint32_t a, uint32_t v) { *gptr<uint32_t>(a) = bswap(v); }
inline void wr16(uint32_t a, uint16_t v) { *gptr<uint16_t>(a) = bswap(v); }
inline void wr8(uint32_t a, uint8_t v) { *gptr<uint8_t>(a) = v; }
inline void wr64(uint32_t a, uint64_t v) { *gptr<uint64_t>(a) = bswap(v); }

inline std::string read_cstr(uint32_t a, size_t max = 4096)
{
    if (!a) return {};
    const char* p = gptr<const char>(a);
    return std::string(p, strnlen(p, max));
}

void memory_init();
uint32_t sys_alloc(uint32_t size, uint32_t align = 16);
bool is_mapped(uint32_t addr, uint32_t size);

}
