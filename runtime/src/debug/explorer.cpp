#include "Common/precompiled.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "imgui/imgui_extension.h"

#include "debug/explorer.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/guest.h"
#include "core/log.h"
#include "enhancements/camera.h"
#include "net/multiplayer.h"
#include "net/remote_render.h"

namespace wf::explorer {
namespace {

constexpr uint32_t kPlayerPtr = 0x1047428C;
constexpr uint32_t kActorPos = 0x314, kActorAngle = 0x320, kActorRoom = 0x326, kActorShape = 0x328;
constexpr uint32_t kStartStage = 0x104741E4;
constexpr uint32_t kCbColor0Base = 0xA010, kCbTargetMask = 0xA08E;
constexpr uint32_t kVsUniformStart = 0xE000 + 0x7E0, kVsAttribStart = 0xE000 + 0x8C0, kPsTexStart = 0xE000;

std::atomic<bool> g_open{false}, g_labels{false};
std::mutex g_eventLock;
std::vector<SDL_Event> g_events;

bool readable(uint32_t a, uint32_t n = 4) { return a >= gmem::kMem2Base && a + n <= gmem::kMem2End; }
float rdf(uint32_t a) { return std::bit_cast<float>(rd32(a)); }

struct Draw
{
    uint32_t index, rt, vb, vbSize, count, instances, tex0;
    uint32_t blocks[6];
    bool depthOnly, hidden;
    int tag, copies;
    uint32_t mesh() const { return (vbSize << 12) ^ count; }
};
std::mutex g_drawLock;
std::vector<Draw> g_drawCur, g_drawLast;
uint32_t g_drawFrame = ~0u, g_drawLastFrame = 0;
std::unordered_set<uint32_t> g_hiddenMeshes;
std::atomic<bool> g_capture{false};
bool g_freeze = false;

uint32_t g_dumpFirst = 0, g_dumpLast = 0;
std::string g_dumpDir = ".";

void init_once()
{
    static bool done = false;
    if (done) return;
    done = true;
    if (const char* e = std::getenv("WF_EXPLORER"); e && *e == '1') g_open = true;
    if (const char* e = std::getenv("WF_EXPLORER_LABELS"); e && *e == '1') g_labels = true;
    if (const char* e = std::getenv("WF_EXPLORER_HIDE"))
    {
        std::lock_guard lk(g_drawLock);
        for (const char* q = e; *q;)
        {
            char* end;
            g_hiddenMeshes.insert((uint32_t)std::strtoul(q, &end, 16));
            q = *end ? end + 1 : end;
        }
        g_capture = true;
    }
    if (const char* e = std::getenv("WF_EXPLORER_DUMP"))
    {
        char dir[512] = "";
        if (std::sscanf(e, "%u-%u:%511s", &g_dumpFirst, &g_dumpLast, dir) >= 2 && dir[0]) g_dumpDir = dir;
        std::error_code ec;
        std::filesystem::create_directories(g_dumpDir, ec);
        g_capture = true;
    }
}

bool project(const game::Camera& cam, float x, float y, float z, float& sx, float& sy)
{
    const auto& c = cam.viewToWorld;
    float dx = x - c[3], dy = y - c[7], dz = z - c[11];
    float vx = c[0] * dx + c[4] * dy + c[8] * dz;
    float vy = c[1] * dx + c[5] * dy + c[9] * dz;
    float vz = c[2] * dx + c[6] * dy + c[10] * dz;
    const auto& p = cam.projection;
    float cx = p[0] * vx + p[1] * vy + p[2] * vz + p[3];
    float cy = p[4] * vx + p[5] * vy + p[6] * vz + p[7];
    float cw = p[12] * vx + p[13] * vy + p[14] * vz + p[15];
    if (cw < 1.0f) return false;
    sx = cx / cw * 0.5f + 0.5f;
    sy = 0.5f - cy / cw * 0.5f;
    return true;
}

struct Actor { uint32_t addr; int16_t id; float x, y, z; float dist; };
uint32_t g_tagOffset = 0;

uint32_t find_tag_offset(uint32_t player)
{
    for (uint32_t off = 0; off < 0x300; off += 4)
    {
        if (rd32(player + off + 12) != player) continue;
        uint32_t next = rd32(player + off + 8), prev = rd32(player + off);
        if ((next && !readable(next, 16)) || (prev && !readable(prev, 16))) continue;
        if (!next && !prev) continue;
        return off;
    }
    return 0;
}

void list_actors(std::vector<Actor>& out)
{
    out.clear();
    uint32_t player = rd32(kPlayerPtr);
    if (!readable(player, 0x400)) return;
    if (!g_tagOffset) g_tagOffset = find_tag_offset(player);
    if (!g_tagOffset) return;
    float px = rdf(player + kActorPos), py = rdf(player + kActorPos + 4), pz = rdf(player + kActorPos + 8);
    uint32_t node = player + g_tagOffset;
    for (int i = 0; i < 4000 && readable(rd32(node), 16); i++) node = rd32(node);
    std::set<uint32_t> seen;
    for (int i = 0; i < 4000 && node && readable(node, 16) && seen.insert(node).second; i++)
    {
        uint32_t a = rd32(node + 12);
        if (readable(a, 0x400))
        {
            Actor ac{a, (int16_t)rd16(a + 8), rdf(a + kActorPos), rdf(a + kActorPos + 4), rdf(a + kActorPos + 8), 0};
            if (!std::isfinite(ac.x) || std::abs(ac.x) > 1e6f) ac.x = ac.y = ac.z = NAN;
            ac.dist = std::isfinite(ac.x) ? std::hypot(ac.x - px, ac.y - py, ac.z - pz) : INFINITY;
            out.push_back(ac);
        }
        node = rd32(node + 8);
    }
}

uint32_t g_memAddr = 0;
char g_memText[16] = "";

void memory_view()
{
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputText("address", g_memText, sizeof g_memText, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue))
        g_memAddr = (uint32_t)std::strtoul(g_memText, nullptr, 16) & ~3u;
    ImGui::SameLine();
    if (ImGui::Button("player")) g_memAddr = rd32(kPlayerPtr);
    ImGui::SameLine();
    if (ImGui::Button("-0x100")) g_memAddr -= 0x100;
    ImGui::SameLine();
    if (ImGui::Button("+0x100")) g_memAddr += 0x100;
    if (!readable(g_memAddr, 0x100))
    {
        ImGui::TextDisabled("not a readable guest address (MEM2 %08X-%08X)", gmem::kMem2Base, gmem::kMem2End);
        return;
    }
    ImGui::TextDisabled("click a word that looks like a pointer to follow it");
    for (uint32_t row = 0; row < 0x100; row += 16)
    {
        uint32_t a = g_memAddr + row;
        ImGui::Text("%08X", a);
        for (uint32_t w = 0; w < 16; w += 4)
        {
            uint32_t v = rd32(a + w);
            ImGui::SameLine();
            ImGui::PushID((int)(a + w));
            bool ptr = readable(v, 4);
            if (ptr) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 200, 255, 255));
            if (ImGui::SmallButton(std::format("{:08X}", v).c_str()) && ptr) g_memAddr = v & ~3u;
            if (ptr) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("+%X  u32 %u  s32 %d  f32 %g  s16 %d %d", row + w, v, (int32_t)v, std::bit_cast<float>(v), (int16_t)(v >> 16), (int16_t)v);
            ImGui::PopID();
        }
        ImGui::SameLine();
        float f[4];
        for (int k = 0; k < 4; k++) f[k] = rdf(a + k * 4);
        ImGui::TextDisabled("%9.2f %9.2f %9.2f %9.2f", f[0], f[1], f[2], f[3]);
    }
}

