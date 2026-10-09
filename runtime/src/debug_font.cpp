// The debug font's text measures (flfnt, RMHP08), on host pointers into guest
// memory. The font work is *(0x80796D98): with its +0x30 clear, control codes
// (below 0x10) take 2 columns and ASCII 1, and only the other characters are
// looked up; with it set every character is. The glyph table at 0x80580818
// holds {u16 code, u16 narrow} pairs up to the code 0xFFFF; an index from the
// font's +0x1C on counts as narrow (1).
//
//   0x8005BF84 FontGlyphIndex(code): the code's index in the table, else '?''s
//   0x8005B83C FontTextColumns(text): the columns of a UTF-8 text, newlines
//              skipped; a looked-up glyph takes 2 when its narrow is 0
//   0x8005B8DC flKnjMsgNumPtr(text, columns): the text after its first
//              `columns` columns (with the character crossing them), 0 if it
//              ends before
//   0x8005BFCC FontGlyphColumns(text): its first character's columns, 1 for a
//              narrow of exactly 1, else 2
//
// The original walked the table from its first code for every character: on
// the MH3SP notice screen flKnjMsgNumPtr took 37.7% of the game thread (devkit
// oct08-tls6). A map built from the table on first use answers a code at once;
// no code writes the table. The characters are read as 0x8005BDA0 reads them:
// 1 to 3 UTF-8 bytes, 0xFFFFFFFF for a malformed sequence (not in the table).
// MH3_PORT_CHECK=flKnjMsgNumPtr,FontTextColumns,FontGlyphColumns,FontGlyphIndex
// checks every call.
#include "abi_bridge.h"
#include "fatal.h"
#include "guest_flat_memory.h"
#include "memory.h"
#include "port_check.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>

extern "C" void func_8005B83C(CpuContext* ctx);
extern "C" void func_8005B8DC(CpuContext* ctx);
extern "C" void func_8005BF84(CpuContext* ctx);
extern "C" void func_8005BFCC(CpuContext* ctx);

