#pragma once
#include <cstdint>
#include <tuple>
#include <type_traits>

#include "core/guest.h"
#include "wf/ppc.h"

namespace wf::hle {

using wf::PPCContext;

void register_function(const char* module, const char* name, PPCFunc* fn);
PPCFunc* find_function(const char* module, const char* name);
void register_data_init(const char* module, const char* name, void (*init)(uint32_t slot));
void bind_imports();

uint32_t make_thunk(PPCFunc* fn);

uint32_t call_guest(PPCContext& ctx, uint32_t func, std::initializer_list<uint32_t> args = {});

template<class T> struct is_guest_ptr : std::false_type {};
template<class T> struct is_guest_ptr<GuestPtr<T>> : std::true_type {};

struct ArgReader
{
    PPCContext& ctx;
    int gpr = 3, fpr = 1, stack = 0;

    uint32_t next_word()
    {
        if (gpr <= 10) return ctx.r[gpr++];
        return rd32(ctx.r[1] + 8 + 4 * stack++);
    }

    template<class T> T get()
    {
        if constexpr (std::is_same_v<T, PPCContext&>)
            return ctx;
        else if constexpr (std::is_floating_point_v<T>)
            return (T)ctx.f[fpr++].ps0;
        else if constexpr (sizeof(T) == 8)
        {
            if (gpr <= 10 && !(gpr & 1)) gpr++;
            uint64_t hi = next_word(), lo = next_word();
            return (T)((hi << 32) | lo);
        }
        else if constexpr (is_guest_ptr<T>::value)
            return T(next_word());
        else if constexpr (std::is_pointer_v<T>)
        {
            uint32_t a = next_word();
            return a ? gptr<std::remove_pointer_t<T>>(a) : nullptr;
        }
        else
            return (T)next_word();
    }
};

template<class R> void set_return(PPCContext& ctx, R v)
{
    if constexpr (std::is_floating_point_v<R>)
    {
        ctx.f[1].ps0 = (double)v;
        if constexpr (std::is_same_v<R, float>) ctx.f[1].ps1 = (double)v;
    }
    else if constexpr (sizeof(R) == 8)
    {
        ctx.r[3] = (uint32_t)((uint64_t)v >> 32);
        ctx.r[4] = (uint32_t)(uint64_t)v;
    }
    else if constexpr (is_guest_ptr<R>::value)
        ctx.r[3] = v.addr;
    else if constexpr (std::is_pointer_v<R>)
        ctx.r[3] = v ? gaddr(v) : 0;
    else
        ctx.r[3] = (uint32_t)(int64_t)v;
}

template<class F> struct fn_traits;
template<class R, class... A> struct fn_traits<R (*)(A...)>
{
    using ret = R;
    using args = std::tuple<A...>;
};

template<auto F> void thunk_impl(PPCContext& __restrict ctx, uint8_t* __restrict)
{
    using T = fn_traits<decltype(F)>;
    ArgReader rd{ctx};
    auto call = [&]<class... A>(std::tuple<A...>*) -> decltype(auto) {
        std::tuple<A...> args{rd.get<A>()...};
        return std::apply(F, std::move(args));
    };
    if constexpr (std::is_void_v<typename T::ret>)
        call((typename T::args*)nullptr);
    else
        set_return(ctx, call((typename T::args*)nullptr));
}

template<auto F> constexpr PPCFunc* wrap() { return &thunk_impl<F>; }

struct Registrar
{
    Registrar(const char* m, const char* n, PPCFunc* f) { register_function(m, n, f); }
};
struct DataRegistrar
{
    DataRegistrar(const char* m, const char* n, void (*init)(uint32_t)) { register_data_init(m, n, init); }
};

}

#define WF_EXPORT(module, fn) \
    static ::wf::hle::Registrar s_reg_##module##_##fn(#module, #fn, ::wf::hle::wrap<&fn>())
#define WF_EXPORT_AS(module, name, fn) \
    static ::wf::hle::Registrar s_reg_##module##_##name(#module, #name, ::wf::hle::wrap<&fn>())
#define WF_EXPORT_RAW(module, name, fn) \
    static ::wf::hle::Registrar s_reg_##module##_##name(#module, #name, &fn)
#define WF_DATA(module, name, initfn) \
    static ::wf::hle::DataRegistrar s_data_##module##_##name(#module, #name, initfn)
