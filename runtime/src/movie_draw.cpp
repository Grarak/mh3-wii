// Movie frame draw, RMHP08 (movie.cpp). C reconstruction of the THP player's
// per-frame draw (0x804344F8; Movie_draw_sub 0x804345C0 tail-calls it),
// decompiled with Ghidra (scripts/dol2elf.py -> ELF import).
//
// It stamps the frame with VIGetRetraceCount, decodes the next video frame
// (panicking on a decode error unless the game is resetting), draws it
// centred in the framebuffer and marks the frame as drawn. The reset fade
// (0x800D2230) reads the drawn flag (movie+0x1A4): it steps 1 per frame only
// at a 1-retrace budget with no movie frame drawn, else 2.
//
// Each call decoded one video frame, so the video followed the game's frame
// rate while its audio streams in real time. The movies run at 29.97 fps, one
// frame per tick of game time (two retraces): the port decodes the ticks of
// this step (none on a short step: the current frame is drawn again), and
// always the first frame after Movie_open clears the drawn flag. At the stock
// step it decodes one frame per call, as the original.
#include "abi_bridge.h"
#include "game_step.h"
#include "native_effect_draw.h"
#include "port_check.h"
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
    DrawFrame = 0x804351C4,   // (rmode, x, y, width, height)
    OSPanic = 0x804CDD30,
};

} // namespace

static void MovieDrawFrame(CpuContext* ctx) {
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
    const uint32_t frames = Memory::Read8(kMovie + kFrameDrawn) ? GameStep::Ticks() : 1;
    for (uint32_t i = 0; i < frames; ++i) {
        if (Memory::Read8(kResetting) == 0) {
            if (call(DecodeFrame) == 1) {
                ctx->cr &= ~(1u << (31 - 6)); // crclr 6: no float varargs
                call(OSPanic, {kSourceFile, 0x11A, kDecodeError});
            } else {
                NativeMovieFrameDecoded();
            }
        }
    }
    const uint32_t rmode = Memory::Read32(sda + uint32_t(kRenderMode));
    const uint32_t width = Memory::Read32(kMovie + kMovieWidth), height = Memory::Read32(kMovie + kMovieHeight);
    // DrawFrame takes the position as s16 (a taller movie than the EFB starts
    // above it) and the size too.
    const uint32_t x = (Memory::Read16(rmode + 4) - width) >> 1;
    const uint32_t y = (Memory::Read16(rmode + 6) - height) >> 1;
    Memory::Write32(kMovie + kDrawResult, call(DrawFrame, {rmode, x, y, width, height}));
    Memory::Write32(kMovie + kPendingFlag, 0);
    Memory::Write8(kMovie + kFrameDrawn, 1);
    *ctx = saved;
}
extern "C" void func_804344F8(CpuContext* ctx);

extern "C" void MH3_MovieDrawFrame(CpuContext* ctx) {
    PortCheck::Call(ctx, "MovieDrawFrame", func_804344F8, MovieDrawFrame);
}
REGISTER_NATIVE_FUNCTION_AS(0x804344F8, MH3_MovieDrawFrame, "MovieDrawFrame");
