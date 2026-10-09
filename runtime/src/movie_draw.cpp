// Movie frame draw, RMHP08 (movie.cpp). C reconstruction of the THP player's
// per-frame draw (0x804344F8; Movie_draw_sub 0x804345C0 tail-calls it),
// decompiled with Ghidra (scripts/dol2elf.py -> ELF import).
//
// It stamps the frame with VIGetRetraceCount, decodes the next video frame
// (panicking on a decode error unless the game is resetting), draws it
// centred in the framebuffer and marks the frame as drawn. The reset fade
// (0x800D2230) reads the drawn flag (movie+0x1A4): it steps 1 per frame only
// at a 1-retrace budget with no movie frame drawn, else 2.
#include "abi_bridge.h"
#include "memory.h"
#include <initializer_list>

namespace {

// Movie player state (0x806D6B80).
constexpr uint32_t kMovie = 0x806D6B80;
enum : uint32_t {
    kMovieWidth = 0x00,        // u32 video width
    kMovieHeight = 0x04,       // u32 video height
    kRetraceStamp = 0x10,      // u32 VI retrace count at the last draw
    kDrawResult = 0x14,        // u32 DrawFrame result
    kPendingFlag = 0x18,       // u32 cleared on every draw
    kFrameDrawn = 0x1A4,       // u8
};

constexpr uint32_t kResetting = 0x8065AA65; // u8: reset/fade-out in progress
constexpr uint32_t kSourceFile = 0x80604BE0; // "movie.cpp"
constexpr uint32_t kDecodeError = 0x80604C2C; // "Failed to decode video data"

enum : int32_t {
    kRenderMode = -0x46B8,   // GXRenderModeObj* (r13)
    kRetraceCount = -0x379C, // VI library retrace count (VIGetRetraceCount, inlined)
};

enum : uint32_t {
    DecodeFrame = 0x80434FFC, // returns 1 on a decode error
    DrawFrame = 0x804351C4,   // (rmode, x, y)
    OSPanic = 0x804CDD30,
};

} // namespace

extern "C" void MH3_MovieDrawFrame(CpuContext* ctx) {
    const CpuContext saved = *ctx;
    const uint32_t sda = saved.gpr[13];
    ctx->gpr[1] = (saved.gpr[1] - 0x10) & ~0xFu;
    Memory::Write32(ctx->gpr[1], saved.gpr[1]);
    auto call = [ctx](uint32_t fn, std::initializer_list<uint32_t> ints = {}) {
        uint32_t r = 3;
        for (uint32_t v : ints) ctx->gpr[r++] = v;
        InvokeIndirectCpu(fn, ctx);
        return ctx->gpr[3];
    };

    Memory::Write32(kMovie + kRetraceStamp, Memory::Read32(sda + uint32_t(kRetraceCount)));
    if (Memory::Read8(kResetting) == 0 && call(DecodeFrame) == 1) {
        ctx->cr &= ~(1u << (31 - 6)); // crclr 6: no float varargs
        call(OSPanic, {kSourceFile, 0x11A, kDecodeError});
    }
    const uint32_t rmode = Memory::Read32(sda + uint32_t(kRenderMode));
    const uint32_t x = (Memory::Read16(rmode + 4) - Memory::Read32(kMovie + kMovieWidth)) >> 1;
    const uint32_t y = (Memory::Read16(rmode + 6) - Memory::Read32(kMovie + kMovieHeight)) >> 1;
    Memory::Write32(kMovie + kDrawResult, call(DrawFrame, {rmode, x, y}));
    Memory::Write32(kMovie + kPendingFlag, 0);
    Memory::Write8(kMovie + kFrameDrawn, 1);
    *ctx = saved;
}
REGISTER_NATIVE_FUNCTION_AS(0x804344F8, MH3_MovieDrawFrame, "MovieDrawFrame");
