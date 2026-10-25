#pragma once
namespace CafeSystem
{
    inline uint64 GetForegroundTitleId() { return 0x0005000010143500ull; }
    inline uint32 GetRPXHashBase() { return 0; }
    inline bool IsTitleRunning() { return true; }
    std::string GetMlcStoragePath(uint64 titleId);
}
extern std::atomic_bool g_isGPUInitFinished;
