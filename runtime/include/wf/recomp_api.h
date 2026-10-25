#pragma once
#include "ppc.h"

namespace wf {

struct ImportInfo
{
    uint32_t id;
    const char* module;
    const char* name;
    bool isFunc;
    uint32_t guest;
};

struct ImageInfo
{
    uint32_t entry;
    uint32_t sdaBase;
    uint32_t sda2Base;
    uint32_t stackSize;
    const char* sha1;
};

#if WF_FREESTANDING
extern const FuncMapping g_funcMappings[];
extern const ImportInfo g_imports[];
extern const uint32_t g_importCount;
extern const ImageInfo g_image;
#else
extern const FuncMapping* g_funcMappings;
extern const ImportInfo* g_imports;
extern uint32_t g_importCount;
extern ImageInfo g_image;
#endif

}
