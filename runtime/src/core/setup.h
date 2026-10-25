#pragma once
#include <filesystem>
#include <string>

namespace wf::setup {

std::string verify(const std::filesystem::path& dir);

std::filesystem::path find_game_dir(const std::filesystem::path& fromCommandLine);

bool build_screen(const std::filesystem::path& gameDir, const std::string& reason);

}
