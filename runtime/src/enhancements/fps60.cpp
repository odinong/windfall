#include <windows.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <tuple>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "gpu/window.h"
#include "wf_render_settings.h"
#include "enhancements/camera.h"
#include "enhancements/cheats.h"
#include "enhancements/playtest.h"
#include "net/multiplayer.h"
#include "net/remote_render.h"
#include "mods/mods.h"
#include "ui/menu.h"
#include "input/input.h"

void wf_render_fast_forward(bool on);
void wf_render_set_vsync_rate(int rate);
#include "ui/settings.h"
#include "gpu/gpu.h"
#include "hle/hle.h"
#include "os/sched.h"
#include "wf/ppc.h"
#include "core/game_module.h"

WF_ORIG(sub_02746790_orig);
WF_ORIG(sub_0274C874_orig);
WF_ORIG(sub_02747BDC_orig);
WF_ORIG(sub_027BF048_orig);
WF_ORIG(sub_027DBC2C_orig);
WF_ORIG(sub_027DDBB0_orig);
WF_ORIG(sub_0282685C_orig);
WF_ORIG(sub_0282167C_orig);
WF_ORIG(sub_027F2FC4_orig);
WF_ORIG(sub_025DE37C_orig);
WF_ORIG(sub_025CBB90_orig);
WF_ORIG(sub_02524CA0_orig);
WF_ORIG(sub_02524DA0_orig);
WF_ORIG(sub_02524EA0_orig);
WF_ORIG(sub_02524FA0_orig);
WF_ORIG(sub_025250A0_orig);
WF_ORIG(sub_0256C8FC_orig);
WF_ORIG(sub_02518798_orig);

namespace wf::game {
std::atomic<int> g_testMode{0};
namespace {

constexpr uint32_t kMtxInit = 0x0200FAC4;
constexpr uint32_t kFpcDwHandler = 0x025DE37C, kFpcDrawIterater = 0x025DF908, kFpcDraw = 0x025DF904;
constexpr uint32_t kUniformTypeTable = 0x10169D50;

int startup_fps()
{
    static const int fps = [] {
        if (const char* e = std::getenv("WF_FPS")) return std::clamp(std::atoi(e), 30, 1000);
        if (const char* e = std::getenv("WF_60FPS")) return *e != '0' ? 60 : 30;
        int f;
        {
            std::lock_guard lk(settings::lock());
            f = settings::get().fps;
        }
        if (f == 0) f = window::refresh_rate();
        return std::clamp(f, 30, 1000);
    }();
    return fps;
}
// the interpolated loop (frame rate above 30) is on; switches with the menu's setting while playing (follow_fps_setting)
std::atomic<int> g_highFps{-1};
bool enabled()
{
    int v = g_highFps.load(std::memory_order_relaxed);
    if (v < 0) g_highFps = v = startup_fps() > 30;
    return v != 0;
}

std::atomic<int> g_liveFps{0};
int target_fps()
{
    int f = g_liveFps.load(std::memory_order_relaxed);
    return f ? f : startup_fps();
}
void follow_fps_setting()
{
    static const bool overridden = std::getenv("WF_FPS") || std::getenv("WF_60FPS");
    if (overridden) return;
    static int s_setting = -1;
    int f;
    {
        std::lock_guard lk(settings::lock());
        f = settings::get().fps;
    }
    if (f == s_setting) return;
    s_setting = f;
    if (f == 0) f = window::refresh_rate();
    if (f > 30) g_liveFps = std::clamp(f, 31, 1000);
    g_highFps = f > 30;
}

std::atomic<uint32_t> g_frame{0};
std::atomic<uint32_t> g_tick{0};
std::atomic<bool> g_logicFrame{true};
std::atomic<float> g_step{1.0f};
bool interpolating() { return g_step.load(std::memory_order_relaxed) < 1.0f && g_testMode.load(std::memory_order_relaxed) != 1; }
std::atomic<bool> g_paused{false};
std::atomic<bool> g_fastForward{false};

uint64_t g_frameStartNs = 0;
std::atomic<uint32_t> g_burstCalls{0}, g_burstSplit{0};
void note_burst()
{
    static const bool on = std::getenv("WF_60FPS_THREADLOG") != nullptr;
    if (!on) return;
    thread_local uint64_t lastNs = 0;
    thread_local uint32_t burstFrame = 0;
    thread_local bool split = false;
    uint64_t now = os::now_ns();
    uint32_t frame = g_frame;
    if (now - lastNs > 2000000)
    {
        burstFrame = frame;
        split = false;
    }
    else if (frame != burstFrame && !split)
    {
        split = true;
        g_burstSplit++;
    }
    lastNs = now;
    g_burstCalls++;
}
struct FrameStats { double workSum = 0, workMax = 0, waitSum = 0; uint32_t count = 0, over = 0; };
FrameStats g_stats[2];

template<typename V>
class FlatMap
{
public:
    V& operator[](uint64_t key)
    {
        if ((m_size + 1) * 4 > m_slots.size() * 3) grow();
        size_t mask = m_slots.size() - 1;
        for (size_t i = index(key, mask);; i = (i + 1) & mask)
        {
            Slot& s = m_slots[i];
            if (s.used && s.key == key) return s.value;
            if (!s.used)
            {
                s.used = true;
                s.key = key;
                s.value = V{};
                m_size++;
                return s.value;
            }
        }
    }
    template<typename Pred>
    void erase_if(Pred pred)
    {
        std::vector<Slot> old(m_slots.size());
        old.swap(m_slots);
        m_size = 0;
        size_t mask = m_slots.size() - 1;
        for (Slot& s : old)
        {
            if (!s.used || pred(s.key, s.value)) continue;
            size_t i = index(s.key, mask);
            while (m_slots[i].used) i = (i + 1) & mask;
            m_slots[i] = std::move(s);
            m_size++;
        }
    }
    template<typename Fn>
    void for_each(Fn fn)
    {
        for (Slot& s : m_slots)
            if (s.used) fn(s.key, s.value);
    }
    size_t size() const { return m_size; }

private:
    struct Slot { uint64_t key = 0; bool used = false; V value{}; };
    static size_t index(uint64_t key, size_t mask) { return (size_t)((key * 0x9E3779B97F4A7C15ull) >> 32) & mask; }
    void grow()
    {
        std::vector<Slot> old(m_slots.empty() ? 64 : m_slots.size() * 2);
        old.swap(m_slots);
        m_size = 0;
        size_t mask = m_slots.size() - 1;
        for (Slot& s : old)
        {
            if (!s.used) continue;
            size_t i = index(s.key, mask);
            while (m_slots[i].used) i = (i + 1) & mask;
            m_slots[i] = std::move(s);
            m_size++;
        }
    }
    std::vector<Slot> m_slots;
    size_t m_size = 0;
};

struct TransformField
{
    uint32_t frame = 0;
    uint32_t tick = 0;
    uint32_t prevSrc = 0, curSrc = 0;
    std::vector<uint32_t> prev, cur;
    uint16_t rows = 0, wordsPerRow = 0, rowStride = 0;
    bool transform = false, blended = false;
};
constexpr size_t kShards = 64;
struct Shard
{
    std::mutex lock;
    FlatMap<TransformField> fields;
};
Shard g_shards[kShards];

struct SourceInfo
{
    uint32_t frame = 0, tick = 0, writes = 0, hash = 0, perFrameUntil = 0;
    uint32_t histTick = 0;
    std::vector<uint32_t> histCur, histPrev;
    uint64_t field = 0;
    bool shared = false;
};
struct SourceShard
{
    std::mutex lock;
    FlatMap<SourceInfo> sources;
};
SourceShard g_sourceShards[kShards];

std::atomic<uint32_t> g_uboStats[2][4];

bool source_track(uint32_t src, uint64_t field, const uint32_t* words, uint32_t n, uint32_t frame, uint32_t tick, bool trackPerFrame,
                  bool& perFrame, std::vector<uint32_t>& prevOut)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ words[i]) * 16777619u;
    SourceShard& sh = g_sourceShards[(src >> 4) % kShards];
    std::lock_guard lk(sh.lock);
    SourceInfo& si = sh.sources[src];
    perFrame = false;
    if (trackPerFrame)
    {
        if (si.field != field) si = SourceInfo{.field = field};
        if (si.frame == frame)
        {
            if (++si.writes > 1) si.shared = true;
        }
        else
        {
            if (!si.shared && si.tick == tick && si.frame + 1 == frame && si.hash != h) si.perFrameUntil = frame + 240;
            si.frame = frame;
            si.tick = tick;
            si.writes = 1;
        }
        si.hash = h;
        perFrame = !si.shared && si.perFrameUntil > frame;
        if (perFrame) return false;
    }
    if (si.field != field) return false;
    if (si.histTick != tick)
    {
        if (si.histTick + 1 == tick && si.histCur.size() == n) si.histPrev.swap(si.histCur);
        else si.histPrev.clear();
        si.histTick = tick;
    }
    si.histCur.assign(words, words + n);
    if (si.shared || si.histPrev.size() != n) return false;
    prevOut.assign(si.histPrev.begin(), si.histPrev.end());
    return true;
}

bool per_frame_source(uint32_t src, uint64_t field, uint32_t n, uint32_t frame, uint32_t tick)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ rd32(src + 4 * i)) * 16777619u;
    SourceShard& sh = g_sourceShards[(src >> 4) % kShards];
    std::lock_guard lk(sh.lock);
    SourceInfo& si = sh.sources[src];
    if (si.field != field) si = SourceInfo{.field = field};
    if (si.frame == frame)
    {
        if (++si.writes > 1) si.shared = true;
    }
    else
    {
        if (!si.shared && si.tick == tick && si.frame + 1 == frame && si.hash != h) si.perFrameUntil = frame + 240;
        si.frame = frame;
        si.tick = tick;
        si.writes = 1;
    }
    si.hash = h;
    return !si.shared && si.perFrameUntil > frame;
}

bool source_history(uint32_t src, uint64_t field, uint32_t n, uint32_t tick, std::vector<uint32_t>& prevOut)
{
    SourceShard& sh = g_sourceShards[(src >> 4) % kShards];
    std::lock_guard lk(sh.lock);
    SourceInfo& si = sh.sources[src];
    if (si.field != field) return false;
    if (si.histTick != tick)
    {
        if (si.histTick + 1 == tick && si.histCur.size() == n) si.histPrev.swap(si.histCur);
        else si.histPrev.clear();
        si.histTick = tick;
    }
    si.histCur.resize(n);
    for (uint32_t i = 0; i < n; i++) si.histCur[i] = rd32(src + 4 * i);
    if (si.shared || si.histPrev.size() != n) return false;
    prevOut = si.histPrev;
    return true;
}

void prune_history(uint32_t tick, uint32_t frame)
{
    {
        Shard& sh = g_shards[tick % kShards];
        std::lock_guard lk(sh.lock);
        sh.fields.erase_if([&](uint64_t, const TransformField& f) { return tick - f.tick > 2; });
    }
    SourceShard& sh = g_sourceShards[tick % kShards];
    std::lock_guard lk(sh.lock);
    sh.sources.erase_if([&](uint64_t, const SourceInfo& si) { return frame - si.frame > 240 && tick - si.histTick > 2; });
}

std::pair<size_t, size_t> history_sizes()
{
    size_t fields = 0, sources = 0;
    for (Shard& sh : g_shards)
    {
        std::lock_guard lk(sh.lock);
        fields += sh.fields.size();
    }
    for (SourceShard& sh : g_sourceShards)
    {
        std::lock_guard lk(sh.lock);
        sources += sh.sources.size();
    }
    return {fields, sources};
}

bool lerpable(float v) { return std::isfinite(v) && (v == 0.0f || std::abs(v) > 1e-30f) && std::abs(v) < 1e7f; }

uint32_t lerp_word(uint32_t prev, uint32_t value)
{
    if (prev == value) return value;
    float a = std::bit_cast<float>(prev), b = std::bit_cast<float>(value);
    if (!lerpable(a) || !lerpable(b)) return value;
    return std::bit_cast<uint32_t>(a + (b - a) * g_step.load(std::memory_order_relaxed));
}

