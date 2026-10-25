#pragma once

#include "Common/platform.h"
#include "Common/version.h"

#define FMT_HEADER_ONLY
#include <fmt/core.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <fmt/xchar.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <bitset>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fstream>
#include <iostream>
#include <sstream>
#include <shared_mutex>
#include <thread>
#include <future>
#include <queue>
#include <stack>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <regex>
#include <ranges>
#include <variant>

#include <immintrin.h>

#include "wf_boost.h"

namespace fs = std::filesystem;
#define ARCH_X86_64

using uint64 = uint64_t;
using uint32 = uint32_t;
using uint16 = uint16_t;
using uint8 = uint8_t;
using sint64 = int64_t;
using sint32 = int32_t;
using sint16 = int16_t;
using sint8 = int8_t;
using MPTR = uint32_t;
#define MPTR_NULL (0)

inline uint64 _swapEndianU64(uint64 v) { return __builtin_bswap64(v); }
inline uint32 _swapEndianU32(uint32 v) { return __builtin_bswap32(v); }
inline sint32 _swapEndianS32(sint32 v) { return (sint32)__builtin_bswap32((uint32)v); }
inline uint16 _swapEndianU16(uint16 v) { return __builtin_bswap16(v); }
inline sint16 _swapEndianS16(sint16 v) { return (sint16)__builtin_bswap16((uint16)v); }

#include "Common/enumFlags.h"
#include "Common/betype.h"

using uint64le = uint64_t;
using uint32le = uint32_t;
using uint16le = uint16_t;
using uint8le = uint8_t;

#ifndef DEFINE_ENUM_FLAG_OPERATORS
#define DEFINE_ENUM_FLAG_OPERATORS(T)                                                                                                       \
    inline T operator~(T a) { return static_cast<T>(~static_cast<std::underlying_type<T>::type>(a)); }                                      \
    inline T operator|(T a, T b) { return static_cast<T>(static_cast<std::underlying_type<T>::type>(a) | static_cast<std::underlying_type<T>::type>(b)); } \
    inline T operator&(T a, T b) { return static_cast<T>(static_cast<std::underlying_type<T>::type>(a) & static_cast<std::underlying_type<T>::type>(b)); } \
    inline T operator^(T a, T b) { return static_cast<T>(static_cast<std::underlying_type<T>::type>(a) ^ static_cast<std::underlying_type<T>::type>(b)); } \
    inline T& operator|=(T& a, T b) { return reinterpret_cast<T&>(reinterpret_cast<std::underlying_type<T>::type&>(a) |= static_cast<std::underlying_type<T>::type>(b)); } \
    inline T& operator&=(T& a, T b) { return reinterpret_cast<T&>(reinterpret_cast<std::underlying_type<T>::type&>(a) &= static_cast<std::underlying_type<T>::type>(b)); } \
    inline T& operator^=(T& a, T b) { return reinterpret_cast<T&>(reinterpret_cast<std::underlying_type<T>::type&>(a) ^= static_cast<std::underlying_type<T>::type>(b)); }
#endif

template<typename T> inline T GetBits(T value, uint32 index, uint32 numBits)
{
    T mask = (1 << numBits) - 1;
    return (value >> index) & mask;
}
template<typename T> inline void SetBits(T& value, uint32 index, uint32 numBits, uint32 bitValue)
{
    T mask = (1 << numBits) - 1;
    value &= ~(mask << index);
    value |= (bitValue << index);
}

#define FORCE_INLINE inline __attribute__((always_inline))
#define UNREACHABLE __builtin_unreachable()
#define ASSUME(c) __builtin_assume(c)
#define DEBUG_BREAK __debugbreak()
#define TLS_WORKAROUND_NOINLINE __attribute__((noinline))
#define DLLEXPORT __declspec(dllexport)
#define NOEXPORT
#define ATTR_MS_ABI
#define TR_NOOP(str) str
inline std::string _tr(std::string_view text) { return std::string{text}; }
template<typename... TArgs> inline std::string _tr(fmt::format_string<TArgs...> text, TArgs... args) { return fmt::format(text, std::forward<TArgs>(args)...); }

