#pragma once
#include <cstdint>
#include <string>

namespace wf::gpu {

void start();
std::string status();
uint32_t frame_count();
void save_screenshot();
void capture_frames();
void toggle_frame_hold();

}
