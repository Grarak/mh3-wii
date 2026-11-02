// Chat on its way out, RMHP08. Whatever is said online queues through 0x80451938
// (the city, the party) or 0x80451A00 (one hunter), whoever wrote it: a
// keyboard, a shout-out out of the save, a language pack.
//
// The game trusts its own keyboard, which writes UTF-8 of at most three bytes a
// character, 30 characters in at most 60 bytes. Longer text overruns the queue's
// 0x48-byte entry and the 76-byte buffer in 0x804518A0 on the way out, here, and
// the 63-byte text of a chat log entry (0x80383E38) at the far end, which cuts
// at 61 characters rather than bytes. A character that is not one sizes as 0 in
// 0x8005BEA4, and a walk that advances by that size never ends.
//
// So chat leaves here as the game's keyboard would write it. Anything that is
// not such a character is a "?", which is also what the font draws for what it
// lacks, and text past the limits is cut between characters. Unmodified games
// showed all of this without trouble on the EU and US discs, so this keeps our
// own memory safe and the text to what they were made for.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"

#include <cstdint>

extern "C" void func_80451938(CpuContext* ctx);
extern "C" void func_80451A00(CpuContext* ctx);

namespace {

constexpr uint32_t kBytes = 60, kCharacters = 30, kFrame = 0x60, kText = 0x10;

bool Continues(uint32_t at) {
    return (MemoryInline::FlatRead8(at) & 0xC0) == 0x80;
}

// 0x8005BEA4: the bytes of the character at `text`, 0 if it is not one.
uint32_t CharSize(uint32_t text) {
    const uint32_t lead = MemoryInline::FlatRead8(text);

    if (lead < 0x80)
        return 1;

    if (lead >= 0xC0 && lead < 0xE0)
        return Continues(text + 1) ? 2 : 0;

    if (lead >= 0xE0 && lead < 0xF0)
        return Continues(text + 1) && Continues(text + 2) ? 3 : 0;

    return 0;
}

bool Safe(uint32_t text) {
    uint32_t length = 0;

    for (uint32_t characters = 0;; ++characters) {
        if (!MemoryInline::FlatRead8(text + length))
            return true;

        const uint32_t size = CharSize(text + length);

        if (!size || length + size > kBytes || characters == kCharacters)
            return false;

        length += size;
    }
}

void CopySafe(uint32_t text, uint32_t to) {
    uint32_t length = 0;

    for (uint32_t characters = 0; characters < kCharacters; ++characters) {
        const uint8_t lead = MemoryInline::FlatRead8(text);
        const uint32_t size = CharSize(text);

        if (!lead || length + (size ? size : 1) > kBytes)
            break;

        if (!size) {
            MemoryInline::FlatWrite8(to + length++, '?');
            ++text;
            continue;
        }

        for (uint32_t i = 0; i < size; ++i)
            MemoryInline::FlatWrite8(to + length++, MemoryInline::FlatRead8(text + i));

        text += size;
    }

    MemoryInline::FlatWrite8(to + length, 0);
}

void Post(CpuContext* ctx, void (*post)(CpuContext*)) {
    const uint32_t text = ctx->gpr[4];

    if (!text || Safe(text)) {
        post(ctx);
        return;
    }

    uint32_t result = 0;

    {
        GuestFrame frame(ctx, kFrame);
        CopySafe(text, frame[kText]);
        ctx->gpr[4] = frame[kText];
        post(ctx);
        result = ctx->gpr[3];
    }

    ctx->gpr[3] = result; // the frame has restored the GPRs
}

} // namespace

extern "C" void MH3_ChatPost(CpuContext* ctx) {
    Post(ctx, func_80451938);
}

extern "C" void MH3_ChatPostDirect(CpuContext* ctx) {
    Post(ctx, func_80451A00);
}

REGISTER_NATIVE_FUNCTION_AS(0x80451938, MH3_ChatPost, "ChatPost");
REGISTER_NATIVE_FUNCTION_AS(0x80451A00, MH3_ChatPostDirect, "ChatPostDirect");
