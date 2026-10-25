#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace wf::archive {

bool is_yaz0(const std::vector<uint8_t>& d);
std::vector<uint8_t> yaz0_decompress(const std::vector<uint8_t>& d);
std::vector<uint8_t> yaz0_store(const std::vector<uint8_t>& d);
std::vector<uint8_t> yaz0_compress(const std::vector<uint8_t>& d);

struct SarcFile
{
    std::string name;
    std::vector<uint8_t> data;
    uint32_t alignment = 0x2000;
};
struct Sarc
{
    bool bigEndian = true;
    uint32_t hashKey = 0x65;
    std::vector<SarcFile> files;
    SarcFile* find(const std::string& name);
};

bool sarc_read(const std::vector<uint8_t>& d, Sarc& out);
std::vector<uint8_t> sarc_write(const Sarc& s);

}
