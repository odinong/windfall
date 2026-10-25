#pragma once
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>

namespace wf::game_build {

bool available();

struct Progress
{
    std::atomic<int> stage{0};
    std::atomic<int> done{0}, total{0};
    std::atomic<bool> finished{false}, failed{false}, cancel{false};
    std::atomic<double> translateSeconds{0};
    std::mutex lock;
    std::string error;
};

void start(const std::filesystem::path& gameDir, Progress& p);

}
