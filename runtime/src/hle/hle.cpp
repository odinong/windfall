#include "hle/hle.h"

#include <map>
#include <mutex>
#include <string>

#include "core/log.h"
#include "wf/recomp_api.h"

namespace wf {
void register_import(uint32_t id, PPCFunc* fn);
}

namespace wf::hle {

namespace {

using Key = std::pair<std::string, std::string>;

std::map<Key, PPCFunc*>& functions()
{
    static std::map<Key, PPCFunc*> m;
    return m;
}
std::map<Key, void (*)(uint32_t)>& data_inits()
{
    static std::map<Key, void (*)(uint32_t)> m;
    return m;
}

std::mutex s_thunkLock;
uint32_t s_nextThunk = gmem::kRuntimeThunkBase;

}

void register_function(const char* module, const char* name, PPCFunc* fn)
{
    functions()[{module, name}] = fn;
}

PPCFunc* find_function(const char* module, const char* name)
{
    auto it = functions().find({module, name});
    return it == functions().end() ? nullptr : it->second;
}

void register_data_init(const char* module, const char* name, void (*init)(uint32_t))
{
    data_inits()[{module, name}] = init;
}

uint32_t make_thunk(PPCFunc* fn)
{
    std::lock_guard lk(s_thunkLock);
    if (s_nextThunk >= gmem::kRuntimeThunkEnd)
        fatal("out of runtime thunk slots");
    uint32_t a = s_nextThunk;
    s_nextThunk += 8;
    wr32(a, 0x44000002);
    wr32(a + 4, 0x4E800020);
    g_funcTable[(a - kCodeBase) / 4] = fn;
    return a;
}

uint32_t call_guest(PPCContext& ctx, uint32_t func, std::initializer_list<uint32_t> args)
{
    uint32_t savedLr = ctx.lr;
    int r = 3;
    for (uint32_t a : args)
        ctx.r[r++] = a;
    ctx.lr = 0;
    call_indirect(ctx, g_base, func, 0);
    ctx.lr = savedLr;
    return ctx.r[3];
}

void bind_imports()
{
    uint32_t funcs = 0, implemented = 0, data = 0;
    for (uint32_t i = 0; i < g_importCount; i++)
    {
        const ImportInfo& imp = g_imports[i];
        if (imp.isFunc)
        {
            funcs++;
            if (PPCFunc* fn = find_function(imp.module, imp.name))
            {
                register_import(imp.id, fn);
                implemented++;
            }
        }
        else
        {
            data++;
            auto it = data_inits().find({imp.module, imp.name});
            if (it != data_inits().end())
                it->second(imp.guest);
            else
                log_warn("no initializer for data import {}::{}", imp.module, imp.name);
        }
    }
    log_info("imports: {}/{} functions implemented, {} data imports", implemented, funcs, data);
    for (uint32_t i = 0; i < g_importCount; i++)
        if (g_imports[i].isFunc && !find_function(g_imports[i].module, g_imports[i].name))
            log_cat("hle", "  missing {}::{}", g_imports[i].module, g_imports[i].name);
}

}
