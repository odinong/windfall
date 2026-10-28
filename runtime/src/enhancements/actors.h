#pragma once
// The game's actors (fopAc_ac_c): every live actor is on one list (g_fopAcTg_Queue). Offsets are WWHD's; after
// `orig` they are the GameCube decomp's plus 0x11C.

#include <cstdint>

#include "core/guest.h"

namespace wf::actors {

constexpr uint32_t kActorList = 0x101F3328;  // node_list_class: head node, tail node, count
constexpr uint32_t kTagNode = 0xC8;          // create_tag_class mAcTg: node {prev, list, next}, then the actor
constexpr uint32_t kProcName = 0x0E;         // s16 profile id
constexpr uint32_t kHome = 0x2EC, kCurrent = 0x314;  // actor_place: pos (3 f32), angle (3 s16), s8 room
constexpr uint32_t kPlaceRoom = 0x12;

inline bool valid_ptr(uint32_t a) { return a >= 0x10000000 && a < 0x50000000 && (a & 3) == 0; }

// calls fn(actor) for each live actor; stops early when fn returns false
template <typename Fn> void for_each(Fn&& fn)
{
    uint32_t node = rd32(kActorList);
    for (int guard = 0; valid_ptr(node) && guard < 4096; guard++)
    {
        uint32_t actor = rd32(node + 0xC);
        uint32_t next = rd32(node + 8);
        if (valid_ptr(actor) && !fn(actor)) return;
        node = next;
    }
}

inline uint16_t proc_name(uint32_t actor) { return rd16(actor + kProcName); }

}
