#pragma once
#include <filesystem>
#include <map>
#include <string>

namespace wf::rando {

std::filesystem::path game_dir();
bool enabled();
void start();
bool playing();
void restart_game();
void open_spoiler_log();

struct Status
{
    bool running = false;
    bool ok = false;
    std::string message;
    std::string seed;
};
bool generator_available();
void generate(const std::string& seed, const std::map<std::string, std::string>& options);
Status status();
std::filesystem::path spoiler_log();
std::filesystem::path work_dir();

}
