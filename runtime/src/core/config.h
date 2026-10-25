#pragma once
#include <filesystem>

namespace wf {

struct Config
{
    std::filesystem::path gameDir;
    std::filesystem::path dataDir;
    std::filesystem::path saveDir;
};

Config& config();

}
