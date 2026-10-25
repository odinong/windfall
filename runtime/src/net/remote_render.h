#pragma once

#include <cstdint>
#include <vector>

namespace wf::mp {

void note_palette(uint32_t obj, uint32_t dest, uint32_t size);

namespace rr_debug {
struct CopyInfo
{
    uint32_t key;
    uint16_t part;
    int player;
    bool rigid, extra, hidden;
    uint32_t rt;
    float x, y, z;
    std::vector<float> bones;
    float m[12];
};
uint32_t snapshot(std::vector<CopyInfo>& out);
bool hidden(uint32_t key);
void set_hidden(uint32_t key, bool on);
}

}

int wf_rr_draw_tag();

int wf_rr_gpu_prepare(uint32_t* regs, uint32_t vsUniformStart, uint32_t gpuFrame, uint32_t psTexStart,
                      uint32_t psTexMask, uint32_t vsAttribStart, uint32_t drawCount);
int wf_rr_gpu_bind(int i);
bool wf_rr_gpu_restore();
bool wf_rr_gpu_wants_record();
void wf_rr_gpu_record(const uint32_t* regs, uint32_t regCount, const uint32_t* params);
int wf_rr_gpu_extras_begin(const uint32_t* regs);
bool wf_rr_gpu_extra_apply(int j, uint32_t* regs, uint32_t* params);
