#pragma once

#include <array>

namespace wf::game {

struct Camera
{
    std::array<float, 16> viewToWorld;
    std::array<float, 16> projection;
};

bool camera(Camera& out);

}