inline uint64 _udiv128(uint64 hi, uint64 lo, uint64 divisor, uint64* remainder)
{
    unsigned __int128 d = (((unsigned __int128)hi) << 64) | lo;
    *remainder = (uint64)(d % divisor);
    return (uint64)(d / divisor);
}
FORCE_INLINE int BSF(uint32 v) { return __builtin_ctz(v); }

namespace wf {
void cemu_log_line(const std::string& s);
[[noreturn]] void cemu_fatal(const char* what);
}

enum class LogType : sint32
{
    Force = 0, Placeholder, APIErrors, GX2, Shader, TextureCache, TextureReadback, OpenGLLogging, VulkanValidation, Patches,
    SoundAPI, InputAPI, Socket, Save, H264, NN_NFP, NN_FP, PRUDP, UnsupportedAPI, CoreinitLogging, CoreinitFile,
    CoreinitThreadSync, CoreinitMem, CoreinitMP, CoreinitThread, NN_NIM, NN_ACT, NN_OLV, NN_SL, NFC, NTAG, Socket2,
};

namespace wf { bool cemu_log_all(); }
inline bool cemuLog_enabled(LogType t) { return t == LogType::Force || wf::cemu_log_all(); }
template<typename... T> inline bool cemuLog_log(LogType t, fmt::format_string<T...> f, T&&... a)
{
    if (!cemuLog_enabled(t)) return false;
    wf::cemu_log_line(fmt::format(f, std::forward<T>(a)...));
    return true;
}
inline bool cemuLog_log(LogType t, std::string_view s)
{
    if (!cemuLog_enabled(t)) return false;
    wf::cemu_log_line(std::string(s));
    return true;
}
template<typename... T> inline bool cemuLog_logDebug(LogType t, fmt::format_string<T...> f, T&&... a) { return cemuLog_log(t, f, std::forward<T>(a)...); }
template<typename... T> inline bool cemuLog_logDebugOnce(LogType t, fmt::format_string<T...> f, T&&... a) { return cemuLog_log(t, f, std::forward<T>(a)...); }
inline bool cemuLog_isLoggingEnabled(LogType) { return false; }
inline void cemuLog_waitForFlush() {}

inline void debug_printf(const char* fmt, ...) {}
inline void cemu_assert(bool c) { if (!c) wf::cemu_fatal("cemu_assert failed"); }
inline void cemu_assert_debug(bool) {}
inline void cemu_assert_unimplemented() {}
inline void cemu_assert_suspicious() {}
inline void cemu_assert_error() { wf::cemu_fatal("cemu_assert_error"); }
#define assert_dbg() ((void)0)

inline bool cemuLog_advancedPPCLoggingEnabled() { return false; }
template<typename... T> inline bool cemuLog_logOnce(LogType t, fmt::format_string<T...> f, T&&... a) { return cemuLog_log(t, f, std::forward<T>(a)...); }

struct PPCInterpreter_t;
extern uint8* memory_base;
inline uint8* memory_getPointerFromVirtualOffset(uint32 o) { return memory_base + o; }
inline uint8* memory_getPointerFromVirtualOffsetAllowNull(uint32 o) { return o ? memory_base + o : nullptr; }
inline uint8* memory_getPointerFromPhysicalOffset(uint32 o) { return memory_base + o; }
inline uint32 memory_getVirtualOffsetFromPointer(const void* p) { return p ? (uint32)((const uint8*)p - memory_base) : 0; }
inline uint32 memory_virtualToPhysical(uint32 v) { return v; }
inline uint32 memory_physicalToVirtual(uint32 p) { return p; }
inline uint32 memory_readU32(uint32 a) { return _swapEndianU32(*(uint32*)(memory_base + a)); }
inline uint16 memory_readU16(uint32 a) { return _swapEndianU16(*(uint16*)(memory_base + a)); }
inline uint8 memory_readU8(uint32 a) { return memory_base[a]; }
inline void memory_writeU32(uint32 a, uint32 v) { *(uint32*)(memory_base + a) = _swapEndianU32(v); }

#include "Common/MemPtr.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Espresso/PPCCallback.h"

