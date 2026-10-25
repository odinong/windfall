#include "net/discord_rpc.h"

#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include "core/guest.h"
#include "core/log.h"
#include "core/places.h"
#include "ui/settings.h"
#include "net/multiplayer.h"

namespace wf::discord {
namespace {

constexpr const char* kClientId = "1563446156753633340";
constexpr uint32_t kCurrentStage = 0x104741E4;
constexpr uint32_t kPlayerPtr = 0x1047428C;

std::string stage_name()
{
    char name[9] = {};
    for (int i = 0; i < 8; i++) name[i] = (char)rd8(kCurrentStage + i);
    return place_name(name, (int8_t)rd8(kCurrentStage + 10));
}

HANDLE g_pipe = INVALID_HANDLE_VALUE;

bool pipe_open()
{
    for (int i = 0; i < 10; i++)
    {
        std::string path = std::format("\\\\.\\pipe\\discord-ipc-{}", i);
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            g_pipe = h;
            return true;
        }
    }
    return false;
}

void pipe_close()
{
    if (g_pipe != INVALID_HANDLE_VALUE) CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
}

bool send_frame(uint32_t op, const std::string& json)
{
    std::vector<char> buf(8 + json.size());
    uint32_t len = (uint32_t)json.size();
    std::memcpy(buf.data(), &op, 4);
    std::memcpy(buf.data() + 4, &len, 4);
    std::memcpy(buf.data() + 8, json.data(), json.size());
    DWORD written = 0;
    return WriteFile(g_pipe, buf.data(), (DWORD)buf.size(), &written, nullptr) && written == buf.size();
}

bool read_frame(std::string& json)
{
    uint32_t head[2];
    DWORD got = 0;
    if (!ReadFile(g_pipe, head, 8, &got, nullptr) || got != 8 || head[1] > 1 << 16) return false;
    json.resize(head[1]);
    return head[1] == 0 || (ReadFile(g_pipe, json.data(), head[1], &got, nullptr) && got == head[1]);
}

void drain()
{
    DWORD avail = 0;
    std::string json;
    while (PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr) && avail >= 8)
        if (!read_frame(json)) break;
}

std::string escape(const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        if (c == '"' || c == '\\') out += '\\';
        if ((unsigned char)c >= 0x20) out += c;
    }
    return out;
}

void run()
{
    const int64_t started = (int64_t)std::time(nullptr);
    uint32_t nonce = 0;
    std::string lastActivity;
    for (;;)
    {
        bool wanted;
        {
            std::lock_guard lk(settings::lock());
            wanted = settings::get().discordPresence;
        }
        if (!wanted)
        {
            // turned off in the menu: clear the status and let go of Discord until it's turned on again
            if (g_pipe != INVALID_HANDLE_VALUE)
            {
                send_frame(1, std::format("{{\"cmd\":\"SET_ACTIVITY\",\"args\":{{\"pid\":{}}},\"nonce\":\"{}\"}}", GetCurrentProcessId(), ++nonce));
                drain();
                pipe_close();
                log_info("discord: rich presence off");
            }
            lastActivity.clear();
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        if (g_pipe == INVALID_HANDLE_VALUE)
        {
            if (!pipe_open())
            {
                std::this_thread::sleep_for(std::chrono::seconds(15));
                continue;
            }
            std::string reply;
            if (!send_frame(0, std::format("{{\"v\":1,\"client_id\":\"{}\"}}", kClientId)) || !read_frame(reply))
            {
                pipe_close();
                std::this_thread::sleep_for(std::chrono::seconds(15));
                continue;
            }
            log_info("discord: rich presence connected");
            lastActivity.clear();
        }
        std::string where = stage_name();
        char stage[9] = {};
        for (int i = 0; i < 8; i++) stage[i] = (char)rd8(kCurrentStage + i);
        bool menus = !stage[0] || std::strcmp(stage, "Name") == 0 || std::strncmp(stage, "sea_T", 5) == 0;
        bool playing = !menus && rd32(kPlayerPtr) != 0 && !where.empty();
        std::string details = playing ? "Exploring " + where : menus ? "On the title screen" : "Exploring the Great Sea";
        std::string state;
        std::string party;
        if (mp::connected())
        {
            int players = mp::player_count();
            state = std::format("Co-op in room {}", mp::room_code());
            party = std::format(",\"party\":{{\"id\":\"windfall-{}\",\"size\":[{},8]}}", escape(mp::room_code()), players);
        }
        else
            state = "Playing solo";
        std::string activity = std::format(
            "{{\"details\":\"{}\",\"state\":\"{}\",\"timestamps\":{{\"start\":{}}},"
            "\"assets\":{{\"large_image\":\"windfallicon\",\"large_text\":\"Windfall: The Wind Waker HD on PC\"}}{}}}",
            escape(details), escape(state), started, party);
        if (activity != lastActivity)
        {
            std::string cmd = std::format("{{\"cmd\":\"SET_ACTIVITY\",\"args\":{{\"pid\":{},\"activity\":{}}},\"nonce\":\"{}\"}}",
                                          GetCurrentProcessId(), activity, ++nonce);
            drain();
            if (!send_frame(1, cmd))
            {
                log_info("discord: connection lost");
                pipe_close();
                continue;
            }
            lastActivity = activity;
        }
        drain();
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

}

void start()
{
    if (const char* e = std::getenv("WF_NO_DISCORD"); e && *e == '1') return;
    std::thread(run).detach();  // follows the setting while running
}

}