const char* tag_name(int tag) { return tag == 1 ? "Link" : tag == 2 ? "item" : ""; }

void process_events()
{
    std::vector<SDL_Event> events;
    {
        std::lock_guard lk(g_eventLock);
        events.swap(g_events);
    }
    ImGuiIO& io = ImGui::GetIO();
    for (const SDL_Event& e : events)
        switch (e.type)
        {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        {
            ImGuiKey k = ImGuiKey_None;
            SDL_Keycode c = e.key.key;
            if (c >= SDLK_0 && c <= SDLK_9) k = (ImGuiKey)(ImGuiKey_0 + (c - SDLK_0));
            else if (c >= SDLK_A && c <= SDLK_Z) k = (ImGuiKey)(ImGuiKey_A + (c - SDLK_A));
            else if (c == SDLK_BACKSPACE) k = ImGuiKey_Backspace;
            else if (c == SDLK_RETURN || c == SDLK_KP_ENTER) k = ImGuiKey_Enter;
            else if (c == SDLK_DELETE) k = ImGuiKey_Delete;
            else if (c == SDLK_LEFT) k = ImGuiKey_LeftArrow;
            else if (c == SDLK_RIGHT) k = ImGuiKey_RightArrow;
            else if (c == SDLK_ESCAPE) k = ImGuiKey_Escape;
            if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == SDL_EVENT_KEY_DOWN);
            break;
        }
        case SDL_EVENT_TEXT_INPUT: io.AddInputCharactersUTF8(e.text.text); break;
        case SDL_EVENT_MOUSE_WHEEL: io.AddMouseWheelEvent(e.wheel.x, e.wheel.y); break;
        default: break;
        }
}

