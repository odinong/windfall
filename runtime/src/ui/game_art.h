#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace wf::ui::art {

struct Image
{
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

bool load();
const Image* get(const std::string& name);
void* texture(const std::string& name);
void* texture_tinted(const std::string& name, uint32_t dark, uint32_t light);

}
