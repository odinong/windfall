#include "net/multiplayer.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>

#include "core/guest.h"
#include "core/log.h"
#include "core/places.h"
#include "ui/menu.h"
#include "ui/settings.h"
#include "enhancements/randomizer.h"
#include "enhancements/camera.h"

namespace wf::mp {
namespace {

constexpr uint8_t kProto = 9;
constexpr uint16_t kDefaultPort = 27015;
constexpr double kInterpDelay = 0.1;
constexpr double kStale = 3.0;
constexpr double kPartStale = 0.25;

constexpr uint32_t kPlayerPtr = 0x1047428C;
constexpr uint32_t kActorPos = 0x314, kActorAngle = 0x320, kActorRoom = 0x326, kActorShape = 0x328;
constexpr uint32_t kStartStage = 0x104741E4;
constexpr uint32_t kShipPtr = 0x10474BEC;

#pragma pack(push, 1)
struct LinkView
{
    uint16_t viewId;
    uint8_t kind;
    char stage[8];
    int8_t room;
    uint8_t flags;
    float x, y, z;
    int16_t angleY, shapeY;
    float shipX, shipY, shipZ;
    int16_t shipAngle[3];
};
#pragma pack(pop)
static_assert(sizeof(LinkView) == 47);

constexpr uint8_t kViewLink = 1, kViewPose = 2, kViewRigid = 3;
constexpr uint32_t kMaxBones = 64;
constexpr float kRotScale = 16384.0f, kPosScale = 8.0f;

struct Snapshot { double time; LinkView view; };
struct PoseSnapshot { double time; std::vector<int16_t> q; std::vector<uint32_t> meshes; };
struct Remote
{
    std::string name;
    uint8_t color = 0;
    std::deque<Snapshot> snaps;
    std::map<uint16_t, std::deque<PoseSnapshot>> poses;
    struct RigidSnapshot { double time; std::vector<int16_t> q; };
    std::map<uint32_t, std::deque<RigidSnapshot>> rigid;
    std::string world;  // the randomized world they play (rando::playing_world), empty for the original game
};

using Clock = std::chrono::steady_clock;
double now_s() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

std::atomic<bool> g_running{false}, g_joined{false};
std::atomic<uint32_t> g_frameNumber{0};
std::thread g_thread;
SOCKET g_sock = INVALID_SOCKET;
sockaddr_storage g_addr{};
int g_addrLen = 0;
std::string g_wsUrl;
HINTERNET g_wsSession = nullptr, g_wsConnect = nullptr, g_ws = nullptr;
std::string g_room = "PUBLIC", g_name = "Link";
std::mutex g_statusLock;
std::string g_status = "Not connected";
void set_status(std::string s)
{
    std::lock_guard lk(g_statusLock);
    g_status = std::move(s);
}
struct ChatLine { std::string name, text; double time; int color; };
std::deque<ChatLine> g_chat;
std::vector<std::string> g_outbox;

constexpr uint32_t kItemSlots[2] = {0x14822BCC, 0x148D1B40};
constexpr int kItemSlotCount = 21;
constexpr uint8_t kEventItem = 1;
constexpr uint8_t kEventWorld = 2;  // payload: the sender's rando::playing_world() text, resent every few seconds
uint8_t g_itemsSeen[kItemSlotCount];
bool g_itemsKnown = false, g_itemsAnnounced = false;
std::vector<std::pair<uint8_t, uint8_t>> g_itemsIn;
std::vector<std::pair<uint8_t, uint8_t>> g_itemsOut;

bool items_valid()
{
    int filled = 0;
    for (int i = 0; i < kItemSlotCount; i++)
    {
        uint8_t a = rd8(kItemSlots[0] + i);
        if (a == 0x00 || a == 0xFE) return false;
        filled += a != 0xFF;
    }
    return filled > 0;
}
std::atomic<int> g_id{-1}, g_color{0};

std::mutex g_lock;
LinkView g_local{};
bool g_localValid = false;
struct LocalPose
{
    uint32_t frame = 0, drawn = 0;
    std::vector<int16_t> q;
    std::map<uint32_t, uint32_t> meshes;
};
std::map<uint16_t, LocalPose> g_localPose;
struct LocalRigid { uint32_t frame; std::vector<int16_t> q; };
std::map<uint32_t, LocalRigid> g_localRigid;
std::map<int, Remote> g_remotes;

void put_str(std::vector<uint8_t>& out, const std::string& s, size_t n)
{
    for (size_t i = 0; i < n; i++) out.push_back(i + 1 < n && i < s.size() ? (uint8_t)s[i] : 0);
}

std::string get_str(const uint8_t* p, size_t n) { return std::string((const char*)p, strnlen((const char*)p, n)); }

void handle(const uint8_t* d, int n)
{
    if (n < 1) return;
    switch (d[0])
    {
    case 'W':
        if (n >= 3)
        {
            if (!g_joined)
            {
                log_info("mp: joined room {} as player {} ({})", g_room, d[1] + 1, g_name);
                set_status(std::format("In room {} as player {}", g_room, d[1] + 1));
            }
            g_id = d[1];
            g_color = d[2];
            g_joined = true;
        }
        break;
    case 'F':
        log_error("mp: the server refused to join: {}", std::string((const char*)d + 1, n - 1));
        set_status("Refused: " + std::string((const char*)d + 1, n - 1));
        g_running = false;
        break;
    case 'P':
    {
        std::lock_guard lk(g_lock);
        std::map<int, Remote> next;
        int count = n >= 2 ? d[1] : 0;
        std::string names;
        for (int i = 0; i < count && 2 + i * 18 + 18 <= n; i++)
        {
            const uint8_t* e = d + 2 + i * 18;
            int id = e[0];
            if (id == g_id) continue;
            Remote r = g_remotes.count(id) ? std::move(g_remotes[id]) : Remote{};
            r.color = e[1];
            r.name = get_str(e + 2, 16);
            names += (names.empty() ? "" : ", ") + r.name;
            next[id] = std::move(r);
        }
        g_remotes = std::move(next);
        log_info("mp: room {}: {} other player(s){}{}", g_room, g_remotes.size(), names.empty() ? "" : ": ", names);
        break;
    }
    case 'S':
    {
        if (n < 7) break;
        int id = d[1];
        int count = d[6];
        double t = now_s();
        std::lock_guard lk(g_lock);
        auto it = g_remotes.find(id);
        if (it == g_remotes.end()) break;
        int at = 7;
        for (int i = 0; i < count && at + 5 <= n; i++)
        {
            uint8_t kind = d[at + 2];
            int len = d[at + 3] | (d[at + 4] << 8);
            const uint8_t* payload = d + at + 5;
            at += 5 + len;
            if (at > n) break;
            if (kind == kViewLink && len == (int)sizeof(LinkView))
            {
                LinkView v;
                memcpy(&v, payload, sizeof v);
                auto& snaps = it->second.snaps;
                snaps.push_back({t, v});
                while (snaps.size() > 64) snaps.pop_front();
            }
            else if (kind == kViewRigid && len >= 1)
            {
                uint32_t count = payload[0];
                int at2 = 1;
                for (uint32_t k = 0; k < count && at2 + 5 <= len; k++)
                {
                    const uint8_t* e = payload + at2;
                    uint32_t key, mats = e[4];
                    memcpy(&key, e, 4);
                    if (mats == 0 || mats > 8 || at2 + 5 + (int)mats * 24 > len) break;
                    Remote::RigidSnapshot r{t, std::vector<int16_t>(mats * 12)};
                    memcpy(r.q.data(), e + 5, mats * 24);
                    at2 += 5 + mats * 24;
                    auto& d = it->second.rigid[key];
                    d.push_back(r);
                    while (d.size() > 16) d.pop_front();
                }
            }
            else if (kind == kViewPose && len >= 2)
            {
                uint32_t bones = payload[0], meshes = payload[1];
                uint16_t part = (uint16_t)(payload[2] | (payload[3] << 8));
                if (bones == 0 || bones > kMaxBones || len < 4 + (int)(meshes * 4 + bones * 24)) continue;
                PoseSnapshot p{t, std::vector<int16_t>(bones * 12), std::vector<uint32_t>(meshes)};
                memcpy(p.meshes.data(), payload + 4, meshes * 4);
                memcpy(p.q.data(), payload + 4 + meshes * 4, bones * 24);
                auto& poses = it->second.poses[part];
                poses.push_back(std::move(p));
                while (poses.size() > 16) poses.pop_front();
            }
        }
        break;
    }
    case 'E':
        if (n >= 7 && d[1] != g_id)
        {
            std::lock_guard lk(g_lock);
            auto it = g_remotes.find(d[1]);
            if (d[6] == kEventWorld && it != g_remotes.end())
            {
                std::string world((const char*)d + 7, n - 7);
                if (it->second.world != world)
                    log_info("mp: {} plays {}", it->second.name, world.empty() ? "the original game" : "a randomizer seed");
                it->second.world = std::move(world);
            }
            // items only go between players of the same world: in different randomizer seeds (or the original game)
            // the same item unlocks different things
            else if (n >= 9 && d[6] == kEventItem && d[7] < kItemSlotCount && it != g_remotes.end() &&
                     it->second.world == rando::playing_world())
                g_itemsIn.push_back({d[7], d[8]});
        }
        break;
    case 'C':
        if (n >= 18)
        {
            std::string name = get_str(d + 2, 16), text((const char*)d + 18, n - 18);
            log_info("mp chat {}: {}", name, text);
            std::lock_guard lk(g_lock);
            int color = -1;
            if (d[1] != g_id)
                if (auto it = g_remotes.find(d[1]); it != g_remotes.end()) color = it->second.color;
            g_chat.push_back({name, text, now_s(), color});
            while (g_chat.size() > 50) g_chat.pop_front();
        }
        break;
    default:
        break;
    }
}

void send_pkt(const std::vector<uint8_t>& pkt)
{
    if (g_ws)
        WinHttpWebSocketSend(g_ws, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, (PVOID)pkt.data(), (DWORD)pkt.size());
    else
        sendto(g_sock, (const char*)pkt.data(), (int)pkt.size(), 0, (sockaddr*)&g_addr, g_addrLen);
}

bool ws_open()
{
    std::string url = g_wsUrl;
    bool secure = url.starts_with("wss://");
    url = (secure ? "https://" : "http://") + url.substr(url.find("://") + 3);
    std::wstring wurl(url.begin(), url.end());
    wchar_t host[256], path[1024];
    URL_COMPONENTS uc{sizeof uc};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;
    if (!path[0]) wcscpy_s(path, L"/");
    g_wsSession = WinHttpOpen(L"Windfall", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_wsSession) return false;
    g_wsConnect = WinHttpConnect(g_wsSession, host, uc.nPort, 0);
    if (!g_wsConnect) return false;
    HINTERNET req = WinHttpOpenRequest(g_wsConnect, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) return false;
    bool ok = WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
              WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    if (ok) g_ws = WinHttpWebSocketCompleteUpgrade(req, 0);
    WinHttpCloseHandle(req);
    return g_ws != nullptr;
}

void ws_receive(HINTERNET ws)
{
    std::vector<uint8_t> buf(65536), msg;
    for (;;)
    {
        DWORD got = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        if (WinHttpWebSocketReceive(ws, buf.data(), (DWORD)buf.size(), &got, &type) != NO_ERROR ||
            type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            break;
        msg.insert(msg.end(), buf.data(), buf.data() + got);
        if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
        {
            handle(msg.data(), (int)msg.size());
            msg.clear();
        }
        else if (type != WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
            msg.clear();
    }
    if (g_running)
    {
        log_error("mp: the connection to the server closed");
        set_status("Connection to the server lost");
    }
    g_running = false;
}

void run()
{
    std::thread receiver;
    if (!g_wsUrl.empty())
    {
        if (!ws_open())
        {
            log_error("mp: can't connect to {} ({})", g_wsUrl, GetLastError());
            set_status("Can't reach " + g_wsUrl);
            g_running = false;
            return;
        }
        receiver = std::thread(ws_receive, g_ws);
    }
    uint32_t seq = 0;
    double lastJoin = 0, lastSend = 0;
    std::vector<uint8_t> pkt;
    while (g_running)
    {
        double t = now_s();
        if (!g_joined && t - lastJoin > 0.5)
        {
            pkt.assign({'J', kProto});
            put_str(pkt, g_room, 16);
            put_str(pkt, g_name, 16);
            send_pkt(pkt);
            lastJoin = t;
        }
        if (g_joined && t - lastSend >= 1.0 / 30)
        {
            std::lock_guard lk(g_lock);
            pkt.assign({'S', (uint8_t)g_id.load()});
            seq++;
            for (int i = 0; i < 4; i++) pkt.push_back((uint8_t)(seq >> (8 * i)));
            auto view = [&](uint16_t id, uint8_t kind, const void* data, size_t len, const void* head = nullptr, size_t headLen = 0) {
                size_t total = headLen + len;
                pkt.insert(pkt.end(), {(uint8_t)id, (uint8_t)(id >> 8), kind, (uint8_t)total, (uint8_t)(total >> 8)});
                if (head) pkt.insert(pkt.end(), (const uint8_t*)head, (const uint8_t*)head + headLen);
                pkt.insert(pkt.end(), (const uint8_t*)data, (const uint8_t*)data + len);
            };
            size_t countAt = pkt.size();
            pkt.push_back(0);
            if (g_localValid)
            {
                view(g_local.viewId, kViewLink, &g_local, sizeof g_local);
                pkt[countAt]++;
                uint32_t frame = g_frameNumber;
                uint16_t n = 1;
                std::vector<uint8_t> rigid{0};
                for (const auto& [key, r] : g_localRigid)
                {
                    if (frame - r.frame > 3 || rigid[0] == 255) continue;
                    rigid.insert(rigid.end(), (const uint8_t*)&key, (const uint8_t*)&key + 4);
                    rigid.push_back((uint8_t)(r.q.size() / 12));
                    rigid.insert(rigid.end(), (const uint8_t*)r.q.data(), (const uint8_t*)(r.q.data() + r.q.size()));
                    rigid[0]++;
                }
                static const bool sendLog = std::getenv("WF_RR_RIGIDLOG") != nullptr;
                static int sent = 0;
                if (sendLog && rigid[0] && sent++ % 300 == 0)
                {
                    std::string ks;
                    for (const auto& [key, r] : g_localRigid) ks += std::format(" {:08X}/{}/{}", key, frame - r.frame, r.q.size() / 12);
                    log_info("mp rigid send: {} of{}", rigid[0], ks);
                }
                if (rigid[0])
                {
                    view((uint16_t)(g_local.viewId + 50), kViewRigid, rigid.data(), rigid.size());
                    pkt[countAt]++;
                }
                for (const auto& [part, pose] : g_localPose)
                {
                    if (frame - pose.frame > 3 || frame - pose.drawn > 3) continue;
                    std::vector<uint8_t> head{(uint8_t)(pose.q.size() / 12), 0, (uint8_t)part, (uint8_t)(part >> 8)};
                    for (const auto& [mesh, f] : pose.meshes)
                        if (pose.frame - f <= 4 && head[1] < 16)
                        {
                            head.insert(head.end(), (const uint8_t*)&mesh, (const uint8_t*)&mesh + 4);
                            head[1]++;
                        }
                    view((uint16_t)(g_local.viewId + n++), kViewPose, pose.q.data(), pose.q.size() * 2, head.data(), head.size());
                    pkt[countAt]++;
                }
            }
            send_pkt(pkt);
            lastSend = t;
        }
        if (g_joined)
        {
            std::vector<std::string> out;
            {
                std::lock_guard lk(g_lock);
                out.swap(g_outbox);
            }
            for (const std::string& text : out)
            {
                pkt.assign({'C', (uint8_t)g_id.load()});
                pkt.insert(pkt.end(), text.begin(), text.begin() + std::min<size_t>(text.size(), 200));
                send_pkt(pkt);
            }
            std::vector<std::pair<uint8_t, uint8_t>> items;
            {
                std::lock_guard lk(g_lock);
                items.swap(g_itemsOut);
            }
            for (const auto& [slot, item] : items)
            {
                seq++;
                pkt.assign({'E', (uint8_t)g_id.load(), (uint8_t)seq, (uint8_t)(seq >> 8), (uint8_t)(seq >> 16), (uint8_t)(seq >> 24), kEventItem, slot, item});
                send_pkt(pkt);
            }
            static double s_worldSent = 0;
            if (t - s_worldSent > 3.0)
            {
                s_worldSent = t;
                seq++;
                const std::string& world = rando::playing_world();
                pkt.assign({'E', (uint8_t)g_id.load(), (uint8_t)seq, (uint8_t)(seq >> 8), (uint8_t)(seq >> 16), (uint8_t)(seq >> 24), kEventWorld});
                pkt.insert(pkt.end(), world.begin(), world.begin() + std::min<size_t>(world.size(), 3000));
                send_pkt(pkt);
            }
        }
        if (g_ws)
        {
            Sleep(5);
            continue;
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(g_sock, &rd);
        timeval tv{0, 5000};
        if (select(0, &rd, nullptr, nullptr, &tv) > 0)
        {
            static uint8_t buf[65536];
            for (;;)
            {
                int n = recv(g_sock, (char*)buf, sizeof buf, 0);
                if (n <= 0) break;
                handle(buf, n);
            }
        }
    }
    if (g_joined)
    {
        send_pkt({'L', (uint8_t)g_id.load()});
    }
    if (g_ws)
    {
        WinHttpWebSocketClose(g_ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(std::exchange(g_ws, nullptr));
    }
    if (receiver.joinable()) receiver.join();
    for (HINTERNET* h : {&g_ws, &g_wsConnect, &g_wsSession})
        if (*h) WinHttpCloseHandle(std::exchange(*h, nullptr));
}

double chat_seconds()
{
    std::lock_guard lk(settings::lock());
    return settings::get().mpChatSeconds;
}
bool share_items()
{
    std::lock_guard lk(settings::lock());
    return settings::get().mpShareItems;
}
bool show_names()
{
    std::lock_guard lk(settings::lock());
    return settings::get().mpShowNames;
}

ImU32 tunic(int color)
{
    static constexpr ImU32 kColors[] = {IM_COL32(40, 170, 70, 255),  IM_COL32(215, 45, 45, 255),
                                        IM_COL32(45, 100, 225, 255), IM_COL32(150, 70, 205, 255),
                                        IM_COL32(235, 195, 40, 255), IM_COL32(240, 130, 30, 255),
                                        IM_COL32(40, 200, 210, 255), IM_COL32(235, 110, 175, 255)};
    return kColors[color & 7];
}

bool junk_bone(const int16_t* q)
{
    constexpr int kFar = (int)(400 * kPosScale);
    return std::abs(q[3]) > kFar || std::abs(q[7]) > kFar || std::abs(q[11]) > kFar;
}

bool sample(const std::deque<Snapshot>& snaps, double t, LinkView& out)
{
    if (snaps.empty() || t - snaps.back().time > kStale + kInterpDelay) return false;
    if (t <= snaps.front().time) { out = snaps.front().view; return true; }
    for (size_t i = 1; i < snaps.size(); i++)
    {
        const Snapshot &a = snaps[i - 1], &b = snaps[i];
        if (t > b.time) continue;
        float k = (float)((t - a.time) / std::max(1e-6, b.time - a.time));
        out = b.view;
        if (memcmp(a.view.stage, b.view.stage, 8) != 0 || a.view.room != b.view.room) return true;
        out.x = a.view.x + (b.view.x - a.view.x) * k;
        out.y = a.view.y + (b.view.y - a.view.y) * k;
        out.z = a.view.z + (b.view.z - a.view.z) * k;
        out.shapeY = (int16_t)(a.view.shapeY + (int16_t)(b.view.shapeY - a.view.shapeY) * k);
        if ((a.view.flags & 1) && (b.view.flags & 1))
        {
            out.shipX = a.view.shipX + (b.view.shipX - a.view.shipX) * k;
            out.shipY = a.view.shipY + (b.view.shipY - a.view.shipY) * k;
            out.shipZ = a.view.shipZ + (b.view.shipZ - a.view.shipZ) * k;
            for (int i = 0; i < 3; i++)
                out.shipAngle[i] = (int16_t)(a.view.shipAngle[i] + (int16_t)(b.view.shipAngle[i] - a.view.shipAngle[i]) * k);
        }
        return true;
    }
    out = snaps.back().view;
    return true;
}

bool project(const game::Camera& cam, float x, float y, float z, ImVec2 size, ImVec2& out, float& depth)
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
    out = ImVec2((cx / cw * 0.5f + 0.5f) * size.x, (0.5f - cy / cw * 0.5f) * size.y);
    depth = cw;
    return true;
}

}

void connect(const std::string& server, const std::string& room, const std::string& name)
{
    disconnect();
    if (server.empty()) return;
    g_room = room.empty() ? "PUBLIC" : room;
    g_name = name.empty() ? "Link" : name;
    std::transform(g_room.begin(), g_room.end(), g_room.begin(), ::toupper);
    g_joined = false;
    g_id = -1;
    g_wsUrl.clear();
    g_itemsKnown = g_itemsAnnounced = false;
    {
        std::lock_guard lk(g_lock);
        g_remotes.clear();
        g_outbox.clear();
    }
    set_status("Connecting to " + server + "...");
    if (std::string_view(server).starts_with("ws://") || std::string_view(server).starts_with("wss://"))
    {
        g_wsUrl = server;
        log_info("mp: joining room {} on {} as {}", g_room, g_wsUrl, g_name);
        g_running = true;
        g_thread = std::thread(run);
        return;
    }
    std::string host = server, port = std::to_string(kDefaultPort);
    if (auto colon = host.rfind(':'); colon != std::string::npos)
    {
        port = host.substr(colon + 1);
        host = host.substr(0, colon);
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res)
    {
        log_error("mp: can't resolve the server {}:{}", host, port);
        set_status("Can't find the server " + server);
        return;
    }
    g_addrLen = (int)res->ai_addrlen;
    memcpy(&g_addr, res->ai_addr, res->ai_addrlen);
    g_sock = socket(res->ai_family, SOCK_DGRAM, IPPROTO_UDP);
    freeaddrinfo(res);
    if (g_sock == INVALID_SOCKET)
    {
        log_error("mp: no UDP socket");
        set_status("No network socket");
        return;
    }
    u_long nonblocking = 1;
    ioctlsocket(g_sock, FIONBIO, &nonblocking);
    if (::connect(g_sock, (sockaddr*)&g_addr, g_addrLen) != 0)
        log_warn("mp: connect() failed ({})", WSAGetLastError());
    log_info("mp: joining room {} on {}:{} as {}", g_room, host, port, g_name);
    g_running = true;
    g_thread = std::thread(run);
}

void disconnect()
{
    bool was = g_running || g_thread.joinable();
    g_running = false;
    if (g_thread.joinable()) g_thread.join();
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    g_sock = INVALID_SOCKET;
    g_joined = false;
    if (was) set_status("Not connected");
}

void start()
{
    const char* server = std::getenv("WF_MP_SERVER");
    if (!server || !*server)
    {
        settings::Settings s;
        {
            std::lock_guard lk(settings::lock());
            s = settings::get();
        }
        if (s.mpAutoConnect && !s.mpServer.empty()) connect(s.mpServer, s.mpRoom, s.mpName);
        return;
    }
    const char* room = std::getenv("WF_MP_ROOM");
    const char* name = std::getenv("WF_MP_NAME");
    connect(server, room ? room : "PUBLIC", name ? name : "Link");
}

void stop() { disconnect(); }

std::string status()
{
    std::lock_guard lk(g_statusLock);
    return g_status;
}

std::vector<std::string> room_players()
{
    std::lock_guard lk(g_lock);
    std::vector<std::string> out;
    if (g_joined) out.push_back(g_name + " (you)");
    for (const auto& [id, r] : g_remotes) out.push_back(r.name);
    return out;
}

std::vector<PlayerInfo> room_info()
{
    std::vector<PlayerInfo> out;
    char here[9] = {};
    for (int i = 0; i < 8; i++) here[i] = (char)rd8(kStartStage + i);
    int8_t hereRoom = (int8_t)rd8(kStartStage + 10);
    std::lock_guard lk(g_lock);
    if (!g_joined) return out;
    out.push_back({g_name, g_color.load(), true, place_name(here, hereRoom), rando::playing_world(), true});
    for (const auto& [id, r] : g_remotes)
    {
        PlayerInfo p{r.name, r.color, false, {}, r.world, r.world == rando::playing_world()};
        if (!r.snaps.empty())
        {
            const LinkView& v = r.snaps.back().view;
            char stage[9] = {};
            std::memcpy(stage, v.stage, 8);
            p.place = place_name(stage, v.room);
        }
        out.push_back(std::move(p));
    }
    return out;
}

uint32_t tunic_color(int color) { return tunic(color); }

std::vector<ChatEntry> recent_chat(size_t count)
{
    std::vector<ChatEntry> out;
    double t = now_s();
    std::lock_guard lk(g_lock);
    size_t first = g_chat.size() > count ? g_chat.size() - count : 0;
    for (size_t i = first; i < g_chat.size(); i++) out.push_back({g_chat[i].name, g_chat[i].text, g_chat[i].color, t - g_chat[i].time});
    return out;
}

void send_chat(const std::string& text)
{
    if (text.empty() || !g_joined) return;
    std::lock_guard lk(g_lock);
    g_outbox.push_back(text);
}

std::atomic<bool> g_chatOpen{false};
std::mutex g_typingLock;
std::string g_typing;

bool chat_open() { return g_chatOpen; }

void on_chat_event(const void* sdlEvent)
{
    const SDL_Event& e = *(const SDL_Event*)sdlEvent;
    if (!g_joined) return;
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat)
    {
        if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER)
        {
            if (g_chatOpen)
            {
                std::string text;
                {
                    std::lock_guard lk(g_typingLock);
                    text.swap(g_typing);
                }
                send_chat(text);
                g_chatOpen = false;
            }
            else
                g_chatOpen = true;
            return;
        }
        if (g_chatOpen && e.key.key == SDLK_ESCAPE)
        {
            std::lock_guard lk(g_typingLock);
            g_typing.clear();
            g_chatOpen = false;
            return;
        }
    }
    if (!g_chatOpen) return;
    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_BACKSPACE)
    {
        std::lock_guard lk(g_typingLock);
        while (!g_typing.empty() && (g_typing.back() & 0xC0) == 0x80) g_typing.pop_back();
        if (!g_typing.empty()) g_typing.pop_back();
    }
    else if (e.type == SDL_EVENT_TEXT_INPUT)
    {
        std::lock_guard lk(g_typingLock);
        if (g_typing.size() < 200) g_typing += e.text.text;
    }
}

void render_chat()
{
    if (!g_joined && g_chat.empty()) return;
    ImVec2 size = ImGui::GetIO().DisplaySize;
    float s = size.y / 1080.0f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    float fs = 26.0f * s;
    double t = now_s();
    std::vector<ChatLine> lines;
    {
        std::lock_guard lk(g_lock);
        for (const ChatLine& c : g_chat)
            if (g_chatOpen || t - c.time < chat_seconds()) lines.push_back(c);
    }
    if (lines.size() > 8) lines.erase(lines.begin(), lines.end() - 8);
    static size_t s_logged = 0;
    if (lines.size() > s_logged) log_info("mp: chat box shows {} line(s), last: {}", lines.size(), lines.back().text);
    s_logged = std::max(s_logged, lines.size());
    float x = 24.0f * s, y = size.y * 0.62f;
    float lineH = fs * 1.25f;
    float boxY = y + 8 * lineH + 8 * s;
    y = boxY - 8 * s - lines.size() * lineH;
    for (const ChatLine& c : lines)
    {
        float fade = g_chatOpen ? 1.0f : (float)std::clamp((chat_seconds() - (t - c.time)) / 2.0, 0.0, 1.0);
        int a = (int)(fade * 255);
        std::string name = c.name + ": ";
        ImVec2 ns = font->CalcTextSizeA(fs, FLT_MAX, 0, name.c_str());
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, c.text.c_str());
        dl->AddRectFilled(ImVec2(x - 6 * s, y - 2 * s), ImVec2(x + ns.x + ts.x + 6 * s, y + lineH - 2 * s), IM_COL32(0, 0, 0, a * 110 / 255), 6 * s);
        ImU32 nameCol = c.color < 0 ? IM_COL32(255, 255, 255, a) : ((tunic(c.color) & 0x00FFFFFF) | ((ImU32)a << 24));
        dl->AddText(font, fs, ImVec2(x + 2 * s, y + 2 * s), IM_COL32(0, 0, 0, a), name.c_str());
        dl->AddText(font, fs, ImVec2(x, y), nameCol, name.c_str());
        dl->AddText(font, fs, ImVec2(x + ns.x + 2 * s, y + 2 * s), IM_COL32(0, 0, 0, a), c.text.c_str());
        dl->AddText(font, fs, ImVec2(x + ns.x, y), IM_COL32(255, 255, 255, a), c.text.c_str());
        y += lineH;
    }
    if (g_chatOpen)
    {
        std::string typing;
        {
            std::lock_guard lk(g_typingLock);
            typing = g_typing;
        }
        bool caret = std::fmod(t, 1.0) < 0.5;
        std::string shown = "Say: " + typing + (caret ? "_" : " ");
        dl->AddRectFilled(ImVec2(x - 8 * s, boxY - 4 * s), ImVec2(x + 760 * s, boxY + lineH + 2 * s), IM_COL32(0, 0, 0, 170), 8 * s);
        dl->AddText(font, fs, ImVec2(x, boxY), IM_COL32(255, 255, 255, 255), shown.c_str());
    }
    else if (g_joined && lines.empty())
    {
        dl->AddText(font, fs * 0.75f, ImVec2(x, boxY), IM_COL32(255, 255, 255, 110), "Enter: chat");
    }
}