template<typename T> void vectorAppendUnique(std::vector<T>& vec, const T& val)
{
    if (std::find(vec.begin(), vec.end(), val) == vec.end()) vec.emplace_back(val);
}
template<typename T> void vectorRemoveByValue(std::vector<T>& vec, const T& val)
{
    vec.erase(std::remove(vec.begin(), vec.end(), val), vec.end());
}
template<typename T> void vectorRemoveByIndex(std::vector<T>& vec, const size_t index) { vec.erase(vec.begin() + index); }
template<typename T1, typename... Types> bool match_any_of(T1&& value, Types&&... others) { return ((value == others) || ...); }

template<typename T1, typename T2> constexpr bool HAS_FLAG(T1 flags, T2 test_flag) { return (flags & (T1)test_flag) == (T1)test_flag; }
template<typename T1, typename T2> constexpr bool HAS_BIT(T1 value, T2 index) { return (value & ((T1)1 << index)) != 0; }
template<typename T> constexpr uint32_t ppcsizeof() { return (uint32_t)sizeof(T); }
static void debugBreakpoint() {}

[[nodiscard]] static std::chrono::high_resolution_clock::time_point now_cached() noexcept
{
    static const long long freq = _Query_perf_frequency();
    const long long ctr = _Query_perf_counter();
    return std::chrono::high_resolution_clock::time_point(std::chrono::nanoseconds((ctr / freq) * std::nano::den + (ctr % freq) * std::nano::den / freq));
}
[[nodiscard]] static std::chrono::steady_clock::time_point tick_cached() noexcept
{
    static const long long freq = _Query_perf_frequency();
    const long long ctr = _Query_perf_counter();
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds((ctr / freq) * std::nano::den + (ctr % freq) * std::nano::den / freq));
}
inline const char* _utf8WrapperPtr(const char8_t* input) { return (const char*)input; }
inline std::string_view _utf8Wrapper(std::u8string_view input) { return {(const char*)input.data(), input.size()}; }
inline std::string _pathToUtf8(const fs::path& path)
{
    std::u8string s = path.generic_u8string();
    return std::string((const char*)s.data(), s.size());
}
inline fs::path _utf8ToPath(std::string_view input) { return fs::path(std::u8string_view((const char8_t*)input.data(), input.size())); }
inline char _ansiToLower(char c) { return (c >= 'A' && c <= 'Z') ? c - ('A' - 'a') : c; }
class RunAtCemuBoot { public: RunAtCemuBoot(void (*f)()) { f(); } };
template<typename T> bool future_is_ready(std::future<T>& f) { return f.wait_for(std::chrono::nanoseconds(0)) == std::future_status::ready; }
template<typename T> std::atomic<T>* _rawPtrToAtomic(T* ptr) { return reinterpret_cast<std::atomic<T>*>(ptr); }
inline uint32 GetTitleIdHigh(uint64 titleId) { return titleId >> 32; }
inline uint32 GetTitleIdLow(uint64 titleId) { return titleId & 0xFFFFFFFF; }

namespace stdx {
template<typename EnumT> requires(std::is_enum_v<EnumT>) constexpr std::underlying_type_t<EnumT> to_underlying(EnumT e) noexcept
{
    return static_cast<std::underlying_type_t<EnumT>>(e);
}
template<typename T> using atomic_ref = std::atomic_ref<T>;
template<typename Fn> class scope_exit
{
    Fn m_func;
    bool m_released = false;
public:
    explicit scope_exit(Fn&& f) noexcept : m_func(std::forward<Fn>(f)) {}
    ~scope_exit() { if (!m_released) m_func(); }
    scope_exit(const scope_exit&) = delete;
    void release() { m_released = true; }
};
}

#define memcpy_dwords(d, s, n) memcpy((d), (s), (n) * sizeof(uint32))
#define memcpy_qwords(d, s, n) memcpy((d), (s), (n) * sizeof(uint64))

template<typename T> using underlying_t = typename std::underlying_type<T>::type;
template<typename Enum> requires std::is_enum_v<Enum> struct fmt::formatter<Enum> : fmt::formatter<underlying_t<Enum>>
{
    auto format(const Enum& e, format_context& ctx) const { return fmt::formatter<underlying_t<Enum>>::format(static_cast<underlying_t<Enum>>(e), ctx); }
};
template<typename T> struct fmt::formatter<betype<T>> : fmt::formatter<T>
{
    auto format(const betype<T>& v, format_context& ctx) const { return fmt::formatter<T>::format(v.value(), ctx); }
};
