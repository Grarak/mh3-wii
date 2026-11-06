// The Vita's system keyboard (SceImeDialog) in place of the game's, RMHP08.
//
// The game's software keyboard sits behind hooks in system_w, reached through
// three wrappers (the name-entry, search, mail, PIN and chat screens all use
// them, a dozen callers, each a small state machine):
//
//   0x800407FC KeyboardOpen(kind) -> 1 when open (KeyboardOpenLayout)
//   0x80040828 KeyboardBuffer(text, limit): the text to edit and its size;
//              chat has none (0x80449A48)
//   0x800407D4 KeyboardUpdate() -> 0 while typing, 1 done, -1 cancelled
//
// Kinds: 0 a long message, 1 chat (0x81 too), 2 a short name, 3 and 4 codes.
// The limit is the text's size in bytes; the game's text box holds half of that
// in characters, and so does the dialog.
// KeyboardOpen and KeyboardUpdate do not touch the game's layout here, so the
// game goes on drawing and running under the dialog.
//
// What the game's keyboard does on OK (0x8044A3C4) happens here: the text as
// UTF-16 at 0x806E8B08, 0x8044A2D8 folding small roman numerals, 0x804AF140
// converting it to the game's text, with the limit as its capacity. A buffer
// gets the text (a message also goes through 0x80416440, which screens it);
// chat puts it at 0x806E8F08 and posts it to the tab last chosen in the game's
// keyboard, as 0x8044A3C4 does: 0x80451A00 offline, 0x80451938 online.
//
// Elsewhere (Linux, the web) or with the option off, the originals run. A
// development build takes MH3_SYSTEM_KEYBOARD=1 there too, with MH3_KEYBOARD_SCRIPT
// in place of the dialog, so all of this but the dialog itself runs on the host.
#include "system_keyboard.h"
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(MH3_DEVELOPMENT) && !defined(__vita__)
#include <chrono>
#endif

#ifdef __vita__
#include <aurora/aurora.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

// In the SceCommonDialog stub, not in the headers.
extern "C" SceUID sceCommonDialogGetWorkerThreadId();
#endif

extern "C" void func_800407D4(CpuContext* ctx);
extern "C" void func_800407FC(CpuContext* ctx);
extern "C" void func_80040828(CpuContext* ctx);

namespace {

bool g_setting = false;
std::atomic<bool> g_showing{false};

} // namespace

bool SystemKeyboard::Setting() {
    return g_setting;
}

void SystemKeyboard::SetSetting(bool on) {
    g_setting = on;
}

bool SystemKeyboard::Showing() {
    return g_showing.load(std::memory_order_relaxed);
}

