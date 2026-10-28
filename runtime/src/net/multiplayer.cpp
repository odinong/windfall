#include "net/multiplayer.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <set>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
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
#include "enhancements/actors.h"
#include "hle/hle.h"
#include "wf/ppc.h"

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

// the save the game plays from (dSv_player_c). 0x14822A00 holds the packed save file image, which the game only reads
// when a file is loaded, so writes there never reach the inventory
constexpr uint32_t kSave = 0x148D196C;
constexpr uint32_t kItems = kSave + 0x3C, kGotItems = kSave + 0x51;  // item menu slots (0xFF empty), obtained bits per slot
constexpr uint32_t kArrows = kSave + 0x69, kBombs = kSave + 0x6A, kMaxArrows = kSave + 0x6F, kMaxBombs = kSave + 0x70;
constexpr uint32_t kWallet = kSave + 0x12, kMaxMagic = kSave + 0x13, kMagic = kSave + 0x14;
constexpr uint32_t kEquip = kSave + 0x0E;    // worn sword, shield, bracelet item ids
constexpr uint32_t kCollect = kSave + 0xB4;  // owned bits: swords, shields, bracelets, pirate's charm, hero's charm
constexpr uint32_t kSongs = kSave + 0xBD, kTriforce = kSave + 0xBE, kPearls = kSave + 0xBF;
constexpr uint32_t kMaxLife = kSave + 0x00, kLife = kSave + 0x02;
// story: event flags, each stage group's memory (chests, switches, keys, dungeon items, boss) and the sea's flags.
// The stage group being played lives in kLiveMemory (kStageNo says which) and goes back to its slot when it's left
constexpr uint32_t kEvents = kSave + 0x624, kStageMemory = kSave + 0x380, kOcean = kSave + 0x5C0;
constexpr uint32_t kCharts = kSave + 0xC4;  // treasure/Triforce charts owned, deciphered, salvaged; sea map sectors
constexpr uint32_t kLiveMemory = kSave + 0x778, kStageNo = kSave + 0x79C;
// temporary switches: the dungeon's (cleared when it's left) and each loaded room's (a zone: room number, 10 bytes of
// switch/item bits, then actor flags; cleared when the room unloads)
constexpr uint32_t kDanSwitches = kSave + 0x7A0, kZones = kSave + 0x7A8;
constexpr int kDanBytes = 8, kZoneSize = 0x4C, kZoneCount = 32, kZoneBits = 2, kZoneBitBytes = 10;
constexpr uint32_t kNextStage = 0x104741F0, kNextEnable = 0x104741FC;
constexpr int kItemSlotCount = 21, kFirstBottle = 14, kBottles = 4, kCollectCount = 5;
constexpr uint8_t kEmptyBottle = 0x50;
constexpr uint8_t kEventWorld = 2;      // payload: the sender's rando::playing_world() text, resent every few seconds
constexpr uint8_t kEventInventory = 3;  // payload: Inventory, sent on change and every few seconds

#pragma pack(push, 1)
struct Inventory
{
    uint8_t items[kItemSlotCount];  // bottles show as empty ones: what's in them stays with their owner
    uint8_t got[kItemSlotCount];
    uint8_t maxArrows, maxBombs, wallet, maxMagic;
    uint8_t collect[kCollectCount];
    uint8_t maxLife[2];  // as in the save: big-endian quarter hearts
    uint8_t songs, triforce, pearls;
};
#pragma pack(pop)
static_assert(sizeof(Inventory) == 56);

std::vector<Inventory> g_invIn;
std::optional<Inventory> g_invOut;
Inventory g_invSent{};
bool g_invAnnounced = false;
double g_invSentTime = 0;

bool is_bottle(int slot) { return slot >= kFirstBottle && slot < kFirstBottle + kBottles; }

// what this game's own story took away (the Forsaken Fortress takes the sword, a pearl set in its statue may go):
// the room doesn't hand it back until this game gives it back itself
bool g_takenSlot[kItemSlotCount] = {};
uint8_t g_takenCollect[kCollectCount] = {}, g_takenPearls = 0;
Inventory g_invLocal{};
bool g_invLocalKnown = false;

// a better version of what a slot already holds
int item_rank(uint8_t item)
{
    switch (item)
    {
    case 0x77: return 1;  // Swift Sail
    case 0x26: return 1;  // Deluxe Picto Box
    case 0x35: return 1;  // Fire and Ice Arrows
    case 0x36: return 2;  // Light Arrows
    default: return 0;
    }
}

bool items_valid()
{
    int filled = 0;
    for (int i = 0; i < kItemSlotCount; i++)
    {
        uint8_t a = rd8(kItems + i);
        if (a == 0x00 || a == 0xFE) return false;
        filled += a != 0xFF;
    }
    return filled > 0;
}

// a save is being played: not the title screen's demo or file select
bool in_game_stage()
{
    char stage[9] = {};
    for (int i = 0; i < 8; i++) stage[i] = (char)rd8(kStartStage + i);
    return stage[0] && std::strcmp(stage, "Name") != 0 && std::strncmp(stage, "sea_T", 5) != 0;
}

Inventory read_inventory()
{
    Inventory v{};
    for (int i = 0; i < kItemSlotCount; i++)
    {
        uint8_t item = rd8(kItems + i);
        v.items[i] = is_bottle(i) && item != 0xFF ? kEmptyBottle : item;
        v.got[i] = is_bottle(i) ? 0 : rd8(kGotItems + i);
    }
    v.maxArrows = rd8(kMaxArrows);
    v.maxBombs = rd8(kMaxBombs);
    v.wallet = rd8(kWallet);
    v.maxMagic = rd8(kMaxMagic);
    for (int i = 0; i < kCollectCount; i++) v.collect[i] = rd8(kCollect + i);
    v.maxLife[0] = rd8(kMaxLife);
    v.maxLife[1] = rd8(kMaxLife + 1);
    v.songs = rd8(kSongs);
    v.triforce = rd8(kTriforce);
    v.pearls = rd8(kPearls);
    return v;
}