// a 3x4 model matrix (host-order float bits, rows of 4) between the previous tick and this one: the translation is
// lerped, the rotation is slerped (as a quaternion) and each axis' scale lerped, so a part turning fast keeps its shape
// in between. A part that turned more than ~100 degrees in one tick (a flip, like Link's hat when his head goes
// upside down in a roll) takes the new matrix: there is no sensible halfway, and blending it sheared the mesh.
void lerp_affine(const uint32_t* prev, const uint32_t* cur, uint32_t* out)
{
    static const bool plain = std::getenv("WF_60FPS_PLAINLERP") != nullptr;
    if (plain)
    {
        for (int i = 0; i < 12; i++) out[i] = lerp_word(prev[i], cur[i]);
        return;
    }
    const float s = g_step.load(std::memory_order_relaxed);
    auto f = [](uint32_t w) { return std::bit_cast<float>(w); };
    auto take_cur = [&] { std::copy(cur, cur + 12, out); };
    // columns, their lengths (scale) and the rotation with the scale taken out
    double ra[3][3], rb[3][3], sa[3], sb[3];
    for (int c = 0; c < 3; c++)
    {
        double la = 0, lb = 0;
        for (int r = 0; r < 3; r++)
        {
            float x = f(prev[r * 4 + c]), y = f(cur[r * 4 + c]);
            if (!lerpable(x) || !lerpable(y)) return take_cur();
            ra[r][c] = x;
            rb[r][c] = y;
            la += (double)x * x;
            lb += (double)y * y;
        }
        sa[c] = std::sqrt(la);
        sb[c] = std::sqrt(lb);
        if (sa[c] < 1e-9 || sb[c] < 1e-9) return take_cur();
        for (int r = 0; r < 3; r++)
        {
            ra[r][c] /= sa[c];
            rb[r][c] /= sb[c];
        }
    }
    // to quaternions (w, x, y, z); a mirrored matrix (determinant < 0) can't be one: blend it as before
    auto det = [](const double m[3][3]) {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    auto to_quat = [](const double m[3][3], double q[4]) {
        double t = m[0][0] + m[1][1] + m[2][2];
        if (t > 0)
        {
            double k = 0.5 / std::sqrt(t + 1.0);
            q[0] = 0.25 / k;
            q[1] = (m[2][1] - m[1][2]) * k;
            q[2] = (m[0][2] - m[2][0]) * k;
            q[3] = (m[1][0] - m[0][1]) * k;
        }
        else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
        {
            double k = 2.0 * std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]);
            q[0] = (m[2][1] - m[1][2]) / k;
            q[1] = 0.25 * k;
            q[2] = (m[0][1] + m[1][0]) / k;
            q[3] = (m[0][2] + m[2][0]) / k;
        }
        else if (m[1][1] > m[2][2])
        {
            double k = 2.0 * std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]);
            q[0] = (m[0][2] - m[2][0]) / k;
            q[1] = (m[0][1] + m[1][0]) / k;
            q[2] = 0.25 * k;
            q[3] = (m[1][2] + m[2][1]) / k;
        }
        else
        {
            double k = 2.0 * std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]);
            q[0] = (m[1][0] - m[0][1]) / k;
            q[1] = (m[0][2] + m[2][0]) / k;
            q[2] = (m[1][2] + m[2][1]) / k;
            q[3] = 0.25 * k;
        }
        double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        for (int i = 0; i < 4; i++) q[i] /= n;
    };
    if (det(ra) <= 0 || det(rb) <= 0)
    {
        for (int i = 0; i < 12; i++) out[i] = lerp_word(prev[i], cur[i]);
        return;
    }
    double qa[4], qb[4];
    to_quat(ra, qa);
    to_quat(rb, qb);
    double dot = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (dot < 0)
    {
        dot = -dot;
        for (double& v : qb) v = -v;
    }
    if (dot < 0.64) return take_cur();  // turned more than ~100 degrees in one tick
    double q[4];
    double theta = std::acos(std::min(1.0, dot));
    if (theta < 1e-4)
        for (int i = 0; i < 4; i++) q[i] = qa[i] + (qb[i] - qa[i]) * s;
    else
    {
        double wa = std::sin((1 - s) * theta) / std::sin(theta), wb = std::sin(s * theta) / std::sin(theta);
        for (int i = 0; i < 4; i++) q[i] = qa[i] * wa + qb[i] * wb;
    }
    double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    double w = q[0] / n, x = q[1] / n, y = q[2] / n, z = q[3] / n;
    const double m[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                            {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                            {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
    for (int c = 0; c < 3; c++)
    {
        double sc = sa[c] + (sb[c] - sa[c]) * s;
        for (int r = 0; r < 3; r++) out[r * 4 + c] = std::bit_cast<uint32_t>((float)(m[r][c] * sc));
    }
    for (int r = 0; r < 3; r++) out[r * 4 + 3] = lerp_word(prev[r * 4 + 3], cur[r * 4 + 3]);
}

std::mutex g_cameraLock;
Camera g_camera{};
bool g_cameraValid = false, g_projectionValid = false;

void record_camera(uint32_t lr, uint32_t n, const uint32_t* words)
{
    auto copy = [&](std::array<float, 16>& to) {
        for (int i = 0; i < 16; i++) to[i] = std::bit_cast<float>(words[i]);
    };
    if (n != 16) return;
    if (lr == 0x02790120)
    {
        std::lock_guard lk(g_cameraLock);
        copy(g_camera.viewToWorld);
        g_cameraValid = true;
    }
    else if (lr == 0x027FBA00 && std::bit_cast<float>(words[14]) == -1.0f && words[15] == 0 && words[12] == 0 && words[13] == 0)
    {
        std::lock_guard lk(g_cameraLock);
        copy(g_camera.projection);
        g_projectionValid = true;
    }
}

bool copy_camera(Camera& out)
{
    std::lock_guard lk(g_cameraLock);
    if (!g_cameraValid || !g_projectionValid) return false;
    out = g_camera;
    return true;
}

using Mat4 = std::array<double, 16>;

Mat4 mat_from_words(const uint32_t* w, uint32_t n = 16)
{
    Mat4 m{};
    for (uint32_t i = 0; i < n; i++) m[i] = std::bit_cast<float>(w[i]);
    if (n == 12) m[15] = 1.0;
    return m;
}

Mat4 mat_mul(const Mat4& a, const Mat4& b)
{
    Mat4 m{};
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            for (int k = 0; k < 4; k++) m[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
    return m;
}

bool mat_affine_inverse(const Mat4& m, Mat4& r)
{
    double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::abs(det) < 1e-20) return false;
    double inv[9] = {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
                     (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
                     (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
    r = Mat4{};
    for (int row = 0; row < 3; row++)
    {
        for (int col = 0; col < 3; col++) r[row * 4 + col] = inv[row * 3 + col];
        r[row * 4 + 3] = -(inv[row * 3 + 0] * m[3] + inv[row * 3 + 1] * m[7] + inv[row * 3 + 2] * m[11]);
    }
    r[15] = 1.0;
    return true;
}

Mat4 mat_between(const Mat4& a, const Mat4& b)
{
    double s = g_step.load(std::memory_order_relaxed);
    Mat4 m;
    for (int i = 0; i < 16; i++) m[i] = a[i] + (b[i] - a[i]) * s;
    return m;
}

bool rebased_halfway(const uint32_t* xPrev, const uint32_t* xCur, uint32_t n, const Mat4& bPrev, const Mat4& bCur,
                     uint32_t* out)
{
    if (n == 0 || n % 16) return false;
    Mat4 bMidInv;
    if (!mat_affine_inverse(mat_between(bPrev, bCur), bMidInv)) return false;
    std::vector<uint32_t> words(n);
    for (uint32_t m = 0; m < n; m += 16)
    {
        Mat4 x = mat_mul(mat_between(mat_mul(mat_from_words(xPrev + m), bPrev), mat_mul(mat_from_words(xCur + m), bCur)), bMidInv);
        for (int i = 0; i < 16; i++)
        {
            if (!std::isfinite(x[i])) return false;
            words[m + i] = std::bit_cast<uint32_t>((float)x[i]);
        }
    }
    std::copy(words.begin(), words.end(), out);
    return true;
}

bool rebased_current(const uint32_t* xCur, uint32_t n, const Mat4& bPrev, const Mat4& bCur, uint32_t* out)
{
    if (n == 0 || n % 16) return false;
    Mat4 bMidInv;
    if (!mat_affine_inverse(mat_between(bPrev, bCur), bMidInv)) return false;
    std::vector<uint32_t> words(n);
    for (uint32_t m = 0; m < n; m += 16)
    {
        Mat4 x = mat_mul(mat_mul(mat_from_words(xCur + m), bCur), bMidInv);
        for (int i = 0; i < 16; i++)
        {
            if (!std::isfinite(x[i])) return false;
            words[m + i] = std::bit_cast<uint32_t>((float)x[i]);
        }
    }
    std::copy(words.begin(), words.end(), out);
    return true;
}
bool shadow_snapped() { static const bool snap = std::getenv("WF_60FPS_SHADOWSNAP") != nullptr; return snap; }

constexpr uint32_t kViewBlockModelView = 0x027FB8BC, kViewBlockProjection = 0x027FBA00;
constexpr uint32_t kShadowCamera = 0x02790120, kShadowLookup = 0x02790158;
constexpr uint32_t kProjectionOffset = 0x1B0;

struct BaseWrite { uint32_t dest = 0, frame = 0, n = 0; bool havePrev = false, havePrevP = false; uint32_t prev[16], cur[16], prevP[16]; };
thread_local BaseWrite t_viewBlockL;

struct ViewBlockSeen { uint32_t ln = 0; bool hasP = false; uint32_t l[16], p[16]; };
std::mutex g_viewBlockLock;
uint32_t g_viewBlockFrame = ~0u;
std::unordered_map<uint32_t, ViewBlockSeen> g_viewBlocksCur, g_viewBlocksPrev;

void view_blocks_tick(uint32_t tick)
{
    if (g_viewBlockFrame == tick) return;
    if (g_viewBlockFrame + 1 == tick) g_viewBlocksPrev.swap(g_viewBlocksCur);
    else g_viewBlocksPrev.clear();
    g_viewBlocksCur.clear();
    g_viewBlockFrame = tick;
}

const ViewBlockSeen* match_view_block(const uint32_t* l, uint32_t n)
{
    const ViewBlockSeen* best = nullptr;
    double bestDist = 300.0;
    for (const auto& [dest, b] : g_viewBlocksPrev)
    {
        if (b.ln != n) continue;
        double d = 0;
        for (int i = 0; i < (int)n; i++)
        {
            double a = std::bit_cast<float>(l[i]), c = std::bit_cast<float>(b.l[i]);
            d += i % 4 == 3 ? std::abs(a - c) : std::abs(a - c) * 1000.0;
        }
        if (d < bestDist)
        {
            bestDist = d;
            best = &b;
        }
    }
    return best;
}

struct FrameHistory
{
    uint32_t tick = 0;
    bool prevValid = false;
    std::vector<uint32_t> cur, prev;
    void write(uint32_t t, const uint32_t* v, uint32_t n)
    {
        if (tick == t) return;
        prevValid = tick + 1 == t && cur.size() == n;
        prev.swap(cur);
        cur.assign(v, v + n);
        tick = t;
    }
    bool ready(uint32_t t, uint32_t n) const { return prevValid && tick == t && cur.size() == n; }
};
std::mutex g_shadowHistoryLock;
FrameHistory g_shadowCamera, g_shadowLookup;

void remember_base(BaseWrite& b, uint32_t dest, uint32_t frame, uint32_t n, bool havePrev, const uint32_t* prev, const uint32_t* cur)
{
    b.dest = dest;
    b.frame = frame;
    b.n = n;
    b.havePrev = havePrev && n <= 16;
    if (!b.havePrev) return;
    std::copy(prev, prev + n, b.prev);
    std::copy(cur, cur + n, b.cur);
}

bool shadow_unblended(uint32_t lr, uint32_t frame, uint32_t n, const uint32_t* cur, uint32_t* out)
{
    static uint32_t lastLogged = ~0u;
    if (lastLogged != frame)
    {
        lastLogged = frame;
        log_info("60fps: shadow factor {:08X} not rebased on frame {} (no history): unblended", lr, frame);
    }
    std::copy(cur, cur + n, out);
    return true;
}

std::atomic<uint32_t> g_cutTick{0};
constexpr double kCutDistance = 120.0;
constexpr double kCutTurn = 0.5;

bool camera_cut(const uint32_t* prev, const uint32_t* cur)
{
    auto f = [](const uint32_t* m, int i) { return (double)std::bit_cast<float>(m[i]); };
    double dx = f(cur, 3) - f(prev, 3), dy = f(cur, 7) - f(prev, 7), dz = f(cur, 11) - f(prev, 11);
    if (dx * dx + dy * dy + dz * dz > kCutDistance * kCutDistance) return true;
    double ax = f(prev, 2), ay = f(prev, 6), az = f(prev, 10), bx = f(cur, 2), by = f(cur, 6), bz = f(cur, 10);
    double la = std::sqrt(ax * ax + ay * ay + az * az), lb = std::sqrt(bx * bx + by * by + bz * bz);
    return la > 0 && lb > 0 && (ax * bx + ay * by + az * bz) / (la * lb) < kCutTurn;
}

std::atomic<uint32_t> g_rewriteFrame{0};
void rewrite_blended(uint32_t frame) { g_rewriteFrame = frame; }

void do_rewrite(uint32_t frame)
{
    uint32_t count = 0;
    for (Shard& shard : g_shards)
    {
        std::lock_guard lk(shard.lock);
        shard.fields.for_each([&](uint64_t dest, TransformField& f) {
            if (f.frame != frame || !f.blended || !f.transform || !f.wordsPerRow) return;
            uint32_t d = (uint32_t)dest;
            for (uint32_t r = 0, i = 0; r < f.rows; r++, d += f.rowStride * 4)
                for (uint32_t w = 0; w < f.wordsPerRow && i < f.cur.size(); w++, i++) *gptr<uint32_t>(d + 4 * w) = f.cur[i];
            f.blended = false;
            count++;
        });
    }
    static const bool cutLog = std::getenv("WF_60FPS_CUTLOG") != nullptr;
    if (cutLog) log_info("60fps: camera cut: {} transforms written again unblended", count);
}

bool shadow_halfway(uint32_t lr, uint32_t dest, uint32_t frame, uint32_t tick, bool logic, uint32_t n, bool havePrev,
                    const uint32_t* prev, const uint32_t* cur, uint32_t* out)
{
    static const bool off = std::getenv("WF_60FPS_NOSHADOWFIX") != nullptr;
    if (off) return false;
    switch (lr)
    {
    case kViewBlockModelView:
    {
        if (n != 12 && n != 16) return false;
        std::lock_guard lk(g_viewBlockLock);
        view_blocks_tick(tick);
        ViewBlockSeen& seen = g_viewBlocksCur[dest];
        seen.ln = n;
        std::copy(cur, cur + n, seen.l);
        if (!logic) return false;
        BaseWrite& b = t_viewBlockL;
        b.havePrevP = false;
        if (havePrev)
        {
            remember_base(b, dest, frame, n, havePrev, prev, cur);
            return false;
        }
        const ViewBlockSeen* m = match_view_block(cur, n);
        if (!m)
        {
            remember_base(b, dest, frame, n, false, prev, cur);
            return false;
        }
        remember_base(b, dest, frame, n, true, m->l, cur);
        if (m->hasP)
        {
            std::copy(m->p, m->p + 16, b.prevP);
            b.havePrevP = true;
        }
        for (uint32_t i = 0; i < n; i++) out[i] = lerp_word(m->l[i], cur[i]);
        return true;
    }
    case kViewBlockProjection:
    {
        if (n != 16) return false;
        {
            std::lock_guard lk(g_viewBlockLock);
            view_blocks_tick(tick);
            ViewBlockSeen& seen = g_viewBlocksCur[dest - kProjectionOffset];
            seen.hasP = true;
            std::copy(cur, cur + n, seen.p);
        }
        if (!logic) return false;
        const BaseWrite& l = t_viewBlockL;
        if (!l.havePrev || l.frame != frame || l.dest + kProjectionOffset != dest) return false;
        const uint32_t* prevP = havePrev ? prev : l.havePrevP ? l.prevP : nullptr;
        if (!prevP || std::equal(prevP, prevP + n, cur)) return false;
        if (shadow_snapped()) return rebased_current(cur, n, mat_from_words(l.prev, l.n), mat_from_words(l.cur, l.n), out);
        return rebased_halfway(prevP, cur, n, mat_from_words(l.prev, l.n), mat_from_words(l.cur, l.n), out);
    }
    case kShadowCamera:
    {
        if (n != 16) return false;
        std::lock_guard lk(g_shadowHistoryLock);
        FrameHistory& h = g_shadowCamera;
        h.write(tick, cur, n);
        Mat4 vPrev, vCur, cMid;
        if (!logic) return false;
        if (h.ready(tick, n) && camera_cut(h.prev.data(), h.cur.data()) && g_cutTick.exchange(tick) != tick)
        {
            static const bool cutLog = std::getenv("WF_60FPS_CUTLOG") != nullptr;
            if (cutLog) log_info("60fps: camera cut on frame {}", frame);
            rewrite_blended(frame);
        }
        if (g_cutTick == tick) return false;
        if (!h.ready(tick, n) || !mat_affine_inverse(mat_from_words(h.prev.data()), vPrev) ||
            !mat_affine_inverse(mat_from_words(h.cur.data()), vCur) || !mat_affine_inverse(mat_between(vPrev, vCur), cMid))
            return shadow_unblended(lr, frame, n, cur, out);
        for (int i = 0; i < 16; i++) out[i] = std::bit_cast<uint32_t>((float)cMid[i]);
        return true;
    }
    case kShadowLookup:
    {
        std::lock_guard lk(g_shadowHistoryLock);
        FrameHistory& h = g_shadowLookup;
        h.write(tick, cur, n);
        const FrameHistory& c = g_shadowCamera;
        Mat4 vPrev, vCur;
        if (!logic || g_cutTick == tick) return false;
        return (h.ready(tick, n) && c.ready(tick, 16) && mat_affine_inverse(mat_from_words(c.prev.data()), vPrev) &&
                mat_affine_inverse(mat_from_words(c.cur.data()), vCur) &&
                (shadow_snapped() ? rebased_current(cur, n, vPrev, vCur, out) : rebased_halfway(h.prev.data(), cur, n, vPrev, vCur, out))) ||
               shadow_unblended(lr, frame, n, cur, out);
    }
    }
    return false;
}

uint32_t lerp_param(uint32_t prev, uint32_t value)
{
    if (prev == value) return value;
    float a = std::bit_cast<float>(prev), b = std::bit_cast<float>(value);
    if (!lerpable(a) || !lerpable(b)) return value;
    if (std::abs(b - a) > 0.25f * std::max({std::abs(a), std::abs(b), 1.0f})) return value;
    return std::bit_cast<uint32_t>(a + (b - a) * g_step.load(std::memory_order_relaxed));
}

struct ParticleField { uint16_t offset; uint8_t kind; };
constexpr ParticleField kParticleFields[] = {
    {0x28, 0}, {0x2C, 0}, {0x30, 0},
    {0x8C, 0}, {0x90, 0}, {0x94, 0},
    {0x98, 0}, {0x9C, 0}, {0xA0, 0},
    {0xAC, 0},
    {0xB8, 1}, {0xBC, 1},
    {0xC0, 2},
};
constexpr size_t kParticleWords = std::size(kParticleFields);
constexpr uint32_t kParticleAge = 0x78;
struct ParticleState { float age; uint32_t life; uint32_t words[kParticleWords]; };

std::mutex g_ptclLock;
std::vector<uint32_t> g_ptclTick;
std::unordered_map<uint32_t, ParticleState> g_ptclPrev;
std::vector<std::pair<uint32_t, ParticleState>> g_ptclRestore;
std::vector<std::tuple<uint32_t, ParticleState, ParticleState>> g_ptclPairs;

ParticleState read_particle(uint32_t p)
{
    ParticleState st;
    st.age = std::bit_cast<float>(rd32(p + kParticleAge));
    st.life = rd32(p + kParticleAge + 4);
    for (size_t i = 0; i < kParticleWords; i++) st.words[i] = rd32(p + kParticleFields[i].offset);
    return st;
}

uint32_t lerp_particle_word(uint8_t kind, uint32_t a, uint32_t b)
{
    if (a == b) return b;
    if (kind == 0) return lerp_word(a, b);
    float t = g_step.load(std::memory_order_relaxed);
    if (kind == 1)
    {
        uint32_t out = 0;
        for (int sh = 0; sh < 32; sh += 8)
        {
            float ca = (float)((a >> sh) & 0xFF), cb = (float)((b >> sh) & 0xFF);
            out |= ((uint32_t)std::lround(ca + (cb - ca) * t) & 0xFF) << sh;
        }
        return out;
    }
    int16_t d = (int16_t)((b >> 16) - (a >> 16));
    uint16_t mid = (uint16_t)((a >> 16) + (int)std::lround(d * t));
    return ((uint32_t)mid << 16) | (b & 0xFFFF);
}

void restore_particles()
{
    std::lock_guard lk(g_ptclLock);
    for (auto& [p, st] : g_ptclRestore)
        for (size_t i = 0; i < kParticleWords; i++) wr32(p + kParticleFields[i].offset, st.words[i]);
    g_ptclRestore.clear();
}

void particles_tick()
{
    static const bool off = std::getenv("WF_60FPS_NOPARTICLES") != nullptr;
    std::lock_guard lk(g_ptclLock);
    g_ptclPairs.clear();
    if (g_ptclTick.empty()) return;
    std::sort(g_ptclTick.begin(), g_ptclTick.end());
    g_ptclTick.erase(std::unique(g_ptclTick.begin(), g_ptclTick.end()), g_ptclTick.end());
    std::unordered_map<uint32_t, ParticleState> next;
    next.reserve(g_ptclTick.size());
    for (uint32_t p : g_ptclTick)
    {
        ParticleState cur = read_particle(p);
        next[p] = cur;
        auto it = g_ptclPrev.find(p);
        if (off || it == g_ptclPrev.end()) continue;
        const ParticleState& prev = it->second;
        float step = cur.age - prev.age;
        static std::map<int, uint32_t> s_steps;
        static const bool stepLog = std::getenv("WF_60FPS_PTCLSTEPS") != nullptr;
        if (stepLog && s_steps[(int)std::lround(step * 4)]++ > 2000)
        {
            std::string out;
            for (auto& [k, v] : s_steps) out += std::format(" {:.2f}:{}", k / 4.0, v);
            log_info("particle age steps per tick:{}", out);
            s_steps.clear();
        }
        if (step <= 0.0f || step > 2.0f || cur.life != prev.life) continue;
        g_ptclPairs.push_back({p, prev, cur});
    }
    {
        static uint32_t s_ticks = 0;
        if (++s_ticks % 150 == 0)
            log_info("high fps: {} particles calculated, {} interpolated", g_ptclTick.size(), g_ptclPairs.size());
    }
    g_ptclPrev.swap(next);
    g_ptclTick.clear();
}

void interpolate_particles()
{
    std::lock_guard lk(g_ptclLock);
    for (const auto& [p, prev, cur] : g_ptclPairs)
    {
        for (size_t i = 0; i < kParticleWords; i++)
            wr32(p + kParticleFields[i].offset, lerp_particle_word(kParticleFields[i].kind, prev.words[i], cur.words[i]));
        g_ptclRestore.push_back({p, cur});
    }
}

struct EffectArray
{
    const char* name;
    uint32_t packetPtr;
    uint32_t array, stride, maxCount;
    uint32_t countAddr;
    uint16_t first, words;
    uint16_t posA, posB;
    float jump;
    std::vector<uint16_t> packetFloats;
};
constexpr uint32_t kEnvLight = 0x10475A68;
constexpr uint16_t kNoPos = 0xFFFF;
const EffectArray kEffectArrays[] = {
    {"waves", kEnvLight + 0xAA0, 0xA0, 0x38, 300, kEnvLight + 0x9F8, 0x00, 12, 0x00, 0x0C, 300.0f, {0x4240, 0x4244}},
    {"clouds", kEnvLight + 0xA94, 0xA4, 0x2C, 100, 0, 0x04, 10, 0x00, kNoPos, 1000.0f, {}},
};
struct EffectState
{
    uint32_t packet = 0, count = 0;
    std::vector<uint32_t> words;
};
EffectState g_effPrev[std::size(kEffectArrays)];
std::vector<std::pair<uint32_t, uint32_t>> g_effRestore;
struct EffectPair { uint32_t addr, prev, cur; };
std::vector<EffectPair> g_effPairs;

void restore_effects()
{
    for (auto& [a, v] : g_effRestore) wr32(a, v);
    g_effRestore.clear();
}

float vec_dist(const uint32_t* a, const uint32_t* b, uint16_t posA, uint16_t posB)
{
    float d2 = 0;
    for (int c = 0; c < 3; c++)
    {
        float pa = std::bit_cast<float>(a[posA / 4 + c]) + (posB == kNoPos ? 0.0f : std::bit_cast<float>(a[posB / 4 + c]));
        float pb = std::bit_cast<float>(b[posA / 4 + c]) + (posB == kNoPos ? 0.0f : std::bit_cast<float>(b[posB / 4 + c]));
        d2 += (pa - pb) * (pa - pb);
    }
    return std::sqrt(d2);
}

void interpolate_effects()
{
    for (const EffectPair& p : g_effPairs)
    {
        g_effRestore.push_back({p.addr, p.cur});
        wr32(p.addr, lerp_word(p.prev, p.cur));
    }
}

void effects_tick()
{
    static const bool off = std::getenv("WF_60FPS_NOEFFECTS") != nullptr;
    g_effPairs.clear();
    for (size_t e = 0; e < std::size(kEffectArrays); e++)
    {
        const EffectArray& d = kEffectArrays[e];
        EffectState cur;
        cur.packet = rd32(d.packetPtr);
        int16_t count = d.countAddr ? (int16_t)rd16(d.countAddr) : (int16_t)d.maxCount;
        if (!cur.packet || count <= 0 || (uint32_t)count > d.maxCount || !is_mapped(cur.packet, d.array + d.stride * d.maxCount))
        {
            g_effPrev[e] = {};
            continue;
        }
        cur.count = (uint32_t)count;
        cur.words.resize(cur.count * d.words + d.packetFloats.size());
        for (uint32_t i = 0; i < cur.count; i++)
            for (uint32_t w = 0; w < d.words; w++) cur.words[i * d.words + w] = rd32(cur.packet + d.array + i * d.stride + d.first + w * 4);
        for (size_t k = 0; k < d.packetFloats.size(); k++) cur.words[cur.count * d.words + k] = rd32(cur.packet + d.packetFloats[k]);
        EffectState& prev = g_effPrev[e];
        if (!off && prev.packet == cur.packet && prev.count == cur.count)
        {
            for (uint32_t i = 0; i < cur.count; i++)
            {
                const uint32_t* a = &prev.words[i * d.words];
                const uint32_t* b = &cur.words[i * d.words];
                if (vec_dist(a, b, d.posA, d.posB) > d.jump) continue;
                for (uint32_t w = 0; w < d.words; w++)
                {
                    if (a[w] == b[w]) continue;
                    g_effPairs.push_back({cur.packet + d.array + i * d.stride + d.first + w * 4, a[w], b[w]});
                }
            }
            for (size_t k = 0; k < d.packetFloats.size(); k++)
            {
                uint32_t a = prev.words[prev.count * d.words + k], b = cur.words[cur.count * d.words + k];
                if (a == b) continue;
                g_effPairs.push_back({cur.packet + d.packetFloats[k], a, b});
            }
        }
        prev = std::move(cur);
    }
}

bool caller_excluded(uint32_t lr, bool transform)
{
    static const std::vector<uint32_t> skip = [] {
        std::vector<uint32_t> v;
        if (const char* e = std::getenv("WF_60FPS_SKIPLR"))
            for (const char* p = e; *p;)
            {
                char* end;
                v.push_back((uint32_t)strtoul(p, &end, 16));
                p = *end ? end + 1 : end;
            }
        return v;
    }();
    static const bool log = std::getenv("WF_60FPS_LOGLR") != nullptr;
    if (log)
    {
        static std::mutex lock;
        static std::set<uint64_t> seen;
        std::lock_guard lk(lock);
        if (seen.insert(((uint64_t)transform << 32) | lr).second)
            log_info("interpolated uniform caller {:08X} {}", lr, transform ? "transform" : "param");
    }
    return std::find(skip.begin(), skip.end(), lr) != skip.end();
}

void jump_log(uint32_t lr, uint32_t field, const std::vector<uint32_t>& prev, const std::vector<uint32_t>& cur)
{
    float worst = 0;
    for (size_t i = 0; i < cur.size(); i++)
    {
        float a = std::bit_cast<float>(prev[i]), b = std::bit_cast<float>(cur[i]);
        if (!lerpable(a) || !lerpable(b)) continue;
        worst = std::max(worst, std::abs(b - a) / std::max({std::abs(a), std::abs(b), 1.0f}));
    }
    static std::mutex lock;
    static std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>> counts;
    static uint64_t lastLog = 0;
    std::lock_guard lk(lock);
    auto& c = counts[{lr, field}];
    c.second++;
    if (worst > 0.2f) c.first++;
    uint64_t now = os::ticks_since_boot();
    if (!lastLog) lastLog = now;
    if (now - lastLog > 5 * os::kTimerClock)
    {
        lastLog = now;
        std::string out = "interpolated fields with big jumps (big/all, caller, field):";
        for (auto& [k, v] : counts)
            if (v.first) out += std::format("\n  {:6}/{:<6} {:08X} f{}", v.first, v.second, k.first, k.second);
        log_info("{}", out);
        counts.clear();
    }
}

void field_stats(uint32_t lr, uint32_t type, uint32_t wordsPerRow, int32_t rows, uint32_t dest, uint32_t src)
{
    static std::mutex lock;
    struct Last { std::vector<uint32_t> words; };
    static std::unordered_map<uint32_t, Last> last;
    struct Count { uint32_t writes = 0, changed = 0; std::string sample; };
    static std::map<std::tuple<uint32_t, uint32_t, int32_t>, Count> counts;
    static uint64_t lastLog = 0;
    std::lock_guard lk(lock);
    uint32_t n = wordsPerRow * (uint32_t)rows;
    std::vector<uint32_t> words(n);
    for (uint32_t i = 0; i < n; i++) words[i] = rd32(src + 4 * i);
    Count& c = counts[{lr, type, rows}];
    c.writes++;
    Last& l = last[dest];
    if (l.words.size() == n && l.words != words)
    {
        c.changed++;
        if (c.sample.empty())
            for (uint32_t i = 0; i < std::min<uint32_t>(n, 8); i++)
                c.sample += std::format(" {:.4g}>{:.4g}", std::bit_cast<float>(l.words[i]), std::bit_cast<float>(words[i]));
    }
    l.words = std::move(words);
    uint64_t now = os::ticks_since_boot();
    if (!lastLog) lastLog = now;
    if (now - lastLog > 5 * os::kTimerClock)
    {
        lastLog = now;
        std::vector<std::pair<uint32_t, std::tuple<uint32_t, uint32_t, int32_t>>> v;
        for (auto& [k, cnt] : counts) if (cnt.changed) v.push_back({cnt.changed, k});
        std::sort(v.rbegin(), v.rend());
        std::string out = "copied uniform fields that changed (changed/writes, caller, type, words x rows):";
        for (size_t i = 0; i < std::min<size_t>(25, v.size()); i++)
        {
            auto [lr, ty, r] = v[i].second;
            Count& cc = counts[v[i].second];
            out += std::format("\n  {:6}/{:<6} {:08X} t{} {}x{}{}", cc.changed, cc.writes, lr, ty,
                               rd8(kUniformTypeTable + ty * 3), r, cc.sample);
        }
        log_info("{}", out);
        counts.clear();
    }
}

}
}

using namespace wf;

namespace wf::game {
void fov_adjust(uint32_t lr, uint32_t* words, uint32_t rowStride)
{
    if (lr != 0x027FBA00) return;
    float k = wf_render_settings().fovScale;
    if (k == 1.0f) return;
    uint32_t* row3 = words + 3 * rowStride;
    if (std::bit_cast<float>(row3[2]) != -1.0f || row3[3] != 0 || row3[0] != 0 || row3[1] != 0) return;
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 3; c++) words[r * rowStride + c] = std::bit_cast<uint32_t>(std::bit_cast<float>(words[r * rowStride + c]) * k);
}

bool paused_now()
{
    return ui::menu::is_open() || (wf_render_settings().pauseInBackground && !window::focused());
}
}

namespace wf::game {
void run_deferred_frame_ctrls(PPCContext& ctx);
}

// GX2SetSwapInterval: the game asks for 2 (30 Hz); the interpolated loop runs the frame loop every vsync
std::atomic<uint32_t> g_requestedSwapInterval{2};
uint32_t wf_game_swap_interval(uint32_t requested)
{
    g_requestedSwapInterval = requested;
    return (game::enabled() && requested == 2) ? 1 : requested;
}

void wf_set_swap_interval(uint32_t interval);
void wf_render_reset_vsync();
namespace wf::game {
int g_rateApplied = 0;  // the frame rate the emulated vsync was last set for (0: set it again)
// the frame rate setting moved between 30 and a higher rate while playing
void switch_loop(bool high)
{
    if (!high)
    {
        restore_particles();
        restore_effects();
        g_step = 1.0f;
        g_logicFrame = true;
        g_fastForward = false;
        wf_render_fast_forward(false);
        wf_render_reset_vsync();
        g_rateApplied = 0;
    }
    wf_set_swap_interval(wf_game_swap_interval(g_requestedSwapInterval));
    log_info("frame rate: {}", high ? "interpolated loop on" : "original 30 fps loop");
}
}

PPC_FUNC(hook_task_calc)
{
    mp::new_frame();
    game::g_paused = game::paused_now();
    {
        bool wasHigh = game::enabled();
        game::follow_fps_setting();
        if (wasHigh != game::enabled()) game::switch_loop(game::enabled());
    }
    if (!game::enabled())
    {
        game::g_logicFrame = !game::g_paused;
        if (!game::g_paused)
        {
            cheats::apply_frame();
            playtest::tick();
            mp::tick();
            mods::tick();
        }
        sub_02746790_orig(ctx, base);
        if (std::getenv("WF_60FPS_PTCLSTEPS")) game::particles_tick();
        return;
    }
    uint32_t frame = ++game::g_frame;
    if (const char* e = std::getenv("WF_60FPS_COUNTERFIND"))
    {
        static const uint32_t at = (uint32_t)std::atoi(e);
        static std::vector<std::vector<uint32_t>> snaps;
        if (frame >= at && snaps.size() < 6)
        {
            snaps.emplace_back(gptr<uint32_t>(0x10000000), gptr<uint32_t>(0x10500000));
            if (snaps.size() == 6)
            {
                std::string out = "counters (+1 every frame for 5 frames):";
                for (size_t i = 0; i < snaps[0].size(); i++)
                {
                    bool inc = true;
                    for (size_t k = 1; k < snaps.size() && inc; k++) inc = bswap(snaps[k][i]) == bswap(snaps[k - 1][i]) + 1;
                    if (inc) out += std::format(" {:08X}={}", 0x10000000 + (uint32_t)i * 4, bswap(snaps[5][i]));
                }
                log_info("{}", out);
            }
        }
    }
    static const bool lagLog = std::getenv("WF_60FPS_LAGLOG") != nullptr;
    if (lagLog && frame % 30 == 0) log_info("lag: game frame {} gpu frames presented {}", frame, gpu::frame_count());
    const int fps = game::target_fps();
    int& s_fps = game::g_rateApplied;
    static uint64_t s_simFrames = 0, s_lastTick = 0;
    static uint64_t s_lastNs = 0;
    static double s_realFrames = 0;
    if (fps != s_fps)
    {
        s_simFrames = s_lastTick * (uint64_t)fps / 30;  // the clock goes on from the last tick at the new rate
        s_lastNs = 0;
        s_fps = fps;
        wf_render_set_vsync_rate(fps);
        log_info("high fps: {} frames per second, game logic at 30 ticks per second", fps);
    }
    bool fastForward = input::fast_forward() && !game::g_paused;
    game::g_fastForward = fastForward;
    wf_render_fast_forward(fastForward);
    bool logic = false;
    float step = 1.0f;
    if (fastForward)
    {
        logic = true;
        s_simFrames = (s_lastTick + 1) * (uint64_t)fps / 30;
    }
    else if (!game::g_paused)
    {
        static const bool fixedClock = std::getenv("WF_SHOT") || std::getenv("WF_FPS_FIXED");
        uint64_t nowNs = os::now_ns();
        if (s_lastNs) s_realFrames += std::min(0.25, (nowNs - s_lastNs) / 1e9) * fps;
        else s_realFrames = (double)s_simFrames;
        s_lastNs = nowNs;
        if (!fixedClock)
        {
            if (s_realFrames > (double)s_simFrames + 1.5) s_simFrames = (uint64_t)s_realFrames - 1;
            else if (s_realFrames < (double)s_simFrames - 3.0) s_realFrames = (double)s_simFrames;
        }
        // behind the last tick (the clock was last counted at another rate): go on from that tick
        if (s_lastTick && (s_simFrames + 1) * 30 <= (s_lastTick - 1) * (uint64_t)fps) s_simFrames = s_lastTick * (uint64_t)fps / 30;
        uint64_t i = ++s_simFrames, num = i * 30;
        uint64_t tick = (num + fps - 1) / fps;
        logic = tick > s_lastTick;
        step = (float)(num - (tick - 1) * fps) / (float)fps;
        if (logic && tick > s_lastTick + 1) s_simFrames = (s_lastTick + 1) * (uint64_t)fps / 30;
    }
    if (game::g_paused || fastForward) s_lastNs = 0;
    if (logic) s_lastTick++;
    game::g_step = step;
    if (logic)
    {
        cheats::apply_frame();
        playtest::tick();
        mp::tick();
        mods::tick();
    }
    game::g_logicFrame = logic;
    if (logic) game::g_tick++;
    static const bool noPrune = std::getenv("WF_60FPS_NOPRUNE") != nullptr;
    if (logic && !noPrune) game::prune_history(game::g_tick, frame);
    static const bool frameLog = std::getenv("WF_60FPS_FRAMELOG") != nullptr;
    if (frameLog)
        log_info("high fps frame {} {} tick {} step {:.3f} (presented {})", frame, logic ? "logic" : "drawing", game::g_tick.load(), step,
                 gpu::frame_count());
    static uint64_t s_windowStart = 0;
    static uint32_t s_windowFrame = 0, s_windowPresents = 0;
    uint64_t now = os::now_ns();
    if (!s_windowStart) { s_windowStart = now; s_windowFrame = frame; s_windowPresents = gpu::frame_count(); }
    else if (now - s_windowStart >= 5000000000ull)
    {
        double secs = (now - s_windowStart) / 1e9;
        static uint32_t s_windowTick = 0;
        log_info("high fps: {:.1f} frames/s ({:.1f} logic ticks/s), {:.1f} images presented/s", (frame - s_windowFrame) / secs,
                 (game::g_tick - s_windowTick) / secs, (gpu::frame_count() - s_windowPresents) / secs);
        s_windowTick = game::g_tick;
        for (int k = 1; k >= 0; k--)
        {
            game::FrameStats& st = game::g_stats[k];
            if (st.count)
                log_info("  {} frames: work avg {:.1f} ms max {:.1f} ms, vsync wait avg {:.1f} ms, {} of {} over a frame",
                         k ? "logic" : "drawing", st.workSum / st.count / 1e6, st.workMax / 1e6, st.waitSum / st.count / 1e6, st.over, st.count);
            st = {};
        }
        {
            auto& u = game::g_uboStats;
            auto [fields, sources] = game::history_sizes();
            log_info("  uniform fields on logic frames (interpolated / other source / per frame / new): transforms {}/{}/{}/{}, "
                     "parameters {}/{}/{}/{}; history {} fields, {} sources", u[0][0].exchange(0), u[0][1].exchange(0), u[0][2].exchange(0),
                     u[0][3].exchange(0), u[1][0].exchange(0), u[1][1].exchange(0), u[1][2].exchange(0), u[1][3].exchange(0), fields, sources);
        }
        if (std::getenv("WF_60FPS_THREADLOG"))
            log_info("  matrix work: {} calls, {} bursts split across a frame boundary", game::g_burstCalls.exchange(0), game::g_burstSplit.exchange(0));
        s_windowStart = now; s_windowFrame = frame; s_windowPresents = gpu::frame_count();
    }
    game::g_frameStartNs = now;
    game::restore_particles();
    game::restore_effects();
    if (logic) game::run_deferred_frame_ctrls(ctx);
    static const uint32_t tickDiffAt = std::getenv("WF_60FPS_TICKDIFF") ? (uint32_t)std::atoi(std::getenv("WF_60FPS_TICKDIFF")) : 0;
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> snap;
    bool tickDiff = tickDiffAt && logic && frame >= tickDiffAt && snap.empty();
    static bool s_tickDiffDone = false;
    if (tickDiff && !s_tickDiffDone)
    {
        for (uint32_t a = 0x10000000; a < 0x50000000;)
        {
            MEMORY_BASIC_INFORMATION mi{};
            if (!VirtualQuery(gptr<uint8_t>(a), &mi, sizeof mi)) break;
            uint32_t end = (uint32_t)std::min<uint64_t>(0x50000000ull, a + (uint64_t)((uint8_t*)mi.BaseAddress + mi.RegionSize - gptr<uint8_t>(a)));
            if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
                snap.push_back({a, std::vector<uint32_t>(gptr<uint32_t>(a), gptr<uint32_t>(a) + (end - a) / 4)});
            a = end;
        }
    }
    sub_02746790_orig(ctx, base);
    if (tickDiff && !s_tickDiffDone)
    {
        s_tickDiffDone = true;
        std::string out = std::format("logic frame {} changed:", frame);
        for (auto& [a, words] : snap)
        {
            for (size_t i = 0; i < words.size();)
            {
                if (*gptr<uint32_t>(a + (uint32_t)i * 4) == words[i]) { i++; continue; }
                size_t j = i, gap = 0;
                while (j < words.size() && gap < 8) { if (*gptr<uint32_t>(a + (uint32_t)j * 4) != words[j]) gap = 0; else gap++; j++; }
                size_t len = j - gap - i;
                if (len >= 24)
                {
                    out += std::format("\n  {:08X} {:6} words:", a + (uint32_t)i * 4, len);
                    for (size_t k = i; k < i + std::min<size_t>(len, 15); k++) out += std::format(" {:.6g}", std::bit_cast<float>(bswap(*gptr<uint32_t>(a + (uint32_t)k * 4))));
                }
                i = j;
            }
        }
        log_info("{}", out);
    }
    if (logic)
    {
        game::particles_tick();
        game::effects_tick();
    }
    if (game::interpolating())
    {
        game::interpolate_particles();
        game::interpolate_effects();
    }
}

namespace wf::game {
thread_local bool t_inDraw = false;
std::mutex g_deferLock;
std::vector<uint32_t> g_deferredFrameCtrls;
void run_deferred_frame_ctrls(PPCContext& ctx)
{
    std::vector<uint32_t> list;
    {
        std::lock_guard lk(g_deferLock);
        list.swap(g_deferredFrameCtrls);
    }
    if (list.empty()) return;
    uint32_t saved[11];
    for (int i = 0; i < 11; i++) saved[i] = ctx.r[3 + i];
    for (uint32_t c : list)
        if (is_mapped(c, 0x10)) hle::call_guest(ctx, 0x027F2FC4, {c});
    for (int i = 0; i < 11; i++) ctx.r[3 + i] = saved[i];
}
}

PPC_FUNC(hook_frame_ctrl_update)
{
    if (game::enabled() && game::t_inDraw)
    {
        if (game::g_logicFrame)
        {
            std::lock_guard lk(game::g_deferLock);
            auto& v = game::g_deferredFrameCtrls;
            if (std::find(v.begin(), v.end(), ctx.r[3]) == v.end()) v.push_back(ctx.r[3]);
        }
        return;
    }
    if (game::enabled() && !game::g_logicFrame) return;
    sub_027F2FC4_orig(ctx, base);
}

PPC_FUNC(hook_tick_vibration)
{
    if (game::enabled() && !game::g_logicFrame) return;
    sub_025CBB90_orig(ctx, base);
}

PPC_FUNC(hook_tick_magma)
{
    if (game::enabled() && !game::g_logicFrame) return;
    sub_02524CA0_orig(ctx, base);
}

PPC_FUNC(hook_tick_grass)
{
    if (game::enabled() && !game::g_logicFrame) return;
    sub_02524DA0_orig(ctx, base);
}

PPC_FUNC(hook_tick_tree)
{
    if (game::enabled() && !game::g_logicFrame) return;
    sub_02524EA0_orig(ctx, base);
}

PPC_FUNC(hook_tick_wood)
{
    if (game::enabled() && !game::g_logicFrame) return;
    sub_02524FA0_orig(ctx, base);
}

PPC_FUNC(hook_tick_flower)
{
    if (game::enabled() && !game::g_logicFrame) return;
    sub_025250A0_orig(ctx, base);
}

PPC_FUNC(hook_tick_poison)
{
    if (game::enabled() && !game::g_logicFrame) return;
    sub_0256C8FC_orig(ctx, base);
}

PPC_FUNC(hook_tick_collision)
{
    if (game::enabled() && !game::g_logicFrame) return;
    sub_02518798_orig(ctx, base);
}

// The draw phase uses the game's random numbers (cM_rnd, seeds at 101FF9D4): lantern and torch light flicker is rolled
// as each actor is drawn. A drawing-only frame replays the numbers of its tick's draw, so lights flicker at the game's
// rate, and the random sequence the game logic sees doesn't depend on how many frames were drawn.
namespace wf::game {
constexpr uint32_t kRandomSeeds = 0x101FF9D4;
std::array<uint32_t, 3> g_rngDrawStart{}, g_rngDrawEnd{};
bool g_rngValid = false;
std::array<uint32_t, 3> rng_get() { return {rd32(kRandomSeeds), rd32(kRandomSeeds + 4), rd32(kRandomSeeds + 8)}; }
void rng_set(const std::array<uint32_t, 3>& v)
{
    for (int i = 0; i < 3; i++) wr32(kRandomSeeds + 4 * i, v[i]);
}
}

PPC_FUNC(hook_draw_handler)
{
    bool was = game::t_inDraw;
    game::t_inDraw = true;
    static const bool noRng = std::getenv("WF_60FPS_NORNG") != nullptr;  // test switch
    bool replay = game::enabled() && !was && !noRng;
    if (replay && game::g_logicFrame) game::g_rngDrawStart = game::rng_get();
    else if (replay && game::g_rngValid) game::rng_set(game::g_rngDrawStart);
    sub_025DE37C_orig(ctx, base);
    if (replay && game::g_logicFrame)
    {
        game::g_rngDrawEnd = game::rng_get();
        game::g_rngValid = true;
    }
    static const bool rngLog = std::getenv("WF_60FPS_RNGLOG") != nullptr;
    static int s_rngLogs = 0;
    if (rngLog && game::g_frame > 2340 && s_rngLogs++ < 30)
        log_info("draw phase frame {} {} nested {} start {:08X} end {:08X} now {:08X}", game::g_frame.load(), game::g_logicFrame ? "logic" : "draw", was,
                 game::g_rngDrawStart[0], game::g_rngDrawEnd[0], rd32(game::kRandomSeeds));
    else if (replay && game::g_rngValid)
        game::rng_set(game::g_rngDrawEnd);
    game::t_inDraw = was;
}

PPC_FUNC(hook_particle_manager_calc)
{
    if (game::enabled() && !game::g_logicFrame) return;
    sub_0282167C_orig(ctx, base);
}

PPC_FUNC(hook_particle_position)
{
    uint32_t particle = ctx.r[3];
    sub_0282685C_orig(ctx, base);
    static const bool stepLog = std::getenv("WF_60FPS_PTCLSTEPS") != nullptr;
    if (stepLog)
    {
        static std::atomic<uint32_t> s_calls[2];
        static std::atomic<uint32_t> s_lastFrame{0};
        s_calls[game::g_logicFrame ? 1 : 0]++;
        uint32_t f = game::g_frame;
        if (f / 300 != s_lastFrame.exchange(f) / 300)
            log_info("particle calcs: {} on logic frames, {} on drawing frames", s_calls[1].exchange(0), s_calls[0].exchange(0));
    }
    if (!game::enabled() && !stepLog) return;
    std::lock_guard lk(game::g_ptclLock);
    game::g_ptclTick.push_back(particle);
    static uint32_t s_logged = 0;
    if (s_logged < 4 && game::g_frame > 2000)
    {
        s_logged++;
        log_info("particle calc {:08X} on {} frame {}, age {}", particle, game::g_logicFrame ? "logic" : "drawing",
                 (uint32_t)game::g_frame, std::bit_cast<float>(rd32(particle + game::kParticleAge)));
    }
}

// The HD menus and HUD (the layout code around 02870000) upload their pane matrices with GX2SetVertexUniformReg rather
// than through the uniform block writer. Each caller and register keeps the list of values written on the last frame of
// the previous tick; a write is paired with the entry at its position in that list, or a neighbour when a pane appearing
// or going has shifted the order (it shifts even between the frames of a tick), and blended between ticks like the
// rest. Values that change within a tick (animated per drawn frame), and moves too large to tell apart from a different
// pane, are written as they are. Returns false to write the values unchanged.
bool wf_lerp_uniform_reg(uint32_t lr, uint32_t offset, uint32_t count, const uint32_t* bigEndian, uint32_t* out)
{
    using namespace wf;
    static const bool off = std::getenv("WF_60FPS_NOUIREGS") != nullptr;
    if (off || !wf_render_settings().smoothUi || !game::enabled() || lr < 0x02870000 || lr >= 0x02880000 || count == 0 || count > 16) return false;
    using Values = std::array<uint32_t, 16>;
    struct Site
    {
        uint32_t tick = 0, frame = 0, firstFrame = 0, n = 0;
        std::vector<Values> prev, cur, first;
        std::vector<bool> used;
    };
    static std::mutex lock;
    static std::unordered_map<uint64_t, Site> sites;
    static uint32_t pruneTick = 0;
    const uint32_t frame = game::g_frame, tick = game::g_tick;
    Values v{};
    for (uint32_t i = 0; i < count; i++) v[i] = __builtin_bswap32(bigEndian[i]);
    std::lock_guard lk(lock);
    if (tick - pruneTick > 30)
    {
        pruneTick = tick;
        std::erase_if(sites, [&](const auto& e) { return tick - e.second.tick > 30; });
    }
    Site& s = sites[((uint64_t)lr << 32) | (offset << 8) | count];
    if (s.tick != tick)
    {
        if (s.tick + 1 == tick && s.n == count) s.prev.swap(s.cur);
        else s.prev.clear();
        s.cur.clear();
        s.first.clear();
        s.tick = tick;
        s.frame = s.firstFrame = frame;
        s.n = count;
        s.used.assign(s.prev.size(), false);
    }
    else if (s.frame != frame)
    {
        s.frame = frame;
        s.cur.clear();
        s.used.assign(s.prev.size(), false);
    }
    const size_t k = s.cur.size();
    s.cur.push_back(v);
    bool perFrame = false;
    if (frame == s.firstFrame) s.first.push_back(v);
    else
    {
        perFrame = true;
        for (long d = -4; d <= 4 && perFrame; d++)
            if (long j = (long)k + d; j >= 0 && j < (long)s.first.size() && s.first[(size_t)j] == v) perFrame = false;
    }
    static uint32_t logFirst = 0, logLast = 0;
    static bool logInit = false;
    if (!logInit)
    {
        logInit = true;
        if (const char* e = std::getenv("WF_60FPS_UIREGLOG")) std::sscanf(e, "%u-%u", &logFirst, &logLast);
    }
    auto note = [&](const char* what, long j) {
        if (!logLast || frame < logFirst || frame > logLast) return;
        std::string vals;
        for (uint32_t i = 0; i < count; i++)
            vals += std::format(" {:.4g}/{:.4g}", j >= 0 ? std::bit_cast<float>(s.prev[(size_t)j][i]) : 0.0f, std::bit_cast<float>(v[i]));
        log_info("ui reg frame {} lr {:08X} off {} n{} #{} {} {}:{}", frame, lr, offset, count, k, what, j, vals);
    };
    if (!game::interpolating()) return false;
    if (perFrame) return note("per-frame", -1), false;
    if (s.prev.empty()) return note("no-history", -1), false;
    for (long d = -8; d <= 8; d++)
        if (long j = (long)k + d; j >= 0 && j < (long)s.prev.size() && !s.used[(size_t)j] && s.prev[(size_t)j] == v)
        {
            s.used[(size_t)j] = true;
            return false;  // unchanged since the last tick
        }
    auto distance = [&](const Values& a) {
        float worst = 0;
        for (uint32_t i = 0; i < count; i++)
        {
            float x = std::bit_cast<float>(a[i]), y = std::bit_cast<float>(v[i]);
            if (!game::lerpable(x) || !game::lerpable(y)) return -1.0f;
            float mag = std::max(std::abs(x), std::abs(y));
            float allowed = 0.15f * mag + (mag > 4.0f ? 16.0f : 0.2f);
            float d = std::abs(y - x) / allowed;
            if (d > 1.0f) return -1.0f;
            worst = std::max(worst, d);
        }
        return worst;
    };
    long best = -1;
    float bestDistance = 2.0f, secondDistance = 2.0f;
    for (long d = -4; d <= 4; d++)
    {
        long j = (long)k + d;
        if (j < 0 || j >= (long)s.prev.size() || s.used[(size_t)j]) continue;
        float dist = distance(s.prev[(size_t)j]);
        if (dist < 0) continue;
        if (dist < bestDistance)
        {
            secondDistance = bestDistance;
            bestDistance = dist;
            best = j;
        }
        else if (dist < secondDistance)
            secondDistance = dist;
    }
    // only a clear match: panes close together moving as a group (the HUD buttons flying in) could pair with each other
    if (best >= 0 && secondDistance < 2.0f && secondDistance < bestDistance * 4.0f + 0.05f)
        return note("ambiguous", best), false;
    if (best >= 0)
    {
        s.used[(size_t)best] = true;
        note("blended", best);
        for (uint32_t i = 0; i < count; i++) out[i] = __builtin_bswap32(game::lerp_word(s.prev[(size_t)best][i], v[i]));
        return true;
    }
    note("unmatched", k < s.prev.size() ? (long)k : -1);
    return false;
}

void wf_pace_swap()
{
    using namespace wf;
    if (!game::enabled() || game::g_fastForward) return;
    static uint64_t s_next = 0;
    const uint64_t period = 1000000000ull / (uint64_t)game::target_fps();
    uint64_t now = os::now_ns();
    if (!s_next || now > s_next + 2 * period) s_next = now;
    while ((int64_t)(s_next - now) > 1500000)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        now = os::now_ns();
    }
    while ((int64_t)(s_next - now) > 0)
    {
        _mm_pause();
        now = os::now_ns();
    }
    s_next += period;
}

PPC_FUNC(hook_wait_vsync)
{
    if (game::g_fastForward) return;
    uint64_t start = os::now_ns();
    sub_0274C874_orig(ctx, base);

    if (!game::enabled() || !game::g_frameStartNs) return;
    game::FrameStats& st = game::g_stats[game::g_logicFrame ? 1 : 0];
    double work = (double)(start - game::g_frameStartNs), wait = (double)(os::now_ns() - start);
    st.workSum += work;
    st.workMax = std::max(st.workMax, work);
    st.waitSum += wait;
    st.count++;
    if (work > 1e9 / game::target_fps()) st.over++;
}

namespace wf::game {
uint32_t delegate_target(uint32_t d, uint32_t* thisOut = nullptr)
{
    uint32_t obj = rd32(d + 4);
    int16_t vidx = (int16_t)rd16(d + 0xA);
    if (!obj || !vidx) return 0;
    uint32_t self = obj + (int16_t)rd16(d + 8);
    if (thisOut) *thisOut = self;
    if (vidx < 0) return rd32(d + 0xC);
    uint32_t vt = rd32(self + (int16_t)rd16(d + 0xE));
    return rd32(vt + vidx * 8 + 4);
}
}

namespace wf::game {
constexpr uint32_t kGameFrameTask = 0x0203593C;
bool renderer_task(uint32_t calc) { return calc == 0x0272A5F0 || calc == 0x0272A6B4 || calc == 0x0272A808; }

uint32_t g_drawFilterThunk = 0;
uint32_t draw_filter_thunk();

void swap_camera_block(PPCContext& ctx)
{
    static const bool off = std::getenv("WF_60FPS_NOCAMSWAP") != nullptr;
    if (off) return;
    uint32_t mgr = rd32(0x101F95D0);
    if (!mgr) return;
    uint32_t holder = rd32(mgr + 0x1024);
    if (!holder) return;
    uint32_t scene = rd32(holder);
    if (!scene) return;
    hle::call_guest(ctx, 0x0278FEBC, {scene + 0x15D4});
}

void draw_only_frame(PPCContext& ctx)
{
    constexpr uint32_t kFrameCounter = 0x101FF560;
    uint32_t counter = rd32(kFrameCounter);
    static const bool noRng = std::getenv("WF_60FPS_NORNG") != nullptr;
    bool replay = g_rngValid && !noRng;
    if (replay) rng_set(g_rngDrawStart);
    swap_camera_block(ctx);
    uint32_t ctxObj = hle::call_guest(ctx, 0x025EE048);
    uint32_t old = 1;
    if (ctxObj) old = hle::call_guest(ctx, 0x02756170, {rd32(0x101F8B4C), ctxObj});
    hle::call_guest(ctx, kMtxInit);
    hle::call_guest(ctx, kFpcDwHandler, {kFpcDrawIterater, draw_filter_thunk()});
    if (old != 1) hle::call_guest(ctx, 0x02756170, {rd32(0x101F8B4C), old});
    wr32(kFrameCounter, counter);
    if (replay) rng_set(g_rngDrawEnd);
}
}

PPC_FUNC(hook_draw_only_process)
{
    uint32_t proc = ctx.r[3];
    static const bool drawLog = std::getenv("WF_60FPS_DRAWLOG") != nullptr;
    if (drawLog)
    {
        static uint32_t s_frame = 0;
        static std::set<std::pair<uint32_t, uint16_t>> s_cur, s_last;
        if (s_frame != wf::game::g_frame)
        {
            if (s_cur != s_last)
            {
                std::string out;
                for (const auto& [type, name] : s_cur)
                    if (!s_last.count({type, name})) out += std::format(" +{:08X}/{:04X}", type, name);
                for (const auto& [type, name] : s_last)
                    if (!s_cur.count({type, name})) out += std::format(" -{:08X}/{:04X}", type, name);
                wf::log_info("draw-only processes on frame {} ({} drawn):{}", s_frame, s_cur.size(), out);
                s_last = s_cur;
            }
            s_cur.clear();
            s_frame = wf::game::g_frame;
        }
        s_cur.insert({rd32(proc), rd16(proc + 8)});
    }
    static const bool findOverlap = std::getenv("WF_60FPS_FINDOVERLAP") != nullptr;
    static bool s_found = false;
    if (findOverlap && !s_found && rd16(proc + 8) < 7)
    {
        s_found = true;
        std::string out;
        for (uint32_t a = 0x10000000; a < 0x10800000; a += 4)
            if (rd32(a) == proc) out += std::format(" {:08X}", a);
        wf::log_info("overlap process {:08X} (name {:04X}) referenced at:{}", proc, rd16(proc + 8), out);
    }
    ctx.r[3] = wf::hle::call_guest(ctx, wf::game::kFpcDraw, {proc});
}

namespace wf::game {
uint32_t draw_filter_thunk()
{
    if (!g_drawFilterThunk) g_drawFilterThunk = hle::make_thunk(hook_draw_only_process);
    return g_drawFilterThunk;
}
}

PPC_FUNC(hook_task_node)
{
    uint32_t node = ctx.r[3];
    if ((!game::enabled() && !game::g_paused) || game::g_logicFrame)
    {
        sub_02747BDC_orig(ctx, base);
        return;
    }
    uint32_t delegate = node + 0x2C;
    uint32_t calc = game::delegate_target(delegate);
    if (!calc || game::renderer_task(calc))
    {
        sub_02747BDC_orig(ctx, base);
        return;
    }
    if (calc == game::kGameFrameTask)
    {
        static const uint32_t diffAt = std::getenv("WF_60FPS_DRAWDIFF") ? (uint32_t)std::atoi(std::getenv("WF_60FPS_DRAWDIFF")) : 0;
        if (diffAt && game::g_frame >= diffAt && game::g_frame < diffAt + 2)
        {
            std::vector<std::pair<uint32_t, std::vector<uint32_t>>> snap;
            for (uint32_t a = 0x10000000; a < 0x50000000;)
            {
                MEMORY_BASIC_INFORMATION mi{};
                if (!VirtualQuery(gptr<uint8_t>(a), &mi, sizeof mi)) break;
                uint32_t end = (uint32_t)std::min<uint64_t>(0x50000000ull, a + (uint64_t)((uint8_t*)mi.BaseAddress + mi.RegionSize - gptr<uint8_t>(a)));
                if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
                    snap.push_back({a, std::vector<uint32_t>(gptr<uint32_t>(a), gptr<uint32_t>(a) + (end - a) / 4)});
                a = end;
            }
            game::draw_only_frame(ctx);
            std::map<uint32_t, uint32_t> pages;
            std::map<uint32_t, std::string> samples;
            for (auto& [a, words] : snap)
                for (size_t i = 0; i < words.size(); i++)
                {
                    uint32_t now = *gptr<uint32_t>(a + (uint32_t)i * 4);
                    if (now == words[i]) continue;
                    uint32_t addr = a + (uint32_t)i * 4, page = addr & ~0xFFFu;
                    if (pages[page]++ < 4)
                        samples[page] += std::format(" {:08X}:{:.5g}>{:.5g}", addr, std::bit_cast<float>(bswap(words[i])), std::bit_cast<float>(bswap(now)));
                }
            std::string out = std::format("draw-only frame {} changed {} pages:", game::g_frame.load(), pages.size());
            for (auto& [pg, n] : pages) out += std::format("\n  {:08X} {:5}{}", pg, n, samples[pg]);
            log_info("{}", out);
        }
        else
            game::draw_only_frame(ctx);
    }
    uint16_t vidx = rd16(delegate + 0xA);
    wr16(delegate + 0xA, 0);
    ctx.r[3] = node;
    sub_02747BDC_orig(ctx, base);
    wr16(delegate + 0xA, vidx);
}

void trace_lr(uint32_t lr, uint32_t src, uint32_t rows)
{
    static const uint32_t want = std::getenv("WF_60FPS_TRACELR") ? (uint32_t)std::strtoul(std::getenv("WF_60FPS_TRACELR"), nullptr, 16) : 0;
    static std::atomic<int> s_count{0};
    if (!want || lr != want || rows < 3) return;
    float tx = std::bit_cast<float>(rd32(src + 12)), ty = std::bit_cast<float>(rd32(src + 28));
    if ((tx == 0.0f && ty == 0.0f) || s_count++ > 300) return;
    std::string vals;
    for (uint32_t i = 0; i < std::min<uint32_t>(rows * 4, 12); i++) vals += std::format(" {:.5g}", std::bit_cast<float>(rd32(src + 4 * i)));
    log_info("trace {:08X} t={:.4f} frame {} src {:08X}:{}", lr, os::now_ns() / 1e9, game::g_frame.load(), src, vals);
}

namespace wf::game {
struct ObjectKey
{
    int32_t x, y, z;
    uint32_t fieldNth;
    bool operator==(const ObjectKey&) const = default;
};
struct ObjectKeyHash
{
    size_t operator()(const ObjectKey& k) const
    {
        uint64_t h = (uint64_t)(uint32_t)k.x * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uint32_t)k.y * 0xC2B2AE3D27D4EB4Full ^
                     (uint64_t)(uint32_t)k.z * 0x165667B19E3779F9ull ^ k.fieldNth;
        return (size_t)(h ^ (h >> 29));
    }
};
struct ObjectHist
{
    uint32_t tick = 0, firstFrame = 0, tickHash = 0, perFrameUntil = 0, n = 0;
    bool prevValid = false;
    std::array<uint32_t, 16> prev{}, cur{};
};
struct ObjectCount { uint32_t frame = 0, count = 0; };
std::mutex g_objectLock;
std::unordered_map<ObjectKey, ObjectHist, ObjectKeyHash> g_objects;
std::unordered_map<ObjectKey, ObjectCount, ObjectKeyHash> g_objectCounts;
uint32_t g_objectPruneTick = 0;
thread_local int32_t t_objectPivot[3] = {};

bool object_writer(uint32_t lr) { return lr == 0x027FBB0C || lr == 0x027FBB30; }

void object_write(uint32_t lr, uint32_t dest, uint32_t rowStride, uint32_t wordsPerRow, uint32_t rows, uint32_t src, uint32_t field)
{
    uint32_t n = rows * wordsPerRow;
    uint32_t words[16];
    for (uint32_t i = 0; i < n; i++) words[i] = rd32(src + 4 * i);
    if (lr == 0x027FBB0C && n >= 12)
        for (int a = 0; a < 3; a++) t_objectPivot[a] = (int32_t)std::lround(std::bit_cast<float>(words[3 + 4 * a]) * 2.0f);
    uint32_t frame = g_frame, tick = g_tick;
    bool transform = wordsPerRow == 4 && rows >= 3;
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < n; i++) hash = (hash ^ words[i]) * 16777619u;
    uint32_t out[16];
    std::copy(words, words + n, out);
    {
        std::lock_guard lk(g_objectLock);
        if (tick - g_objectPruneTick > 30)
        {
            g_objectPruneTick = tick;
            std::erase_if(g_objects, [&](const auto& e) { return tick - e.second.tick > 30; });
            std::erase_if(g_objectCounts, [&](const auto& e) { return frame - e.second.frame > 60; });
        }
        ObjectKey pivot{t_objectPivot[0], t_objectPivot[1], t_objectPivot[2], field << 16};
        ObjectCount& c = g_objectCounts[pivot];
        if (c.frame != frame) c = {frame, 0};
        ObjectHist& h = g_objects[{pivot.x, pivot.y, pivot.z, (field << 16) | (c.count++ & 0xFFFF)}];
        if (h.tick != tick)
        {
            h.prevValid = h.tick + 1 == tick && h.n == n;
            h.prev = h.cur;
            h.tick = tick;
            h.firstFrame = frame;
            h.tickHash = hash;
        }
        else if (frame != h.firstFrame && hash != h.tickHash)
            h.perFrameUntil = frame + 240;
        std::copy(words, words + n, h.cur.begin());
        h.n = n;
        if (interpolating() && h.prevValid && h.perFrameUntil <= frame)
        {
            for (uint32_t i = 0; i < n; i++) out[i] = transform ? lerp_word(h.prev[i], words[i]) : lerp_param(h.prev[i], words[i]);
            static const bool stats = std::getenv("WF_60FPS_OBJSTATS") != nullptr;
            static uint32_t s_blends = 0, s_far = 0, s_lastFrame = 0;
            if (stats && transform)
            {
                float d = 0;
                for (uint32_t i = 0; i < 12; i++)
                    if (i % 4 != 3) d = std::max(d, std::abs(std::bit_cast<float>(h.prev[i]) - std::bit_cast<float>(words[i])));
                s_blends++;
                if (d > 0.3f) s_far++;
                if (frame - s_lastFrame >= 300)
                {
                    log_info("object blends: {} ({} between far-apart matrices), {} objects tracked", s_blends, s_far, g_objects.size());
                    s_lastFrame = frame;
                    s_blends = s_far = 0;
                }
            }
        }
    }
    for (uint32_t r = 0, i = 0; r < rows; r++, dest += rowStride * 4)
        for (uint32_t w = 0; w < wordsPerRow; w++, i++)
            *gptr<uint32_t>(dest + 4 * w) = out[i];
}
}