void write_dump(uint32_t frame, bool haveCamera, const game::Camera& cam, const std::vector<mp::rr_debug::CopyInfo>& copies, uint32_t copyFrame)
{
    std::string j = std::format("{{\"frame\":{},\"copyFrame\":{},", frame, copyFrame);
    uint32_t player = rd32(kPlayerPtr);
    if (readable(player, 0x400))
        j += std::format("\"player\":[{:.2f},{:.2f},{:.2f}],", rdf(player + kActorPos), rdf(player + kActorPos + 4), rdf(player + kActorPos + 8));
    {
        std::vector<Actor> actors;
        list_actors(actors);
        j += "\"actors\":[";
        bool first = true;
        for (const Actor& a : actors)
            if (std::isfinite(a.dist) && a.dist < 3000)
            {
                j += std::format("{}[\"{:08X}\",\"{:04X}\",{:.0f},{:.0f},{:.0f}]", first ? "" : ",", a.addr, (uint16_t)a.id, a.x, a.y, a.z);
                first = false;
            }
        j += "],";
    }
    j += "\"copies\":[";
    for (size_t i = 0; i < copies.size(); i++)
    {
        const auto& c = copies[i];
        float sx = -1, sy = -1;
        bool on = haveCamera && project(cam, c.x, c.y, c.z, sx, sy);
        j += std::format("{}{{\"key\":\"{:08X}\",\"part\":{},\"player\":{},\"kind\":\"{}\",\"hidden\":{},\"rt\":\"{:08X}\",\"pos\":[{:.1f},{:.1f},{:.1f}],\"screen\":{}}}",
                         i ? "," : "", c.key, c.part, c.player, c.extra ? "extra" : c.rigid ? "item" : "skin", c.hidden ? "true" : "false", c.rt, c.x, c.y, c.z,
                         on ? std::format("[{:.4f},{:.4f}]", sx, sy) : "null");
        if (c.rigid)
        {
            j.pop_back();
            j += std::format(",\"m\":[{:.3f},{:.3f},{:.3f},{:.1f},{:.3f},{:.3f},{:.3f},{:.1f},{:.3f},{:.3f},{:.3f},{:.1f}]}}", c.m[0], c.m[1], c.m[2], c.m[3],
                             c.m[4], c.m[5], c.m[6], c.m[7], c.m[8], c.m[9], c.m[10], c.m[11]);
        }
        if (!c.bones.empty())
        {
            j.pop_back();
            j += ",\"bones\":[";
            for (size_t b = 0; b + 2 < c.bones.size(); b += 3)
            {
                bool bon = haveCamera && project(cam, c.bones[b], c.bones[b + 1], c.bones[b + 2], sx, sy);
                j += std::format("{}{}", b ? "," : "", bon ? std::format("[{:.4f},{:.4f}]", sx, sy) : "null");
            }
            j += "]}";
        }
    }
    j += "],\"draws\":[";
    {
        std::lock_guard lk(g_drawLock);
        for (size_t i = 0; i < g_drawLast.size(); i++)
        {
            const Draw& d = g_drawLast[i];
            j += std::format("{}[{},\"{:08X}\",\"{:08X}\",{},{},\"{}\",{},{},\"{:08X}\",\"{:X} {:X} {:X} {:X} {:X}\"]", i ? "," : "", d.index, d.rt, d.mesh(),
                             d.count, d.depthOnly ? 1 : 0, tag_name(d.tag), d.copies, d.hidden ? 1 : 0, d.vb, d.blocks[0], d.blocks[1], d.blocks[2], d.blocks[3],
                             d.blocks[4]);
        }
    }
    j += "]}";
    if (FILE* f = std::fopen(std::format("{}/explorer_{:05}.json", g_dumpDir, frame).c_str(), "wb"))
    {
        std::fwrite(j.data(), 1, j.size(), f);
        std::fclose(f);
    }
}

}

