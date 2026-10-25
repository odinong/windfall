#pragma once
#include <filesystem>
#include <string>

#include "wf/game_module.h"

namespace wf::game_module {

std::filesystem::path path();
std::filesystem::path built_dir();

std::string toolchain_build_id();

std::string load(const std::filesystem::path& dll);

void set_func_table(PPCFunc** table);
void set_interp_active(bool active);

struct OrigSlot
{
    OrigSlot(const char* name, PPCFunc** slot);
    const char* name;
    PPCFunc** slot;
    OrigSlot* next;
};

}

#define WF_ORIG(name) wf::PPCFunc* name = nullptr; static wf::game_module::OrigSlot name##_slot{#name, &name}