PPC_FUNC(hook_ubo_write)
{
    trace_lr(ctx.lr, ctx.r[6], ctx.r[8]);
    if (!game::enabled())
    {
        uint32_t lr = ctx.lr, src = ctx.r[6], rows = ctx.r[8];
        uint32_t fovDest = 0, fovStride = 0;
        if (lr == 0x027FBA00 && rows == 4)
        {
            uint32_t entry = rd32(rd32(ctx.r[3])) + ctx.r[5] * 6;
            fovStride = rd8(game::kUniformTypeTable + rd8(entry) * 3 + 2);
            fovDest = ctx.r[4] + rd16(entry + 4) + ctx.r[7] * fovStride * 4;
        }
        sub_027BF048_orig(ctx, base);
        if (fovDest && fovStride >= 4) game::fov_adjust(lr, gptr<uint32_t>(fovDest), fovStride);
        if ((lr == 0x02790120 || lr == 0x027FBA00) && rows == 4)
        {
            uint32_t words[16];
            for (int i = 0; i < 16; i++) words[i] = rd32(src + 4 * i);
            game::record_camera(lr, 16, words);
        }
        return;
    }
    uint32_t layout = ctx.r[3], buffer = ctx.r[4], field = ctx.r[5], src = ctx.r[6], element = ctx.r[7];
    int32_t rows = (int32_t)ctx.r[8];
    uint32_t entry = rd32(rd32(layout)) + field * 6;
    uint32_t type = rd8(entry);
    uint32_t wordsPerRow = rd8(game::kUniformTypeTable + type * 3 + 0);
    uint32_t rowStride = rd8(game::kUniformTypeTable + type * 3 + 2);
    uint32_t dest = buffer + rd16(entry + 4) + element * rowStride * 4;
    const int testMode = game::g_testMode.load(std::memory_order_relaxed);
    static const bool noParamsEnv = std::getenv("WF_60FPS_NOPARAMS") != nullptr;
    const bool paramInterp = !noParamsEnv && testMode != 2;
    bool transform = wordsPerRow == 4 && rows >= 3;
    if (game::object_writer(ctx.lr) && testMode != 7 && testMode != 1 && wordsPerRow && wordsPerRow <= 4 && rows > 0 &&
        (uint32_t)rows * wordsPerRow <= 16 && (transform || paramInterp))
    {
        game::object_write(ctx.lr, dest, rowStride, wordsPerRow, (uint32_t)rows, src, field);
        return;
    }
    {
        static uint32_t s_wlFirst = 0, s_wlLast = 0, s_wlLo = 0, s_wlHi = 0;
        static bool s_wlInit = false;
        if (!s_wlInit)
        {
            s_wlInit = true;
            if (const char* e = std::getenv("WF_60FPS_WRITELOG")) std::sscanf(e, "%u-%u:%x-%x", &s_wlFirst, &s_wlLast, &s_wlLo, &s_wlHi);
        }
        uint32_t fr = game::g_frame;
        static const uint32_t s_wlLr = std::getenv("WF_60FPS_WRITELOG_LR") ? (uint32_t)std::strtoul(std::getenv("WF_60FPS_WRITELOG_LR"), nullptr, 16) : 0;
        if (s_wlLast && fr >= s_wlFirst && fr <= s_wlLast && dest >= s_wlLo && dest < s_wlHi && (!s_wlLr || ctx.lr == s_wlLr))
        {
            std::string vals;
            for (uint32_t i = 0; i < std::min<uint32_t>(wordsPerRow * (uint32_t)rows, 12); i++)
                vals += std::format(" {:.4g}", std::bit_cast<float>(rd32(src + 4 * i)));
            log_info("write t {} frame {} {} step {:.2f} lr {:08X} f{} type {} {}x{} dest {:08X} src {:08X}:{} thread {} core {}",
                     std::chrono::steady_clock::now().time_since_epoch().count(), fr, game::g_logicFrame ? "logic" : "draw",
                     game::g_step.load(), ctx.lr, field, type, wordsPerRow, rows, dest, src, vals, os::current(ctx).name, ctx.core);
            if (field == 4)
            {
                std::string obj;
                for (uint32_t i = 0; i < 16; i++) obj += std::format(" {:08X}", rd32(layout + 4 * i));
                log_info("  layout object {:08X}:{}", layout, obj);
            }
        }
    }
    if (wordsPerRow == 0 || wordsPerRow > 4 || (!transform && !paramInterp))
    {
        static const bool fieldStats = std::getenv("WF_60FPS_FIELDSTATS") != nullptr;
        if (fieldStats) game::field_stats(ctx.lr, type, wordsPerRow, rows, dest, src);
        for (int32_t r = 0; r < rows; r++, dest += rowStride * 4)
            for (uint32_t w = 0; w < wordsPerRow; w++, src += 4)
                *gptr<uint32_t>(dest + 4 * w) = rd32(src);
        return;
    }
    uint32_t frame = game::g_frame, tick = game::g_tick;
    if (uint32_t rf = game::g_rewriteFrame.exchange(0); rf && rf == frame) game::do_rewrite(rf);
    static const bool noInterpEnv = std::getenv("WF_60FPS_NOINTERP") != nullptr;
    const bool noInterp = noInterpEnv || testMode == 1 || (testMode == 4 && wordsPerRow == 4 && rows >= 3) ||
                          (testMode == 7 && (ctx.lr == 0x027FBB0C || ctx.lr == 0x027FBB30)) ||
                          (testMode == 8 && ctx.lr >= 0x027FD408 && ctx.lr < 0x027FD6F4) ||
                          (testMode == 3 && (ctx.lr == game::kViewBlockModelView || ctx.lr == game::kViewBlockProjection));
    bool interpolate = game::interpolating() && !noInterp;
    uint32_t n = (uint32_t)rows * wordsPerRow;
    game::Shard& shard = game::g_shards[(dest >> 6) % game::kShards];
    std::lock_guard lk(shard.lock);
    game::TransformField& f = shard.fields[dest];
    if (f.tick != tick)
    {
        if (f.cur.size() == n && f.tick + 1 == tick) f.prev.swap(f.cur);
        else f.prev.clear();
        f.prevSrc = f.curSrc;
        f.tick = tick;
    }
    f.frame = frame;
    f.cur.resize(n);
    bool havePrev = interpolate && f.prev.size() == n;
    static const bool noPerFrame = std::getenv("WF_60FPS_NOPERFRAME") != nullptr;
    bool moved = havePrev && f.prevSrc != src;
    thread_local std::vector<uint32_t> t_words, t_srcPrev, t_out;
    t_words.resize(n);
    for (uint32_t i = 0; i < n; i++) t_words[i] = rd32(src + 4 * i);
    bool perFrame = false;
    std::vector<uint32_t>& srcPrev = t_srcPrev;
    bool haveSrcPrev = game::source_track(src, ((uint64_t)field << 32) | n, t_words.data(), n, frame, tick, !noPerFrame, perFrame, srcPrev);
    static const bool perFrameLog = std::getenv("WF_60FPS_PERFRAMELOG") != nullptr;
    if (perFrameLog && perFrame)
    {
        static std::mutex lock;
        static std::set<uint64_t> seen;
        std::lock_guard plk(lock);
        if (seen.insert(((uint64_t)ctx.lr << 32) | (field << 16) | n).second)
        {
            std::string vals;
            for (uint32_t i = 0; i < std::min<uint32_t>(n, 8); i++) vals += std::format(" {:.4g}", std::bit_cast<float>(rd32(src + 4 * i)));
            log_info("per-frame field: caller {:08X} field {} words {} src {:08X}:{}", ctx.lr, field, n, src, vals);
        }
    }
    if (interpolate && !perFrame && (moved || !havePrev) && haveSrcPrev && f.tick == tick)
    {
        f.prev.assign(srcPrev.begin(), srcPrev.end());
        havePrev = true;
        moved = false;
    }
    if (interpolate)
        game::g_uboStats[transform ? 0 : 1][moved ? 1 : perFrame ? 2 : havePrev ? 0 : 3]++;
    static const uint32_t noPrevAt = std::getenv("WF_60FPS_NOPREVLOG") ? (uint32_t)std::atoi(std::getenv("WF_60FPS_NOPREVLOG")) : 0;
    if (noPrevAt && interpolate && transform && frame >= noPrevAt && frame < noPrevAt + 2 && (moved || perFrame || !havePrev))
        log_info("unblended frame {} lr {:08X} f{} n {} dest {:08X} src {:08X} moved {} perFrame {} havePrev {} t=({:.1f},{:.1f},{:.1f})", frame, ctx.lr,
                 field, n, dest, src, moved, perFrame, havePrev, std::bit_cast<float>(rd32(src + 12)), std::bit_cast<float>(rd32(src + 28)),
                 std::bit_cast<float>(rd32(src + 44)));
    if (moved || perFrame) havePrev = false;
    if (havePrev && game::caller_excluded(ctx.lr, transform)) havePrev = false;
    static const bool jumpLog = std::getenv("WF_60FPS_JUMPLOG") != nullptr;
    if (jumpLog && havePrev)
    {
        std::vector<uint32_t> now(n);
        for (uint32_t r = 0, i = 0; r < (uint32_t)rows; r++)
            for (uint32_t w = 0; w < wordsPerRow; w++, i++) now[i] = rd32(src + 4 * i);
        game::jump_log(ctx.lr, field, f.prev, now);
    }
    static const uint32_t dumpAt = std::getenv("WF_60FPS_DUMPAT") ? (uint32_t)atoi(std::getenv("WF_60FPS_DUMPAT")) : 0;
    if (dumpAt && transform && frame >= dumpAt && frame < dumpAt + 2 && interpolate)
    {
        std::string out = std::format("transform {:08X} f{} dest {:08X} src {:08X} moved {} perFrame {} prev/cur:", ctx.lr, field, dest, src, moved, perFrame);
        for (uint32_t i = 0; i < n; i++)
            out += std::format("{}{:.5g}/{:.5g}", i % 4 ? " " : "\n   ", std::bit_cast<float>(i < f.prev.size() ? f.prev[i] : 0u), std::bit_cast<float>(rd32(src + 4 * i)));
        log_info("{}", out);
    }
    f.curSrc = src;
    std::copy(t_words.begin(), t_words.end(), f.cur.begin());
    std::vector<uint32_t>& out = t_out;
    out.assign(f.cur.begin(), f.cur.end());
    bool rebased = transform && !noInterp &&
                   game::shadow_halfway(ctx.lr, dest, frame, tick, interpolate, n, havePrev, f.prev.data(), f.cur.data(), out.data());
    static const bool shadowLog = std::getenv("WF_60FPS_SHADOWLOG") != nullptr;
    static std::atomic<uint32_t> s_writeFrame{0}, s_writeIndex{0};
    if (shadowLog && transform)
    {
        if (s_writeFrame.exchange(frame) != frame) s_writeIndex = 0;
        uint32_t index = s_writeIndex++;
        if (ctx.lr == 0x027FB8BC || ctx.lr == 0x027FBA00 || ctx.lr == 0x02790120 || ctx.lr == 0x02790158)
            log_info("shadow {:08X} dest {:08X} frame {} write {} logic {} prev {} n {} -> {} t=({:.0f},{:.0f},{:.0f})", ctx.lr, dest, frame, index,
                     interpolate, havePrev, n, rebased ? "rebased" : havePrev ? "lerped" : "raw", std::bit_cast<float>(f.cur[3]),
                     std::bit_cast<float>(f.cur[7]), std::bit_cast<float>(f.cur[11]));
    }
    bool cut = interpolate && game::g_cutTick == tick;
    if (cut && rebased)
    {
        std::copy(f.cur.begin(), f.cur.end(), out.begin());
        rebased = false;
    }
    if (havePrev && !rebased && !cut)
        for (uint32_t i = 0; i < n; i++)
            out[i] = transform ? game::lerp_word(f.prev[i], f.cur[i]) : game::lerp_param(f.prev[i], f.cur[i]);
    f.rows = (uint16_t)rows;
    f.wordsPerRow = (uint16_t)wordsPerRow;
    f.rowStride = (uint16_t)rowStride;
    f.transform = transform;
    f.blended = !cut && (rebased || havePrev);
    if (transform && n == 16 && wordsPerRow == 4) game::fov_adjust(ctx.lr, out.data(), 4);
    static uint32_t s_blFirst = 0, s_blLast = 0, s_blLo = 0, s_blHi = 0;
    static bool s_blInit = false;
    if (!s_blInit)
    {
        s_blInit = true;
        if (const char* e = std::getenv("WF_60FPS_BLOCKLOG")) std::sscanf(e, "%u-%u:%x-%x", &s_blFirst, &s_blLast, &s_blLo, &s_blHi);
    }
    if (transform && s_blLast && frame >= s_blFirst && frame <= s_blLast && dest >= s_blLo && dest < s_blHi && n >= 12)
        log_info("block frame {} {} lr {:08X} f{} dest {:08X} src {:08X} prev {} raw t=({:.1f},{:.1f},{:.1f}) out t=({:.1f},{:.1f},{:.1f})", frame,
                 game::g_logicFrame ? "logic" : "draw", ctx.lr, field, dest, src, havePrev, std::bit_cast<float>(f.cur[3]), std::bit_cast<float>(f.cur[7]),
                 std::bit_cast<float>(f.cur[11]), std::bit_cast<float>(out[3]), std::bit_cast<float>(out[7]), std::bit_cast<float>(out[11]));
    {
        static uint32_t s_flLr = 0, s_flLr2 = 0, s_flFirst = 0, s_flLast = 0;
        static bool s_flInit = false;
        if (!s_flInit)
        {
            s_flInit = true;
            if (const char* e = std::getenv("WF_60FPS_FIELDLOG"))
                if (std::sscanf(e, "%x,%x:%u-%u", &s_flLr, &s_flLr2, &s_flFirst, &s_flLast) != 4) std::sscanf(e, "%x:%u-%u", &s_flLr, &s_flFirst, &s_flLast);
        }
        if (s_flLr && (ctx.lr == s_flLr || ctx.lr == s_flLr2) && frame >= s_flFirst && frame <= s_flLast)
        {
            std::string raw, wr, pv;
            for (uint32_t i = 0; i < std::min<uint32_t>(n, 12); i++)
            {
                raw += std::format(" {:.4g}", std::bit_cast<float>(f.cur[i]));
                wr += std::format(" {:.4g}", std::bit_cast<float>(out[i]));
                pv += std::format(" {:.4g}", i < f.prev.size() ? std::bit_cast<float>(f.prev[i]) : 0.0f);
            }
            log_info("field frame {} {} lr {:08X} f{} {}x{} dest {:08X} src {:08X} prevSrc {:08X} perFrame {} haveSrcPrev {} havePrev {} rebased {}\n   prev{}\n   raw {}\n   out {}",
                     frame, game::g_logicFrame ? "logic" : "draw", ctx.lr, field, wordsPerRow, rows, dest, src, f.prevSrc, perFrame, haveSrcPrev, havePrev,
                     rebased, pv, raw, wr);
        }
    }
    if (transform) game::record_camera(ctx.lr, n, out.data());
    for (int32_t r = 0, i = 0; r < rows; r++, dest += rowStride * 4)
        for (uint32_t w = 0; w < wordsPerRow; w++, i++)
            *gptr<uint32_t>(dest + 4 * w) = out[i];
}

