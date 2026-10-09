// THPVideoDecode (0x804E17A0) for the movies. The SDK decoder runs its
// paired-single IDCT through the translated FPU path, about 320 ms per
// 640x480 frame on the Vita, so movies such as the opening after character
// creation crawl at a few frames per second. The native decoder writes the
// same tiled Y/U/V planes (runtime/native/mh3_thp_video.h). On the Vita the
// hardware JPEG decoder decodes them (vita/thp_jpeg.h), ahead of the game
// from the movie reader's ring, with the software decoder for anything it
// refuses.
#include "abi_bridge.h"
#include "guest_span.h"
#include "guest_call_trace.h"
#include "memory.h"
#include "../native/mh3_thp_video.h"
#ifdef __vita__
#include "../vita/thp_jpeg.h"
#endif

namespace {

#ifdef __vita__
// The movie player (0x806D6B80): its component count and types (0: video).
constexpr uint32_t kMovieComponents = 0x806D6C0C, kMovieComponentTypes = 0x806D6C10;

// The video frame's size in bytes, 0 when unknown. The player's frame buffer
// holds the next and previous frame sizes, one size per component, then the
// components' data: a movie with only video passes its data right after its
// size (MovieDecodeFrame 0x80434FFC, every movie on the disc).
uint32_t VideoBytes(uint32_t file) {
    if (Memory::Read32(kMovieComponents) != 1 || Memory::Read8(kMovieComponentTypes) != 0)
        return 0;

    const uint32_t bytes = Memory::Read32(file - 4);
    return bytes <= 256 * 1024 && GuestFlat::HostPointer(file, bytes) ? bytes : 0;
}

// The movie reader's ring of frame buffers (DecodeFrame 0x80434FFC): the
// index of the frame being decoded, then 10 entries {buffer, frame, ready}.
constexpr uint32_t kRingIndex = 0x806D6C44, kRing = 0x806D6C54, kRingEntries = 10, kRingEntryBytes = 12;

// The frames the reader has read after `file`, in order, up to `count`.
size_t FramesAhead(uint32_t file, VitaThpFrame* ahead, size_t count) {
    const uint32_t index = Memory::Read32(kRingIndex);

    if (index >= kRingEntries || Memory::Read32(kRing + index * kRingEntryBytes) + 12 != file)
        return 0;

    size_t found = 0;

    for (uint32_t i = 1; i <= count; ++i) {
        const uint32_t entry = kRing + (index + i) % kRingEntries * kRingEntryBytes;

        if (!Memory::Read32(entry + 8))
            break;

        const uint32_t frame = Memory::Read32(entry) + 12;
        const uint32_t bytes = VideoBytes(frame);

        if (!bytes)
            break;

        ahead[found++] = {GuestRange(frame, bytes), bytes};
    }

    return found;
}
#endif

} // namespace

extern "C" void MH3_NativeThpVideoDecode(CpuContext* c) {
    // THPVideoDecode(file, tileY, tileU, tileV, work): the SDK's argument checks.
    const uint32_t file = c->gpr[3], y = c->gpr[4], u = c->gpr[5], v = c->gpr[6], work = c->gpr[7];
    FunctionTraceMovieFrame();

    if (!file) {
        c->gpr[3] = 0x19;
        return;
    }

    if (!y || !u || !v) {
        c->gpr[3] = 0x1b;
        return;
    }

    if (!work) {
        c->gpr[3] = 0x1a;
        return;
    }

    // The frame's size is not an argument: bound the reads by the mapped RAM
    // after it (frames are below 90 KiB; the scan reads zeros past the bound).
    size_t available = 256 * 1024;

    while (available > 4096 && !GuestFlat::HostPointer(file, available))
        available >>= 1;

    mh3_thp_frame frame;
    int result = mh3_thp_parse(GuestRange(file, available), available, &frame);

    if (result == MH3_THP_OK) {
        const size_t luma = size_t(frame.width) * frame.height;
#ifdef __vita__
        const uint32_t bytes = VideoBytes(file);
        VitaThpFrame ahead[2];
        const size_t aheadCount = bytes ? FramesAhead(file, ahead, 2) : 0;

        if (bytes && VitaThpDecode(GuestRange(file, bytes), bytes, frame.width, frame.height, GuestRange(y, luma), GuestRange(u, luma / 4), GuestRange(v, luma / 4),
                                   ahead, aheadCount)) {
            c->gpr[3] = MH3_THP_OK;
            return;
        }
#endif
        result = mh3_thp_decode(&frame, GuestRange(y, luma), GuestRange(u, luma / 4), GuestRange(v, luma / 4));
    }

    c->gpr[3] = uint32_t(result);
}
REGISTER_NATIVE_FUNCTION_AS(0x804E17A0, MH3_NativeThpVideoDecode, "THPVideoDecode");
