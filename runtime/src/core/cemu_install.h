#pragma once
#include <filesystem>
#include <optional>

namespace wf {

std::optional<std::filesystem::path> cemu_install_dir();

}
