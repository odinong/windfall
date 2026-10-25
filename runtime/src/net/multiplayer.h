#pragma once

#include <cstdint>
#include <string>
#include <array>
#include <vector>

namespace wf::mp {

void start();
void stop();
void connect(const std::string& server, const std::string& room, const std::string& name);
void disconnect();
std::string status();
std::vector<std::string> room_players();
struct PlayerInfo
{
    std::string name;
    int color = 0;
    bool self = false;
    std::string place;
    std::string world;       // the randomized world they play (rando::playing_world text), empty for the original game
    bool sameWorld = true;   // the same as ours: items are shared with them
};
std::vector<PlayerInfo> room_info();
uint32_t tunic_color(int color);
struct ChatEntry { std::string name, text; int color; double age; };
std::vector<ChatEntry> recent_chat(size_t count);
void send_chat(const std::string& text);
void render_chat();
bool chat_open();
void on_chat_event(const void* sdlEvent);
void tick();
void render_overlay();
bool connected();
std::string room_code();
int player_count();

bool active();
void new_frame();
uint32_t frame_number();
struct Placement
{
    float x, y, z, yaw;
    int color;
    struct Pose
    {
        uint16_t part;
        std::vector<float> bones;
        std::vector<uint32_t> meshes;
    };
    std::vector<Pose> poses;
    std::vector<std::pair<uint32_t, std::vector<float>>> rigid;
    bool ship = false;
    float shipX = 0, shipY = 0, shipZ = 0, shipAngle[3] = {};
};
void mark_part_drawn(uint16_t part, uint32_t mesh, uint32_t paletteFrame);
void set_local_rigid(uint32_t key, const float* m, uint32_t matrices, float x, float y, float z);
void set_local_pose(uint16_t part, const float* bones, uint32_t count, float x, float y, float z);
void remote_placements(std::vector<Placement>& out);

}
