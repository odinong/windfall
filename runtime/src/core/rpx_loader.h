#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace wf {

struct RpxLoadResult
{
    uint32_t dataEnd = 0;
    uint32_t relocations = 0;
};

RpxLoadResult load_rpx(const std::filesystem::path& path);
RpxLoadResult load_rpx_patched(const std::filesystem::path& original, const std::filesystem::path& patched);
std::string file_sha1(const std::filesystem::path& path);

}
