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
//
// The movie players (0x8021F62C, 0x8028C1CC, 0x8028D7DC, 0x803A16F0 and the
// demo module's 0x8097EBE4) open a movie (Movie_open 0x80434344), poll
// Check_movie_finish (0x80434604) each step and close it (Movie_close
// 0x804345C4) when it says the last frame was drawn; Movie_cut (0x80434648)
// closes it early where the player lets a button skip it. The debug port's
// `movie skip` (SkipMovie) makes the next finish check say so, once no frame
// is pending (Movie_cut's condition).
#include "movie_draw.h"

#include "abi_bridge.h"
#include "game_step.h"
#include "native_effect_draw.h"
#include "memory.h"
#include "port_check.h"
#include <initializer_list>

namespace {

// Movie player state (0x806D6B80).
constexpr uint32_t kMovie = 0x806D6B80;
enum : uint32_t {
    kMovieWidth = 0x00,   // u32 video width
    kMovieHeight = 0x04,  // u32 video height
    kRetraceStamp = 0x10, // u32 VI retrace count at the last draw
    kDrawResult = 0x14,   // u32 DrawFrame result
    kPendingFlag = 0x18,  // u32 cleared on every draw
    kFrameCount = 0x70,   // u32 the THP header's frame count
    kOpen = 0xB0,         // u32 nonzero while a movie is open
    kFrameDrawn = 0x1A4,  // u8
};

constexpr uint32_t kResetting = 0x8065AA65;   // u8: reset/fade-out in progress
constexpr uint32_t kSourceFile = 0x80604BE0;  // "movie.cpp"
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

        for (uint32_t v : ints)
            ctx->gpr[r++] = v;

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

extern "C" void MH3_MovieDrawFrame(CpuContext* ctx) {
    MovieDrawFrame(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x804344F8, MH3_MovieDrawFrame, "MovieDrawFrame");

namespace {

#ifdef MH3_DEVELOPMENT
bool skipMovie = false;
#endif

// The open movie's frame count, 0 while none is open (0x804352A4).
uint32_t MovieFrames() {
    return Memory::Read32(kMovie + kOpen) ? Memory::Read32(kMovie + kFrameCount) : 0;
}

// Check_movie_finish: whether the frame drawn last (DrawFrame's result) is
// the movie's last, compared unsigned (never with no movie open).
void CheckMovieFinishPort(CpuContext* ctx) {
    ctx->gpr[3] = Memory::Read32(kMovie + kDrawResult) >= MovieFrames() - 1;
}

} // namespace

extern "C" void func_80434604(CpuContext* ctx);

extern "C" void MH3_MovieFrames(CpuContext* ctx) {
    ctx->gpr[3] = MovieFrames();
}
REGISTER_NATIVE_FUNCTION_AS(0x804352A4, MH3_MovieFrames, "MovieFrames");

extern "C" void MH3_CheckMovieFinish(CpuContext* ctx) {
    PortCheck::Call(ctx, "CheckMovieFinish", func_80434604, CheckMovieFinishPort);
#ifdef MH3_DEVELOPMENT
    // As Movie_cut, only with no frame pending (a read or decode in flight
    // would land in the closed movie's buffers).
    if (skipMovie && Memory::Read32(kMovie + kPendingFlag) == 0) {
        skipMovie = false;
        ctx->gpr[3] = 1;
    }
#endif
}
REGISTER_NATIVE_FUNCTION_AS(0x80434604, MH3_CheckMovieFinish, "Check_movie_finish");
#ifdef MH3_DEVELOPMENT

bool SkipMovie() {
    skipMovie = MovieFrames() != 0;
    return skipMovie;
}
#endif
