#pragma once
#include <string>
#include <vector>

namespace wf::cheats {

struct Cheat
{
    std::string name, description, source;
};

const std::vector<Cheat>& list();
void apply_frame();

}
