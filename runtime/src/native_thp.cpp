// THPVideoDecode (0x804E17A0) for the movies. The SDK decoder runs its
// paired-single IDCT through the translated FPU path, about 320 ms per
// 640x480 frame on the Vita, so movies such as the opening after character
// creation crawl at a few frames per second. The native decoder writes the
// same tiled Y/U/V planes (runtime/native/mh3_thp_video.h).
#include "abi_bridge.h"
#include "guest_span.h"
#include "guest_call_trace.h"
#include "../native/mh3_thp_video.h"

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
        result = mh3_thp_decode(&frame, GuestRange(y, luma), GuestRange(u, luma / 4), GuestRange(v, luma / 4));
    }
    c->gpr[3] = uint32_t(result);
}
REGISTER_NATIVE_FUNCTION_AS(0x804E17A0, MH3_NativeThpVideoDecode, "THPVideoDecode");
