#pragma once
#include <cstdint>

#include "wf/ppc.h"

namespace wf::hle {

void mutex_init(uint32_t key);
void mutex_lock(PPCContext& ctx, uint32_t key);
bool mutex_trylock(PPCContext& ctx, uint32_t key);
void mutex_unlock(PPCContext& ctx, uint32_t key);

void event_init(uint32_t e, bool signaled, bool autoReset);
void event_signal(uint32_t e);
void event_signal_all(uint32_t e);
bool event_wait(PPCContext& ctx, uint32_t e, uint64_t deadlineNs = 0);

void start_alarm_thread();

}
