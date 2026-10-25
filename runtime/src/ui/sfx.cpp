#include "ui/sfx.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "ui/settings.h"

namespace wf::ui::sfx {
namespace {

constexpr int kRate = 48000;
constexpr float kPi = 3.14159265f;

struct Voice { const std::vector<float>* samples; size_t pos; };

std::once_flag g_once;
std::mutex g_lock;
SDL_AudioStream* g_stream = nullptr;
std::vector<float> g_sounds[kSoundCount];
std::vector<Voice> g_voices;

void bell(std::vector<float>& out, float start, float freq, float amp, float decay)
{
    size_t from = (size_t)(start * kRate), len = (size_t)(decay * 6 * kRate);
    if (out.size() < from + len) out.resize(from + len, 0.0f);
    for (size_t i = 0; i < len; i++)
    {
        float t = (float)i / kRate;
        float env = std::exp(-t / decay) * std::min(1.0f, t * 400.0f);
        float v = std::sin(2 * kPi * freq * t) + 0.35f * std::sin(2 * kPi * freq * 2.0f * t) * std::exp(-t / (decay * 0.5f)) +
                  0.12f * std::sin(2 * kPi * freq * 3.01f * t) * std::exp(-t / (decay * 0.3f));
        out[from + i] += amp * env * v;
    }
}

void build()
{
    const float C5 = 523.25f, E5 = 659.25f, G5 = 783.99f, A5 = 880.0f, C6 = 1046.5f, D6 = 1174.7f, E6 = 1318.5f, G6 = 1568.0f;
    bell(g_sounds[kMove], 0.0f, E6, 0.10f, 0.025f);
    bell(g_sounds[kSelect], 0.0f, A5, 0.14f, 0.04f);
    bell(g_sounds[kSelect], 0.045f, E6, 0.14f, 0.05f);
    bell(g_sounds[kConfirm], 0.0f, C6, 0.16f, 0.08f);
    bell(g_sounds[kConfirm], 0.0f, G6, 0.08f, 0.06f);
    bell(g_sounds[kBack], 0.0f, E6, 0.13f, 0.04f);
    bell(g_sounds[kBack], 0.05f, A5, 0.13f, 0.06f);
    bell(g_sounds[kTab], 0.0f, D6, 0.13f, 0.07f);
    bell(g_sounds[kTab], 0.0f, G6, 0.06f, 0.05f);
    bell(g_sounds[kOpen], 0.0f, C5, 0.12f, 0.08f);
    bell(g_sounds[kOpen], 0.06f, E5, 0.12f, 0.08f);
    bell(g_sounds[kOpen], 0.12f, G5, 0.12f, 0.09f);
    bell(g_sounds[kOpen], 0.18f, C6, 0.13f, 0.14f);
    bell(g_sounds[kClose], 0.0f, C6, 0.12f, 0.07f);
    bell(g_sounds[kClose], 0.06f, G5, 0.12f, 0.07f);
    bell(g_sounds[kClose], 0.12f, C5, 0.12f, 0.12f);
}

void SDLCALL feed(void*, SDL_AudioStream* stream, int additional, int)
{
    int frames = additional / (int)sizeof(float);
    if (frames <= 0) return;
    std::vector<float> mix((size_t)frames, 0.0f);
    float volume;
    {
        std::lock_guard lk(settings::lock());
        volume = settings::get().volume / 100.0f;
    }
    {
        std::lock_guard lk(g_lock);
        for (Voice& v : g_voices)
            for (int i = 0; i < frames && v.pos < v.samples->size(); i++) mix[(size_t)i] += (*v.samples)[v.pos++];
        std::erase_if(g_voices, [](const Voice& v) { return v.pos >= v.samples->size(); });
    }
    for (float& s : mix) s = std::clamp(s * volume, -1.0f, 1.0f);
    SDL_PutAudioStreamData(stream, mix.data(), frames * (int)sizeof(float));
}

void init()
{
    build();
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    SDL_AudioSpec spec{SDL_AUDIO_F32, 1, kRate};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, nullptr);
    if (!g_stream)
    {
        log_warn("menu sounds: no audio device ({})", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
}

}

void play(Sound s)
{
    std::call_once(g_once, init);
    if (!g_stream || s < 0 || s >= kSoundCount) return;
    std::lock_guard lk(g_lock);
    if (g_voices.size() < 8) g_voices.push_back({&g_sounds[s], 0});
}

}
