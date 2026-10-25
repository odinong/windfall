#include "net/remote_render.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/guest.h"
#include "core/log.h"
#include "enhancements/camera.h"
#include "net/multiplayer.h"

void LatteBufferCache_invalidate(uint32_t physAddress, uint32_t size);

namespace wf::hle {
uint32_t default_heap_alloc(uint32_t size, int32_t align);
}

namespace wf::mp {
namespace {

constexpr uint32_t kPlayerPtr = 0x1047428C, kActorPos = 0x314, kActorShape = 0x328;
constexpr uint32_t kSlots = 16;
constexpr uint32_t kRingSize = 4 << 20;
constexpr uint32_t kAlbedoUnit = 4;
constexpr float kPartNearPlayer = 250.0f;

std::mutex g_lock;
struct PaletteInfo { uint32_t size; uint16_t part; };
std::unordered_map<uint32_t, PaletteInfo> g_palettes;
struct PaletteSeen { uint32_t size; uint16_t part; uint32_t frame; float x, y, z; };
std::unordered_map<uint64_t, PaletteSeen> g_paletteContents;

uint64_t palette_hash(uint32_t addr)
{
    const uint32_t* w = gptr<uint32_t>(addr);
    uint64_t h = 1469598103934665603ull;
    for (int k = 0; k < 24; k++) h = (h ^ w[k]) * 1099511628211ull;
    return h;
}
uint32_t g_ring = 0, g_ringPos = 0;
constexpr uint32_t kTexPoolSize = 24 << 20;
uint32_t g_texPool = 0, g_texPoolPos = 0;
std::unordered_map<uint64_t, uint32_t> g_recolored;

uint32_t* g_regs = nullptr;
uint32_t g_vsStart = 0;
using M34 = std::array<float, 12>;

M34 m34_load(const float* f)
{
    M34 m;
    std::copy(f, f + 12, m.begin());
    return m;
}

M34 m34_mul(const M34& a, const M34& b)
{
    M34 r{};
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j];
        r[i * 4 + 3] += a[i * 4 + 3];
    }
    return r;
}

