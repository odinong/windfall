#include "Common/precompiled.h"
#include "audio/IAudioAPI.h"

#include <SDL3/SDL.h>

#include "core/log.h"

std::shared_mutex g_audioMutex;
AudioAPIPtr g_tvAudio;
AudioAPIPtr g_padAudio;
std::atomic_int32_t g_padVolume = 0;
AudioAPIPtr g_portalAudio;

namespace {

class WavDump
{
public:
    WavDump(const char* path, uint32 rate, uint32 channels) : m_rate(rate), m_channels(channels)
    {
        m_fp = std::fopen(path, "wb");
        if (m_fp) write_header();
    }
    ~WavDump() { if (m_fp) std::fclose(m_fp); }
    void write(const void* data, uint32 bytes)
    {
        if (!m_fp) return;
        std::fwrite(data, 1, bytes, m_fp);
        m_dataBytes += bytes;
        write_header();
        std::fseek(m_fp, 0, SEEK_END);
    }
private:
    void write_header()
    {
        struct { char riff[4]; uint32 size; char wave[4]; char fmt[4]; uint32 fmtSize; uint16 format, channels; uint32 rate, byteRate; uint16 align, bits; char data[4]; uint32 dataSize; } h{
            {'R', 'I', 'F', 'F'}, 36 + m_dataBytes, {'W', 'A', 'V', 'E'}, {'f', 'm', 't', ' '}, 16, 1, (uint16)m_channels, m_rate,
            m_rate * m_channels * 2, (uint16)(m_channels * 2), 16, {'d', 'a', 't', 'a'}, m_dataBytes};
        std::fseek(m_fp, 0, SEEK_SET);
        std::fwrite(&h, sizeof(h), 1, m_fp);
    }
    FILE* m_fp = nullptr;
    uint32 m_rate, m_channels, m_dataBytes = 0;
};

class SDLAudioDevice final : public IAudioAPI
{
public:
    SDLAudioDevice(sint32 rate, sint32 channels, sint32 samplesPerBlock, sint32 bitsPerSample)
        : IAudioAPI(rate, channels, samplesPerBlock, bitsPerSample)
    {
        SDL_AudioSpec spec{SDL_AUDIO_S16LE, channels, rate};
        m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!m_stream)
            throw std::runtime_error(fmt::format("SDL_OpenAudioDeviceStream failed: {}", SDL_GetError()));
    }
    void EnableDump(const char* path) { m_dump = std::make_unique<WavDump>(path, m_samplerate, m_channels); }
    ~SDLAudioDevice() override { SDL_DestroyAudioStream(m_stream); }

    void SetVolume(sint32 volume) override
    {
        IAudioAPI::SetVolume(volume);
        SDL_SetAudioStreamGain(m_stream, (float)volume / 100.0f);
    }

    bool NeedAdditionalBlocks() const override
    {
        return SDL_GetAudioStreamQueued(m_stream) < (int)(m_bytesPerBlock * GetAudioDelay());
    }

    bool FeedBlock(sint16* data) override
    {
        if (SDL_GetAudioStreamQueued(m_stream) > (int)(m_bytesPerBlock * kBlockCount))
            return false;
        if (m_dump)
            m_dump->write(data, m_bytesPerBlock);
        return SDL_PutAudioStreamData(m_stream, data, (int)m_bytesPerBlock);
    }

    bool Play() override
    {
        if (!m_playing)
            m_playing = SDL_ResumeAudioStreamDevice(m_stream);
        return m_playing;
    }

    bool Stop() override
    {
        if (m_playing)
            m_playing = !SDL_PauseAudioStreamDevice(m_stream);
        return !m_playing;
    }

private:
    SDL_AudioStream* m_stream = nullptr;
    std::unique_ptr<WavDump> m_dump;
};

bool init_sdl_audio()
{
    static const bool ok = [] {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            wf::log_warn("audio disabled: {}", SDL_GetError());
            return false;
        }
        return true;
    }();
    return ok;
}

}

AudioAPIPtr IAudioAPI::CreateDeviceFromConfig(AudioType type, sint32 rate, sint32 samplesPerBlock, sint32 bitsPerSample)
{
    const auto& config = GetConfig();
    AudioChannels channels = type == Gamepad ? config.pad_channels : config.tv_channels;
    return CreateDeviceFromConfig(type, rate, CemuConfig::AudioChannelsToNChannels(channels), samplesPerBlock, bitsPerSample);
}

AudioAPIPtr IAudioAPI::CreateDeviceFromConfig(AudioType type, sint32 rate, sint32 channels, sint32 samplesPerBlock, sint32 bitsPerSample)
{
    const auto& config = GetConfig();
    sint32 volume = type == Gamepad ? config.pad_volume : config.tv_volume;
    if (type == Portal || (volume <= 0 && type != TV) || !init_sdl_audio())
        return {};
    auto device = std::make_unique<SDLAudioDevice>(rate, channels, samplesPerBlock, bitsPerSample);
    device->SetVolume(volume);
    if (const char* dump = std::getenv("WF_AUDIO_DUMP"); dump && type == TV)
        device->EnableDump(dump);
    wf::log_info("audio: {} output, {} Hz, {} channels", type == TV ? "TV" : "GamePad", rate, channels);
    return device;
}
