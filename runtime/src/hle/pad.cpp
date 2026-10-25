#include <bit>
#include <mutex>

#include "core/log.h"
#include "hle/hle.h"
#include "input/input.h"
#include "os/sched.h"

namespace wf::hle {
namespace {

constexpr uint32_t kDevURCC = 31, kDevNotFound = 253;
constexpr uint8_t kDataFormatURCC = 22;
constexpr int32_t kWpadErrNone = 0, kWpadErrNoController = -1;
constexpr int32_t kKpadErrNone = 0, kKpadErrNoSampleData = -1, kKpadErrNoController = -2;
constexpr int32_t kVpadErrNoController = -2;

constexpr uint32_t kKpadDevType = 0x5C, kKpadWpadErr = 0x5D, kKpadDataFormat = 0x5F;
constexpr uint32_t kKpadUcHold = 0x60, kKpadUcTrig = 0x64, kKpadUcRelease = 0x68;
constexpr uint32_t kKpadUcLStick = 0x6C, kKpadUcRStick = 0x74, kKpadUcCharge = 0x7C, kKpadUcCable = 0x80;
constexpr uint32_t kKpadSize = 0xF0;

std::mutex g_lock;
uint32_t g_lastHold = 0;
uint64_t g_lastReadNs = 0;

void wrf(uint32_t addr, float v) { wr32(addr, std::bit_cast<uint32_t>(v)); }

int32_t VPADRead(uint32_t, uint32_t, uint32_t, uint32_t errOut)
{
    if (errOut) wr32(errOut, (uint32_t)kVpadErrNoController);
    return 0;
}
WF_EXPORT(vpad, VPADRead);
void VPADControlMotor(uint32_t, uint32_t, uint32_t) {}
WF_EXPORT(vpad, VPADControlMotor);
void VPADStopMotor(uint32_t) {}
WF_EXPORT(vpad, VPADStopMotor);
uint32_t VPADBASEGetHeadphoneStatus(uint32_t) { return 0; }
WF_EXPORT(vpadbase, VPADBASEGetHeadphoneStatus);

int32_t WPADProbe(uint32_t chan, uint32_t typeOut)
{
    bool present = chan == 0 && input::read().connected;
    if (typeOut) wr32(typeOut, present ? kDevURCC : kDevNotFound);
    return present ? kWpadErrNone : kWpadErrNoController;
}
WF_EXPORT(padscore, WPADProbe);

uint32_t WPADGetBatteryLevel(uint32_t) { return 4; }
WF_EXPORT(padscore, WPADGetBatteryLevel);

void WPADControlMotor(uint32_t chan, uint32_t command)
{
    if (chan == 0) input::rumble(command == 1);
}
WF_EXPORT(padscore, WPADControlMotor);

void KPADInitEx(uint32_t, uint32_t) {}
WF_EXPORT(padscore, KPADInitEx);
uint32_t KPADGetMplsWorkSize() { return 0x5FE0; }
WF_EXPORT(padscore, KPADGetMplsWorkSize);
void KPADSetMplsWorkarea(uint32_t) {}
WF_EXPORT(padscore, KPADSetMplsWorkarea);

int32_t KPADReadEx(uint32_t chan, uint32_t buf, uint32_t length, uint32_t errOut)
{
    auto fail = [&](int32_t err) {
        if (errOut) wr32(errOut, (uint32_t)err);
        return 0;
    };
    input::ProState s = input::read();
    if (chan != 0 || !s.connected)
        return fail(kKpadErrNoController);
    std::lock_guard lk(g_lock);
    uint64_t now = os::now_ns();
    if (length == 0 || now - g_lastReadNs < 1000000)
        return fail(kKpadErrNoSampleData);
    g_lastReadNs = now;

    memset(gptr(buf), 0, kKpadSize);
    wr8(buf + kKpadDevType, (uint8_t)kDevURCC);
    wr8(buf + kKpadWpadErr, 0);
    wr8(buf + kKpadDataFormat, kDataFormatURCC);
    wr32(buf + kKpadUcHold, s.buttons);
    wr32(buf + kKpadUcTrig, s.buttons & ~g_lastHold);
    wr32(buf + kKpadUcRelease, g_lastHold & ~s.buttons);
    g_lastHold = s.buttons;
    wrf(buf + kKpadUcLStick, s.lx);
    wrf(buf + kKpadUcLStick + 4, s.ly);
    wrf(buf + kKpadUcRStick, s.rx);
    wrf(buf + kKpadUcRStick + 4, s.ry);
    wr32(buf + kKpadUcCharge, 0);
    wr32(buf + kKpadUcCable, 1);
    if (errOut) wr32(errOut, kKpadErrNone);
    return 1;
}
WF_EXPORT(padscore, KPADReadEx);

}
}
