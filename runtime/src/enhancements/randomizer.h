#pragma once
#include <filesystem>
#include <map>
#include <string>

namespace wf::rando {

std::filesystem::path game_dir();
bool enabled();
void start();
bool playing();

// a randomized world: the seed and options it was generated from (the same pair gives the same game)
struct World
{
    std::string seed;
    std::map<std::string, std::string> options;
};
// this session's world as text (lines "seed=..." and "key=value"), empty when playing the original game
const std::string& playing_world();
std::string world_text(const World& w);
World parse_world(const std::string& text);
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
