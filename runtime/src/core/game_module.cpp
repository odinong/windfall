#include "core/game_module.h"

#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>

#include <shlobj.h>
#include <windows.h>

#include "core/log.h"
#include "cpu/interp.h"

PPC_FUNC(hook_task_calc);
PPC_FUNC(hook_ubo_write);
PPC_FUNC(hook_task_node);
PPC_FUNC(hook_skin_palette);
PPC_FUNC(hook_wait_vsync);
PPC_FUNC(hook_particle_position);
PPC_FUNC(hook_shape_matrix);
PPC_FUNC(hook_particle_manager_calc);
PPC_FUNC(hook_frame_ctrl_update);
PPC_FUNC(hook_draw_handler);

namespace wf {

const FuncMapping* g_funcMappings = nullptr;
const ImportInfo* g_imports = nullptr;
uint32_t g_importCount = 0;
ImageInfo g_image{};

void yield(PPCContext& ctx, uint8_t* base);
void indirect_miss(PPCContext& ctx, uint8_t* base, uint32_t target, uint32_t from);

namespace game_module {
namespace {

const NamedFunc kHooks[] = {
    {"hook_task_calc", hook_task_calc},
    {"hook_ubo_write", hook_ubo_write},
    {"hook_task_node", hook_task_node},
    {"hook_skin_palette", hook_skin_palette},
    {"hook_wait_vsync", hook_wait_vsync},
    {"hook_particle_position", hook_particle_position},
    {"hook_shape_matrix", hook_shape_matrix},
    {"hook_particle_manager_calc", hook_particle_manager_calc},
    {"hook_frame_ctrl_update", hook_frame_ctrl_update},
    {"hook_draw_handler", hook_draw_handler},
    {nullptr, nullptr},
};

OrigSlot* s_origs = nullptr;
const GameModule* s_module = nullptr;

std::filesystem::path exe_dir()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

}

OrigSlot::OrigSlot(const char* n, PPCFunc** s) : name(n), slot(s), next(s_origs) { s_origs = this; }

std::filesystem::path built_dir()
{
    PWSTR p = nullptr;
    std::filesystem::path r;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p) == S_OK)
        r = std::filesystem::path(p) / L"WWHDRecomp" / L"game_module";
    CoTaskMemFree(p);
    return r;
}

std::filesystem::path path()
{
    if (const wchar_t* e = _wgetenv(L"WF_GAME_MODULE")) return e;
    std::filesystem::path dev = exe_dir() / "windfall_game.dll";
    if (std::filesystem::exists(dev)) return dev;
    return built_dir() / "windfall_game.dll";
}

std::string toolchain_build_id()
{
    std::ifstream f(exe_dir() / "toolchain" / "BUILD_ID");
    std::string id;
    f >> id;
    return id;
}

std::string load(const std::filesystem::path& dll)
{
    if (!std::filesystem::exists(dll)) return "the game code hasn't been built yet";
    HMODULE h = LoadLibraryW(dll.c_str());
    if (!h) return std::format("couldn't load {} (error {})", dll.string(), GetLastError());
    auto entry = (GameModuleEntry)GetProcAddress(h, "wf_game_module");
    if (!entry)
    {
        FreeLibrary(h);
        return std::format("{} is not a Windfall game module", dll.string());
    }
    static const RuntimeApi api = {
        kGameModuleVersion, sizeof(PPCContext),
        [](PPCContext& ctx, uint32_t target, uint32_t from) { bad_indirect_call(ctx, target, from); },
        yield, trap, call_import, probe, indirect_miss, interp::entry, kHooks,
    };
    const GameModule* m = entry(&api);
    std::string why;
    if (m->version != kGameModuleVersion || m->contextSize != sizeof(PPCContext))
        why = "the game code was built for another version of Windfall";
    else if (std::string want = toolchain_build_id(); !want.empty() && want != m->buildId)
        why = std::format("the game code was built by another version of Windfall ({}, this one is {})", m->buildId, want);
    else if (m->missingHook)
        why = std::format("the game code needs {}, which this version of Windfall doesn't have", m->missingHook);
    for (OrigSlot* o = s_origs; o && why.empty(); o = o->next)
    {
        for (const NamedFunc* f = m->origs; f->name; f++)
            if (std::strcmp(f->name, o->name) == 0) *o->slot = f->fn;
        if (!*o->slot) why = std::format("the game code has no {}", o->name);
    }
    if (!why.empty())
    {
        FreeLibrary(h);
        return why;
    }
    s_module = m;
    g_funcMappings = m->funcMappings;
    g_imports = m->imports;
    g_importCount = m->importCount;
    g_image = *m->image;
    log_info("game module {} (build {})", dll.string(), m->buildId);
    return {};
}

void set_func_table(PPCFunc** table)
{
    if (s_module) *s_module->funcTable = table;
}

void set_interp_active(bool active)
{
    if (s_module) __atomic_store_n(s_module->interpActive, active, __ATOMIC_RELAXED);
}

}
}