PPC_FUNC(hook_skin_palette)
{
    uint32_t obj = ctx.r[3], index = ctx.r[4];
    uint32_t size = rd32(obj + 0x1C);
    uint32_t dest = rd32(obj + 0x3C);
    if (!dest) dest = rd32(obj + 0x18) + size * index;
    sub_027DBC2C_orig(ctx, base);
    game::note_burst();
    struct NotePalette
    {
        uint32_t obj, dest, size;
        ~NotePalette() { if (dest) mp::note_palette(obj, dest, size); }
    } note{obj, dest, size};
    static const bool noSkin = std::getenv("WF_60FPS_NOSKIN") != nullptr;
    if (!game::enabled() || noSkin || game::g_testMode.load(std::memory_order_relaxed) == 5)
        return;
    if (!dest || size == 0 || size > 0x10000 || (size & 3))
        return;
    uint32_t n = size / 4, frame = game::g_frame;
    static uint32_t s_logFirst = 0, s_logLast = 0;
    static bool s_logInit = false;
    if (!s_logInit)
    {
        s_logInit = true;
        if (const char* e = std::getenv("WF_60FPS_SKINLOG")) std::sscanf(e, "%u-%u", &s_logFirst, &s_logLast);
    }
    if (frame >= s_logFirst && frame <= s_logLast && s_logLast)
        log_info("skin frame {} {} step {:.3f} obj {:08X} index {} dest {:08X} size {:X}", frame, game::g_logicFrame ? "logic" : "draw",
                 game::g_step.load(), obj, index, dest, size);
    game::Shard& shard = game::g_shards[(obj >> 6) % game::kShards];
    std::lock_guard lk(shard.lock);
    game::TransformField& f = shard.fields[(1ull << 63) | ((uint64_t)obj << 16) | (index & 0xFFFF)];
    uint32_t tick = game::g_tick;
    if (f.tick != tick)
    {
        if (f.cur.size() == n && f.tick + 1 == tick) f.prev.swap(f.cur);
        else f.prev.clear();
        f.tick = tick;
    }
    f.frame = frame;
    f.cur.resize(n);
    bool interpolate = game::interpolating() && f.prev.size() == n;
    uint32_t* words = gptr<uint32_t>(dest);
    std::copy(words, words + n, f.cur.begin());
    if (interpolate)
    {
        uint32_t whole = n / 12 * 12;
        for (uint32_t m = 0; m < whole; m += 12) game::lerp_affine(f.prev.data() + m, f.cur.data() + m, words + m);
        for (uint32_t i = whole; i < n; i++) words[i] = game::lerp_word(f.prev[i], f.cur[i]);
    }
    if (frame >= s_logFirst && frame <= s_logLast && s_logLast && n >= 32)
    {
        std::string w;
        for (uint32_t i = 0; i < 32; i++) w += std::format(" {:.3g}", std::bit_cast<float>(words[i]));
        log_info("skin out frame {} obj {:08X} index {} interp {} words:{}", frame, obj, index, interpolate, w);
    }
}