// takes whatever a partner has that this save doesn't: items and their upgrades, obtained flags, bigger quiver, bomb bag,
// wallet and magic meter, and swords, shields and bracelets
void merge_inventory(const Inventory& in)
{
    for (int i = 0; i < kItemSlotCount; i++)
    {
        uint8_t mine = rd8(kItems + i), theirs = in.items[i];
        if (is_bottle(i))
        {
            if (theirs != 0xFF && mine == 0xFF)
            {
                wr8(kItems + i, kEmptyBottle);
                log_info("mp: shared an empty bottle into slot {}", i);
            }
            continue;
        }
        if (theirs != 0xFF && !g_takenSlot[i] && (mine == 0xFF || item_rank(theirs) > item_rank(mine)))
        {
            wr8(kItems + i, theirs);
            log_info("mp: shared item {:02X} into slot {}", theirs, i);
        }
        if (in.got[i] & ~rd8(kGotItems + i)) wr8(kGotItems + i, rd8(kGotItems + i) | in.got[i]);
    }
    // a bigger capacity comes full, as when the game hands out the upgrade
    auto raise = [](uint32_t maxAddr, uint32_t curAddr, uint8_t v, const char* what) {
        if (v <= rd8(maxAddr)) return;
        wr8(maxAddr, v);
        if (curAddr) wr8(curAddr, v);
        log_info("mp: shared {} ({})", what, v);
    };
    raise(kMaxArrows, kArrows, in.maxArrows, "quiver");
    raise(kMaxBombs, kBombs, in.maxBombs, "bomb bag");
    raise(kMaxMagic, kMagic, in.maxMagic, "magic meter");
    raise(kWallet, 0, in.wallet, "wallet");
    if (uint16_t life = (uint16_t)(in.maxLife[0] << 8 | in.maxLife[1]); life > rd16(kMaxLife))
    {
        wr16(kMaxLife, life);
        wr16(kLife, life);
        log_info("mp: shared hearts ({} quarters)", life);
    }
    for (auto [addr, bits, what] : {std::tuple{kSongs, in.songs, "songs"}, {kTriforce, in.triforce, "Triforce pieces"},
                                    {kPearls, (uint8_t)(in.pearls & ~g_takenPearls), "pearls"}})
        if (bits & ~rd8(addr))
        {
            wr8(addr, rd8(addr) | bits);
            log_info("mp: shared {} ({:02X})", what, rd8(addr));
        }
    // a newly gained sword, shield or bracelet is worn, best first
    static const uint8_t kSwords[] = {0x38, 0x39, 0x3A, 0x3E}, kShields[] = {0x3B, 0x3C}, kBracelets[] = {0x28};
    static const std::pair<const uint8_t*, int> kWear[] = {{kSwords, 4}, {kShields, 2}, {kBracelets, 1}};
    for (int c = 0; c < kCollectCount; c++)
    {
        uint8_t mine = rd8(kCollect + c), all = mine | (in.collect[c] & ~g_takenCollect[c]);
        if (all == mine) continue;
        wr8(kCollect + c, all);
        log_info("mp: shared equipment {} ({:02X})", c, all);
        if (c >= 3) continue;
        auto [ids, n] = kWear[c];
        for (int b = n - 1; b >= 0; b--)
            if (all >> b & 1)
            {
                wr8(kEquip + c, ids[b]);
                break;
            }
    }
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

// Story sync. Everyone plays the host's story (the first player in the room among those playing the same world): a
// player who joins takes on the host's story once (a snapshot), and after that every change anyone's game makes to the
// story bytes goes to everyone as bit changes. Bits the games keep flipping against each other (a switch only held
// down in one game, a flag that follows where each player is) stop being shared; the rest of their byte still is.
constexpr int kEventBytes = 0x100, kMemBytes = 0x24, kMemCount = 16, kOceanBytes = 0x64, kChartBytes = 0x81;
constexpr int kStoryMemory = kEventBytes, kStoryOcean = kStoryMemory + kMemBytes * kMemCount, kStoryCharts = kStoryOcean + kOceanBytes;
constexpr int kStoryBytes = kStoryCharts + kChartBytes;
constexpr uint8_t kEventStory = 4;   // payload: the host's story bytes, every few seconds
constexpr uint8_t kEventChange = 5;  // payload: {u16 offset, u8 set, u8 clear}... (offsets into the story bytes)
constexpr uint8_t kEventLocal = 6;   // payload: stage[8], {u8 area (0 dungeon, 1 room), s8 room, u8 offset, u8 set, u8 clear}...
constexpr double kChangeResend = 2.0;

struct StoryChange { uint16_t offset; uint8_t set, clear; };
struct Churn { double since = 0; uint8_t count[8] = {}; uint8_t muted = 0; };  // per bit
std::array<uint8_t, kStoryBytes> g_storyShadow{};
std::array<Churn, kStoryBytes> g_churn{};
std::array<std::pair<uint8_t, double>, kStoryBytes> g_recent{};  // bits changed here lately and when, resent a while
struct StorySnap { int from; uint8_t epoch; std::vector<uint8_t> bytes; };
std::optional<StorySnap> g_storySnapIn;
int g_storyFrom = -1, g_storyEpochSeen = -1;  // whose story this game took on, and which of their loads
uint8_t g_storyEpoch = 0;                       // bumped when this game (as host) loads a file again
// temporary switches, shared with whoever is in the same stage (and room)
struct LocalChange { uint8_t area; int8_t room; uint8_t offset, set, clear; };
struct ZoneShadow { int room = -1, age = 0; uint8_t bits[kZoneBitBytes] = {}; };
char g_localStage[8] = {};
bool g_danValid = false;
uint8_t g_danShadow[kDanBytes] = {};
ZoneShadow g_zoneShadow[kZoneCount];
std::map<uint32_t, std::pair<uint8_t, double>> g_localRecent;
std::map<uint32_t, Churn> g_localChurn;
std::vector<std::pair<std::array<char, 8>, LocalChange>> g_localIn;
double g_localSent = 0;

// Enemies: anything locked onto in battle, matched between games by profile, room and spawn point. Damage is shared
// (each game keeps the lowest health anyone reported) and an enemy another player defeated is removed here.
constexpr uint32_t kAttnFlags = 0x39C, kMaxHealth = 0x3A0, kHealth = 0x3A1;
constexpr uint32_t kAttnBattle = 4;
constexpr uint16_t kProcPlayer = 0x00A8;
constexpr uint32_t kFpcDtDelete = 0x025DE1B8;  // fpcDt_Delete(process): queues it for deletion, as an enemy's death does
constexpr uint8_t kEventEnemy = 7;  // payload: stage[8], {u32 key, s8 health}...
constexpr uint8_t kEventWarp = 8;   // payload: s8 stage group, next stage {name[8], s16 point, s8 room, s8 layer}
constexpr uint8_t kEventEnemyPos = 9;  // payload: stage[8], f32 time in the stage, {u32 key, f32 x, y, z, s16 angle, s16 shape angle}...
// where enemies are: the player nearest an enemy runs it (their game's copy moves it); everyone else's copy follows
struct EnemyPos { float x, y, z; int16_t angle, shape; double time; int from; };
std::map<uint32_t, EnemyPos> g_enemyPosIn;
std::array<char, 8> g_enemyPosStage{};
std::map<uint32_t, int> g_enemyOwner;
double g_enemyPosSent = 0;
struct StageTime { std::array<char, 8> stage; float seconds; double received; };
std::map<int, StageTime> g_stageTime;  // how long each other player has been in their stage, as they last said
double g_stageSince = 0;               // when this game came into its stage
// pickups the room already took (a chest or heart still showing as there here): a heart container or key gained
// without anything else in the story changing around it is a second copy of the same pickup, and is taken back
double g_lastStoryBitTime = -100;
struct HeldKey { int offset; uint8_t before, after; double time; };
std::vector<HeldKey> g_heldKeys;
uint16_t g_lifeSeen = 0;
bool g_lifeKnown = false;
struct HeldLife { uint16_t amount; double time; };
std::vector<HeldLife> g_heldLife;
struct EnemySeen { uint32_t actor; int8_t health; };
std::map<uint32_t, EnemySeen> g_enemies;
std::map<uint32_t, std::pair<int8_t, double>> g_enemyRecent;  // health this game dealt lately, resent a while
// the lowest health the others reported for each enemy, held for the copy that was alive here when it came: one that
// comes back later (the room loaded again) is a fresh enemy, as it would be in the game (a key carrier drops its key)
struct EnemyFloor { int8_t health; uint32_t actor; };
std::map<uint32_t, EnemyFloor> g_enemyFloor;
std::vector<std::pair<std::array<char, 8>, std::pair<uint32_t, int8_t>>> g_enemyIn;
std::vector<uint32_t> g_enemyDelete;  // actors to remove, done by run_game_calls
std::vector<uint32_t> g_actorRebuild; // actors to make again from their placement, done by run_game_calls
// chests and doors read the save once, when made: after the room changes one in the area this game is in, they're
// made again (once the player isn't right next to them) so they show it
bool g_staleChests = false, g_staleDoors = false;
double g_staleSince = 0;
std::set<uint64_t> g_staleDone;  // placements (profile and spot) already made again for the current change
constexpr uint32_t kFopAcmCreate = 0x025D5928;  // fopAcM_create(proc, param, pos*, room, angle*, scale*, subtype, func)
constexpr uint32_t kCurrentLayer = 0x101F3AE8;  // fpcLy_CurrentLayer: new processes join this layer
constexpr uint32_t kProcLayer = 0x2C;           // a process's layer (its layer tag's layer pointer)
// heaps: processes are made under the game heap, with the heap to go back to kept in mDoExt's save slot (the frame
// handler sets both up around the stage's processes; making one between frames has to do the same)
constexpr uint32_t kGameHeap = 0x025EE048, kSetCurrentHeap = 0x02756170, kHeapMgr = 0x101F8B4C, kSavedHeap = 0x1048CED8;
constexpr uint32_t kParam = 0xB0, kOrigAngle = 0x2F8, kOrigRoom = 0x2FE, kScale = 0x330, kSubtype = 0x2DD, kSetId = 0x2D8;
std::set<uint32_t> g_enemyGone;       // actors already removed this stage visit
char g_enemyStage[8] = {};
double g_enemySent = 0;
// boss warp: the dungeon group whose boss this game beat, then the warp out of it
int g_bossGroup = -1;
double g_bossTime = 0;
bool g_warpSent = false;
struct PendingWarp { int group; uint8_t dest[12]; double time; };
std::optional<PendingWarp> g_warpIn;
// story cutscenes that start with a stage change into one of the stage's event layers (ordinary doors and exits ask
// for layer -1): players in the same stage and room are taken along, so everyone watches it
constexpr uint8_t kEventFollow = 10;  // payload: stage[8], s8 room (-128: everyone), f32 x, z, next stage {name[8], s16 point, s8 room, s8 layer}
constexpr float kFollowReach = 1500.0f;  // a cutscene takes along the players this close (a whole island is one room)
constexpr int8_t kEveryone = -128;
// the ending (after Ganondorf falls the game goes to the ending sea): everyone playing the same game goes along,
// wherever they are, so the whole room watches it together
bool ending_stage(uint32_t stageAddr)
{
    static const char kEnding[] = "ENDumi";
    for (int i = 0; i < 6; i++)
        if ((char)rd8(stageAddr + i) != kEnding[i]) return false;
    return true;
}
struct PendingFollow { std::array<char, 8> stage; int room; float x, z; uint8_t dest[12]; double time; std::string who; };
std::optional<PendingFollow> g_followIn;
bool g_nextWasSet = false;   // the stage change request was already up last tick
bool g_ownRequest = false;   // the request up now is one this code made (not to be passed on)
std::vector<StoryChange> g_changesIn;
std::vector<std::vector<uint8_t>> g_storyOut;  // event kind + payload
bool g_storyBaseline = false, g_storyAdopted = false;
int g_storyStable = 0, g_storyGroup = -2;
double g_storySent = 0, g_changesSent = 0;

int stage_group()
{
    int n = (int8_t)rd8(kStageNo);
    return n >= 0 && n < kMemCount ? n : -1;
}

uint32_t story_addr(int offset, int group)
{
    if (offset < kStoryMemory) return kEvents + offset;
    if (offset >= kStoryCharts) return kCharts + offset - kStoryCharts;
    if (offset >= kStoryOcean) return kOcean + offset - kStoryOcean;
    int i = (offset - kStoryMemory) / kMemBytes, b = (offset - kStoryMemory) % kMemBytes;
    return i == group ? kLiveMemory + b : kStageMemory + i * kMemBytes + b;
}

// the host is the first one in the room (lowest id) of those playing the same world
int story_host()
{
    int host = g_id;
    for (const auto& [id, r] : g_remotes)
        if (r.world == rando::playing_world() && id < host) host = id;
    return host;
}

void reset_story()
{
    g_storyBaseline = g_storyAdopted = false;
    g_storyStable = 0;
    g_storySnapIn.reset();
    g_changesIn.clear();
    g_storyOut.clear();
    g_churn = {};
    g_recent = {};
    g_storyFrom = g_storyEpochSeen = -1;
    std::memset(g_localStage, 0, sizeof g_localStage);
    g_localRecent.clear();
    g_localChurn.clear();
    g_localIn.clear();
    g_enemies.clear();
    g_enemyRecent.clear();
    g_enemyFloor.clear();
    g_enemyIn.clear();
    g_enemyDelete.clear();
    g_enemyGone.clear();
    std::memset(g_enemyStage, 0, sizeof g_enemyStage);
    g_bossGroup = -1;
    g_warpSent = false;
    g_warpIn.reset();
    g_followIn.reset();
    g_enemyPosIn.clear();
    g_enemyOwner.clear();
    g_heldKeys.clear();
    g_heldLife.clear();
    g_lifeKnown = false;
}


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
            else if (n >= 7 + (int)sizeof(Inventory) && d[6] == kEventInventory && it != g_remotes.end() &&
                     it->second.world == rando::playing_world())
            {
                Inventory inv;
                memcpy(&inv, d + 7, sizeof inv);
                g_invIn.push_back(inv);
            }
            else if (d[6] == kEventStory && n == 8 + kStoryBytes && d[1] == story_host())
                g_storySnapIn = StorySnap{d[1], d[7], std::vector<uint8_t>(d + 8, d + n)};
            else if (d[6] == kEventEnemy && n >= 15 && it != g_remotes.end() && it->second.world == rando::playing_world())
            {
                std::array<char, 8> stage;
                std::memcpy(stage.data(), d + 7, 8);
                for (int i = 15; i + 5 <= n; i += 5)
                    g_enemyIn.push_back({stage, {(uint32_t)(d[i] | d[i + 1] << 8 | d[i + 2] << 16 | d[i + 3] << 24), (int8_t)d[i + 4]}});
            }
            else if (d[6] == kEventEnemyPos && n >= 19 && it != g_remotes.end() && it->second.world == rando::playing_world())
            {
                std::array<char, 8> stage;
                std::memcpy(stage.data(), d + 7, 8);
                double now = now_s();
                float seconds;
                std::memcpy(&seconds, d + 15, 4);
                g_stageTime[d[1]] = {stage, seconds, now};
                if (stage != g_enemyPosStage)
                {
                    g_enemyPosIn.clear();
                    g_enemyPosStage = stage;
                }
                for (int i = 19; i + 20 <= n; i += 20)
                {
                    uint32_t key;
                    EnemyPos e{};
                    std::memcpy(&key, d + i, 4);
                    std::memcpy(&e.x, d + i + 4, 4);
                    std::memcpy(&e.y, d + i + 8, 4);
                    std::memcpy(&e.z, d + i + 12, 4);
                    std::memcpy(&e.angle, d + i + 16, 2);
                    std::memcpy(&e.shape, d + i + 18, 2);
                    e.time = now;
                    e.from = d[1];
                    g_enemyPosIn[key] = e;
                }
            }
            else if (d[6] == kEventFollow && n == 36 && it != g_remotes.end() && it->second.world == rando::playing_world())
            {
                PendingFollow f{};
                std::memcpy(f.stage.data(), d + 7, 8);
                f.room = (int8_t)d[15];
                std::memcpy(&f.x, d + 16, 4);
                std::memcpy(&f.z, d + 20, 4);
                std::memcpy(f.dest, d + 24, 12);
                f.time = now_s();
                f.who = it->second.name;
                g_followIn = f;
            }
            else if (d[6] == kEventWarp && n == 20 && it != g_remotes.end() && it->second.world == rando::playing_world())
            {
                PendingWarp w{(int8_t)d[7], {}, now_s()};
                std::memcpy(w.dest, d + 8, 12);
                g_warpIn = w;
                log_info("mp: {} warped out of the dungeon after its boss", it->second.name);
            }
            else if (d[6] == kEventLocal && n >= 15 && it != g_remotes.end() && it->second.world == rando::playing_world())
            {
                std::array<char, 8> stage;
                std::memcpy(stage.data(), d + 7, 8);
                for (int i = 15; i + 5 <= n; i += 5) g_localIn.push_back({stage, {d[i], (int8_t)d[i + 1], d[i + 2], d[i + 3], d[i + 4]}});
            }
            else if (d[6] == kEventChange && it != g_remotes.end() && it->second.world == rando::playing_world())
                for (int i = 7; i + 4 <= n; i += 4) g_changesIn.push_back({(uint16_t)(d[i] | d[i + 1] << 8), d[i + 2], d[i + 3]});
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
            std::optional<Inventory> inv;
            {
                std::lock_guard lk(g_lock);
                inv.swap(g_invOut);
            }
            if (inv)
            {
                seq++;
                pkt.assign({'E', (uint8_t)g_id.load(), (uint8_t)seq, (uint8_t)(seq >> 8), (uint8_t)(seq >> 16), (uint8_t)(seq >> 24), kEventInventory});
                pkt.insert(pkt.end(), (const uint8_t*)&*inv, (const uint8_t*)&*inv + sizeof(Inventory));
                send_pkt(pkt);
            }
            std::vector<std::vector<uint8_t>> story;
            {
                std::lock_guard lk(g_lock);
                story.swap(g_storyOut);
            }
            for (const std::vector<uint8_t>& ev : story)
            {
                seq++;
                pkt.assign({'E', (uint8_t)g_id.load(), (uint8_t)seq, (uint8_t)(seq >> 8), (uint8_t)(seq >> 16), (uint8_t)(seq >> 24)});
                pkt.insert(pkt.end(), ev.begin(), ev.end());
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
    g_invAnnounced = false;
    {
        std::lock_guard lk(g_lock);
        g_remotes.clear();
        g_outbox.clear();
        reset_story();
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

bool share_story()
{
    std::lock_guard lk(settings::lock());
    return settings::get().mpShareStory;
}

// counts a byte's changed bits; a bit that changes more than 8 times in 20 s stops being shared. Returns the rest
uint8_t unmuted(Churn& c, uint8_t bits, double t, const std::string& what)
{
    if (t - c.since > 20.0)
    {
        c.since = t;
        std::fill(std::begin(c.count), std::end(c.count), 0);
    }
    for (int b = 0; b < 8; b++)
        if ((bits >> b & 1) && !(c.muted >> b & 1) && ++c.count[b] > 8)
        {
            c.muted |= (uint8_t)(1 << b);
            log_info("mp: {} bit {} keeps changing, no longer shared", what, b);
        }
    return bits & ~c.muted;
}

uint32_t local_key(int area, int room, int offset) { return (uint32_t)area << 16 | (uint32_t)(uint8_t)room << 8 | (uint32_t)offset; }

// the zone holding a loaded room, 0 if it isn't loaded
uint32_t zone_of(int room)
{
    for (int k = 0; k < kZoneCount; k++)
        if ((int8_t)rd8(kZones + k * kZoneSize) == room) return kZones + k * kZoneSize;
    return 0;
}

uint32_t local_addr(int area, int room, int offset)
{
    if (area == 0) return offset < kDanBytes ? kDanSwitches + offset : 0;
    uint32_t z = offset < kZoneBitBytes ? zone_of(room) : 0;
    return z ? z + kZoneBits + offset : 0;
}

void mark_stale(bool chests, bool doors, double t)
{
    if ((chests && !g_staleChests) || (doors && !g_staleDoors)) g_staleDone.clear();
    g_staleChests |= chests;
    g_staleDoors |= doors;
    g_staleSince = t;
}

bool chest_proc(uint16_t p) { return p == 0x0124 || p == 0x0125; }
bool door_proc(uint16_t p) { return p == 0x012C || p == 0x012D || p == 0x012E || p == 0x0130 || p == 0x0131; }

// makes the stale chests and doors again, leaving any the player is right at (opening it, walking through) for later
void refresh_stale(double t)
{
    if (!g_staleChests && !g_staleDoors) return;
    if (t - g_staleSince < 0.3) return;  // a burst of changes is handled together
    uint32_t pl = rd32(kPlayerPtr);
    if (!actors::valid_ptr(pl)) return;
    auto f32 = [](uint32_t a) { return std::bit_cast<float>(rd32(a)); };
    float px = f32(pl + kActorPos), py = f32(pl + kActorPos + 4), pz = f32(pl + kActorPos + 8);
    bool waiting = false;
    actors::for_each([&](uint32_t a) {
        uint16_t p = actors::proc_name(a);
        if (!((g_staleChests && chest_proc(p)) || (g_staleDoors && door_proc(p)))) return true;
        // a made-again actor has a new address: its placement says it's been done
        uint64_t spot = (uint64_t)p << 48 ^ (uint64_t)(uint32_t)std::lround(f32(a + actors::kHome)) << 24 ^
                        (uint64_t)(uint32_t)std::lround(f32(a + actors::kHome + 4)) << 12 ^ (uint32_t)std::lround(f32(a + actors::kHome + 8));
        if (g_staleDone.count(spot)) return true;
        float dx = f32(a + kActorPos) - px, dy = f32(a + kActorPos + 4) - py, dz = f32(a + kActorPos + 8) - pz;
        if (dx * dx + dy * dy + dz * dz < 250.0f * 250.0f)
        {
            waiting = true;
            return true;
        }
        g_actorRebuild.push_back(a);
        g_staleDone.insert(spot);
        return true;
    });
    if (!waiting)
    {
        g_staleChests = g_staleDoors = false;
        g_staleDone.clear();
    }
}

// temporary switches: changes go to whoever is in the same stage, applied to the same room's zone
void sync_local(double t)
{
    char stage[8];
    for (int i = 0; i < 8; i++) stage[i] = (char)rd8(kStartStage + i);
    if (std::memcmp(stage, g_localStage, 8) != 0)
    {
        std::memcpy(g_localStage, stage, 8);
        g_staleChests = g_staleDoors = false;
        g_staleDone.clear();
        g_danValid = false;
        for (ZoneShadow& z : g_zoneShadow) z = {};
        g_localRecent.clear();
    }
    bool changed = false;
    auto local_change = [&](int area, int room, int offset, uint8_t before, uint8_t after) {
        uint32_t key = local_key(area, room, offset);
        uint8_t bits = unmuted(g_localChurn[key], before ^ after, t,
                               std::format("{} switch byte {}:{}", area ? "room" : "dungeon", room, offset));
        g_lastStoryBitTime = t;  // a room's own bits change with its pickups too (a key an enemy dropped)
        if (!bits) return;
        auto& [mask, when] = g_localRecent[key];
        mask |= bits;
        when = t;
        changed = true;
    };
    for (int i = 0; i < kDanBytes; i++)
    {
        uint8_t v = rd8(kDanSwitches + i);
        if (g_danValid && v != g_danShadow[i]) local_change(0, 0, i, g_danShadow[i], v);
        g_danShadow[i] = v;
    }
    g_danValid = true;
    // a zone is compared once its room has stayed loaded a while: a room loading fresh clears its bits, which isn't
    // a change to pass on
    for (int k = 0; k < kZoneCount; k++)
    {
        uint32_t zone = kZones + k * kZoneSize;
        int room = (int8_t)rd8(zone);
        ZoneShadow& z = g_zoneShadow[k];
        if (room != z.room) z = {room, 0, {}};
        bool compare = room >= 0 && ++z.age > 30;
        for (int i = 0; i < kZoneBitBytes; i++)
        {
            uint8_t v = rd8(zone + kZoneBits + i);
            if (compare && v != z.bits[i]) local_change(1, room, i, z.bits[i], v);
            z.bits[i] = v;
        }
    }
    for (const auto& [from, c] : g_localIn)
    {
        if (std::memcmp(from.data(), stage, 8) != 0) continue;
        uint32_t a = local_addr(c.area, c.room, c.offset);
        if (!a) continue;
        uint8_t keep = g_localChurn[local_key(c.area, c.room, c.offset)].muted;
        uint8_t v = rd8(a), nv = (uint8_t)((v | (c.set & ~keep)) & ~(c.clear & ~keep));
        if (nv == v) continue;
        wr8(a, nv);
        // the shadow follows, so this isn't passed back
        if (c.area == 0) g_danShadow[c.offset] = nv;
        else
            for (ZoneShadow& z : g_zoneShadow)
                if (z.room == c.room) z.bits[c.offset] = nv;
        log_info("mp: {} switch byte {}:{} {:02X} -> {:02X} from the room", c.area ? "room" : "dungeon", c.room, c.offset, v, nv);
        mark_stale(false, true, t);
    }
    g_localIn.clear();
    if (!changed && t - g_localSent < 0.5) return;
    g_localSent = t;
    std::vector<uint8_t> out{kEventLocal};
    out.insert(out.end(), stage, stage + 8);
    for (auto it = g_localRecent.begin(); it != g_localRecent.end();)
    {
        auto [mask, when] = it->second;
        int area = it->first >> 16, room = (int8_t)(it->first >> 8), offset = it->first & 0xFF;
        uint32_t a = local_addr(area, room, offset);
        if (t - when > kChangeResend || !a)
        {
            it = g_localRecent.erase(it);
            continue;
        }
        uint8_t v = rd8(a);
        out.insert(out.end(), {(uint8_t)area, (uint8_t)room, (uint8_t)offset, (uint8_t)(v & mask), (uint8_t)(~v & mask)});
        ++it;
    }
    if (out.size() > 9) g_storyOut.push_back(std::move(out));
}

bool share_enemies()
{
    std::lock_guard lk(settings::lock());
    return settings::get().mpShareEnemies;
}

// boss rooms: their bosses are never removed (their own defeat runs the warp and the heart container), only their
// damage is shared where they keep it in the usual place
bool boss_stage(const char* stage)
{
    static const char* const kBossStages[] = {"M_DragB", "kinBOSS", "SirenB", "M_DaiB", "kazeB", "GanonJ", "GanonK", "GanonL",
                                              "GanonM", "GanonN", "M2tower", "M2ganon", "Xboss0", "Xboss1", "Xboss2", "Xboss3"};
    for (const char* b : kBossStages)
        if (std::strncmp(stage, b, 8) == 0) return true;
    // and anything named like one (the dungeons' boss rooms end in B, the refights start with Xboss)
    std::string name(stage, strnlen(stage, 8));
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find("boss") != std::string::npos || (name.size() > 2 && name.back() == 'B');
}

// bosses by profile (the actor name table: Btd Gohma, Bwd/Bwds Kalle Demos, Bst Gohdan, Bdk/Bdkobj Helmaroc King,
// big_pow Jalhalla, Bkm, Fganon Phantom Ganon, Bgn Puppet Ganon, Gnd Ganondorf, Daiocta/Daioeye Big Octo)
bool boss_proc(uint16_t p)
{
    switch (p)
    {
    case 0x00EA: case 0x00D9: case 0x00DA: case 0x00F0: case 0x00EE: case 0x00EF: case 0x00D3: case 0x00EB:
    case 0x00F1: case 0x00F3: case 0x00F6: case 0x00E1: case 0x00E2:
        return true;
    default:
        return false;
    }
}

uint32_t enemy_key(uint32_t actor)
{
    auto f32 = [](uint32_t a) { return std::bit_cast<float>(rd32(a)); };
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) {
        for (int i = 0; i < 4; i++) h = (h ^ (v >> (i * 8) & 0xFF)) * 16777619u;
    };
    mix(actors::proc_name(actor));
    mix(rd8(actor + actors::kHome + actors::kPlaceRoom));
    for (int i = 0; i < 3; i++) mix((uint32_t)(int32_t)std::lround(f32(actor + actors::kHome + i * 4)));
    return h;
}

// boss rooms aren't moved around: bosses follow scripts that their place is part of
void sync_enemy_positions(double t, const char* stage, const std::map<uint32_t, uint32_t>& enemies, const std::set<uint32_t>& twice,
                          bool bossRoom)
{
    auto f32 = [](uint32_t a) { return std::bit_cast<float>(rd32(a)); };
    struct Player { int id; float x, z; };
    std::vector<Player> players;
    uint32_t pl = rd32(kPlayerPtr);
    if (!actors::valid_ptr(pl)) return;
    players.push_back({g_id, f32(pl + kActorPos), f32(pl + kActorPos + 8)});
    for (const auto& [id, r] : g_remotes)
        if (!r.snaps.empty() && r.world == rando::playing_world() && t - r.snaps.back().time < kStale &&
            std::memcmp(r.snaps.back().view.stage, stage, 8) == 0)
            players.push_back({id, r.snaps.back().view.x, r.snaps.back().view.z});
    if (players.size() < 2)
    {
        g_enemyOwner.clear();
        return;
    }
    // a boss fight is run by whoever has been in the room longest (it stays theirs while they're there), so the boss
    // follows one game through the whole fight instead of changing hands as people move around it
    int bossOwner = g_id;
    float longest = (float)(t - g_stageSince);
    for (const Player& p : players)
    {
        auto st = g_stageTime.find(p.id);
        if (p.id == g_id || st == g_stageTime.end() || std::memcmp(st->second.stage.data(), stage, 8) != 0 || t - st->second.received > 2.0)
            continue;
        float theirs = st->second.seconds + (float)(t - st->second.received);
        if (theirs > longest + 0.5f || (std::fabs(theirs - longest) <= 0.5f && p.id < bossOwner)) longest = theirs, bossOwner = p.id;
    }
    std::vector<uint8_t> out{kEventEnemyPos};
    out.insert(out.end(), stage, stage + 8);
    float here = (float)(t - g_stageSince);
    out.insert(out.end(), (const uint8_t*)&here, (const uint8_t*)&here + 4);
    bool sendNow = t - g_enemyPosSent >= 0.1;
    for (const auto& [key, a] : enemies)
    {
        if (twice.count(key)) continue;
        bool boss = boss_proc(actors::proc_name(a));
        if (bossRoom && !boss) continue;  // the rest of a boss room (its parts, its minions) runs in each game
        float ex = f32(a + kActorPos), ez = f32(a + kActorPos + 8);
        auto dist = [&](const Player& p) { return std::hypot(p.x - ex, p.z - ez); };
        const Player* best = &players[0];
        for (const Player& p : players)
            if (dist(p) < dist(*best) || (dist(p) == dist(*best) && p.id < best->id)) best = &p;
        // the owner only changes when someone is clearly nearer, so two games don't both (or neither) run it
        int& owner = g_enemyOwner.try_emplace(key, best->id).first->second;
        const Player* cur = nullptr;
        for (const Player& p : players)
            if (p.id == owner) cur = &p;
        if (!cur || dist(*best) * 1.25f + 300.0f < dist(*cur)) owner = best->id;
        if (boss) owner = bossOwner;
        if (owner == g_id)
        {
            if (!sendNow || out.size() > 1180) continue;
            float pos[3] = {ex, f32(a + kActorPos + 4), ez};
            int16_t ang[2] = {(int16_t)rd16(a + kActorAngle + 2), (int16_t)rd16(a + kActorShape + 2)};
            out.insert(out.end(), (const uint8_t*)&key, (const uint8_t*)&key + 4);
            out.insert(out.end(), (const uint8_t*)pos, (const uint8_t*)pos + 12);
            out.insert(out.end(), (const uint8_t*)ang, (const uint8_t*)ang + 4);
            continue;
        }
        auto in = g_enemyPosIn.find(key);
        if (in == g_enemyPosIn.end() || std::memcmp(g_enemyPosStage.data(), stage, 8) != 0 || t - in->second.time > 0.5) continue;
        const EnemyPos& e = in->second;
        float dx = e.x - ex, dy = e.y - f32(a + kActorPos + 4), dz = e.z - ez;
        // ease toward where it is in the owner's game; a big gap (it was knocked away, or just started following) jumps
        float gap = std::sqrt(dx * dx + dy * dy + dz * dz), k = gap > 600.0f ? 1.0f : 0.35f;
        static const bool debug = std::getenv("WF_MP_DEBUG") != nullptr;
        static double s_logged = 0;
        if (debug && t - s_logged > 1.0)
        {
            s_logged = t;
            log_info("mp: enemy {:04X} follows player {}: {:.0f} apart", actors::proc_name(a), e.from, gap);
        }
        wr32(a + kActorPos, std::bit_cast<uint32_t>(ex + dx * k));
        wr32(a + kActorPos + 4, std::bit_cast<uint32_t>(f32(a + kActorPos + 4) + dy * k));
        wr32(a + kActorPos + 8, std::bit_cast<uint32_t>(ez + dz * k));
        auto turn = [&](uint32_t at, int16_t to) {
            int16_t from = (int16_t)rd16(at);
            wr16(at, (uint16_t)(int16_t)(from + (int16_t)((int16_t)(to - from) * k)));
        };
        turn(a + kActorAngle + 2, e.angle);
        turn(a + kActorShape + 2, e.shape);
    }
    // sent even without enemies of ours in it: the others need how long this game has been here
    if (sendNow)
    {
        g_enemyPosSent = t;
        g_storyOut.push_back(std::move(out));
    }
}

void sync_enemies(double t)
{
    char stage[8];
    for (int i = 0; i < 8; i++) stage[i] = (char)rd8(kStartStage + i);
    if (std::memcmp(stage, g_enemyStage, 8) != 0)
    {
        std::memcpy(g_enemyStage, stage, 8);
        g_stageSince = t;
        g_enemies.clear();
        g_enemyRecent.clear();
        g_enemyFloor.clear();
        g_enemyGone.clear();
    }
    for (const auto& [from, e] : g_enemyIn)
    {
        if (std::memcmp(from.data(), stage, 8) != 0) continue;
        auto seen = g_enemies.find(e.first);
        if (seen == g_enemies.end()) continue;  // not loaded here: it'll be a fresh one by the time this game gets there
        auto [it, fresh] = g_enemyFloor.try_emplace(e.first, EnemyFloor{e.second, seen->second.actor});
        if (!fresh && it->second.actor == seen->second.actor) it->second.health = std::min(it->second.health, e.second);
        else if (!fresh) it->second = {e.second, seen->second.actor};
    }
    g_enemyIn.clear();
    bool bossRoom = boss_stage(stage);
    // this game's enemies; a spawn point two enemies share can't be told apart, so it's left alone
    std::map<uint32_t, uint32_t> now;
    std::set<uint32_t> twice;
    actors::for_each([&](uint32_t a) {
        bool boss = boss_proc(actors::proc_name(a));
        if (boss) bossRoom = true;
        uint32_t flags = rd32(a + kAttnFlags);
        if ((!(flags & kAttnBattle) && !boss) || flags == 0xFFFFFFFF || actors::proc_name(a) == kProcPlayer || !rd8(a + kMaxHealth)) return true;
        uint32_t key = enemy_key(a);
        if (!now.emplace(key, a).second) twice.insert(key);
        return true;
    });
    bool changed = false;
    for (const auto& [key, a] : now)
    {
        if (twice.count(key)) continue;
        int8_t h = (int8_t)rd8(a + kHealth);
        auto seen = g_enemies.find(key);
        if (seen != g_enemies.end() && seen->second.actor == a && h < seen->second.health)
        {
            g_enemyRecent[key] = {h, t};
            changed = true;
        }
        // what the others dealt
        if (auto f = g_enemyFloor.find(key); f != g_enemyFloor.end() && f->second.actor != a) g_enemyFloor.erase(f);
        else if (f != g_enemyFloor.end() && std::max<int8_t>(f->second.health, 0) < h)
        {
            int8_t to = std::max<int8_t>(f->second.health, 0);
            wr8(a + kHealth, (uint8_t)to);
            // big enemies (and anything in a boss room) aren't removed: their own defeat may open the way on, so they
            // stay at no health and fall to the next hit here
            bool remove = f->second.health <= 0 && !bossRoom && rd8(a + kMaxHealth) < 10 && !g_enemyGone.count(a);
            if (remove)
            {
                g_enemyDelete.push_back(a);
                g_enemyGone.insert(a);
            }
            log_info("mp: enemy {:04X} health {} -> {} from the room{}", actors::proc_name(a), h, to, remove ? ", removed" : "");
            h = to;
        }
        g_enemies[key] = {a, h};
    }
    for (auto it = g_enemies.begin(); it != g_enemies.end();)
        it = now.count(it->first) ? std::next(it) : g_enemies.erase(it);
    sync_enemy_positions(t, stage, now, twice, bossRoom);
    if (!changed && t - g_enemySent < 0.5) return;
    g_enemySent = t;
    std::vector<uint8_t> out{kEventEnemy};
    out.insert(out.end(), stage, stage + 8);
    for (auto it = g_enemyRecent.begin(); it != g_enemyRecent.end();)
    {
        auto [h, when] = it->second;
        if (t - when > kChangeResend)
        {
            it = g_enemyRecent.erase(it);
            continue;
        }
        uint32_t k = it->first;
        out.insert(out.end(), {(uint8_t)k, (uint8_t)(k >> 8), (uint8_t)(k >> 16), (uint8_t)(k >> 24), (uint8_t)h});
        ++it;
    }
    if (out.size() > 9) g_storyOut.push_back(std::move(out));
}

// after this game beats a dungeon's boss, its warp out takes everyone else in that dungeon along
void sync_boss_warp(double t, int group, bool playing)
{
    if (g_bossGroup >= 0 && (t - g_bossTime > 600.0 || (group != g_bossGroup && group >= 0)))
    {
        g_bossGroup = -1;
        g_warpSent = false;
    }
    if (g_bossGroup >= 0 && !g_warpSent && rd8(kNextEnable))
    {
        std::vector<uint8_t> out{kEventWarp, (uint8_t)g_bossGroup};
        for (uint32_t i = 0; i < 12; i++) out.push_back(rd8(kNextStage + i));
        g_storyOut.push_back(std::move(out));
        g_warpSent = true;
        log_info("mp: warping out after the boss, taking the others in the dungeon along");
    }
    if (!g_warpIn) return;
    if (t - g_warpIn->time > 30.0)
    {
        g_warpIn.reset();
        return;
    }
    if (!playing || !in_game_stage() || rd8(kNextEnable)) return;
    if (group == g_warpIn->group)
    {
        for (uint32_t i = 0; i < 12; i++) wr8(kNextStage + i, g_warpIn->dest[i]);
        wr8(kNextEnable, 1);
        g_ownRequest = true;
        log_info("mp: following the warp out of the dungeon");
    }
    g_warpIn.reset();
}

bool share_cutscenes()
{
    std::lock_guard lk(settings::lock());
    return settings::get().mpShareCutscenes;
}

// a story cutscene started here with a stage change goes to the others in the same place; one started by someone else
// in the same place takes this game along
void sync_cutscene_follow(double t, bool playing)
{
    bool up = rd8(kNextEnable) != 0;
    bool ending = up && ending_stage(kNextStage);
    if (up && !g_nextWasSet && !g_ownRequest && playing && (rd8(kNextStage + 11) != 0xFF || ending))
    {
        uint32_t pl = rd32(kPlayerPtr);
        std::vector<uint8_t> out{kEventFollow};
        for (uint32_t i = 0; i < 8; i++) out.push_back(rd8(kStartStage + i));
        out.push_back(ending ? (uint8_t)kEveryone : actors::valid_ptr(pl) ? rd8(pl + kActorRoom) : 0xFF);
        float where[2] = {actors::valid_ptr(pl) ? std::bit_cast<float>(rd32(pl + kActorPos)) : 0.0f,
                          actors::valid_ptr(pl) ? std::bit_cast<float>(rd32(pl + kActorPos + 8)) : 0.0f};
        out.insert(out.end(), (const uint8_t*)where, (const uint8_t*)where + 8);
        if (ending) log_info("mp: the ending starts here, taking everyone along");
        for (uint32_t i = 0; i < 12; i++) out.push_back(rd8(kNextStage + i));
        g_storyOut.push_back(std::move(out));
        log_info("mp: a cutscene starts here (layer {}), taking the others here along", (int8_t)rd8(kNextStage + 11));
    }
    if (!up) g_ownRequest = false;
    g_nextWasSet = up;
    if (!g_followIn) return;
    if (t - g_followIn->time > 10.0)
    {
        g_followIn.reset();
        return;
    }
    if (!playing || !in_game_stage() || up) return;
    uint32_t pl = rd32(kPlayerPtr);
    bool here = true;
    for (int i = 0; i < 8 && here; i++) here = (char)rd8(kStartStage + i) == g_followIn->stage[i];
    bool close = here && actors::valid_ptr(pl) && (int8_t)rd8(pl + kActorRoom) == g_followIn->room &&
                std::hypot(std::bit_cast<float>(rd32(pl + kActorPos)) - g_followIn->x,
                           std::bit_cast<float>(rd32(pl + kActorPos + 8)) - g_followIn->z) < kFollowReach;
    if (g_followIn->room == kEveryone || close)
    {
        for (uint32_t i = 0; i < 12; i++) wr8(kNextStage + i, g_followIn->dest[i]);
        wr8(kNextEnable, 1);
        g_ownRequest = true;
        log_info("mp: going along into {}'s cutscene", g_followIn->who);
    }
    g_followIn.reset();
}

void sync_story(bool playing)
{
    if (!g_joined || !share_story())
    {
        g_storySnapIn.reset();
        g_changesIn.clear();
        g_localIn.clear();
        return;
    }
    bool host = story_host() == g_id;
    if (host && !g_storyAdopted)
    {
        g_storyAdopted = true;
        g_storyBaseline = false;
        log_info("mp: hosting the room's story");
    }
    // only while a stage has been playing steadily: around stage changes the game moves memory between slots
    int group = stage_group();
    if (g_storyAdopted && share_enemies()) sync_boss_warp(now_s(), group, playing);
    if (g_storyAdopted && share_cutscenes()) sync_cutscene_follow(now_s(), playing);
    bool steady = playing && in_game_stage() && items_valid() && !rd8(kNextEnable) && group == g_storyGroup;
    g_storyGroup = group;
    g_storyStable = steady ? g_storyStable + 1 : 0;
    // the save went away (title screen, file select): another file may come back, so compare afresh. Across a stage
    // change the baseline stays: what the game set while loading is compared once the new stage has settled
    if (!in_game_stage() || !items_valid())
    {
        if (g_storyBaseline)
        {
            if (host) g_storyEpoch++;
            else g_storyAdopted = false;
        }
        g_storyBaseline = false;
    }
    if (g_storyStable < 30) return;
    double t = now_s();
    std::array<uint8_t, kStoryBytes> now;
    for (int i = 0; i < kStoryBytes; i++) now[i] = rd8(story_addr(i, group));
    if (!g_storyBaseline)
    {
        g_storyShadow = now;
        g_storyBaseline = true;
    }
    // what this game changed
    bool changed = false;
    for (int i = 0; i < kStoryBytes; i++)
    {
        if (now[i] == g_storyShadow[i]) continue;
        uint8_t before = g_storyShadow[i], bits = now[i] ^ before;
        static const bool debug = std::getenv("WF_MP_DEBUG") != nullptr;
        if (debug) log_info("mp: story byte {:03X} {:02X} -> {:02X}", i, g_storyShadow[i], now[i]);
        g_storyShadow[i] = now[i];
        bits = unmuted(g_churn[i], bits, t, std::format("story byte {:03X}", i));
        bool keyCount = i >= kStoryMemory && i < kStoryOcean && (i - kStoryMemory) % kMemBytes == 0x20;
        if (!keyCount && bits) g_lastStoryBitTime = t;
        // a key gained is held back a moment: one with nothing else changing around it came from a chest or key the
        // room already took (still showing here), and is taken back
        if (keyCount && bits && g_storyAdopted && share_story() && now[i] > before)
        {
            g_heldKeys.push_back({i, before, now[i], t});
            continue;
        }
        // a dungeon's boss beaten here (its dungeon item bit 3)
        if (i >= kStoryMemory && i < kStoryOcean && (i - kStoryMemory) % kMemBytes == 0x21 && (bits & now[i] & 8))
        {
            g_bossGroup = (i - kStoryMemory) / kMemBytes;
            g_bossTime = t;
            g_warpSent = false;
            log_info("mp: beat the boss of stage group {}", g_bossGroup);
        }
        if (!bits || !g_storyAdopted) continue;
        g_recent[i] = {(uint8_t)(g_recent[i].first | bits), t};
        changed = true;
    }
    for (auto it = g_heldKeys.begin(); it != g_heldKeys.end();)
    {
        if (t - it->time < 3.0)
        {
            ++it;
            continue;
        }
        uint32_t a = story_addr(it->offset, group);
        uint8_t cur = rd8(a);
        if (cur == it->after && g_lastStoryBitTime < it->time - 6.0)
        {
            wr8(a, it->before);
            g_storyShadow[it->offset] = now[it->offset] = it->before;
            log_info("mp: took back a key the room already had (picked up a second time here)");
        }
        else if (cur != it->before)
        {
            g_recent[it->offset] = {(uint8_t)(g_recent[it->offset].first | (it->before ^ cur)), t};
            changed = true;
        }
        it = g_heldKeys.erase(it);
    }
    // the host's story: on joining, after this game loads a file, and after the host loads theirs again
    if (g_storySnapIn && !host && g_storySnapIn->from == g_storyFrom && g_storySnapIn->epoch != g_storyEpochSeen)
        g_storyAdopted = false;
    if (!g_storyAdopted && g_storySnapIn)
    {
        g_storyFrom = g_storySnapIn->from;
        g_storyEpochSeen = g_storySnapIn->epoch;
        const std::vector<uint8_t>& snap = g_storySnapIn->bytes;
        int written = 0;
        for (int i = 0; i < kStoryBytes; i++)
        {
            uint8_t keep = g_churn[i].muted, v = (uint8_t)((snap[i] & ~keep) | (now[i] & keep));
            if (v == now[i]) continue;
            wr8(story_addr(i, group), v);
            g_storyShadow[i] = v;
            written++;
        }
        g_storyAdopted = true;
        g_storySnapIn.reset();
        g_changesIn.clear();
        log_info("mp: took on the host's story ({} bytes differed)", written);
        // reload the area so what's in it follows the new story
        if (written)
        {
            for (uint32_t i = 0; i < 12; i++) wr8(kNextStage + i, rd8(kStartStage + i));
            wr8(kNextStage + 11, 0xFF);
            wr8(kNextEnable, 1);
            g_ownRequest = true;
            log_info("mp: reloading the area for the new story");
        }
        return;
    }
    // everyone else's changes
    if (g_storyAdopted)
        for (const StoryChange& c : g_changesIn)
        {
            if (c.offset >= kStoryBytes) continue;
            uint8_t keep = g_churn[c.offset].muted;
            uint32_t a = story_addr(c.offset, group);
            uint8_t v = rd8(a), nv = (uint8_t)((v | (c.set & ~keep)) & ~(c.clear & ~keep));
            if (nv != v)
            {
                wr8(a, nv);
                log_info("mp: story byte {:03X} {:02X} -> {:02X} from the room", c.offset, v, nv);
                // the area being played: its chests (treasure bits) or doors (switches, dungeon items) may show it wrong
                if (c.offset >= kStoryMemory && c.offset < kStoryOcean && (c.offset - kStoryMemory) / kMemBytes == group)
                {
                    int b = (c.offset - kStoryMemory) % kMemBytes;
                    if (b < 4) mark_stale(true, false, t);
                    else if (b < 0x14 || b == 0x20 || b == 0x21) mark_stale(false, true, t);
                }
            }
            g_storyShadow[c.offset] = nv;
        }
    g_changesIn.clear();
    if (g_storyAdopted) sync_local(t);
    refresh_stale(t);
    if (g_storyAdopted && share_enemies()) sync_enemies(t);
    // send changes now, and resend the latest state of recently changed bits for a while (packets can be lost; a
    // repeat is harmless since it carries the bits' current values)
    if (changed || (t - g_changesSent > 0.5))
    {
        std::vector<uint8_t> out{kEventChange};
        for (int i = 0; i < kStoryBytes && out.size() < 1000; i++)
        {
            auto& [bits, when] = g_recent[i];
            if (!bits) continue;
            if (t - when > kChangeResend)
            {
                bits = 0;
                continue;
            }
            out.insert(out.end(), {(uint8_t)i, (uint8_t)(i >> 8), (uint8_t)(now[i] & bits), (uint8_t)(~now[i] & bits)});
        }
        if (out.size() > 1) g_storyOut.push_back(std::move(out));
        g_changesSent = t;
    }
    if (host && t - g_storySent > 3.0)
    {
        std::vector<uint8_t> out{kEventStory, g_storyEpoch};
        out.insert(out.end(), now.begin(), now.end());
        g_storyOut.push_back(std::move(out));
        g_storySent = t;
    }
}

void sync_items(bool playing)
{
    if (!g_joined || !playing || !in_game_stage() || !items_valid() || !share_items())
    {
        g_invIn.clear();
        if (!in_game_stage() || !items_valid())
        {
            g_lifeKnown = false, g_heldLife.clear();
            // another file may come: what this one's story took doesn't carry over
            g_invLocalKnown = false;
            std::fill(std::begin(g_takenSlot), std::end(g_takenSlot), false);
            std::fill(std::begin(g_takenCollect), std::end(g_takenCollect), 0);
            g_takenPearls = 0;
        }
        return;
    }
    double t = now_s();
    // a heart container or piece gained here with nothing else in the story changing around it was one the room
    // already took (still showing here): it's taken back, and not passed on meanwhile
    bool checkTaken = share_story() && g_storyAdopted;
    uint16_t life = rd16(kMaxLife);
    if (!g_lifeKnown) g_lifeSeen = life, g_lifeKnown = true;
    if (life > g_lifeSeen && checkTaken) g_heldLife.push_back({(uint16_t)(life - g_lifeSeen), t});
    {
        Inventory cur = read_inventory();
        if (g_invLocalKnown)
        {
            for (int i = 0; i < kItemSlotCount; i++)
            {
                if (is_bottle(i)) continue;
                if (g_invLocal.items[i] != 0xFF && cur.items[i] == 0xFF && !g_takenSlot[i])
                {
                    g_takenSlot[i] = true;
                    log_info("mp: the story took item {:02X} (slot {}) here; the room won't hand it back", g_invLocal.items[i], i);
                }
                if (cur.items[i] != 0xFF) g_takenSlot[i] = false;
            }
            for (int c = 0; c < kCollectCount; c++)
            {
                uint8_t lost = g_invLocal.collect[c] & ~cur.collect[c];
                if (lost) log_info("mp: the story took equipment {} ({:02X}) here; the room won't hand it back", c, lost);
                // the story giving any of it back (the sword found again) ends the hold, upgrades and all
                bool gained = cur.collect[c] & ~g_invLocal.collect[c];
                g_takenCollect[c] = gained ? 0 : (uint8_t)((g_takenCollect[c] | lost) & ~cur.collect[c]);
            }
            g_takenPearls = (uint8_t)((g_takenPearls | (g_invLocal.pearls & ~cur.pearls)) & ~cur.pearls);
        }
    }
    for (const Inventory& in : g_invIn) merge_inventory(in);
    g_invIn.clear();
    g_invLocal = read_inventory();
    g_invLocalKnown = true;
    uint16_t held = 0;
    for (auto it = g_heldLife.begin(); it != g_heldLife.end();)
    {
        if (t - it->time < 3.0)
        {
            held += it->amount;
            ++it;
            continue;
        }
        if (g_lastStoryBitTime < it->time - 6.0)
        {
            uint16_t ml = rd16(kMaxLife), to = ml > it->amount ? (uint16_t)(ml - it->amount) : ml;
            wr16(kMaxLife, to);
            if (rd16(kLife) > to) wr16(kLife, to);
            log_info("mp: took back {} quarter hearts the room already had (picked up a second time here)", it->amount);
        }
        it = g_heldLife.erase(it);
    }
    g_lifeSeen = rd16(kMaxLife);
    // resent every few seconds too, so someone who joins later catches up
    Inventory now = read_inventory();
    if (held)
    {
        uint16_t shown = (uint16_t)(now.maxLife[0] << 8 | now.maxLife[1]);
        shown = shown > held ? (uint16_t)(shown - held) : shown;
        now.maxLife[0] = (uint8_t)(shown >> 8);
        now.maxLife[1] = (uint8_t)shown;
    }
    if (!g_invAnnounced || memcmp(&now, &g_invSent, sizeof now) != 0 || t - g_invSentTime > 3.0)
    {
        g_invOut = now;
        g_invSent = now;
        g_invSentTime = t;
        g_invAnnounced = true;
    }
}

void run_game_calls(wf::PPCContext& ctx)
{
    std::vector<uint32_t> remove, rebuild;
    {
        std::lock_guard lk(g_lock);
        remove.swap(g_enemyDelete);
        rebuild.swap(g_actorRebuild);
    }
    // WF_REBUILD=<frame>:<proc>[,...]: makes every actor of that profile again at that frame (testing)
    static const char* spec = std::getenv("WF_REBUILD");
    if (spec)
    {
        static uint32_t s_last = 0;
        uint32_t frame = g_frameNumber;
        for (const char* p = spec; p && *p;)
        {
            unsigned f = 0, proc = 0;
            if (std::sscanf(p, "%u:%x", &f, &proc) == 2 && f > s_last && f <= frame)
                actors::for_each([&](uint32_t a) {
                    if (actors::proc_name(a) == proc) rebuild.push_back(a);
                    return true;
                });
            p = std::strchr(p, ',');
            if (p) p++;
        }
        s_last = frame;
    }
    // WF_SET_HEALTH=<frame>:<proc>:<health>: sets that health on every actor of that profile at that frame (testing)
    static const char* setHealth = std::getenv("WF_SET_HEALTH");
    if (setHealth)
    {
        static uint32_t s_last = 0;
        unsigned f = 0, proc = 0, h = 0;
        if (std::sscanf(setHealth, "%u:%x:%u", &f, &proc, &h) == 3 && f > s_last && f <= g_frameNumber)
            actors::for_each([&](uint32_t a) {
                if (actors::proc_name(a) == proc) wr8(a + kHealth, (uint8_t)h);
                return true;
            });
        s_last = g_frameNumber;
    }
    if (remove.empty() && rebuild.empty()) return;
    // the caller's registers go on to the game's own frame function: keep them
    uint32_t r[32], ctr = ctx.ctr, fpscr = ctx.fpscr;
    uint8_t cr[32], xer[4] = {ctx.xer_so, ctx.xer_ov, ctx.xer_ca, ctx.xer_bc};
    decltype(ctx.f) f;
    std::memcpy(r, ctx.r, sizeof r);
    std::memcpy(cr, ctx.cr, sizeof cr);
    std::memcpy(f, ctx.f, sizeof f);
    for (uint32_t a : remove)
    {
        // still alive and on the actor list (it may have died or unloaded since)
        bool listed = false;
        actors::for_each([&](uint32_t x) { return !(listed = x == a); });
        if (listed && (int8_t)rd8(a + kHealth) <= 0) hle::call_guest(ctx, kFpcDtDelete, {a});
    }
    // made again from where the stage placed them, so they read the save as it is now (a chest a partner opened opens,
    // a door a partner unlocked unlocks): the old one is removed and a new one asked for in its place
    for (uint32_t a : rebuild)
    {
        bool listed = false;
        actors::for_each([&](uint32_t x) { return !(listed = x == a); });
        if (!listed) continue;
        uint16_t proc = actors::proc_name(a);
        uint32_t param = rd32(a + kParam);
        int8_t room = (int8_t)rd8(a + kOrigRoom), subtype = (int8_t)rd8(a + kSubtype);
        uint32_t layer = rd32(a + kProcLayer);
        uint16_t setId = rd16(a + kSetId);
        uint8_t place[32];  // pos (12), angle (6, padded to 8), scale (12)
        for (int i = 0; i < 12; i++) place[i] = rd8(a + actors::kHome + i);
        for (int i = 0; i < 6; i++) place[12 + i] = rd8(a + kOrigAngle + i);
        place[18] = place[19] = 0;
        for (int i = 0; i < 12; i++) place[20 + i] = rd8(a + kScale + i);
        hle::call_guest(ctx, kFpcDtDelete, {a});
        // the placement goes in a frame of our own below the stack pointer, with a back chain and the ninth argument
        // (unused without a create function) zeroed
        uint32_t sp = ctx.r[1], frame = sp - 0x60;
        wr32(frame, sp);
        wr32(frame + 8, 0);
        for (int i = 0; i < 32; i++) wr8(frame + 0x20 + i, place[i]);
        ctx.r[1] = frame;
        // made in the layer the old one lived in (the stage's), not whatever layer is current between frames
        uint32_t oldLayer = rd32(kCurrentLayer);
        if (actors::valid_ptr(layer)) wr32(kCurrentLayer, layer);
        uint32_t heap = hle::call_guest(ctx, kGameHeap), prevHeap = 0, slot = rd32(kSavedHeap);
        if (heap)
        {
            prevHeap = hle::call_guest(ctx, kSetCurrentHeap, {rd32(kHeapMgr), heap});
            wr32(kSavedHeap, prevHeap);
        }
        uint32_t id = hle::call_guest(ctx, kFopAcmCreate, {proc, param, frame + 0x20, (uint32_t)(int32_t)room, frame + 0x2C,
                                                            frame + 0x34, (uint32_t)(int32_t)subtype, 0});
        if (heap)
        {
            // the save slot is used up when the making restored the heap itself; otherwise go back by hand
            if (rd32(kSavedHeap) == prevHeap) hle::call_guest(ctx, kSetCurrentHeap, {rd32(kHeapMgr), prevHeap});
            wr32(kSavedHeap, slot);
        }
        wr32(kCurrentLayer, oldLayer);
        ctx.r[1] = sp;
        // fopAcM_create hands back the new actor; it keeps the stage's set number like the one it replaces
        if (actors::valid_ptr(id)) wr16(id + kSetId, setId);
        log_info("mp: made actor {:04X} again ({:08X}) as {:08X}", proc, param, id);
    }
    std::memcpy(ctx.r, r, sizeof r);
    std::memcpy(ctx.cr, cr, sizeof cr);
    std::memcpy(ctx.f, f, sizeof f);
    ctx.ctr = ctr;
    ctx.fpscr = fpscr;
    ctx.xer_so = xer[0], ctx.xer_ov = xer[1], ctx.xer_ca = xer[2], ctx.xer_bc = xer[3];
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
    sync_story(actor != 0);
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