#if defined(__vita__) || defined(MH3_DEVELOPMENT)
namespace {

#ifndef __vita__
using SceWChar16 = uint16_t;
#endif

constexpr uint32_t kWideIn = 0x806E8A08u, kWideOut = 0x806E8B08u, kChat = 0x806E8F08u, kChatTarget = 0x806E8F70u;
constexpr uint32_t kWideBytes = 0x400;
constexpr int32_t kTab = -0x405E, kTabTarget = -0x4093; // r13
constexpr uint32_t kMaxText = 256, kChatLimit = 0x3C; // as 0x80449100 sets for chat
constexpr uint32_t kChatColour = 0xF0F0F0FFu, kPartyColour = 0xFFA461FFu, kDirectColour = 0xFFFF50FFu;

enum class State { Idle, Opened, Showing, Closing };

struct Keyboard {
    State state = State::Idle;
    uint32_t kind = 0, buffer = 0, limit = 0, chars = 0;
    bool accepted = false;
    uint64_t opened = 0;
    alignas(64) SceWChar16 initial[kMaxText + 1]{};
    alignas(64) SceWChar16 text[kMaxText + 1]{};
    alignas(64) SceWChar16 title[1]{};
};

Keyboard g_keyboard;

bool Enabled() {
    static const bool forced = [] {
        const char* value = std::getenv("MH3_SYSTEM_KEYBOARD");
        return value && value[0] == '1';
    }();
    return g_setting || forced;
}

bool IsChat(uint32_t kind) {
    return (kind & 0x7F) == 1;
}

// The buffer's text through 0x80455EAC, as 0x80449A48 reads it, for the dialog.
void ReadInitial(GuestCall& game, GuestFrame& frame, Keyboard& k) {
    std::fill(std::begin(k.initial), std::end(k.initial), SceWChar16(0));

    if (!k.buffer || !MemoryInline::FlatRead8(k.buffer))
        return;

    Memory::Write32(frame[8], k.limit);

    for (uint32_t i = 0; i < kWideBytes; i += 4)
        Memory::Write32(kWideIn + i, 0);

    game.Call<0x80455EAC>({kWideIn, frame[8], k.buffer, 0});

    for (uint32_t i = 0; i < k.chars; ++i) {
        const uint16_t c = MemoryInline::FlatRead16(kWideIn + i * 2);

        if (!c)
            break;

        k.initial[i] = c;
    }
}

uint64_t NowMicroseconds() {
#ifdef __vita__
    return sceKernelGetProcessTimeWide();
#else
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

#ifdef MH3_DEVELOPMENT
// A run without hands: a UTF-8 text for each time the keyboard opens, split by
// "|", the last one repeating. "-" cancels. The text is typed after two and a
// half seconds.
void TypeScript(Keyboard& k) {
    static const char* next = [] {
        const char* script = std::getenv("MH3_KEYBOARD_SCRIPT");
        return script ? script : "-";
    }();
    const char* c = next;
    k.accepted = !(c[0] == '-' && (c[1] == '|' || !c[1]));
    uint32_t length = 0;

    while (*c && *c != '|') {
        const uint8_t lead = uint8_t(*c++);
        const uint32_t extra = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
        uint32_t code = extra == 3 ? lead & 7 : extra == 2 ? lead & 15 : extra == 1 ? lead & 31 : lead;

        for (uint32_t i = 0; i < extra && *c; ++i)
            code = code << 6 | (uint8_t(*c++) & 63);

        if (code >= 0x10000 && length + 1 < k.chars) {
            k.text[length++] = SceWChar16(0xD800 + ((code - 0x10000) >> 10));
            k.text[length++] = SceWChar16(0xDC00 + ((code - 0x10000) & 0x3FF));
        } else if (code < 0x10000 && length < k.chars) {
            k.text[length++] = SceWChar16(code);
        }
    }

    k.text[length] = 0;

    if (*c == '|')
        next = c + 1;
}
#endif

#ifdef __vita__
// The dialog's worker draws the keyboard. Left where the system put it, the
// draw thread's and the guest worker's jobs ran 400-550 ms while the keyboard
// was up (cores 1 and 3), and the game fell to 13 fps on the devkit. It gets
// the render thread's core instead, below our threads' priority, so it runs
// in what the render thread leaves; `opened` false reports its time at close.
void PlaceDialogWorker(bool opened) {
    constexpr int kDefaultPriority = 0x10000100; // SCE_KERNEL_DEFAULT_PRIORITY_USER
    const SceUID worker = sceCommonDialogGetWorkerThreadId();
    SceKernelThreadInfo info{};
    info.size = sizeof(info);

    if (worker < 0 || sceKernelGetThreadInfo(worker, &info) < 0) {
        std::fprintf(stderr, "MH3 keyboard: no dialog worker (0x%08x)\n", unsigned(worker));
        return;
    }

    if (!opened) {
        std::fprintf(stderr, "MH3 keyboard: dialog worker ran %llu ms in all\n", (unsigned long long)(info.runClocks / 1000));
        return;
    }

    const int core = sceKernelChangeThreadCpuAffinityMask(worker, SCE_KERNEL_CPU_MASK_USER_0);
    const int priority = sceKernelChangeThreadPriority(worker, kDefaultPriority + 16);
    std::fprintf(stderr, "MH3 keyboard: dialog worker %s, priority 0x%x, cores 0x%x, ran %llu ms: to core 0 (0x%08x), priority 0x%x (0x%08x)\n", info.name, unsigned(info.currentPriority),
                 unsigned(info.currentCpuAffinityMask), (unsigned long long)(info.runClocks / 1000), unsigned(core), unsigned(kDefaultPriority + 16), unsigned(priority));
}
#endif

bool OpenDialog(Keyboard& k) {
#ifdef __vita__
    static bool configured = false;

    if (!configured) {
        SceCommonDialogConfigParam config{};
        sceCommonDialogConfigParamInit(&config);
        sceCommonDialogSetConfigParam(&config);
        configured = true;
    }

    SceImeDialogParam param{};
    sceImeDialogParamInit(&param);
    const uint32_t kind = k.kind & 0x7F;
    param.type = kind >= 3 ? SCE_IME_TYPE_BASIC_LATIN : SCE_IME_TYPE_DEFAULT;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.title = k.title;
    param.maxTextLength = k.chars;
    param.initialText = k.initial;
    param.inputTextBuffer = k.text;

    if (const int error = sceImeDialogInit(&param); error < 0) {
        std::fprintf(stderr, "MH3 keyboard: the system keyboard failed to open (0x%08x)\n", unsigned(error));
        return false;
    }

    PlaceDialogWorker(true);
    aurora_request_common_dialog(true);
#else
    (void)k;
#endif
    return true;
}

bool Show(GuestCall& game, GuestFrame& frame, Keyboard& k) {
    k.limit = std::clamp<uint32_t>(k.limit, 1, kMaxText);
    k.chars = std::max<uint32_t>(k.limit / 2, 1);
    ReadInitial(game, frame, k);
    std::fill(std::begin(k.text), std::end(k.text), SceWChar16(0));

    if (!OpenDialog(k))
        return false;

    g_showing.store(true, std::memory_order_relaxed);
    k.opened = NowMicroseconds();
    std::fprintf(stderr, "MH3 keyboard: kind %u, limit %u\n", unsigned(k.kind), unsigned(k.limit));
    return true;
}

// The dialog is done: k.accepted, and k.text if it was.
bool DialogFinished(Keyboard& k) {
#ifdef __vita__
#ifdef MH3_DEVELOPMENT
    static const bool scripted = std::getenv("MH3_KEYBOARD_SCRIPT") != nullptr;

    if (scripted && NowMicroseconds() - k.opened > 2500000 && sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING)
        sceImeDialogAbort();
#endif

    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return false;

    SceImeDialogResult result{};
    sceImeDialogGetResult(&result);
    k.accepted = result.button == SCE_IME_DIALOG_BUTTON_ENTER;
#ifdef MH3_DEVELOPMENT
    if (scripted)
        TypeScript(k);
#endif
    aurora_request_common_dialog(false);
    return true;
#else
    if (NowMicroseconds() - k.opened <= 2500000)
        return false;

    TypeScript(k);
    return true;
#endif
}

// The present has let the dialog go.
bool DialogClosed() {
#ifdef __vita__
    if (aurora_common_dialog_request_pending())
        return false;

    PlaceDialogWorker(false);
    sceImeDialogTerm();
#endif
    return true;
}

// 0x806E8F08 to the tab the game's keyboard last chose.
void PostChat(GuestCall& game, uint32_t r13) {
    const int32_t tab = int8_t(MemoryInline::FlatRead8(r13 + kTab));

    if (tab >= 2) {
        game.Call<0x80451A00>({uint32_t(tab), kChat, kDirectColour, uint32_t(int32_t(int8_t(MemoryInline::FlatRead8(r13 + kTabTarget)))), kChatTarget});
        return;
    }

    if (game.Call<0x8045169C>() != 0)
        game.Call<0x80451938>({0, kChat, kChatColour, 0});
    else if (tab == 0)
        game.Call<0x80451A00>({0, kChat, kChatColour, 0, 0});
    else if (game.Call<0x8042A634>() == 1)
        game.Call<0x80451938>({1, kChat, kPartyColour, 0});
}

// The text to the game, what 0x8044A3C4 does on OK. A character outside the
// BMP becomes "?", as the font draws it: the game's text is UTF-8 of at most
// three bytes a character, 0x8005BEA4 sizes a longer one as 0, and the copy
// into a shout-out never gets past it.
void Deliver(GuestCall& game, GuestFrame& frame, const Keyboard& k, uint32_t r13) {
    uint32_t length = 0;

    for (uint32_t i = 0; i < kMaxText && k.text[i]; ++i) {
        const uint16_t c = k.text[i];

        if (c >= 0xDC00 && c < 0xE000)
            continue;

        MemoryInline::FlatWrite16(kWideOut + length++ * 2, c >= 0xD800 && c < 0xDC00 ? u'?' : c);
    }

    MemoryInline::FlatWrite16(kWideOut + length * 2, 0);

    if (k.buffer) {
        for (uint32_t i = 0; i <= k.limit; ++i)
            MemoryInline::FlatWrite8(k.buffer + i, 0);
    }

    if (!length) {
        if (!k.buffer)
            game.Call<0x800DBC94>({2}); // sysSE_req: nothing to post

        return;
    }

    game.Call<0x8044A2D8>({kWideOut});
    const uint32_t to = k.buffer ? k.buffer : kChat;
    Memory::Write32(frame[8], k.limit);
    game.Call<0x804AF140>({to, frame[8], kWideOut, 0});
    MemoryInline::FlatWrite8(to + Memory::Read32(frame[8]), 0);

    if (k.buffer && (k.kind & 0x7F) == 0)
        game.Call<0x80416440>({game.Call<0x800E8A3C>(), k.buffer});

    if (!k.buffer && IsChat(k.kind))
        PostChat(game, r13);
}

// 0 while the dialog is up, 1 with the text delivered, -1 cancelled.
int32_t Update(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    Keyboard& k = g_keyboard;
    const uint32_t r13 = frame.Arg(13);

    switch (k.state) {
    case State::Idle:
        return -1;

    case State::Opened:
        if (!k.buffer)
            k.limit = kChatLimit;

        if (!Show(game, frame, k)) {
            k.state = State::Idle;
            return -1;
        }

        k.state = State::Showing;
        return 0;

    case State::Showing:
        if (!DialogFinished(k))
            return 0;

        k.state = State::Closing;
        return 0;

    case State::Closing:
        if (!DialogClosed())
            return 0;

        g_showing.store(false, std::memory_order_relaxed);
        k.state = State::Idle;
        std::fprintf(stderr, "MH3 keyboard: %s\n", k.accepted ? "accepted" : "cancelled");

        if (!k.accepted)
            return -1;

        Deliver(game, frame, k, r13);
        return 1;
    }

    return -1;
}

} // namespace

extern "C" void MH3_KeyboardOpen(CpuContext* ctx) {
    if (!Enabled()) {
        func_800407FC(ctx);
        return;
    }

    Keyboard& k = g_keyboard;

    if (k.state != State::Idle) {
        ctx->gpr[3] = 0;
        return;
    }

    k.kind = ctx->gpr[3] & 0xFF;
    k.buffer = 0;
    k.limit = 0;
    k.state = State::Opened;
    ctx->gpr[3] = 1;
}

extern "C" void MH3_KeyboardBuffer(CpuContext* ctx) {
    if (!Enabled()) {
        func_80040828(ctx);
        return;
    }

    g_keyboard.buffer = ctx->gpr[3];
    g_keyboard.limit = ctx->gpr[4];
}

extern "C" void MH3_KeyboardUpdate(CpuContext* ctx) {
    if (!Enabled()) {
        func_800407D4(ctx);
        return;
    }

    const int32_t result = Update(ctx); // its frame has restored the GPRs
    ctx->gpr[3] = uint32_t(result);
}
#else
extern "C" void MH3_KeyboardOpen(CpuContext* ctx) {
    func_800407FC(ctx);
}

extern "C" void MH3_KeyboardBuffer(CpuContext* ctx) {
    func_80040828(ctx);
}

extern "C" void MH3_KeyboardUpdate(CpuContext* ctx) {
    func_800407D4(ctx);
}
#endif

REGISTER_NATIVE_FUNCTION_AS(0x800407FC, MH3_KeyboardOpen, "KeyboardOpen");
REGISTER_NATIVE_FUNCTION_AS(0x80040828, MH3_KeyboardBuffer, "KeyboardBuffer");
REGISTER_NATIVE_FUNCTION_AS(0x800407D4, MH3_KeyboardUpdate, "KeyboardUpdate");