bool is_open() { return g_open; }
void toggle()
{
    g_open = !g_open;
    log_info("explorer: {}", g_open ? "open (F3 closes)" : "closed");
}

void on_sdl_event(const SDL_Event& e)
{
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F3)
    {
        toggle();
        return;
    }
    if (!g_open) return;
    if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_TEXT_INPUT || e.type == SDL_EVENT_MOUSE_WHEEL)
    {
        std::lock_guard lk(g_eventLock);
        g_events.push_back(e);
    }
}

void render()
{
    init_once();
    uint32_t frame = LatteGPUState.frameCounter;
    bool anyHidden;
    {
        std::lock_guard lk(g_drawLock);
        anyHidden = !g_hiddenMeshes.empty();
    }
    g_capture = g_open || anyHidden || (g_dumpLast && frame + 2 >= g_dumpFirst && frame <= g_dumpLast);
    game::Camera cam;
    bool haveCamera = game::camera(cam);
    std::vector<mp::rr_debug::CopyInfo> copies;
    uint32_t copyFrame = mp::rr_debug::snapshot(copies);
    if (g_dumpLast && frame >= g_dumpFirst && frame <= g_dumpLast) write_dump(frame, haveCamera, cam, copies, copyFrame);

    ImVec2 size = ImGui::GetIO().DisplaySize;
    if (g_labels && haveCamera)
    {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        for (const auto& c : copies)
        {
            float sx, sy;
            if (!project(cam, c.x, c.y, c.z, sx, sy)) continue;
            ImVec2 at(sx * size.x, sy * size.y);
            ImU32 col = c.hidden ? IM_COL32(150, 150, 150, 200) : c.extra ? IM_COL32(255, 120, 255, 255) : c.rigid ? IM_COL32(255, 230, 60, 255) : IM_COL32(80, 220, 255, 255);
            dl->AddCircleFilled(at, 3.5f, col);
            dl->AddText(ImVec2(at.x + 5, at.y - 7), col, std::format("{:08X}", c.key).c_str());
        }
    }
    if (!g_open) return;
    process_events();

    ImGui::SetNextWindowPos(ImVec2(20, 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(760, 620), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.9f);
    bool open = true;
    if (!ImGui::Begin("Windfall Explorer (F3)", &open))
    {
        ImGui::End();
        if (!open) g_open = false;
        return;
    }
    if (!open) g_open = false;
    bool labels = g_labels;
    if (ImGui::Checkbox("labels on screen", &labels)) g_labels = labels;
    ImGui::SameLine();
    ImGui::Checkbox("freeze draws", &g_freeze);
    ImGui::SameLine();
    ImGui::TextDisabled("gpu frame %u", frame);

    if (ImGui::BeginTabBar("tabs"))
    {
        if (ImGui::BeginTabItem("Scene"))
        {
            uint32_t player = rd32(kPlayerPtr);
            char stage[9] = {};
            for (int i = 0; i < 8; i++) stage[i] = (char)*gptr<uint8_t>(kStartStage + i);
            ImGui::Text("stage %s  room %d  point %d", stage, (int8_t)*gptr<uint8_t>(kStartStage + 10), (int16_t)rd16(kStartStage + 8));
            if (readable(player, 0x400))
            {
                ImGui::Text("player actor %08X", player);
                ImGui::Text("pos   %.1f %.1f %.1f", rdf(player + kActorPos), rdf(player + kActorPos + 4), rdf(player + kActorPos + 8));
                ImGui::Text("angle %d %d %d  shape %d  room %d", (int16_t)rd16(player + kActorAngle), (int16_t)rd16(player + kActorAngle + 2),
                            (int16_t)rd16(player + kActorAngle + 4), (int16_t)rd16(player + kActorShape + 2), (int8_t)*gptr<uint8_t>(player + kActorRoom));
                if (ImGui::SmallButton("inspect player memory")) g_memAddr = player;
            }
            else
                ImGui::TextDisabled("no player actor");
            if (haveCamera)
            {
                const auto& c = cam.viewToWorld;
                ImGui::Text("camera %.1f %.1f %.1f  fov y %.1f deg", c[3], c[7], c[11], 2 * std::atan(1.0f / cam.projection[5]) * 57.29578f);
            }
            ImGui::Text("game frame %u  multiplayer %s", mp::frame_number(), mp::connected() ? "connected" : "off");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Actors"))
        {
            static std::vector<Actor> actors;
            static int sortNear = 1;
            static double last = 0;
            double now = ImGui::GetTime();
            if (now - last > 0.5)
            {
                list_actors(actors);
                if (sortNear) std::sort(actors.begin(), actors.end(), [](const Actor& a, const Actor& b) { return a.dist < b.dist; });
                last = now;
            }
            ImGui::RadioButton("list order", &sortNear, 0);
            ImGui::SameLine();
            ImGui::RadioButton("nearest first", &sortNear, 1);
            ImGui::SameLine();
            ImGui::TextDisabled("%zu actors (list node at +%X)", actors.size(), g_tagOffset);
            if (ImGui::BeginTable("actors", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, -4)))
            {
                ImGui::TableSetupColumn("actor");
                ImGui::TableSetupColumn("proc");
                ImGui::TableSetupColumn("position");
                ImGui::TableSetupColumn("distance");
                ImGui::TableSetupColumn("");
                ImGui::TableHeadersRow();
                for (const Actor& a : actors)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%08X%s", a.addr, a.addr == rd32(kPlayerPtr) ? " (player)" : "");
                    ImGui::TableNextColumn();
                    ImGui::Text("%04X", (uint16_t)a.id);
                    ImGui::TableNextColumn();
                    if (std::isfinite(a.x)) ImGui::Text("%.0f %.0f %.0f", a.x, a.y, a.z);
                    ImGui::TableNextColumn();
                    if (std::isfinite(a.dist)) ImGui::Text("%.0f", a.dist);
                    ImGui::TableNextColumn();
                    ImGui::PushID((int)a.addr);
                    if (ImGui::SmallButton("memory")) g_memAddr = a.addr;
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Draws"))
        {
            static bool onlyLink = false, skipDepth = true;
            ImGui::Checkbox("Link and items only", &onlyLink);
            ImGui::SameLine();
            ImGui::Checkbox("skip depth-only", &skipDepth);
            std::vector<Draw> draws;
            uint32_t df;
            {
                std::lock_guard lk(g_drawLock);
                draws = g_drawLast;
                df = g_drawLastFrame;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("frame %u: %zu draws", df, draws.size());
            if (ImGui::BeginTable("draws", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, -4)))
            {
                for (const char* h : {"#", "target", "mesh", "count", "blocks", "tex0", "kind", "copies", "hide"}) ImGui::TableSetupColumn(h);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                for (const Draw& d : draws)
                {
                    if (onlyLink && !d.tag) continue;
                    if (skipDepth && d.depthOnly) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", d.index);
                    ImGui::TableNextColumn();
                    ImGui::Text("%08X%s", d.rt, d.depthOnly ? " z" : "");
                    ImGui::TableNextColumn();
                    ImGui::Text("%08X", d.mesh());
                    ImGui::TableNextColumn();
                    ImGui::Text("%u%s", d.count, d.instances > 1 ? std::format(" x{}", d.instances).c_str() : "");
                    ImGui::TableNextColumn();
                    ImGui::Text("%X %X %X %X %X", d.blocks[0], d.blocks[1], d.blocks[2], d.blocks[3], d.blocks[4]);
                    ImGui::TableNextColumn();
                    ImGui::Text("%08X", d.tex0);
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", tag_name(d.tag));
                    ImGui::TableNextColumn();
                    if (d.copies) ImGui::Text("%d", d.copies);
                    ImGui::TableNextColumn();
                    std::lock_guard lk(g_drawLock);
                    bool h = g_hiddenMeshes.count(d.mesh()) != 0;
                    ImGui::PushID((int)d.index);
                    if (ImGui::Checkbox("##h", &h))
                    {
                        if (h) g_hiddenMeshes.insert(d.mesh());
                        else g_hiddenMeshes.erase(d.mesh());
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Multiplayer"))
        {
            ImGui::Text("copies drawn in frame %u: %zu", copyFrame, copies.size());
            ImGui::TextDisabled("hide a key to stop drawing that copy; labels: yellow item, cyan skinned, pink replayed, grey hidden");
            if (ImGui::BeginTable("copies", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, -4)))
            {
                for (const char* h : {"key", "kind", "player", "target", "world", "screen", "hide"}) ImGui::TableSetupColumn(h);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                int row = 0;
                for (const auto& c : copies)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%08X", c.key);
                    ImGui::TableNextColumn();
                    ImGui::Text("%s%s", c.extra ? "replay" : c.rigid ? "item" : std::format("skin +{:X}", c.part).c_str(), c.hidden ? " (hidden)" : "");
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", c.player);
                    ImGui::TableNextColumn();
                    ImGui::Text("%08X", c.rt);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f %.0f %.0f", c.x, c.y, c.z);
                    ImGui::TableNextColumn();
                    float sx, sy;
                    if (haveCamera && project(cam, c.x, c.y, c.z, sx, sy)) ImGui::Text("%.0f, %.0f", sx * size.x, sy * size.y);
                    ImGui::TableNextColumn();
                    bool h = mp::rr_debug::hidden(c.key);
                    ImGui::PushID(row++);
                    if (ImGui::Checkbox("##h", &h)) mp::rr_debug::set_hidden(c.key, h);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Memory"))
        {
            memory_view();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

}

void wf_explorer_render() { wf::explorer::render(); }

bool wf_explorer_draw(const uint32_t* regs, uint32_t count, uint32_t instances, int copies)
{
    using namespace wf::explorer;
    if (!g_capture) return false;
    uint32_t frame = LatteGPUState.frameCounter;
    std::lock_guard lk(g_drawLock);
    if (frame != g_drawFrame)
    {
        if (!g_freeze || g_drawLast.empty())
        {
            g_drawLast.swap(g_drawCur);
            g_drawLastFrame = g_drawFrame;
        }
        g_drawCur.clear();
        g_drawFrame = frame;
    }
    Draw d{};
    d.index = (uint32_t)g_drawCur.size();
    d.rt = regs[kCbColor0Base] << 8;
    d.depthOnly = regs[kCbTargetMask] == 0;
    d.vb = regs[kVsAttribStart];
    d.vbSize = regs[kVsAttribStart + 1] + 1;
    d.count = count;
    d.instances = instances;
    d.tex0 = regs[kPsTexStart + 2] << 8;
    for (int b = 0; b < 6; b++) d.blocks[b] = regs[kVsUniformStart + b * 7] ? regs[kVsUniformStart + b * 7 + 1] + 1 : 0;
    d.tag = copies < 0 ? 0 : wf_rr_draw_tag();
    d.copies = std::max(copies, 0);
    d.hidden = g_hiddenMeshes.count(d.mesh()) != 0;
    if (g_drawCur.size() < 20000) g_drawCur.push_back(d);
    return d.hidden;
}
