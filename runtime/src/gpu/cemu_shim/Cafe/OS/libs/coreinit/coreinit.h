#pragma once
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/coreinit_Scheduler.h"

MPTR coreinit_allocFromSysArea(uint32 size, uint32 alignment);

namespace coreinit
{
    uint32 OSGetCoreId();
    uint32 __OSGetProcessSDKVersion();
    void* _weak_MEMAllocFromDefaultHeapEx(uint32 size, sint32 alignment);
    void* _weak_MEMAllocFromDefaultHeap(uint32 size);
    void _weak_MEMFreeToDefaultHeap(void* ptr);
}
