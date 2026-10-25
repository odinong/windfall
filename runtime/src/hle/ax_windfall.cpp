#include "Common/precompiled.h"
#include "Cafe/OS/libs/snd_core/ax_internal.h"
#include "audio/IAudioAPI.h"

#include "core/log.h"
#include "hle/cemu_bridge.h"
#include "os/sched.h"

namespace {

constexpr uint64_t kFrameNs = 3000000, kFrameFastNs = 2900000;
constexpr uint64_t kMaxLagNs = 100000000;

wf::os::Thread* g_ist = nullptr;

bool device_wants_more()
{
    std::shared_lock lock(g_audioMutex, std::try_to_lock);
    return lock && ((g_tvAudio && g_tvAudio->NeedAdditionalBlocks()) || (g_padAudio && g_padAudio->NeedAdditionalBlocks()));
}

void ist_thread(wf::os::Thread& self)
{
    wf::cemu::with_context(self.ctx, [&self] {
        snd_core::AXOut_updateDevicePlayState(true);
        uint64_t next = wf::os::now_ns();
        for (;;)
        {
            uint64_t now = wf::os::now_ns();
            if (now < next)
            {
                wf::os::LockGuard g;
                wf::os::WaitQueue none;
                wf::os::block(self, &none, next);
                continue;
            }
            if (now - next > kMaxLagNs)
                next = now;
            next += device_wants_more() ? kFrameFastNs : kFrameNs;
            snd_core::AXIst_RunFrame();
        }
    });
}

}

void wf_ax_start_ist_thread()
{
    if (g_ist)
        return;
    g_ist = wf::os::create_host_thread("AX IST", 14, 0x7, 0x4000, ist_thread);
    wf::log_info("AX started (3 ms frames, {})", g_tvAudio ? "audio output on" : "no audio device");
}

OSThread_t* wf_ax_ist_thread()
{
    return g_ist ? (OSThread_t*)memory_getPointerFromVirtualOffset(g_ist->guest) : nullptr;
}
