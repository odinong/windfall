#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace wf::mods {

struct Mod
{
    std::string folder;
    std::string name, author, version, description;
    std::filesystem::path dir;
    bool hasContent = false, hasPlugin = false;
};

const std::vector<Mod>& list();

void start();

std::filesystem::path resolve_content(const std::string& relPath, const std::filesystem::path& original);

void tick();

}