bool connected() { return g_joined; }
std::string room_code() { return g_room; }
int player_count()
{
    std::lock_guard lk(g_lock);
    return (int)g_remotes.size() + 1;
}

void sync_items(bool playing)
{
    if (!g_joined || !playing || !items_valid() || !share_items())
    {
        g_itemsIn.clear();
        return;
    }
    for (const auto& [slot, item] : g_itemsIn)
    {
        if (item == 0xFF || rd8(kItemSlots[0] + slot) != 0xFF) continue;
        for (uint32_t base : kItemSlots) wr8(base + slot, item);
        g_itemsSeen[slot] = item;
        log_info("mp: shared item {:02X} into slot {}", item, slot);
    }
    g_itemsIn.clear();
    for (int i = 0; i < kItemSlotCount; i++)
    {
        uint8_t v = rd8(kItemSlots[0] + i);
        bool announce = !g_itemsAnnounced ? v != 0xFF : g_itemsKnown && v != 0xFF && v != g_itemsSeen[i];
        if (announce) g_itemsOut.push_back({(uint8_t)i, v});
        g_itemsSeen[i] = v;
    }
    g_itemsKnown = g_itemsAnnounced = true;
}

void tick()
{
    static const bool ghost = std::getenv("WF_MP_GHOST") != nullptr;
    if (!g_running && !ghost) return;
    static const char* say = std::getenv("WF_MP_SAY");
    static bool said = false;
    if (say && !said && g_joined && g_frameNumber >= (uint32_t)std::atoi(say))
        if (const char* text = std::strchr(say, ':'))
        {
            std::string t = text + 1;
            std::replace(t.begin(), t.end(), '_', ' ');
            send_chat(t);
            said = true;
        }
    uint32_t actor = rd32(kPlayerPtr);
    std::lock_guard lk(g_lock);
    sync_items(actor != 0);
    g_localValid = false;
    if (actor < 0x10000000 || actor >= 0x50000000) return;
    LinkView& v = g_local;
    v.viewId = (uint16_t)(std::max(0, g_id.load()) * 100 + 1);
    v.kind = 1;
    for (int i = 0; i < 8; i++) v.stage[i] = (char)rd8(kStartStage + i);
    v.room = (int8_t)rd8(actor + kActorRoom);
    v.flags = 0;
    v.x = std::bit_cast<float>(rd32(actor + kActorPos));
    v.y = std::bit_cast<float>(rd32(actor + kActorPos + 4));
    v.z = std::bit_cast<float>(rd32(actor + kActorPos + 8));
    v.angleY = (int16_t)rd16(actor + kActorAngle + 2);
    v.shapeY = (int16_t)rd16(actor + kActorShape + 2);
    uint32_t ship = rd32(kShipPtr);
    if (ship >= 0x10000000 && ship < 0x50000000)
    {
        v.shipX = std::bit_cast<float>(rd32(ship + kActorPos));
        v.shipY = std::bit_cast<float>(rd32(ship + kActorPos + 4));
        v.shipZ = std::bit_cast<float>(rd32(ship + kActorPos + 8));
        for (int i = 0; i < 3; i++) v.shipAngle[i] = (int16_t)rd16(ship + kActorShape + 2 * i);
        static const float testDx = std::getenv("WF_MP_TEST_SHIPDX") ? (float)std::atof(std::getenv("WF_MP_TEST_SHIPDX")) : 0.0f;
        v.shipX += testDx;
        if (std::isfinite(v.shipX) && std::isfinite(v.shipY) && std::isfinite(v.shipZ)) v.flags |= 1;
    }
    g_localValid = std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

void render_overlay()
{
    if (!g_running) return;
    game::Camera cam;
    bool haveCamera = game::camera(cam);
    ImVec2 size = ImGui::GetIO().DisplaySize;
    double t = now_s() - kInterpDelay;
    std::lock_guard lk(g_lock);
    static const bool debug = std::getenv("WF_MP_DEBUG") != nullptr;
    static double lastDebug = 0;
    if (debug && t - lastDebug > 2)
    {
        lastDebug = t;
        std::string out = std::format("mp debug: camera {} local {} ({:.0f},{:.0f},{:.0f}) {} display {}x{}", haveCamera, g_localValid,
                                      g_local.x, g_local.y, g_local.z, std::string(g_local.stage, strnlen(g_local.stage, 8)), size.x, size.y);
        for (auto& [id, r] : g_remotes)
        {
            LinkView v{};
            bool ok = sample(r.snaps, t, v);
            ImVec2 sp{};
            float depth = 0;
            bool on = ok && haveCamera && project(cam, v.x, v.y, v.z, size, sp, depth);
            out += std::format(" | {} snaps {} sample {} ({:.0f},{:.0f},{:.0f}) {} screen {} ({:.0f},{:.0f}) depth {:.0f}", r.name, r.snaps.size(), ok,
                               v.x, v.y, v.z, std::string(v.stage, strnlen(v.stage, 8)), on, sp.x, sp.y, depth);
        }
        if (haveCamera)
            out += std::format(" | C t=({:.0f},{:.0f},{:.0f}) P00 {:.3f} P11 {:.3f} P32 {:.2f}", cam.viewToWorld[3], cam.viewToWorld[7],
                               cam.viewToWorld[11], cam.projection[0], cam.projection[5], cam.projection[14]);
        log_info("{}", out);
    }
    if (!haveCamera) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!g_localValid || !show_names() || ui::menu::is_open()) return;
    for (auto& [id, r] : g_remotes)
    {
        LinkView v;
        if (!sample(r.snaps, t, v)) continue;
        if (memcmp(v.stage, g_local.stage, 8) != 0) continue;
        ImVec2 feet, head;
        float depth;
        if (!project(cam, v.x, v.y, v.z, size, feet, depth) || !project(cam, v.x, v.y + 140.0f, v.z, size, head, depth)) continue;
        float s = std::clamp(size.y / 1080.0f * 1500.0f / depth, 0.35f, 1.6f);
        ImU32 col = tunic(r.color);
        dl->AddLine(head, feet, (col & 0x00FFFFFF) | 0x90000000, 3.0f * s);
        dl->AddCircleFilled(head, 11.0f * s, col, 20);
        dl->AddCircle(head, 11.0f * s, IM_COL32(255, 255, 255, 230), 20, 2.5f * s);
        ImFont* font = ImGui::GetFont();
        float fs = std::max(14.0f, 26.0f * s);
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, r.name.c_str());
        ImVec2 at(head.x - ts.x * 0.5f, head.y - 16.0f * s - ts.y);
        dl->AddRectFilled(ImVec2(at.x - 8, at.y - 3), ImVec2(at.x + ts.x + 8, at.y + ts.y + 3), IM_COL32(0, 0, 0, 150), 8.0f);
        dl->AddText(font, fs, at, IM_COL32(255, 255, 255, 255), r.name.c_str());
    }
}

}

