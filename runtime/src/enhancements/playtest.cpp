#include "enhancements/playtest.h"

#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/log.h"
#include "gpu/gpu.h"
#include "core/guest.h"

namespace wf::playtest {
namespace {

constexpr uint32_t kStartStage = 0x104741E4, kNextStage = 0x104741F0, kNextEnable = 0x104741FC;
constexpr uint8_t kOutsetRoom = 44;
constexpr uint16_t kOutsetDockPoint = 0;

enum Phase : int { kMenus, kPrologue, kWarp, kArriving, kDone };
std::atomic<bool> g_enabled{false};
std::atomic<int> g_phase{kMenus};
std::atomic<uint32_t> g_arrivalFrame{0};
uint32_t g_arrivedTicks = 0;

std::string stage_name(uint32_t addr)
{
    char name[9] = {};
    for (int i = 0; i < 8; i++) name[i] = (char)rd8(addr + i);
    return name;
}

bool new_game_start()
{
    static const bool keep = std::getenv("WF_PLAYTEST_POINT") && std::strcmp(std::getenv("WF_PLAYTEST_POINT"), "new") == 0;
    return keep;
}

uint16_t spawn_point()
{
    static const char* point = std::getenv("WF_PLAYTEST_POINT");
    return point ? (uint16_t)std::atoi(point) : kOutsetDockPoint;
}

void set_phase(Phase p, const char* what)
{
    g_phase = p;
    log_info("playtest: {}", what);
}

void debug_player(bool arriving)
{
    constexpr uint32_t kPlayerPtr = 0x1047428C, kOld = 0x300, kPos = 0x314, kAngle = 0x320, kShapeAngle = 0x328;
    static const bool posLog = std::getenv("WF_POSLOG") != nullptr;
    static const char* teleport = std::getenv("WF_TELEPORT");
    if (!posLog && !teleport) return;
    static uint32_t s_ticks = 0;
    if (arriving)
    {
        s_ticks = 0;
        return;
    }
    s_ticks++;
    uint32_t pl = rd32(kPlayerPtr);
    if (!pl) return;
    auto f32 = [](uint32_t a) { return std::bit_cast<float>(rd32(a)); };
    if (posLog && s_ticks % 30 == 0)
        log_info("player tick {} pos {:.0f} {:.0f} {:.0f} angle {}", s_ticks, f32(pl + kPos), f32(pl + kPos + 4), f32(pl + kPos + 8),
                 (int16_t)rd16(pl + kShapeAngle + 2));
    for (const char* p = teleport; p && *p;)
    {
        unsigned tick = 0;
        float x, y, z;
        int angle = 0;
        if (std::sscanf(p, "%u:%f,%f,%f,%d", &tick, &x, &y, &z, &angle) >= 4 && tick == s_ticks)
        {
            for (uint32_t at : {kPos, kOld})
            {
                wr32(pl + at, std::bit_cast<uint32_t>(x));
                wr32(pl + at + 4, std::bit_cast<uint32_t>(y));
                wr32(pl + at + 8, std::bit_cast<uint32_t>(z));
            }
            wr16(pl + kAngle + 2, (uint16_t)angle);
            wr16(pl + kShapeAngle + 2, (uint16_t)angle);
            log_info("teleport: player to {:.0f} {:.0f} {:.0f} angle {}", x, y, z, angle);
        }
        p = std::strchr(p, ';');
        if (p) p++;
    }
}

}

bool enabled() { return g_enabled || std::getenv("WF_PLAYTEST"); }
void set_enabled() { g_enabled = true; }
bool driving() { return enabled() && g_phase != kDone; }
uint32_t arrival_frame() { return g_arrivalFrame; }

void apply(input::ProState& s, bool& fastForward)
{
    if (!driving()) return;
    fastForward = true;
    s = input::ProState{};
    uint32_t f = gpu::frame_count();
    if (f % 16 >= 4) return;
    switch (g_phase.load())
    {
    case kMenus:
        s.buttons = (f / 16) % 3 == 2 ? input::kPlus : input::kA;
        break;
    case kPrologue:
        s.buttons = input::kPlus;
        break;
    default:
        break;
    }
}

void tick()
{
    debug_player(driving());
    if (!driving()) return;
    std::string start = stage_name(kStartStage), next = stage_name(kNextStage);
    switch (g_phase.load())
    {
    case kMenus:
        if (start == "Name" && next == "sea") set_phase(kPrologue, "new game created, skipping the prologue");
        break;
    case kPrologue:
        if (start == "sea" && new_game_start()) set_phase(kDone, "on Outset at the new game's start");
        else if (start == "sea") set_phase(kWarp, "on Outset, going to the dock");
        break;
    case kWarp:
        if (rd8(kNextEnable)) break;
        wr16(kNextStage + 8, spawn_point());
        wr8(kNextStage + 10, kOutsetRoom);
        wr8(kNextStage + 11, 0xFF);
        wr8(kNextEnable, 1);
        g_arrivedTicks = 0;
        set_phase(kArriving, "stage change requested");
        break;
    case kArriving:
        if (start == "sea" && rd16(kStartStage + 8) == spawn_point() && rd8(kStartStage + 10) == kOutsetRoom && !rd8(kNextEnable))
        {
            if (++g_arrivedTicks == 1) g_arrivalFrame = gpu::frame_count();
            if (g_arrivedTicks >= 45) set_phase(kDone, "ready on Outset Island - have fun");
        }
        else
            g_arrivedTicks = 0;
        break;
    default:
        break;
    }
}

}
