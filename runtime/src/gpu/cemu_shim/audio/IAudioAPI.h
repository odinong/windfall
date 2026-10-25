#pragma once
#include "config/CemuConfig.h"

class IAudioAPI
{
public:
    enum AudioType { TV = 0, Gamepad, Portal };
    static constexpr uint32 kBlockCount = 24;

    IAudioAPI(uint32 samplerate, uint32 channels, uint32 samplesPerBlock, uint32 bitsPerSample)
        : m_samplerate(samplerate), m_channels(channels), m_samplesPerBlock(samplesPerBlock), m_bitsPerSample(bitsPerSample),
          m_bytesPerBlock(samplesPerBlock * channels * (bitsPerSample / 8)) {}
    virtual ~IAudioAPI() = default;

    sint32 GetChannels() const { return m_channels; }
    virtual sint32 GetVolume() const { return m_volume; }
    virtual void SetVolume(sint32 volume) { m_volume = volume; }

    virtual bool NeedAdditionalBlocks() const = 0;
    virtual bool FeedBlock(sint16* data) = 0;
    virtual bool Play() = 0;
    virtual bool Stop() = 0;
    void SetAudioDelayOverride(uint32 delay) { m_audioDelayOverride = delay; }
    uint32 GetAudioDelay() const { return m_audioDelayOverride ? m_audioDelayOverride : 2; }

    static std::unique_ptr<IAudioAPI> CreateDeviceFromConfig(AudioType type, sint32 rate, sint32 samplesPerBlock, sint32 bitsPerSample);
    static std::unique_ptr<IAudioAPI> CreateDeviceFromConfig(AudioType type, sint32 rate, sint32 channels, sint32 samplesPerBlock, sint32 bitsPerSample);

protected:
    uint32 m_samplerate, m_channels, m_samplesPerBlock, m_bitsPerSample;
    uint32 m_bytesPerBlock;
    sint32 m_volume = 100;
    bool m_playing = false;
    uint32 m_audioDelayOverride = 0;
};

using AudioAPIPtr = std::unique_ptr<IAudioAPI>;
extern std::shared_mutex g_audioMutex;
extern AudioAPIPtr g_tvAudio;
extern AudioAPIPtr g_padAudio;
extern std::atomic_int32_t g_padVolume;
extern AudioAPIPtr g_portalAudio;
