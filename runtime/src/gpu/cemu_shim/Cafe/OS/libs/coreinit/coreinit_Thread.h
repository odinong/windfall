#pragma once

struct OSThread_t
{
    enum class THREAD_TYPE : uint32 { TYPE_DRIVER = 0, TYPE_IO = 1, TYPE_APP = 2 };
    enum ATTR_BIT : uint32 { ATTR_AFFINITY_CORE0 = 1, ATTR_AFFINITY_CORE1 = 2, ATTR_AFFINITY_CORE2 = 4, ATTR_DETACHED = 8 };
    uint8 raw[0x6A0];
};

namespace coreinit
{
    struct OSEvent
    {
        enum class EVENT_MODE : uint32 { MODE_MANUAL = 0, MODE_AUTO = 1 };
        enum class EVENT_STATE : uint32 { STATE_NOT_SIGNALED = 0, STATE_SIGNALED = 1 };
         uint32be magic;
         MEMPTR<void> userData;
         uint32be ukn08;
         betype<EVENT_STATE> state;
         uint8 threadQueue[0x10];
         betype<EVENT_MODE> mode;
    };
    static_assert(sizeof(OSEvent) == 0x24);

    struct OSMutex
    {
         uint32 magic;
         MEMPTR<void> userData;
         uint32be ukn08;
         uint8 threadQueue[0x10];
         MEMPTR<OSThread_t> owner;
         sint32be lockCount;
         MEMPTR<OSMutex> next;
         MEMPTR<OSMutex> prev;
    };
    static_assert(sizeof(OSMutex) == 0x2C);

    OSThread_t* OSGetCurrentThread();
    void OSInitMutexEx(OSMutex* mutex, void* name);
    void OSLockMutex(OSMutex* mutex);
    void OSUnlockMutex(OSMutex* mutex);
    void OSExitThread(sint32 exitValue);
    bool OSIsThreadTerminated(OSThread_t* thread);
    void OSSleepTicks(uint64 ticks);
    void OSInitEvent(OSEvent* event, OSEvent::EVENT_STATE initialState, OSEvent::EVENT_MODE mode);
    void OSWaitEvent(OSEvent* event);
    void OSSignalEvent(OSEvent* event);
    void OSSignalEventAllInternal(OSEvent* event);
}