PPC_FUNC(hook_shape_matrix)
{
    uint32_t obj = ctx.r[3], slot = ctx.r[4], buffer = ctx.r[6];
    uint32_t desc = rd32(obj + 0xC) + slot * ((rd32(obj + 0x8) >> 16) & 0xFF) * 20;
    uint32_t dest = rd32(desc) + rd32(desc + 4) * buffer;
    bool swapped = (rd32(obj + 0x4) & 2) != 0;
    sub_027DDBB0_orig(ctx, base);
    game::note_burst();
    static const bool noShape = std::getenv("WF_60FPS_NOSHAPE") != nullptr;
    if (!game::enabled() || noShape || !dest || game::g_testMode.load(std::memory_order_relaxed) == 6)
        return;
    constexpr uint32_t n = 12;
    uint32_t frame = game::g_frame;
    game::Shard& shard = game::g_shards[(obj >> 6) % game::kShards];
    std::lock_guard lk(shard.lock);
    game::TransformField& f =
        shard.fields[(3ull << 62) | ((uint64_t)obj << 24) | ((uint64_t)(slot & 0xFFFF) << 8) | (buffer & 0xFF)];
    uint32_t tick = game::g_tick;
    if (f.tick != tick)
    {
        if (f.cur.size() == n && f.tick + 1 == tick) f.prev.swap(f.cur);
        else f.prev.clear();
        f.tick = tick;
    }
    static const bool multiLog = std::getenv("WF_60FPS_MULTIWRITE") != nullptr;  // test: one part written twice in a frame
    if (multiLog && f.frame == frame && f.cur.size() == n)
    {
        uint32_t* w = gptr<uint32_t>(dest);
        bool differs = false;
        for (uint32_t i = 0; i < n; i++) differs |= f.cur[i] != (swapped ? w[i] : __builtin_bswap32(w[i]));
        static std::atomic<int> s_count{0};
        if (s_count++ < 40)
            log_info("shape written again in frame {}: obj {:08X} slot {} buf {} dest {:08X} {} lr {:08X}", frame, obj, slot, buffer, dest,
                     differs ? "DIFFERENT" : "same", ctx.lr);
    }
    f.frame = frame;
    f.cur.resize(n);
    uint32_t* words = gptr<uint32_t>(dest);
    for (uint32_t i = 0; i < n; i++) f.cur[i] = swapped ? words[i] : __builtin_bswap32(words[i]);
    auto fl = [](uint32_t w) { float v; std::memcpy(&v, &w, 4); return v; };
    static uint32_t s_logFirst = 0, s_logLast = 0;
    static bool s_logInit = false;
    if (!s_logInit)
    {
        s_logInit = true;
        if (const char* e = std::getenv("WF_60FPS_SHAPELOG")) std::sscanf(e, "%u-%u", &s_logFirst, &s_logLast);
    }
    bool log = s_logLast && frame >= s_logFirst && frame <= s_logLast;
    if (!game::interpolating() || f.prev.size() != n)
    {
        if (log) log_info("shape frame {} {} obj {:08X} slot {} buf {} t {:.1f} {:.1f} {:.1f}", frame, game::g_logicFrame ? "logic" : "draw", obj, slot, buffer, fl(f.cur[3]), fl(f.cur[7]), fl(f.cur[11]));
        return;
    }
    float dx = fl(f.cur[3]) - fl(f.prev[3]), dy = fl(f.cur[7]) - fl(f.prev[7]), dz = fl(f.cur[11]) - fl(f.prev[11]);
    if (!(dx * dx + dy * dy + dz * dz < 300.0f * 300.0f))
        return;
    uint32_t out[n];
    game::lerp_affine(f.prev.data(), f.cur.data(), out);
    for (uint32_t i = 0; i < n; i++) words[i] = swapped ? out[i] : __builtin_bswap32(out[i]);
    if (log)
        log_info("shape frame {} {} obj {:08X} slot {} buf {} t {:.1f} {:.1f} {:.1f} (step {:.3f})", frame, game::g_logicFrame ? "logic" : "draw", obj, slot,
                 buffer, fl(game::lerp_word(f.prev[3], f.cur[3])), fl(game::lerp_word(f.prev[7], f.cur[7])), fl(game::lerp_word(f.prev[11], f.cur[11])),
                 game::g_step.load());
}

namespace wf::game {
bool camera(Camera& out) { return copy_camera(out); }
}
