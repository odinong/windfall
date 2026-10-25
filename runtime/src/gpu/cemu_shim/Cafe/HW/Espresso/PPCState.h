#pragma once
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/HW/Espresso/Const.h"

union FPR_t
{
    double fpr;
    struct { double fp0; double fp1; };
    struct { uint64 guint; };
    struct { uint64 fp0int; uint64 fp1int; };
};

struct PPCInterpreter_t
{
    uint32 instructionPointer;
    uint32 gpr[32];
    FPR_t fpr[32];
    uint32 fpscr;
    uint8 cr[32];
    uint8 xer_ca, xer_so, xer_ov;
    sint32 remainingCycles, skippedCycles;
    struct { uint32 LR, CTR, XER, UPIR, UGQR[8]; } spr;
    uint32 reservedMemAddr, reservedMemValue;
    void* wfContext;
};

PPCInterpreter_t* PPCInterpreter_getCurrentInstance();
inline uint32 PPCInterpreter_getCoreIndex(PPCInterpreter_t* hCPU) { return hCPU->spr.UPIR; }
uint32 PPCInterpreter_getCurrentCoreIndex();
uint64 PPCInterpreter_getMainCoreCycleCounter();
PPCInterpreter_t* PPCCore_executeCallbackInternal(uint32 functionMPTR);
MPTR PPCInterpreter_makeCallableExportDepr(void (*fn)(PPCInterpreter_t*));
inline void PPCCore_boostQuantum(sint32 numCycles) {}
inline void PPCCore_deboostQuantum(sint32 numCycles) {}
void PPCCore_switchToScheduler();

static uint32 PPCInterpreter_getCallParamU32(PPCInterpreter_t* hCPU, uint32 index)
{
    if (index >= 8)
        return memory_readU32(hCPU->gpr[1] + 8 + (index - 8) * 4);
    return hCPU->gpr[3 + index];
}

static uint64 PPCInterpreter_getCallParamU64(PPCInterpreter_t* hCPU, uint32 index)
{
    uint64 v = ((uint64)PPCInterpreter_getCallParamU32(hCPU, index)) << 32ULL;
    v |= ((uint64)PPCInterpreter_getCallParamU32(hCPU, index + 1));
    return v;
}

#define ppcGetCallParamU32(__index) PPCInterpreter_getCallParamU32(hCPU, __index)
#define ppcGetCallParamU16(__index) ((uint16)(PPCInterpreter_getCallParamU32(hCPU, __index) & 0xFFFF))
#define ppcGetCallParamU8(__index) ((uint8)(PPCInterpreter_getCallParamU32(hCPU, __index) & 0xFF))
#define ppcGetCallParamStruct(__index, __type) ((__type*)memory_getPointerFromVirtualOffsetAllowNull(PPCInterpreter_getCallParamU32(hCPU, __index)))

#define ppcDefineParamU32(__name, __index) uint32 __name = PPCInterpreter_getCallParamU32(hCPU, __index)
#define ppcDefineParamU16(__name, __index) uint16 __name = (uint16)PPCInterpreter_getCallParamU32(hCPU, __index)
#define ppcDefineParamU32BEPtr(__name, __index) uint32be* __name = (uint32be*)((uint8*)memory_getPointerFromVirtualOffsetAllowNull(PPCInterpreter_getCallParamU32(hCPU, __index)))
#define ppcDefineParamS32(__name, __index) sint32 __name = (sint32)PPCInterpreter_getCallParamU32(hCPU, __index)
#define ppcDefineParamU64(__name, __index) uint64 __name = PPCInterpreter_getCallParamU64(hCPU, __index)
#define ppcDefineParamMPTR(__name, __index) MPTR __name = (MPTR)PPCInterpreter_getCallParamU32(hCPU, __index)
#define ppcDefineParamMEMPTR(__name, __type, __index) MEMPTR<__type> __name{PPCInterpreter_getCallParamU32(hCPU, __index)}
#define ppcDefineParamU8(__name, __index) uint8 __name = (PPCInterpreter_getCallParamU32(hCPU, __index) & 0xFF)
#define ppcDefineParamStructPtr(__name, __type, __index) __type* __name = ((__type*)memory_getPointerFromVirtualOffsetAllowNull(PPCInterpreter_getCallParamU32(hCPU, __index)))
#define ppcDefineParamTypePtr(__name, __type, __index) __type* __name = ((__type*)memory_getPointerFromVirtualOffsetAllowNull(PPCInterpreter_getCallParamU32(hCPU, __index)))
#define ppcDefineParamPtr(__name, __type, __index) __type* __name = ((__type*)memory_getPointerFromVirtualOffsetAllowNull(PPCInterpreter_getCallParamU32(hCPU, __index)))
#define ppcDefineParamStr(__name, __index) char* __name = ((char*)memory_getPointerFromVirtualOffsetAllowNull(PPCInterpreter_getCallParamU32(hCPU, __index)))
