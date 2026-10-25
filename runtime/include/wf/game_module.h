#pragma once
#include "recomp_api.h"

namespace wf {

inline constexpr uint32_t kGameModuleVersion = 1;

struct NamedFunc
{
    const char* name;
    PPCFunc* fn;
};

struct RuntimeApi
{
    uint32_t version;
    uint32_t contextSize;
    void (*bad_indirect_call)(PPCContext& ctx, uint32_t target, uint32_t from);
    void (*yield)(PPCContext& ctx, uint8_t* base);
    void (*trap)(PPCContext& ctx, uint8_t* base, uint32_t addr);
    void (*call_import)(PPCContext& ctx, uint8_t* base, uint32_t id);
    void (*probe)(PPCContext& ctx, uint8_t* base, uint32_t addr, const char* name);
    void (*indirect_miss)(PPCContext& ctx, uint8_t* base, uint32_t target, uint32_t from);
    bool (*interp_entry)(PPCContext& ctx, uint8_t* base, uint32_t addr);
    const NamedFunc* hooks;
};

struct GameModule
{
    uint32_t version;
    uint32_t contextSize;
    const char* buildId;
    const char* missingHook;
    const FuncMapping* funcMappings;
    const ImportInfo* imports;
    uint32_t importCount;
    const ImageInfo* image;
    const NamedFunc* origs;
    PPCFunc*** funcTable;
    bool* interpActive;
};

using GameModuleEntry = const GameModule* (*)(const RuntimeApi* api);

}
