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
// Elsewhere (Linux, the web) or with the option off, the originals run.
#include "system_keyboard.h"
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifdef __vita__
#include <aurora/aurora.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/processmgr.h>
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

#ifdef __vita__
namespace {

constexpr uint32_t kWideIn = 0x806E8A08u, kWideOut = 0x806E8B08u, kChat = 0x806E8F08u, kChatTarget = 0x806E8F70u;
constexpr uint32_t kWideBytes = 0x400, kChatBytes = 0x68;
constexpr int32_t kLimitWord = -0x51F4, kTab = -0x405E, kTabTarget = -0x4093; // r13
constexpr uint32_t kMaxText = 256;
constexpr uint32_t kChatColour = 0xF0F0F0FFu, kPartyColour = 0xFFA461FFu, kDirectColour = 0xFFFF50FFu;

enum class State { Idle, Opened, Showing, Closing };

struct Keyboard {
    State state = State::Idle;
    uint32_t kind = 0, buffer = 0, limit = 0;
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

    for (uint32_t i = 0; i < std::min(k.limit, kMaxText); ++i) {
        const uint16_t c = MemoryInline::FlatRead16(kWideIn + i * 2);

        if (!c)
            break;

        k.initial[i] = c;
    }
}

bool Show(GuestCall& game, GuestFrame& frame, Keyboard& k) {
    static bool configured = false;

    if (!configured) {
        SceCommonDialogConfigParam config{};
        sceCommonDialogConfigParamInit(&config);
        sceCommonDialogSetConfigParam(&config);
        configured = true;
    }

    ReadInitial(game, frame, k);
    std::fill(std::begin(k.text), std::end(k.text), SceWChar16(0));
    k.limit = std::clamp<uint32_t>(k.limit, 1, kMaxText);

    SceImeDialogParam param{};
    sceImeDialogParamInit(&param);
    const uint32_t kind = k.kind & 0x7F;
    param.type = kind >= 3 ? SCE_IME_TYPE_BASIC_LATIN : SCE_IME_TYPE_DEFAULT;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.title = k.title;
    param.maxTextLength = k.limit;
    param.initialText = k.initial;
    param.inputTextBuffer = k.text;

    if (const int error = sceImeDialogInit(&param); error < 0) {
        std::fprintf(stderr, "MH3 keyboard: the system keyboard failed to open (0x%08x)\n", unsigned(error));
        return false;
    }

    aurora_request_common_dialog(true);
    g_showing.store(true, std::memory_order_relaxed);
    k.opened = sceKernelGetProcessTimeWide();
    std::fprintf(stderr, "MH3 keyboard: kind %u, limit %u\n", unsigned(k.kind), unsigned(k.limit));
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

// The text to the game, what 0x8044A3C4 does on OK.
void Deliver(GuestCall& game, GuestFrame& frame, const Keyboard& k, uint32_t r13) {
    uint32_t length = 0;

    while (length < kMaxText && k.text[length]) {
        MemoryInline::FlatWrite16(kWideOut + length * 2, k.text[length]);
        ++length;
    }

    MemoryInline::FlatWrite16(kWideOut + length * 2, 0);

    if (k.buffer) {
        for (uint32_t i = 0; i <= k.limit; ++i)
            MemoryInline::FlatWrite8(k.buffer + i, 0);
    }

    if (!length)
        return;

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
        if (!k.buffer) {
            // Chat keeps the limit of the game's last keyboard: at most its buffer.
            const uint32_t limit = Memory::Read32(r13 + kLimitWord);
            k.limit = limit > 0 && limit < kChatBytes ? limit : kChatBytes - 1;
        }

        if (!Show(game, frame, k)) {
            k.state = State::Idle;
            return -1;
        }

        k.state = State::Showing;
        return 0;

    case State::Showing: {
#ifdef MH3_DEVELOPMENT
        // A run without hands: the text typed after two and a half seconds,
        // "-" cancels.
        static const char* const script = std::getenv("MH3_KEYBOARD_SCRIPT");

        if (script && sceKernelGetProcessTimeWide() - k.opened > 2500000 && sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING)
            sceImeDialogAbort();
#endif

        if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
            return 0;

        SceImeDialogResult result{};
        sceImeDialogGetResult(&result);
        k.accepted = result.button == SCE_IME_DIALOG_BUTTON_ENTER;
#ifdef MH3_DEVELOPMENT
        if (script && script[0] != '-') {
            uint32_t i = 0;

            for (; script[i] && i < k.limit; ++i)
                k.text[i] = SceWChar16(uint8_t(script[i]));

            k.text[i] = 0;
            k.accepted = true;
        }
#endif
        aurora_request_common_dialog(false);
        k.state = State::Closing;
        return 0;
    }

    case State::Closing:
        if (aurora_common_dialog_request_pending())
            return 0;

        sceImeDialogTerm();
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