void wf_mp_render()
{
    wf::mp::render_overlay();
    wf::mp::render_chat();
}

namespace wf::mp {

bool active()
{
    static const bool ghost = std::getenv("WF_MP_GHOST") != nullptr;
    return (g_running && g_joined) || ghost;
}
void new_frame() { g_frameNumber++; }
uint32_t frame_number() { return g_frameNumber; }

void set_local_pose(uint16_t part, const float* bones, uint32_t count, float x, float y, float z)
{
    count = std::min(count, kMaxBones);
    std::vector<int16_t> q(count * 12);
    auto quant = [](float v, float scale) { return (int16_t)std::clamp(std::lround(v * scale), -32767l, 32767l); };
    for (uint32_t b = 0; b < count; b++)
    {
        const float* m = bones + b * 12;
        int16_t* o = q.data() + b * 12;
        for (int r = 0; r < 3; r++)
        {
            for (int c = 0; c < 3; c++) o[r * 4 + c] = quant(m[r * 4 + c], kRotScale);
            o[r * 4 + 3] = quant(m[r * 4 + 3] - (r == 0 ? x : r == 1 ? y : z), kPosScale);
        }
    }
    std::lock_guard lk(g_lock);
    LocalPose& lp = g_localPose[part];
    lp.frame = g_frameNumber.load();
    lp.q = std::move(q);
    if (g_localValid)
    {
        g_local.x = x;
        g_local.y = y;
        g_local.z = z;
    }
}

void mark_part_drawn(uint16_t part, uint32_t mesh, uint32_t paletteFrame)
{
    std::lock_guard lk(g_lock);
    auto it = g_localPose.find(part);
    if (it == g_localPose.end()) return;
    it->second.drawn = g_frameNumber.load();
    auto& meshes = it->second.meshes;
    meshes[mesh] = std::max(meshes[mesh], paletteFrame);
    if (meshes.size() > 24) std::erase_if(meshes, [&](const auto& kv) { return paletteFrame - kv.second > 3; });
}

void set_local_rigid(uint32_t key, const float* f, uint32_t matrices, float x, float y, float z)
{
    LocalRigid r{g_frameNumber.load(), std::vector<int16_t>(matrices * 12)};
    auto quant = [](float v, float scale) { return (int16_t)std::clamp(std::lround(v * scale), -32767l, 32767l); };
    for (uint32_t mi = 0; mi < matrices; mi++)
        for (int row = 0; row < 3; row++)
        {
            const float* fm = f + mi * 12;
            int16_t* q = r.q.data() + mi * 12;
            for (int c = 0; c < 3; c++) q[row * 4 + c] = quant(fm[row * 4 + c], kRotScale);
            q[row * 4 + 3] = quant(fm[row * 4 + 3] - (row == 0 ? x : row == 1 ? y : z), kPosScale);
        }
    std::lock_guard lk(g_lock);
    g_localRigid[key] = r;
    if (g_localRigid.size() > 128)
        std::erase_if(g_localRigid, [&](const auto& kv) { return r.frame - kv.second.frame > 120; });
}

void set_ship(Placement& p, const LinkView& v, bool ghost)
{
    if (!(v.flags & 1)) return;
    p.ship = true;
    p.shipX = v.shipX;
    p.shipY = v.shipY;
    p.shipZ = v.shipZ;
    game::Camera cam;
    if (ghost && game::camera(cam))
    {
        const auto& c = cam.viewToWorld;
        float fx = -c[2], fz = -c[10], len = std::sqrt(fx * fx + fz * fz);
        if (len > 0.01f)
        {
            p.shipX = c[3] + 1600.0f * fx / len + 700.0f * c[0];
            p.shipZ = c[11] + 1600.0f * fz / len + 700.0f * c[8];
        }
    }
    for (int i = 0; i < 3; i++) p.shipAngle[i] = v.shipAngle[i] * (3.14159265f / 32768.0f);
}

void remote_placements(std::vector<Placement>& out)
{
    out.clear();
    double t = now_s() - kInterpDelay;
    std::lock_guard lk(g_lock);
    if (!g_localValid) return;
    static const bool ghost = std::getenv("WF_MP_GHOST") != nullptr;
    if (ghost)
    {
        float yaw = g_local.shapeY * (3.14159265f / 32768.0f);
        static const int ghostColor = std::clamp(std::atoi(std::getenv("WF_MP_GHOST")), 1, 7);
        Placement p{g_local.x + 70 * std::cos(yaw), g_local.y, g_local.z - 70 * std::sin(yaw), yaw, ghostColor, {}, {}};
        set_ship(p, g_local, true);
        uint32_t frame = g_frameNumber;
        for (const auto& [key, r] : g_localRigid)
        {
            if (frame - r.frame > 2) continue;
            std::vector<float> mat(r.q.size());
            for (size_t i = 0; i < mat.size(); i++) mat[i] = r.q[i] * ((i % 4 == 3) ? 1.0f / kPosScale : 1.0f / kRotScale);
            p.rigid.emplace_back(key, std::move(mat));
        }
        for (const auto& [part, pose] : g_localPose)
        {
            if (frame - pose.frame > 2 || frame - pose.drawn > 2) continue;
            Placement::Pose pp{part, std::vector<float>(pose.q.size()), {}};
            for (size_t i = 0; i < pp.bones.size(); i++) pp.bones[i] = pose.q[i] * ((i % 4 == 3) ? 1.0f / kPosScale : 1.0f / kRotScale);
            for (const auto& [mesh, f] : pose.meshes)
                if (pose.frame - f <= 4) pp.meshes.push_back(mesh);
            p.poses.push_back(std::move(pp));
        }
        out.push_back(std::move(p));
    }
    for (auto& [id, r] : g_remotes)
    {
        LinkView v;
        if (!sample(r.snaps, t, v) || memcmp(v.stage, g_local.stage, 8) != 0) continue;
        Placement p{v.x, v.y, v.z, v.shapeY * (3.14159265f / 32768.0f), r.color, {}, {}};
        set_ship(p, v, false);
        for (const auto& [key, d] : r.rigid)
        {
            if (d.empty() || t - d.back().time > kPartStale) continue;
            const Remote::RigidSnapshot* a = &d.front();
            const Remote::RigidSnapshot* b = a;
            for (const auto& s : d)
            {
                b = &s;
                if (s.time >= t) break;
                a = b;
            }
            float k = b->time > a->time ? std::clamp((float)((t - a->time) / (b->time - a->time)), 0.0f, 1.0f) : 1.0f;
            if (a->q.size() != b->q.size()) a = b;
            std::vector<float> mat(b->q.size());
            for (size_t i = 0; i < mat.size(); i++)
                mat[i] = (a->q[i] + (b->q[i] - a->q[i]) * k) * ((i % 4 == 3) ? 1.0f / kPosScale : 1.0f / kRotScale);
            p.rigid.emplace_back(key, std::move(mat));
        }
        for (const auto& [part, poses] : r.poses)
        {
        if (!poses.empty() && t - poses.back().time < kPartStale)
        {
            const PoseSnapshot* a = &poses.front();
            const PoseSnapshot* b = a;
            for (size_t i = 0; i < poses.size(); i++)
            {
                b = &poses[i];
                if (poses[i].time >= t) break;
                a = b;
            }
            if (a->meshes != b->meshes) a = b;
            if (a->q.size() == b->q.size())
            {
                float k = b->time > a->time ? std::clamp((float)((t - a->time) / (b->time - a->time)), 0.0f, 1.0f) : 1.0f;
                Placement::Pose pp{part, std::vector<float>(a->q.size()), b->meshes};
                for (size_t bone = 0; bone + 12 <= a->q.size(); bone += 12)
                {
                    float kb = junk_bone(a->q.data() + bone) || junk_bone(b->q.data() + bone) ? 1.0f : k;
                    for (size_t i = bone; i < bone + 12; i++)
                    {
                        float scale = (i % 4 == 3) ? 1.0f / kPosScale : 1.0f / kRotScale;
                        pp.bones[i] = (a->q[i] + (b->q[i] - a->q[i]) * kb) * scale;
                    }
                }
                p.poses.push_back(std::move(pp));
            }
        }
        }
        out.push_back(std::move(p));
    }
}

}