bool m34_inverse(const M34& m, M34& r)
{
    float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::abs(det) < 1e-12f) return false;
    float inv[9] = {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
                    (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
                    (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
    for (int row = 0; row < 3; row++)
    {
        for (int col = 0; col < 3; col++) r[row * 4 + col] = inv[row * 3 + col];
        r[row * 4 + 3] = -(inv[row * 3 + 0] * m[3] + inv[row * 3 + 1] * m[7] + inv[row * 3 + 2] * m[11]);
    }
    return true;
}

struct Slot { uint32_t index, addr, size; uint16_t part; bool rigid = false; uint32_t key = 0; };
std::vector<Slot> g_slots;
uint16_t g_bodyPart = 0;
uint32_t g_bodyDest = 0, g_bodySize = 0;
uint32_t g_drawCounter = 0, g_lastLinkDraw = 0, g_drawCount = 0;
std::unordered_map<uint32_t, uint32_t> g_occurrence;
struct DrawRecord
{
    std::vector<uint32_t> regs;
    std::vector<uint8_t> block;
    struct Saved { uint32_t reg; std::vector<uint8_t> bytes; };
    std::vector<Saved> others;
    uint32_t slot = 0, rt = 0, frame = 0;
    uint32_t params[6]{};
};
std::unordered_map<uint32_t, DrawRecord> g_records;
uint32_t g_recordKey = 0, g_recordSlot = 0, g_recordSize = 0;
bool g_groupActive = false;
uint32_t g_groupRt = 0, g_drawsSinceLink = 0;
std::unordered_set<uint32_t> g_frameDrawn, g_frameReplayed;
uint32_t g_drawnFrame = ~0u;
float g_drawPos[3] = {};
uint32_t g_drawPosFrame = ~0u;
std::vector<Placement> g_framePlacements;
uint32_t g_placementsFrame = ~0u;

const std::vector<Placement>& frame_placements(uint32_t gpuFrame)
{
    if (g_placementsFrame != gpuFrame)
    {
        remote_placements(g_framePlacements);
        static const bool log = std::getenv("WF_RR_PLACELOG") != nullptr;
        if (log)
            log_info("rr placements gpu {} (was {}): {}", gpuFrame, g_placementsFrame,
                     g_framePlacements.empty() ? std::string("none") : std::format("{:.1f},{:.1f},{:.1f}", g_framePlacements[0].x, g_framePlacements[0].y, g_framePlacements[0].z));
        g_placementsFrame = gpuFrame;
    }
    return g_framePlacements;
}
std::vector<wf::mp::rr_debug::CopyInfo> g_dbgCur, g_dbgLast;
uint32_t g_dbgLastFrame = 0;
std::unordered_set<uint32_t> g_dbgHidden = [] {
    std::unordered_set<uint32_t> keys;
    if (const char* e = std::getenv("WF_RR_HIDEKEYS"))
        for (const char* q = e; *q;)
        {
            char* end;
            keys.insert((uint32_t)std::strtoul(q, &end, 16));
            q = *end ? end + 1 : end;
        }
    return keys;
}();
int g_drawTag = 0;
struct Extra { uint32_t key, recordKey; int copy; };
std::vector<Extra> g_extras;
std::vector<Placement> g_extraPlayers;
uint32_t g_vsStartForRecord = 0;
uint32_t g_groupView = 0;
constexpr uint32_t kViewSlot = 2;
constexpr int kPsUniformFromVs = 0x250 - 0x7E0;
uint32_t g_occurrenceFrame = ~0u;
uint32_t g_vsAttrib = 0, g_meshKey = 0;
constexpr uint32_t kCbColor0Base = 0xA010;
constexpr uint32_t kCbColor1Base = 0xA011, kDbDepthBase = 0xA003;

uint32_t pass_id(const uint32_t* regs)
{
    return (regs[kCbColor0Base] << 8) ^ (regs[kCbColor1Base] * 0x9E3779B1u) ^ (regs[kDbDepthBase] * 0x85EBCA6Bu);
}
constexpr uint32_t kRigidWindow = 40;
constexpr float kRigidNearPlayer = 150.0f;
constexpr float kMaxBoneOffset = 400.0f;
struct TexSlot { uint32_t unit, base, mip; };
std::vector<TexSlot> g_texSlots;
uint32_t g_psTexStart = 0;
std::vector<Placement> g_copies;
float g_rootX = 0, g_rootY = 0, g_rootZ = 0, g_localYaw = 0;
float g_playerX = 0, g_playerY = 0, g_playerZ = 0;
constexpr uint32_t kCbBlend0 = 0xA1E0, kBlendDstAlpha = 6;
constexpr uint32_t kBlendSrcAlpha = 0x05040504;
uint32_t g_blendSave[2];
bool g_blendSaved = false;
std::unordered_map<uint64_t, uint32_t> g_patched;
uint32_t g_patchedFrame = ~0u;
std::unordered_set<uint64_t> g_hiddenCopies;

std::vector<std::pair<uint32_t, uint32_t>> g_ringWritten;

uint32_t ring_alloc(uint32_t size)
{
    size = (size + 0xFF) & ~0xFFu;
    if (!g_ring || size > kRingSize) return 0;
    if (g_ringPos + size > kRingSize)
    {
        g_ringPos = 0;
        static const bool log = std::getenv("WF_RR_RINGLOG") != nullptr;
        if (log) log_info("rr: ring wrapped at gpu frame {}", g_patchedFrame);
    }
    uint32_t a = g_ring + g_ringPos;
    g_ringPos += size;
    g_ringWritten.emplace_back(a, size);
    return a;
}

void ring_flush()
{
    for (const auto& [a, size] : g_ringWritten) LatteBufferCache_invalidate(a, size);
    g_ringWritten.clear();
}

uint32_t mesh_key(uint32_t vb, uint32_t vbSize, uint32_t count)
{
    uint32_t h = 2166136261u;
    if (vb >= 0x10000000 && vb + 64 <= 0x50000000 && vbSize >= 64)
    {
        const uint32_t* w = gptr<uint32_t>(vb);
        for (int i = 0; i < 16; i++) h = (h ^ w[i]) * 16777619u;
    }
    return ((vbSize << 12) ^ count) ^ (h & 0xFFF00000u) ^ (h << 4 & 0x000FF000u);
}

bool player_position(float& x, float& y, float& z, float* yaw = nullptr)
{
    uint32_t actor = rd32(kPlayerPtr);
    if (actor < 0x10000000 || actor >= 0x50000000) return false;
    x = std::bit_cast<float>(rd32(actor + kActorPos));
    y = std::bit_cast<float>(rd32(actor + kActorPos + 4));
    z = std::bit_cast<float>(rd32(actor + kActorPos + 8));
    if (yaw) *yaw = (int16_t)rd16(actor + kActorShape + 2) * (3.14159265f / 32768.0f);
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

constexpr float kTunicHue[8] = {-1, 0, 222, 282, 30, 185, 320, 52};

void rgb_to_hsv(float r, float g, float b, float& h, float& sat, float& v)
{
    float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    v = mx;
    sat = mx > 0 ? d / mx : 0;
    if (d <= 0) { h = 0; return; }
    if (mx == r) h = 60 * std::fmod((g - b) / d, 6.0f);
    else if (mx == g) h = 60 * ((b - r) / d + 2);
    else h = 60 * ((r - g) / d + 4);
    if (h < 0) h += 360;
}

void hsv_to_rgb(float h, float sat, float v, float& r, float& g, float& b)
{
    float c = v * sat, x = c * (1 - std::abs(std::fmod(h / 60, 2.0f) - 1)), m = v - c;
    int k = (int)(h / 60) % 6;
    float rr[6] = {c, x, 0, 0, x, c}, gg[6] = {x, c, c, x, 0, 0}, bb[6] = {0, 0, x, c, c, x};
    r = rr[k] + m;
    g = gg[k] + m;
    b = bb[k] + m;
}

bool g_recolorShip = false;

uint16_t recolor565(uint16_t c, float target)
{
    float r = ((c >> 11) & 31) / 31.0f, g = ((c >> 5) & 63) / 63.0f, b = (c & 31) / 31.0f, h, sat, v;
    rgb_to_hsv(r, g, b, h, sat, v);
    if (g_recolorShip)
    {
        float d = h > 180 ? h - 360 : h;
        if (d < -25 || d > 20 || sat < 0.35f || v < 0.08f) return c;
        h = std::fmod(target + d * 0.5f + 360, 360.0f);
    }
    else if (h < 65 || h > 165 || sat < 0.2f || v < 0.08f)
        return c;
    else
        h = std::fmod(target + (h - 105) * 0.5f + 360, 360.0f);
    hsv_to_rgb(h, std::min(1.0f, sat * 1.05f), v, r, g, b);
    return (uint16_t)((std::lround(r * 31) << 11) | (std::lround(g * 63) << 5) | std::lround(b * 31));
}

void recolor_block(uint8_t* blk, float target, bool bc1)
{
    uint16_t c0 = blk[0] | (blk[1] << 8), c1 = blk[2] | (blk[3] << 8);
    uint16_t n0 = recolor565(c0, target), n1 = recolor565(c1, target);
    if (n0 == c0 && n1 == c1) return;
    bool fourColor = !bc1 || c0 > c1;
    if (fourColor && n0 == n1) n0 ^= 1;
    bool nowFour = n0 > n1;
    if (bc1 && nowFour != fourColor)
    {
        std::swap(n0, n1);
        uint32_t idx = blk[4] | (blk[5] << 8) | (blk[6] << 16) | ((uint32_t)blk[7] << 24), out = 0;
        for (int i = 0; i < 16; i++)
        {
            uint32_t v = (idx >> (2 * i)) & 3;
            v = v < 2 ? v ^ 1 : (fourColor ? v ^ 1 : v);
            out |= v << (2 * i);
        }
        for (int i = 0; i < 4; i++) blk[4 + i] = (uint8_t)(out >> (8 * i));
    }
    else if (!bc1 && !nowFour)
    {
        std::swap(n0, n1);
        uint32_t idx = blk[4] | (blk[5] << 8) | (blk[6] << 16) | ((uint32_t)blk[7] << 24), out = 0;
        for (int i = 0; i < 16; i++) out |= (((idx >> (2 * i)) & 3) ^ 1) << (2 * i);
        for (int i = 0; i < 4; i++) blk[4 + i] = (uint8_t)(out >> (8 * i));
    }
    blk[0] = (uint8_t)n0;
    blk[1] = (uint8_t)(n0 >> 8);
    blk[2] = (uint8_t)n1;
    blk[3] = (uint8_t)(n1 >> 8);
}

uint32_t g_changedBlocks = 0, g_greenSamples = 0;
uint32_t pool_copy(uint32_t src, uint32_t size, float target, bool bc1)
{
    uint32_t start = (g_texPoolPos + 0xFFFF) & ~0xFFFFu;
    uint32_t addr = g_texPool + start + (src & 0xFFFF);
    if (!g_texPool || start + (src & 0xFFFF) + size > kTexPoolSize) return 0;
    g_texPoolPos = start + (src & 0xFFFF) + size;
    uint8_t* dst = gptr<uint8_t>(addr);
    std::memcpy(dst, gptr<uint8_t>(src), size);
    uint32_t blockSize = bc1 ? 8 : 16, colorAt = bc1 ? 0 : 8;
    g_changedBlocks = 0;
    for (uint32_t o = 0; o + blockSize <= size; o += blockSize)
    {
        uint64_t before;
        std::memcpy(&before, dst + o + colorAt, 8);
        recolor_block(dst + o + colorAt, target, bc1);
        if (std::memcmp(&before, dst + o + colorAt, 8)) g_changedBlocks++;
    }
    return addr;
}

constexpr float kShipHueFor[8] = {130, -1, 222, 282, 48, 185, 320, 95};

bool recolored(const uint32_t* w, int color, uint32_t& base, uint32_t& mip, bool ship = false)
{
    float target = ship ? kShipHueFor[color & 7] : kTunicHue[color & 7];
    uint32_t format = w[1] >> 26, src = w[2] << 8, srcMip = w[3] << 8;
    bool bc1 = format == 0x31;
    uint64_t key = ((uint64_t)src << 8) | (uint32_t)(color & 7) | (ship ? 0x40 : 0);
    struct ModeGuard { ModeGuard(bool v) { g_recolorShip = v; } ~ModeGuard() { g_recolorShip = false; } } guard(ship);
    auto it = g_recolored.find(key);
    if (it != g_recolored.end())
    {
        if (!it->second) return false;
        base = it->second;
        mip = g_recolored[key | (1ull << 7)];
        return true;
    }
    uint32_t width = (w[0] >> 19) + 1, pitch = (((w[0] >> 8) & 0x7FF) + 1) * 8, height = (w[1] & 0x1FFF) + 1;
    uint32_t level0 = srcMip > src ? srcMip - src : (pitch / 4) * ((height + 3) / 4) * (bc1 ? 8 : 16);
    level0 = std::min(level0, 4u << 20);
    uint32_t poolPos = g_texPoolPos;
    base = pool_copy(src, level0, target, bc1);
    uint32_t changed = g_changedBlocks;
    if (base && changed == 0)
    {
        g_texPoolPos = poolPos;
        g_recolored[key] = 0;
        return false;
    }
    mip = srcMip == src ? base : (base ? pool_copy(srcMip, level0, target, bc1) : 0);
    g_recolored[key] = base && mip ? base : 0;
    g_recolored[key | (1ull << 7)] = mip;
    log_info("mp: {} color {} for texture {:08X} ({}x{} {}, {} blocks of {} recolored): {} at {:08X} mip {:08X}", ship ? "ship" : "tunic", color, src, width, height,
             bc1 ? "BC1" : "BC3", changed, level0 / (bc1 ? 8 : 16), base && mip ? "ok" : "failed (pool full)", base, mip);
    return base && mip;
}

constexpr uint32_t kActorScan = 0x6000, kModelSkin = 0x2C;
std::unordered_map<uint32_t, int32_t> g_linkParts;
uint32_t g_partsActor = 0, g_partsFrame = 0;

int32_t link_part(uint32_t obj)
{
    uint32_t actor = rd32(kPlayerPtr);
    if (actor < 0x10000000 || actor >= 0x50000000) return -1;
    if (actor != g_partsActor || frame_number() - g_partsFrame > 600)
    {
        g_linkParts.clear();
        g_partsActor = actor;
        g_partsFrame = frame_number();
    }
    auto it = g_linkParts.find(obj);
    if (it != g_linkParts.end()) return it->second;
    int32_t part = -1;
    for (uint32_t o = 0; o < kActorScan && part < 0; o += 4)
    {
        uint32_t model = rd32(actor + o);
        if (model >= 0x10000000 && model < 0x50000000 && rd32(model + kModelSkin) == obj) part = (int32_t)o;
    }
    g_linkParts[obj] = part;
    static const bool debug = std::getenv("WF_RR_DEBUG") != nullptr;
    if (debug) log_info("rr: skin object {:08X}: {}", obj, part >= 0 ? std::format("Link's part +{:X}", part) : std::string("not Link's"));
    return part;
}

constexpr uint32_t kShipPtr = 0x10474BEC;
constexpr uint32_t kActorPosBE = 0x314, kActorOldPosBE = 0x300, kActorShapeBE = 0x328;
std::unordered_map<uint64_t, uint32_t> g_shipPalettes;
std::unordered_map<uint32_t, bool> g_shipSkins;
uint32_t g_shipSkinsActor = 0;
bool g_shipDraw = false;
std::vector<int> g_shipCopy;
using D34 = std::array<double, 12>;
D34 g_shipLinv{};
D34 g_shipK{};
bool g_shipKKnown = false;
D34 g_shipC{};
bool g_shipCKnown = false;
D34 g_shipB0{};
uint32_t g_shipB0Frame = ~0u;

float be_float(uint32_t a) { return std::bit_cast<float>(rd32(a)); }

D34 d34_mul(const D34& a, const D34& b)
{
    D34 r{};
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j];
        r[i * 4 + 3] += a[i * 4 + 3];
    }
    return r;
}

bool d34_inv(const D34& m, D34& r)
{
    double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::abs(det) < 1e-12) return false;
    double inv[9] = {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det, (f * g - d * i) / det, (a * i - c * g) / det,
                     (c * d - a * f) / det, (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
    for (int row = 0; row < 3; row++)
    {
        for (int col = 0; col < 3; col++) r[row * 4 + col] = inv[row * 3 + col];
        r[row * 4 + 3] = -(inv[row * 3] * m[3] + inv[row * 3 + 1] * m[7] + inv[row * 3 + 2] * m[11]);
    }
    return true;
}

D34 d34_from(const float* f)
{
    D34 m;
    for (int i = 0; i < 12; i++) m[i] = f[i];
    return m;
}

D34 actor_transform(double x, double y, double z, double ax, double ay, double az)
{
    double cx = std::cos(ax), sx = std::sin(ax), cy = std::cos(ay), sy = std::sin(ay), cz = std::cos(az), sz = std::sin(az);
    D34 ry{cy, 0, sy, 0, 0, 1, 0, 0, -sy, 0, cy, 0}, rx{1, 0, 0, 0, 0, cx, -sx, 0, 0, sx, cx, 0}, rz{cz, -sz, 0, 0, sz, cz, 0, 0, 0, 0, 1, 0};
    D34 r = d34_mul(d34_mul(ry, rx), rz);
    r[3] = x;
    r[7] = y;
    r[11] = z;
    return r;
}

D34 local_ship_actor(uint32_t ship)
{
    constexpr double k = 3.14159265358979 / 32768.0;
    return actor_transform(be_float(ship + kActorPosBE), be_float(ship + kActorPosBE + 4), be_float(ship + kActorPosBE + 8),
                           (int16_t)rd16(ship + kActorShapeBE) * k, (int16_t)rd16(ship + kActorShapeBE + 2) * k, (int16_t)rd16(ship + kActorShapeBE + 4) * k);
}

bool is_ship_skin(uint32_t obj, uint32_t dest)
{
    uint32_t ship = rd32(kShipPtr);
    if (ship < 0x10000000 || ship >= 0x50000000) return false;
    if (ship != g_shipSkinsActor)
    {
        g_shipSkins.clear();
        g_shipSkinsActor = ship;
    }
    auto it = g_shipSkins.find(obj);
    if (it != g_shipSkins.end()) return it->second;
    const float* f = gptr<float>(dest);
    float dx = f[3] - be_float(ship + kActorPosBE), dy = f[7] - be_float(ship + kActorPosBE + 4), dz = f[11] - be_float(ship + kActorPosBE + 8);
    if (!(dx * dx + dy * dy + dz * dz < 300.0f * 300.0f)) return false;
    log_info("mp: ship skin object {:08X} (first bone {:.0f},{:.0f},{:.0f} from the ship)", obj, dx, dy, dz);
    return g_shipSkins[obj] = true;
}

int ship_prepare(uint32_t* regs, uint32_t vsUniformStart, uint32_t gpuFrame, uint32_t psTexStart, uint32_t psTexMask)
{
    g_shipDraw = false;
    uint32_t ship = rd32(kShipPtr);
    if (ship < 0x10000000 || ship >= 0x50000000) return 0;
    auto blockSize = [&](uint32_t b) { return regs[vsUniformStart + b * 7] ? regs[vsUniformStart + b * 7 + 1] + 1 : 0; };
    std::vector<Slot> slots;
    bool hull = false;
    float ox = be_float(ship + kActorOldPosBE), oy = be_float(ship + kActorOldPosBE + 4), oz = be_float(ship + kActorOldPosBE + 8);
    float cx = be_float(ship + kActorPosBE), cy = be_float(ship + kActorPosBE + 4), cz = be_float(ship + kActorPosBE + 8);
    auto atShip = [&](const float* f) {
        float sx = cx - ox, sy = cy - oy, sz = cz - oz, len2 = sx * sx + sy * sy + sz * sz;
        float t = len2 > 0 ? std::clamp(((f[3] - ox) * sx + (f[7] - oy) * sy + (f[11] - oz) * sz) / len2, 0.0f, 1.0f) : 0.0f;
        float dx = f[3] - (ox + sx * t), dy = f[7] - (oy + sy * t), dz = f[11] - (oz + sz * t);
        return dx * dx + dy * dy + dz * dz < 9.0f;
    };
    for (uint32_t i = 0; i < kSlots; i++)
    {
        uint32_t addr = regs[vsUniformStart + i * 7], size = blockSize(i);
        if (size < 48 || size > 0x2000 || addr < 0x10000000 || addr >= 0x50000000) continue;
        if (!g_shipPalettes.count(palette_hash(addr))) continue;
        bool isHull = atShip(gptr<float>(addr));
        slots.push_back({i, addr, size, 0, isHull});
        hull = hull || isHull;
    }
    if (slots.empty()) return 0;
    D34 L;
    const float* first = gptr<float>(slots[0].addr);
    if (hull)
    {
        for (const Slot& s : slots)
            if (s.rigid) first = gptr<float>(s.addr);
        L = d34_from(first);
        if (g_shipB0Frame == gpuFrame)
        {
            D34 b0inv;
            if (d34_inv(g_shipB0, b0inv))
            {
                g_shipC = d34_mul(b0inv, L);
                g_shipCKnown = true;
            }
        }
        if (!g_shipKKnown && std::abs(ox - cx) < 0.01f && std::abs(oz - cz) < 0.01f)
        {
            D34 ainv;
            if (d34_inv(local_ship_actor(ship), ainv))
            {
                g_shipK = d34_mul(ainv, L);
                g_shipKKnown = true;
                log_info("mp: ship model matrix vs actor: rot ({:.3f} {:.3f} {:.3f} / {:.3f} {:.3f} {:.3f} / {:.3f} {:.3f} {:.3f}) t ({:.1f} {:.1f} {:.1f})",
                         g_shipK[0], g_shipK[1], g_shipK[2], g_shipK[4], g_shipK[5], g_shipK[6], g_shipK[8], g_shipK[9], g_shipK[10], g_shipK[3],
                         g_shipK[7], g_shipK[11]);
            }
        }
    }
    else
    {
        g_shipB0 = d34_from(first);
        g_shipB0Frame = gpuFrame;
        L = g_shipCKnown ? d34_mul(g_shipB0, g_shipC) : local_ship_actor(ship);
    }
    if (!d34_inv(L, g_shipLinv)) return 0;
    if (!g_shipKKnown) g_shipK = D34{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    g_copies = frame_placements(gpuFrame);
    g_shipCopy.clear();
    for (int c = 0; c < (int)g_copies.size(); c++)
    {
        const Placement& p = g_copies[c];
        float dx = p.shipX - cx, dz = p.shipZ - cz;
        if (p.ship && dx * dx + dz * dz > 60.0f * 60.0f) g_shipCopy.push_back(c);
    }
    if (g_shipCopy.empty()) return 0;
    g_slots = std::move(slots);
    g_regs = regs;
    g_vsStart = vsUniformStart;
    g_psTexStart = psTexStart;
    g_texSlots.clear();
    for (uint32_t u = 0; u < 18; u++)
    {
        if (!(psTexMask & (1u << u))) continue;
        const uint32_t* w = regs + psTexStart + u * 7;
        uint32_t format = w[1] >> 26, width = (w[0] >> 19) + 1;
        if (u == kAlbedoUnit && format == 0x33 && (w[0] & 7) == 1 && width <= 1024 && (w[2] << 8) >= 0x10000000 && (w[2] << 8) < 0x50000000)
            g_texSlots.push_back({u, w[2], w[3]});
    }
    g_shipDraw = true;
    return (int)g_shipCopy.size();
}

int ship_bind(int i)
{
    if (i < 0 || i >= (int)g_shipCopy.size()) return 2;
    const Placement& p = g_copies[g_shipCopy[i]];
    D34 R = d34_mul(actor_transform(p.shipX, p.shipY, p.shipZ, p.shipAngle[0], p.shipAngle[1], p.shipAngle[2]), g_shipK);
    D34 D = d34_mul(R, g_shipLinv);
    for (const Slot& slot : g_slots)
    {
        uint32_t copy = ring_alloc(slot.size);
        if (!copy)
        {
            ring_flush();
            return 2;
        }
        const float* src = gptr<float>(slot.addr);
        float* dst = gptr<float>(copy);
        std::memcpy(dst, src, slot.size);
        for (uint32_t m = 0; m + 12 <= slot.size / 4; m += 12)
        {
            D34 out = d34_mul(D, d34_from(src + m));
            for (int k = 0; k < 12; k++) dst[m + k] = (float)out[k];
        }
        g_regs[g_vsStart + slot.index * 7] = copy;
    }
    ring_flush();
    bool texturesChanged = false;
    float hue = kShipHueFor[p.color & 7];
    for (const TexSlot& t : g_texSlots)
    {
        uint32_t* w = g_regs + g_psTexStart + t.unit * 7;
        if (w[2] != t.base) texturesChanged = true;
        w[2] = t.base;
        w[3] = t.mip;
        uint32_t base, mip;
        if (hue >= 0 && recolored(w, p.color, base, mip, true))
        {
            w[2] = base >> 8;
            w[3] = mip >> 8;
            texturesChanged = true;
        }
    }
    return texturesChanged ? 1 : 0;
}

}

void note_palette(uint32_t obj, uint32_t dest, uint32_t size)
{
    if (!active()) return;
    if (!g_ring)
    {
        g_ring = hle::default_heap_alloc(kRingSize, 0x100);
        g_texPool = hle::default_heap_alloc(kTexPoolSize, 0x10000);
        if (g_ring) log_info("mp: {} KB for the other players' Links at {:08X}, textures at {:08X}", kRingSize >> 10, g_ring, g_texPool);
    }
    std::lock_guard lk(g_lock);
    if (size >= 48 && size <= 0x2000 && link_part(obj) < 0 && is_ship_skin(obj, dest))
    {
        uint32_t now = frame_number();
        g_shipPalettes[palette_hash(dest)] = now;
        if (g_shipPalettes.size() > 64) std::erase_if(g_shipPalettes, [now](const auto& kv) { return now - kv.second > 8; });
        return;
    }
    int32_t part = size >= 48 && size <= 0x2000 ? link_part(obj) : -1;
    float px, py, pz;
    if (part < 0 || !player_position(px, py, pz))
    {
        g_palettes.erase(dest);
        return;
    }
    const float* root = gptr<float>(dest);
    if (std::hypot(root[3] - px, root[7] - py, root[11] - pz) > kPartNearPlayer)
    {
        g_palettes.erase(dest);
        return;
    }
    g_palettes[dest] = {size, (uint16_t)part};
    uint32_t now = frame_number();
    g_paletteContents[palette_hash(dest)] = {size, (uint16_t)part, now, px, py, pz};
    if (g_paletteContents.size() > 256)
        std::erase_if(g_paletteContents, [now](const auto& kv) { return now - kv.second.frame > 8; });
    if (size > g_bodySize || (uint16_t)part == g_bodyPart)
    {
        g_bodyPart = (uint16_t)part;
        g_bodyDest = dest;
        g_bodySize = size;
    }
    static const bool debug = std::getenv("WF_RR_DEBUG") != nullptr;
    if (debug && frame_number() % 200 == 0) log_info("rr: Link palette {:08X} part +{:X} size {:X} at frame {}", dest, part, size, frame_number());
    set_local_pose((uint16_t)part, gptr<float>(dest), size / 48, px, py, pz);
}

}

using namespace wf;
using namespace wf::mp;

int wf_rr_gpu_prepare(uint32_t* regs, uint32_t vsUniformStart, uint32_t gpuFrame, uint32_t psTexStart, uint32_t psTexMask,
                      uint32_t vsAttribStart, uint32_t drawCount)
{
    g_drawCount = drawCount;
    g_vsAttrib = vsAttribStart;
    g_vsStartForRecord = vsUniformStart;
    g_meshKey = mesh_key(regs[vsAttribStart], regs[vsAttribStart + 1] + 1, drawCount);
    g_drawTag = 0;
    if (!active()) return 0;
    std::lock_guard lk(g_lock);
    if (!g_ring) return 0;
    g_shipDraw = false;
    if (int ships = g_shipPalettes.empty() ? 0 : ship_prepare(regs, vsUniformStart, gpuFrame, psTexStart, psTexMask))
    {
        g_recordKey = 0;
        return ships;
    }
    if (g_paletteContents.empty()) return 0;
    if (g_drawnFrame != gpuFrame)
    {
        g_frameDrawn.clear();
        g_frameReplayed.clear();
        g_drawnFrame = gpuFrame;
        g_dbgLast.swap(g_dbgCur);
        g_dbgCur.clear();
        g_dbgLastFrame = gpuFrame - 1;
    }
    float px, py, pz, yaw;
    if (!player_position(px, py, pz, &yaw)) return 0;
    g_playerX = px;
    g_playerY = py;
    g_playerZ = pz;
    g_slots.clear();
    g_drawCounter++;
    g_recordKey = 0;
    static constexpr uint32_t kMinSkinnedCount = 24;
    for (uint32_t i = 0; i < kSlots && drawCount >= kMinSkinnedCount; i++)
    {
        uint32_t addr = regs[vsUniformStart + i * 7], size = regs[vsUniformStart + i * 7 + 1] + 1;
        if (size < 48 || addr < 0x10000000 || addr >= 0x50000000) continue;
        auto it = g_paletteContents.find(palette_hash(addr));
        if (it == g_paletteContents.end()) continue;
        g_slots.push_back({i, addr, std::min(size, it->second.size), it->second.part});
        mark_part_drawn(it->second.part, g_meshKey, it->second.frame);
        if (g_drawPosFrame != gpuFrame)
        {
            g_drawPos[0] = it->second.x;
            g_drawPos[1] = it->second.y;
            g_drawPos[2] = it->second.z;
            g_drawPosFrame = gpuFrame;
        }
    }
    if (!g_slots.empty())
    {
        g_drawTag = 1;
        g_lastLinkDraw = g_drawCounter;
        if (!g_groupActive || g_groupRt != pass_id(regs)) g_groupView = 0;
        g_groupActive = true;
        g_groupRt = pass_id(regs);
        g_drawsSinceLink = 0;
    }
    else if (g_drawCounter - g_lastLinkDraw <= kRigidWindow && !std::getenv("WF_RR_NORIGID"))
    {
        uint32_t vbSize = regs[vsAttribStart + 1] + 1;
        auto blockSize = [&](uint32_t b) { return regs[vsUniformStart + b * 7] ? regs[vsUniformStart + b * 7 + 1] + 1 : 0; };
        bool modelDraw = blockSize(2) == 0x200 &&
                         ((blockSize(3) == 0x400 && (blockSize(4) == 0x300 || blockSize(4) == 0x400)) ||
                          (blockSize(3) == 0x2A0 && blockSize(4) == 0x400));
        uint32_t mesh = g_meshKey;
        if (modelDraw) g_groupView = regs[vsUniformStart + kViewSlot * 7];
        if (g_occurrenceFrame != gpuFrame)
        {
            g_occurrence.clear();
            g_occurrenceFrame = gpuFrame;
        }
        for (uint32_t i = 0; i < kSlots && modelDraw; i++)
        {
            uint32_t addr = regs[vsUniformStart + i * 7], size = regs[vsUniformStart + i * 7 + 1] + 1;
            if (size != 0x100 || addr < 0x10000000 || addr >= 0x50000000 || g_paletteContents.count(palette_hash(addr))) continue;
            const float* f = gptr<float>(addr);
            bool framePos = g_drawPosFrame == gpuFrame;
            float rx = framePos ? g_drawPos[0] : px, ry = framePos ? g_drawPos[1] : py, rz = framePos ? g_drawPos[2] : pz;
            if (std::hypot(f[3] - rx, f[7] - ry, f[11] - rz) > kRigidNearPlayer) continue;
            uint32_t matrices = 1;
            static const std::vector<uint32_t> skipCounts = [] {
                std::vector<uint32_t> v;
                if (const char* e = std::getenv("WF_RR_SKIPN"))
                    for (const char* q = e; *q;)
                    {
                        char* end;
                        v.push_back((uint32_t)std::strtoul(q, &end, 16));
                        q = *end ? end + 1 : end;
                    }
                return v;
            }();
            if (std::find(skipCounts.begin(), skipCounts.end(), g_drawCount) != skipCounts.end()) break;
            uint32_t pass = pass_id(regs);
            uint32_t passMesh = mesh ^ (pass * 0x9E3779B1u);
            uint32_t key = mesh ^ (g_occurrence[passMesh]++ << 28);
            uint32_t recordKey = key ^ (pass * 0x9E3779B1u);
            set_local_rigid(key, f, matrices, rx, ry, rz);
            g_drawTag = 2;
            g_frameDrawn.insert(recordKey);
            {
                auto rec = g_records.find(recordKey);
                if (rec == g_records.end() || gpuFrame - rec->second.frame > 120)
                {
                    g_recordKey = recordKey;
                    g_recordSlot = i;
                    g_recordSize = size;
                }
            }
            static const bool rigidLog = std::getenv("WF_RR_RIGIDLOG") != nullptr;
            static std::unordered_map<uint32_t, int> dumped;
            if (rigidLog && dumped[key]++ < 2)
            {
                std::string all;
                for (uint32_t bi = 0; bi < kSlots; bi++)
                {
                    uint32_t ba = regs[vsUniformStart + bi * 7], bs = regs[vsUniformStart + bi * 7 + 1] + 1;
                    if (!ba || bs > 0x2000) continue;
                    const float* bf = gptr<float>(ba);
                    all += std::format("\n    block {} {:08X} size {:X}:", bi, ba, bs);
                    for (uint32_t mi = 0; mi < std::min(bs / 48, 6u); mi++)
                        all += std::format(" [{:.2f} {:.2f} {:.2f} | t-p=({:.0f},{:.0f},{:.0f})]", bf[mi * 12], bf[mi * 12 + 5], bf[mi * 12 + 10],
                                           bf[mi * 12 + 3] - px, bf[mi * 12 + 7] - py, bf[mi * 12 + 11] - pz);
                }
                log_info("rr rigid draw key {:08X} slot block {}:{}", key, i, all);
            }
            if (rigidLog)
            {
                std::string t;
                for (uint32_t mi = 0; mi < size / 48; mi++)
                    t += std::format(" m{}=({:.0f},{:.0f},{:.0f})", mi, f[mi * 12 + 3] - px, f[mi * 12 + 7] - py, f[mi * 12 + 11] - pz);
                log_info("rr rigid gpu {} key {:08X} vb {:08X}+{:X} n {} draw+{} view {:08X} rot [{:.2f} {:.2f} {:.2f} / {:.2f} {:.2f} {:.2f} / {:.2f} {:.2f} {:.2f}]:{}",
                         gpuFrame, key, regs[vsAttribStart], vbSize, g_drawCount, g_drawCounter - g_lastLinkDraw, regs[vsUniformStart + 2 * 7],
                         f[0], f[1], f[2], f[4], f[5], f[6], f[8], f[9], f[10], t.substr(0, 40));
            }
            g_slots.push_back({i, addr, size, 0, true, key});
            break;
        }
    }
    if (!g_slots.empty() && g_slots[0].rigid) g_drawsSinceLink = 0;
    if (g_slots.empty()) return 0;
    g_texSlots.clear();
    g_psTexStart = psTexStart;
    for (uint32_t u = 0; u < 18; u++)
    {
        if (!(psTexMask & (1u << u))) continue;
        const uint32_t* w = regs + psTexStart + u * 7;
        uint32_t format = w[1] >> 26, width = (w[0] >> 19) + 1;
        if (u == kAlbedoUnit && format == 0x31 && (w[0] & 7) == 1 && width <= 1024 && (w[2] << 8) >= 0x10000000 && (w[2] << 8) < 0x50000000)
            g_texSlots.push_back({u, w[2], w[3]});
    }
    static const bool texLog = std::getenv("WF_RR_TEXLOG") != nullptr;
    if (texLog && gpuFrame % 300 == 0)
        for (uint32_t u = 0; u < 18; u++)
        {
            if (!(psTexMask & (1u << u))) continue;
            const uint32_t* w = regs + psTexStart + u * 7;
            log_info("rr tex: unit {} base {:08X} mip {:08X} dim {} tile {} pitch {} width {} height {} format {:02X} w4 {:08X}",
                     u, w[2] << 8, w[3] << 8, w[0] & 7, (w[0] >> 3) & 15, (((w[0] >> 8) & 0x7FF) + 1) * 8, (w[0] >> 19) + 1,
                     (w[1] & 0x1FFF) + 1, w[1] >> 26, w[4]);
        }
    g_copies = frame_placements(gpuFrame);
    if (g_copies.empty()) return 0;
    g_rootX = px;
    g_rootY = py;
    g_rootZ = pz;
    g_localYaw = yaw;
    g_regs = regs;
    g_vsStart = vsUniformStart;
    if (g_patchedFrame != gpuFrame)
    {
        g_patched.clear();
        g_hiddenCopies.clear();
        g_patchedFrame = gpuFrame;
    }
    return (int)g_copies.size();
}

int wf_rr_gpu_bind(int i)
{
    std::lock_guard lk(g_lock);
    if (g_shipDraw) return ship_bind(i);
    if (i < 0 || i >= (int)g_copies.size() || !g_regs) return 2;
    const Placement& p = g_copies[i];
    bool texturesChanged = false, hidden = false;
    float a = p.yaw - g_localYaw, c = std::cos(a), s = std::sin(a);
    g_blendSaved = false;
    if (!g_slots.empty() && g_slots[0].rigid)
    {
        uint32_t& blend = g_regs[kCbBlend0];
        if ((blend & 0x1F) == kBlendDstAlpha)
        {
            g_blendSave[0] = blend;
            g_blendSave[1] = g_regs[kCbBlend0 + 1];
            blend = g_regs[kCbBlend0 + 1] = kBlendSrcAlpha;
            g_blendSaved = true;
        }
    }
    for (const TexSlot& t : g_texSlots)
    {
        uint32_t* w = g_regs + g_psTexStart + t.unit * 7;
        if (w[2] != t.base) texturesChanged = true;
        w[2] = t.base;
        w[3] = t.mip;
    }
    static const bool noTunic = std::getenv("WF_MP_NOTUNIC") != nullptr;
    if (kTunicHue[p.color & 7] >= 0 && !noTunic)
        for (const TexSlot& t : g_texSlots)
        {
            uint32_t* w = g_regs + g_psTexStart + t.unit * 7;
            uint32_t base, mip;
            if (!recolored(w, p.color, base, mip)) continue;
            w[2] = base >> 8;
            w[3] = mip >> 8;
            texturesChanged = true;
        }
    for (const Slot& slot : g_slots)
    {
        uint64_t key = ((uint64_t)slot.addr << 8) | (uint32_t)i;
        key ^= (uint64_t)slot.part * 0x9E3779B97F4A7C15ull;
        {
            const uint32_t* w = gptr<uint32_t>(slot.addr);
            uint64_t h = 1469598103934665603ull;
            for (uint32_t k = 0; k < std::min<uint32_t>(slot.size / 4, 48); k++) h = (h ^ w[k]) * 1099511628211ull;
            key ^= h;
        }
        uint32_t copy;
        if (auto it = g_patched.find(key); !slot.rigid && it != g_patched.end())
        {
            copy = it->second;
            if (g_hiddenCopies.count(key)) hidden = true;
        }
        else
        {
            copy = ring_alloc(slot.size);
            if (!copy)
            {
                ring_flush();
                return (texturesChanged ? 1 : 0) | 2;
            }
            const float* src = gptr<float>(slot.addr);
            float* dst = gptr<float>(copy);
            std::memcpy(dst, src, slot.size);
            if (slot.rigid)
            {
                const std::vector<float>* m = nullptr;
                for (const auto& [key, mat] : p.rigid)
                    if (key == slot.key) m = &mat;
                static const bool rigidLog = std::getenv("WF_RR_RIGIDLOG") != nullptr;
                static int missLog = 0;
                if (rigidLog && !m && missLog++ < 6)
                {
                    std::string have;
                    for (const auto& [k, mat] : p.rigid) have += std::format(" {:08X}/{}", k, mat.size() / 12);
                    log_info("rr miss {:08X}: remote has{}", slot.key, have);
                }
                if (rigidLog)
                    log_info("rr bind gpu {} key {:08X} {} vb {:08X} rel ({:.0f},{:.0f},{:.0f})", g_patchedFrame, slot.key, m ? "found" : "MISSING",
                             g_regs[g_vsAttrib], m ? (*m)[3] : 0.f, m ? (*m)[7] : 0.f, m ? (*m)[11] : 0.f);
                static const bool ghostCheck = std::getenv("WF_RR_GHOSTCHECK") != nullptr;
                if (ghostCheck && m)
                {
                    float err = 0;
                    for (int k = 0; k < 12; k++)
                    {
                        float local = src[k] - (k == 3 ? g_playerX : k == 7 ? g_playerY : k == 11 ? g_playerZ : 0.0f);
                        err = std::max(err, std::abs(local - (*m)[k]) * (k % 4 == 3 ? 0.05f : 1.0f));
                    }
                    if (err > 0.05f)
                        log_info("rr ghost mismatch: key {:08X} vb {:08X} n {} err {:.2f} local t=({:.0f},{:.0f},{:.0f}) remote t=({:.0f},{:.0f},{:.0f})", slot.key,
                                 g_regs[g_vsAttrib], g_drawCount, err, src[3] - g_playerX, src[7] - g_playerY, src[11] - g_playerZ, (*m)[3], (*m)[7], (*m)[11]);
                }
                if (g_dbgHidden.count(slot.key)) m = nullptr;
                if (m && m->size() >= 12 && slot.size >= 48)
                {
                    std::copy(m->begin(), m->begin() + 12, dst);
                    dst[3] += p.x;
                    dst[7] += p.y;
                    dst[11] += p.z;
                }
                else
                    hidden = true;
                g_regs[g_vsStart + slot.index * 7] = copy;
                continue;
            }
            static const bool solo = std::getenv("WF_RR_SOLO") != nullptr;
            if (solo)
            {
                hidden = true;
                g_patched[key] = copy;
                g_regs[g_vsStart + slot.index * 7] = copy;
                continue;
            }
            const std::vector<float>* pose = nullptr;
            for (const auto& pp : p.poses)
                if (pp.part == slot.part && pp.bones.size() / 12 == slot.size / 48 &&
                    std::find(pp.meshes.begin(), pp.meshes.end(), g_meshKey) != pp.meshes.end())
                    pose = &pp.bones;
            static const bool poseLog = std::getenv("WF_RR_POSELOG") != nullptr;
            if (poseLog)
            {
                std::string have;
                for (const auto& pp : p.poses)
                    if (pp.part == slot.part) have += std::format(" [bones {} meshes {}]", pp.bones.size() / 12, pp.meshes.size());
                log_info("rr pose gpu {} part +{:X} size {:X} mesh {:08X}: {}{}", g_patchedFrame, slot.part, slot.size, g_meshKey, pose ? "found" : "HIDDEN", have);
            }
            if (pose)
            {
                for (uint32_t m = 0; m < pose->size(); m += 12)
                {
                    const float* b = pose->data() + m;
                    bool far = std::abs(b[3]) > kMaxBoneOffset || std::abs(b[7]) > kMaxBoneOffset || std::abs(b[11]) > kMaxBoneOffset;
                    for (int r = 0; r < 3; r++)
                    {
                        for (int col = 0; col < 3; col++) dst[m + r * 4 + col] = far ? 0.0f : b[r * 4 + col];
                        dst[m + r * 4 + 3] = (far ? 0.0f : b[r * 4 + 3]) + (r == 0 ? p.x : r == 1 ? p.y : p.z);
                    }
                }
                g_patched[key] = copy;
                g_regs[g_vsStart + slot.index * 7] = copy;
                continue;
            }
            hidden = true;
            g_patched[key] = copy;
            g_hiddenCopies.insert(key);
        }
        g_regs[g_vsStart + slot.index * 7] = copy;
    }
    if (!hidden && g_dbgHidden.count(g_meshKey)) hidden = true;
    for (const Slot& slot : g_slots)
    {
        const float* m = gptr<float>(g_regs[g_vsStart + slot.index * 7]);
        g_dbgCur.push_back({slot.rigid ? slot.key : g_meshKey, slot.part, i, slot.rigid, false, hidden,
                            pass_id(g_regs), m[3], m[7], m[11], {}, {}});
        std::copy(m, m + 12, g_dbgCur.back().m);
        if (!slot.rigid)
            for (uint32_t b = 0; b < slot.size / 48; b++)
                g_dbgCur.back().bones.insert(g_dbgCur.back().bones.end(), {m[b * 12 + 3], m[b * 12 + 7], m[b * 12 + 11]});
    }
    ring_flush();
    return (texturesChanged ? 1 : 0) | (hidden ? 2 : 0);
}

bool wf_rr_gpu_restore()
{
    std::lock_guard lk(g_lock);
    if (!g_regs) return false;
    for (const Slot& slot : g_slots) g_regs[g_vsStart + slot.index * 7] = slot.addr;
    if (g_blendSaved)
    {
        g_regs[kCbBlend0] = g_blendSave[0];
        g_regs[kCbBlend0 + 1] = g_blendSave[1];
        g_blendSaved = false;
    }
    bool texturesChanged = false;
    for (const TexSlot& t : g_texSlots)
    {
        if (g_regs[g_psTexStart + t.unit * 7 + 2] != t.base) texturesChanged = true;
        g_regs[g_psTexStart + t.unit * 7 + 2] = t.base;
        g_regs[g_psTexStart + t.unit * 7 + 3] = t.mip;
    }
    return texturesChanged;
}

bool wf_rr_gpu_wants_record() { return g_recordKey != 0; }

void wf_rr_gpu_record(const uint32_t* regs, uint32_t regCount, const uint32_t* params)
{
    std::lock_guard lk(g_lock);
    if (!g_recordKey) return;
    DrawRecord& r = g_records[g_recordKey];
    r.regs.assign(regs, regs + regCount);
    r.slot = g_recordSlot;
    r.rt = pass_id(regs);
    r.frame = g_patchedFrame;
    uint32_t blockAddr = regs[g_vsStartForRecord + g_recordSlot * 7];
    r.block.assign(gptr<uint8_t>(blockAddr), gptr<uint8_t>(blockAddr) + g_recordSize);
    r.others.clear();
    for (uint32_t start : {g_vsStartForRecord, (uint32_t)((int)g_vsStartForRecord + kPsUniformFromVs)})
        for (uint32_t i = 0; i < kSlots; i++)
        {
            uint32_t reg = start + i * 7, addr = regs[reg], size = regs[reg + 1] + 1;
            if (reg == g_vsStartForRecord + g_recordSlot * 7 || reg == g_vsStartForRecord + kViewSlot * 7) continue;
            if (!addr || addr < 0x10000000 || addr >= 0x50000000 || size > 0x1000) continue;
            r.others.push_back({reg, std::vector<uint8_t>(gptr<uint8_t>(addr), gptr<uint8_t>(addr) + size)});
        }
    std::copy(params, params + 6, r.params);
    g_recordKey = 0;
}

int wf_rr_gpu_extras_begin(const uint32_t* regs)
{
    uint32_t rt = pass_id(regs);
    std::lock_guard lk(g_lock);
    g_extras.clear();
    if (!g_groupActive) return 0;
    if (rt == g_groupRt && ++g_drawsSinceLink <= kRigidWindow) return 0;
    g_groupActive = false;
    static const bool noExtras = std::getenv("WF_RR_NOEXTRAS") != nullptr;
    if (!active() || !g_ring || noExtras) return 0;
    std::vector<Placement> players;
    players = frame_placements(g_drawnFrame);
    for (int c = 0; c < (int)players.size(); c++)
        for (const auto& [key, mat] : players[c].rigid)
        {
            uint32_t recordKey = key ^ (g_groupRt * 0x9E3779B1u);
            if (g_frameDrawn.count(recordKey) || g_dbgHidden.count(key)) continue;
            auto rec = g_records.find(recordKey);
            if (rec == g_records.end() || mat.size() < 12) continue;
            if (!g_frameReplayed.insert(recordKey ^ (uint32_t)c * 0x85EBCA6Bu).second) continue;
            g_extras.push_back({key, recordKey, c});
        }
    g_extraPlayers = std::move(players);
    return (int)g_extras.size();
}

bool wf_rr_gpu_extra_apply(int j, uint32_t* regs, uint32_t* params)
{
    std::lock_guard lk(g_lock);
    if (j < 0 || j >= (int)g_extras.size()) return false;
    const Extra& e = g_extras[j];
    const DrawRecord& r = g_records[e.recordKey];
    const Placement& p = g_extraPlayers[e.copy];
    const std::vector<float>* mat = nullptr;
    for (const auto& [key, m] : p.rigid)
        if (key == e.key) mat = &m;
    if (!mat || !g_groupView) return false;
    uint32_t copy = ring_alloc((uint32_t)r.block.size());
    if (!copy) return false;
    std::memcpy(gptr<uint8_t>(copy), r.block.data(), r.block.size());
    float* dst = gptr<float>(copy);
    std::copy(mat->begin(), mat->begin() + 12, dst);
    dst[3] += p.x;
    dst[7] += p.y;
    dst[11] += p.z;
    std::memcpy(regs, r.regs.data(), r.regs.size() * 4);
    regs[g_vsStartForRecord + r.slot * 7] = copy;
    regs[g_vsStartForRecord + kViewSlot * 7] = g_groupView;
    g_dbgCur.push_back({e.key, 0, e.copy, true, true, false, r.rt, dst[3], dst[7], dst[11], {}, {}});
    std::copy(dst, dst + 12, g_dbgCur.back().m);
    static const bool rigidLog = std::getenv("WF_RR_RIGIDLOG") != nullptr;
    if (rigidLog)
        log_info("rr extra gpu {} key {:08X} rec {} rt {:08X} vb {:08X}/{:X} idx {:08X} n {} rel ({:.0f},{:.0f},{:.0f})", g_patchedFrame, e.key,
                 r.frame, r.rt, r.regs[g_vsAttrib], r.regs[g_vsAttrib + 1] + 1, r.params[4], r.params[3], (*mat)[3], (*mat)[7], (*mat)[11]);
    for (const auto& o : r.others)
    {
        uint32_t c = ring_alloc((uint32_t)o.bytes.size());
        if (!c) return false;
        std::memcpy(gptr<uint8_t>(c), o.bytes.data(), o.bytes.size());
        regs[o.reg] = c;
    }
    std::copy(r.params, r.params + 6, params);
    ring_flush();
    return true;
}

int wf_rr_draw_tag() { return g_drawTag; }

namespace wf::mp::rr_debug {

uint32_t snapshot(std::vector<CopyInfo>& out)
{
    std::lock_guard lk(g_lock);
    out = g_dbgLast;
    return g_dbgLastFrame;
}

bool hidden(uint32_t key)
{
    std::lock_guard lk(g_lock);
    return g_dbgHidden.count(key) != 0;
}

void set_hidden(uint32_t key, bool on)
{
    std::lock_guard lk(g_lock);
    if (on) g_dbgHidden.insert(key);
    else g_dbgHidden.erase(key);
}

}
