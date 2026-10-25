#pragma once
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"

namespace coreinit
{
    struct OSMessage
    {
        uint32be message;
        uint32be data0;
        uint32be data1;
        uint32be data2;
    };

    struct OSMessageQueue
    {
        uint8 raw[0x3C];
    };

    #define OS_MESSAGE_BLOCK 1
    #define OS_MESSAGE_HIGH_PRIORITY 2
}
