#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "hle/hle.h"
#include "hle/swkbd.h"
#include "input/input.h"
#include "os/sched.h"

namespace wf::hle {
namespace {

constexpr uint32_t kStateBlank = 0, kStateDisplayed = 2;
constexpr size_t kMaxLength = 500;

constexpr uint32_t kKeyboardArgReceiver = 0xA8;
constexpr uint32_t kAppearArgInitialText = 0xC8, kAppearArgMaxLength = 0xD0;

std::mutex g_lock;
bool g_active = false, g_keyboardOnly = false, g_decided = false;
std::u16string g_text;
size_t g_maxLength = kMaxLength;
uint32_t g_receiver = 0, g_stringBuf = 0, g_stringBufSize = 0;
std::u16string g_pendingText;
int g_pendingBackspaces = 0;
bool g_pendingDecide = false, g_prevPlus = false;
uint32_t g_formBuffer = 0, g_changeParam = 0;
int g_calcFont = 0, g_calcPredict = 0;

std::u16string default_name()
{
    std::string name = std::getenv("WF_NAME") ? std::getenv("WF_NAME") : "Link";
    return std::u16string(name.begin(), name.end());
}

void sleep_ticks(PPCContext& ctx, uint64_t ns)
{
    os::LockGuard g;
    os::WaitQueue none;
    os::block(os::current(ctx), &none, os::now_ns() + ns);
}

struct Notify { uint32_t fn = 0, receiver = 0; };
Notify publish()
{
    if (g_stringBuf && g_stringBufSize > 1)
    {
        size_t n = std::min<size_t>(g_text.size(), g_stringBufSize - 1);
        for (size_t i = 0; i < n; i++) wr16(g_stringBuf + 2 * (uint32_t)i, g_text[i]);
        wr16(g_stringBuf + 2 * (uint32_t)n, 0);
    }
    if (!g_receiver) return {};
    uint32_t vtable = rd32(g_receiver);
    return {vtable ? rd32(vtable + 0x0C) : 0, g_receiver};
}

void notify(PPCContext& ctx, Notify n)
{
    if (!n.fn) return;
    if (!g_changeParam) g_changeParam = sys_alloc(8, 4);
    wr32(g_changeParam, 0);
    wr32(g_changeParam + 4, 0);
    call_guest(ctx, n.fn, {n.receiver, g_changeParam});
}

size_t max_length()
{
    if (g_keyboardOnly) return g_stringBufSize > 1 ? g_stringBufSize - 1 : 0;
    return g_maxLength;
}

void appear(std::u16string initial)
{
    g_active = true;
    g_decided = false;
    g_text = std::move(initial);
    if (g_text.size() > max_length()) g_text.resize(max_length());
    g_pendingText.clear();
    g_pendingBackspaces = 0;
    g_pendingDecide = false;
    g_prevPlus = (input::read().buttons & input::kPlus) != 0;
    input::set_text_capture(true);
    log_info("swkbd: keyboard shown (type a name, Enter to confirm)");
}

uint32_t SwkbdCreate(PPCContext& ctx, uint32_t, uint32_t, uint32_t, uint32_t)
{
    sleep_ticks(ctx, 50000000);
    std::lock_guard lk(g_lock);
    g_calcFont = g_calcPredict = 3;
    return 1;
}
WF_EXPORT_AS(swkbd, SwkbdCreate__3RplFPUcQ3_2nn5swkbd10RegionTypeUiP8FSClient, SwkbdCreate);

uint32_t SwkbdGetState()
{
    std::lock_guard lk(g_lock);
    return g_active ? kStateDisplayed : kStateBlank;
}
WF_EXPORT_AS(swkbd, SwkbdGetStateKeyboard__3RplFv, SwkbdGetState);
WF_EXPORT_AS(swkbd, SwkbdGetStateInputForm__3RplFv, SwkbdGetState);

uint32_t SwkbdSetReceiver(uint32_t receiverArg)
{
    std::lock_guard lk(g_lock);
    g_receiver = rd32(receiverArg);
    g_stringBuf = rd32(receiverArg + 4);
    g_stringBufSize = rd32(receiverArg + 8);
    return 0;
}
WF_EXPORT_AS(swkbd, SwkbdSetReceiver__3RplFRCQ3_2nn5swkbd11ReceiverArg, SwkbdSetReceiver);

uint32_t SwkbdAppearKeyboard(PPCContext& ctx, uint32_t keyboardArg)
{
    std::unique_lock lk(g_lock);
    g_keyboardOnly = true;
    g_receiver = rd32(keyboardArg + kKeyboardArgReceiver);
    g_stringBuf = rd32(keyboardArg + kKeyboardArgReceiver + 4);
    g_stringBufSize = rd32(keyboardArg + kKeyboardArgReceiver + 8);
    std::u16string initial;
    for (uint32_t i = 0; g_stringBuf && i + 1 < g_stringBufSize; i++)
    {
        char16_t c = rd16(g_stringBuf + 2 * i);
        if (!c) break;
        initial.push_back(c);
    }
    appear(initial.empty() ? default_name() : initial);
    Notify n = publish();
    lk.unlock();
    notify(ctx, n);
    return 1;
}
WF_EXPORT_AS(swkbd, SwkbdAppearKeyboard__3RplFRCQ3_2nn5swkbd11KeyboardArg, SwkbdAppearKeyboard);

uint32_t SwkbdAppearInputForm(uint32_t appearArg)
{
    std::lock_guard lk(g_lock);
    g_keyboardOnly = false;
    uint32_t maxLen = rd32(appearArg + kAppearArgMaxLength);
    g_maxLength = maxLen == 0 ? kMaxLength : std::min<size_t>(maxLen, kMaxLength);
    std::u16string initial;
    for (uint32_t p = rd32(appearArg + kAppearArgInitialText), i = 0; p && i < g_maxLength; i++)
    {
        char16_t c = rd16(p + 2 * i);
        if (!c) break;
        initial.push_back(c);
    }
    appear(initial.empty() ? default_name() : initial);
    return 1;
}
WF_EXPORT_AS(swkbd, SwkbdAppearInputForm__3RplFRCQ3_2nn5swkbd9AppearArg, SwkbdAppearInputForm);

uint32_t SwkbdDisappear()
{
    std::lock_guard lk(g_lock);
    g_active = false;
    input::set_text_capture(false);
    return 1;
}
WF_EXPORT_AS(swkbd, SwkbdDisappearInputForm__3RplFv, SwkbdDisappear);
WF_EXPORT_AS(swkbd, SwkbdDisappearKeyboard__3RplFv, SwkbdDisappear);

uint32_t SwkbdGetInputFormString()
{
    std::lock_guard lk(g_lock);
    if (!g_formBuffer) g_formBuffer = sys_alloc(2 * (kMaxLength + 1), 4);
    for (size_t i = 0; i < g_text.size(); i++) wr16(g_formBuffer + 2 * (uint32_t)i, g_text[i]);
    wr16(g_formBuffer + 2 * (uint32_t)g_text.size(), 0);
    return g_formBuffer;
}
WF_EXPORT_AS(swkbd, SwkbdGetInputFormString__3RplFv, SwkbdGetInputFormString);

uint32_t SwkbdIsDecideOkButton(uint32_t)
{
    std::lock_guard lk(g_lock);
    return g_decided;
}
WF_EXPORT_AS(swkbd, SwkbdIsDecideOkButton__3RplFPb, SwkbdIsDecideOkButton);
uint32_t SwkbdIsDecideCancelButton(uint32_t) { return 0; }
WF_EXPORT_AS(swkbd, SwkbdIsDecideCancelButton__3RplFPb, SwkbdIsDecideCancelButton);

void SwkbdCalc(PPCContext& ctx, uint32_t)
{
    std::unique_lock lk(g_lock);
    if (!g_active) return;
    bool plus = (input::read().buttons & input::kPlus) != 0;
    bool decide = g_pendingDecide || (plus && !g_prevPlus);
    g_prevPlus = plus;
    bool changed = false;
    for (; g_pendingBackspaces > 0; g_pendingBackspaces--)
        if (!g_text.empty()) { g_text.pop_back(); changed = true; }
    for (char16_t c : g_pendingText)
        if (g_text.size() < max_length()) { g_text.push_back(c); changed = true; }
    g_pendingText.clear();
    g_pendingDecide = false;
    if (decide && !g_text.empty())
    {
        g_decided = true;
        log_info("swkbd: confirmed");
    }
    if (!changed) return;
    Notify n = publish();
    lk.unlock();
    notify(ctx, n);
}
WF_EXPORT_AS(swkbd, SwkbdCalc__3RplFRCQ3_2nn5swkbd14ControllerInfo, SwkbdCalc);

uint32_t SwkbdInitLearnDic(uint32_t) { return 1; }
WF_EXPORT_AS(swkbd, SwkbdInitLearnDic__3RplFPv, SwkbdInitLearnDic);

uint32_t SwkbdIsNeedCalcSubThreadFont() { std::lock_guard lk(g_lock); return g_calcFont > 0; }
WF_EXPORT_AS(swkbd, SwkbdIsNeedCalcSubThreadFont__3RplFv, SwkbdIsNeedCalcSubThreadFont);
uint32_t SwkbdIsNeedCalcSubThreadPredict() { std::lock_guard lk(g_lock); return g_calcPredict > 0; }
WF_EXPORT_AS(swkbd, SwkbdIsNeedCalcSubThreadPredict__3RplFv, SwkbdIsNeedCalcSubThreadPredict);
void SwkbdCalcSubThreadFont(PPCContext& ctx)
{
    {
        std::lock_guard lk(g_lock);
        if (g_calcFont == 0) return;
        g_calcFont--;
    }
    sleep_ticks(ctx, 5000000);
}
WF_EXPORT_AS(swkbd, SwkbdCalcSubThreadFont__3RplFv, SwkbdCalcSubThreadFont);
void SwkbdCalcSubThreadPredict(PPCContext& ctx)
{
    {
        std::lock_guard lk(g_lock);
        if (g_calcPredict == 0) return;
        g_calcPredict--;
    }
    sleep_ticks(ctx, 5000000);
}
WF_EXPORT_AS(swkbd, SwkbdCalcSubThreadPredict__3RplFv, SwkbdCalcSubThreadPredict);

uint32_t SwkbdGetDrawStringInfo(uint32_t info)
{
    for (uint32_t i = 0; i < 6; i++) wr32(info + 4 * i, 0xFFFFFFFF);
    wr8(info + 0x18, 0);
    return 0;
}
WF_EXPORT_AS(swkbd, SwkbdGetDrawStringInfo__3RplFPQ3_2nn5swkbd14DrawStringInfo, SwkbdGetDrawStringInfo);

}

bool swkbd_active()
{
    std::lock_guard lk(g_lock);
    return g_active;
}

void swkbd_text(const char* utf8)
{
    std::lock_guard lk(g_lock);
    if (!g_active) return;
    for (const unsigned char* p = (const unsigned char*)utf8; *p;)
    {
        uint32_t c = *p++;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        c &= extra ? (0x3F >> extra) : 0x7F;
        for (; extra > 0 && (*p & 0xC0) == 0x80; extra--) c = (c << 6) | (*p++ & 0x3F);
        if (c >= 0x20 && c < 0x10000) g_pendingText.push_back((char16_t)c);
    }
}

void swkbd_backspace()
{
    std::lock_guard lk(g_lock);
    if (g_active) g_pendingBackspaces++;
}

void swkbd_enter()
{
    std::lock_guard lk(g_lock);
    if (g_active) g_pendingDecide = true;
}

}