namespace {

constexpr uint32_t kFontWork = 0x80796D98, kGlyphs = 0x80580818;
constexpr uint8_t kAbsent = 0xFF;

// Code -> table index (kAbsent: none), built on first use.
uint8_t glyphIndex[0x10000];
std::atomic<int> glyphState{0}; // 0 unbuilt, 1 building, 2 built

const uint8_t* GlyphMap() {
    if (glyphState.load(std::memory_order_acquire) == 2) [[likely]]
        return glyphIndex;

    int unbuilt = 0;

    if (!glyphState.compare_exchange_strong(unbuilt, 1, std::memory_order_acquire)) {
        while (glyphState.load(std::memory_order_acquire) != 2) {
        }

        return glyphIndex;
    }

    std::memset(glyphIndex, kAbsent, sizeof(glyphIndex));

    for (uint32_t i = 0;; ++i) {
        const uint16_t code = Memory::Read16(kGlyphs + i * 4);

        if (code == 0xFFFF)
            break;

        if (i >= kAbsent)
            MH3_THROW(std::runtime_error("Debug font glyph table exceeds its native map"));

        // The original returns the first match.
        if (glyphIndex[code] == kAbsent)
            glyphIndex[code] = uint8_t(i);
    }

    // Without '?' the original searches forever.
    if (glyphIndex['?'] == kAbsent)
        MH3_THROW(std::runtime_error("Debug font glyph table has no '?'"));

    glyphState.store(2, std::memory_order_release);
    return glyphIndex;
}

uint32_t GlyphIndex(const uint8_t* map, uint32_t code) {
    const uint8_t index = code <= 0xFFFF ? map[code] : kAbsent;
    return index != kAbsent ? index : map['?'];
}

// 0x8005C010: the glyph's narrow, 1 past the font's glyphs.
uint32_t GlyphNarrow(uint32_t font, uint32_t index) {
    return index < Memory::Read32(font + 0x1C) ? Memory::Read16(kGlyphs + index * 4 + 2) : 1;
}

// 0x8005BDA0: the next character, moving p past the bytes it read (a NUL
// ending a sequence stays unread).
uint32_t Utf8Next(const uint8_t*& p) {
    const uint32_t lead = *p++;

    if (lead < 0x80)
        return lead;

    const uint32_t length = lead - 0xC0 < 0x20 ? 2 : lead - 0xE0 < 0x10 ? 3 : 0;

    if (!length)
        return 0xFFFFFFFF;

    uint32_t code = lead & (length == 2 ? 0x1F : 0x0F);

    for (uint32_t i = 1; i < length; ++i) {
        const uint32_t next = *p;

        if (!next)
            return 0xFFFFFFFF;

        ++p;

        if (next - 0x80 > 0x3F)
            return 0xFFFFFFFF;

        code = code << 6 | (next & 0x3F);
    }

    return code;
}

// The columns of a character other than NUL and newline.
uint32_t Columns(uint32_t font, const uint8_t* map, uint32_t code) {
    if (!Memory::Read32(font + 0x30)) {
        if (int32_t(code) <= 0x0F)
            return 2;

        if (int32_t(code) <= 0x7F)
            return 1;
    }

    return GlyphNarrow(font, GlyphIndex(map, code)) == 0 ? 2 : 1;
}

uint32_t TextColumns(uint32_t text) {
    const uint32_t font = Memory::Read32(kFontWork);
    const uint8_t* map = GlyphMap();
    const uint8_t* p = GuestFlat::Host(text);
    uint32_t columns = 0;

    while (const uint32_t code = Utf8Next(p))
        if (code != '\n')
            columns += Columns(font, map, code);

    return columns;
}

uint32_t TextAfterColumns(uint32_t text, int32_t limit) {
    const uint32_t font = Memory::Read32(kFontWork);
    const uint8_t* map = GlyphMap();
    const uint8_t* start = GuestFlat::Host(text);
    const uint8_t* p = start;
    int32_t columns = 0;

    while (columns < limit) {
        const uint32_t code = Utf8Next(p);

        if (!code)
            return 0;

        if (code != '\n')
            columns += int32_t(Columns(font, map, code));
    }

    return text + uint32_t(p - start);
}

uint32_t GlyphColumns(uint32_t text) {
    const uint32_t font = Memory::Read32(kFontWork);
    const uint8_t* p = GuestFlat::Host(text);
    return GlyphNarrow(font, GlyphIndex(GlyphMap(), Utf8Next(p))) == 1 ? 1 : 2;
}

[[maybe_unused]] void GlyphIndexPort(CpuContext* cpu) {
    cpu->gpr[3] = GlyphIndex(GlyphMap(), cpu->gpr[3]);
}

[[maybe_unused]] void TextColumnsPort(CpuContext* cpu) {
    cpu->gpr[3] = TextColumns(cpu->gpr[3]);
}

[[maybe_unused]] void TextAfterColumnsPort(CpuContext* cpu) {
    cpu->gpr[3] = TextAfterColumns(cpu->gpr[3], int32_t(cpu->gpr[4]));
}

[[maybe_unused]] void GlyphColumnsPort(CpuContext* cpu) {
    cpu->gpr[3] = GlyphColumns(cpu->gpr[3]);
}

} // namespace

// Precise natives: translated callers call them directly.
extern "C" void MH3_FontGlyphIndex(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::RunPurePrecise("FontGlyphIndex", ctx->gpr[1], PortCheck::Returns::Gpr3, func_8005BF84, GlyphIndexPort)) [[unlikely]]
        return;
#endif
    ctx->gpr[3] = GlyphIndex(GlyphMap(), ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8005BF84, MH3_FontGlyphIndex, "FontGlyphIndex");

extern "C" void MH3_FontTextColumns(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::RunPurePrecise("FontTextColumns", ctx->gpr[1], PortCheck::Returns::Gpr3, func_8005B83C, TextColumnsPort)) [[unlikely]]
        return;
#endif
    ctx->gpr[3] = TextColumns(ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8005B83C, MH3_FontTextColumns, "FontTextColumns");

extern "C" void MH3_FlKnjMsgNumPtr(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::RunPurePrecise("flKnjMsgNumPtr", ctx->gpr[1], PortCheck::Returns::Gpr3, func_8005B8DC, TextAfterColumnsPort)) [[unlikely]]
        return;
#endif
    ctx->gpr[3] = TextAfterColumns(ctx->gpr[3], int32_t(ctx->gpr[4]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8005B8DC, MH3_FlKnjMsgNumPtr, "flKnjMsgNumPtr");

extern "C" void MH3_FontGlyphColumns(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::RunPurePrecise("FontGlyphColumns", ctx->gpr[1], PortCheck::Returns::Gpr3, func_8005BFCC, GlyphColumnsPort)) [[unlikely]]
        return;
#endif
    ctx->gpr[3] = GlyphColumns(ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8005BFCC, MH3_FontGlyphColumns, "FontGlyphColumns");
