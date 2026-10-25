#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace wf::gp {

struct Preset
{
    std::string name, category;
    bool isDefault = false;
    std::vector<std::pair<std::string, std::string>> vars;
};

struct Pack
{
    std::filesystem::path dir;
    std::string id;
    std::string name, description;
    std::vector<std::pair<std::string, std::string>> defaults;
    std::vector<Preset> presets;
    std::vector<std::string> categories;
    std::vector<std::map<std::string, std::string>> textureRules;
    int shaderCount = 0, dataPatchCount = 0, codePatchCount = 0;
    bool supported() const { return codePatchCount == 0; }
};

const std::vector<Pack>& packs();
const Pack* find(const std::string& id);
const Pack* resolution_pack();
constexpr const char* kMatchWindow = "Match window";

void activate();
void apply_data_patches();
// GPU thread, between frames: applies pack changes made in the menu
void frame_check();

const std::string* find_custom_shader(uint64_t baseHash, uint64_t auxHash, int type);

}
