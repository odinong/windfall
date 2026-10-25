#include <atomic>
#include <mutex>
#include <thread>

#include "core/log.h"
#include "hle/hle.h"
#include "os/sched.h"

namespace wf::hle {
namespace {

constexpr uint32_t kNnResultFail = 0xC0A00000;

uint32_t nn_fail() { return kNnResultFail; }
uint32_t nn_ok() { return 0; }
WF_EXPORT_AS(nn_ac, Connect__Q2_2nn2acFv, nn_fail);
WF_EXPORT_AS(nn_ac, Initialize__Q2_2nn2acFv, nn_ok);
WF_EXPORT_AS(nn_olv, Initialize__Q2_2nn3olvFPCQ3_2nn3olv15InitializeParam, nn_fail);
WF_EXPORT_AS(nn_boss, Initialize__Q2_2nn4bossFv, nn_fail);
WF_EXPORT_AS(nn_act, Initialize__Q2_2nn3actFv, nn_ok);
uint8_t act_slot() { return 1; }
WF_EXPORT_AS(nn_act, GetSlotNo__Q2_2nn3actFv, act_slot);
uint32_t act_parental(uint32_t out, uint32_t) { if (out) wr8(out, 1); return 0; }
WF_EXPORT_AS(nn_act, GetParentalControlSlotNoEx__Q2_2nn3actFPUcUc, act_parental);

}
}
