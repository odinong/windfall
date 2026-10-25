#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WINDFALL_MOD_API_VERSION 1

typedef void (*WindfallFrameCallback)(void* user);

typedef struct WindfallModAPI
{
    uint32_t version;

    void (*log)(const char* mod, const char* message);

    int (*is_mapped)(uint32_t address, uint32_t size);
    uint8_t (*read8)(uint32_t address);
    uint16_t (*read16)(uint32_t address);
    uint32_t (*read32)(uint32_t address);
    float (*read_float)(uint32_t address);
    void (*write8)(uint32_t address, uint8_t value);
    void (*write16)(uint32_t address, uint16_t value);
    void (*write32)(uint32_t address, uint32_t value);
    void (*write_float)(uint32_t address, float value);
    uint8_t* (*host_pointer)(uint32_t address);

    void (*on_game_tick)(WindfallFrameCallback callback, void* user);

    uint32_t (*frame_count)(void);
    uint32_t (*tick_count)(void);

    uint32_t addr_game_info;
    uint32_t addr_env_light;
} WindfallModAPI;

typedef int (*WindfallModInit)(const WindfallModAPI* api);

#ifdef __cplusplus
}
#endif
