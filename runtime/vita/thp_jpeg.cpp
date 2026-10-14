// THP movie frames on the Vita's hardware JPEG decoder, decoded ahead.
//
// A THP video frame is a baseline JPEG whose scan has no 0xFF00 byte
// stuffing. Re-inserting it makes a standard JPEG (bit-identical pictures in
// ffmpeg), which SceJpeg's motion-JPEG decoder takes as is: it decodes on the
// media engine (the Venezia) into planar Y, Cb and Cr, which the game thread
// tiles into the game's GX I8 planes.
//
// SceJpeg lives in SceAvcodecUser, which every application has loaded; the
// import is weak, so a firmware without it only loses the hardware path.
// sceJpegDecodeMJpegYCbCr is declared here with the argument order Vita3K
// (run against commercial games) uses: VitaSDK's psp2/jpeg.h orders the
// output buffer after the decode mode.
//
// The media engine reads the stream and writes the planes by DMA. The
// kernel side (avcodec.skprx)
// takes buffers that are one physical range in CDRAM, or in main memory when
// the decoder was initialized with option 1; any other buffer gives
// SCE_JPEG_ERROR_NOT_PHY_CONTINUOUS_MEMORY (0x80650053). In main memory it
// cleans the stream's and invalidates the planes' cache lines itself.
//
// Measured on the devkit (MH3_THP_BENCH, removed since): a
// 640x480 frame takes the media engine 20 ms even when black and about
// 30 ms in the opening movie, slower than the software decoder's 26 ms. The
// engine runs at 166 MHz (vnz_wrapper); only the kernel sets its clock, and
// _sceCodecEngineChangeNumWorkerCoresMax changed nothing. Cached buffers
// added 11 ms of kernel cache maintenance on the calling thread; uncached
// ones (or CDRAM) none. Two decoders run at once, a pair in 35 ms.
//
// So the hardware decodes ahead of the game: when the game decodes frame N,
// the movie reader has usually read N+1 and N+2 already (native_thp.cpp
// passes them), and two worker threads, one per decoder, decode them into
// uncached buffers while the game runs. The game thread stuffs a frame's
// stream when it hands the frame over, waits only for a frame not done yet
// and tiles the planes into the game's: about 6.4 ms a frame, the game's
// memory bound (tiling on the workers into cached buffers left 5 ms of
// copying their tiles over; 4-byte loads from the uncached planes took
// 9.3 ms for the Y plane alone).
//
// MH3_THP_JPEG=off (development) decodes in software.
#include "thp_jpeg.h"

#include "../native/mh3_thp_video.h"
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <arm_neon.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

struct JpegPitch {
    uint32_t x, y;
};

// SceJpegMJpegInitParam.
struct JpegInitParam {
    uint32_t size;
    int32_t decoders, options;
};

static_assert(sizeof(JpegInitParam) == 0xC);

// SceJpegOutputInfo.
struct JpegOutputInfo {
    int32_t colorSpace;
    uint16_t width, height;
    uint32_t outputSize, tempBufferSize, coefBufferSize;
    JpegPitch pitch[4];
};

static_assert(sizeof(JpegOutputInfo) == 0x34);

} // namespace

extern "C" {
int sceJpegInitMJpegWithParam(const JpegInitParam* param);
int sceJpegGetOutputInfo(const uint8_t* jpeg, SceSize bytes, int format, int mode, JpegOutputInfo* info);
int sceJpegDecodeMJpegYCbCr(const uint8_t* jpeg, SceSize bytes, uint8_t* output, SceSize outputBytes, int mode, void* coef, SceSize coefBytes);
}

namespace {

constexpr int kWithDht = 0;                // SCE_JPEG_MJPEG_WITH_DHT
constexpr int kNoCsc = -1;                 // SCE_JPEG_NO_CSC_OUTPUT: planar Y, Cb, Cr
constexpr size_t kStreamBytes = 512 << 10; // a frame below 256 KiB, stuffed at most twice as long
constexpr size_t kBlockBytes = 1 << 20;    // stream, then the planes
constexpr int kSlots = 2;                  // decoders, each with its worker thread
constexpr int kWorkerPriority = 0x10000100; // SCE_KERNEL_DEFAULT_PRIORITY_USER

// A frame handed to a worker: its buffers, the THP frame it holds and the
// worker's result.
struct Slot {
    SceUID block = -1, thread = -1, start = -1, done = -1;
    uint8_t* stream = nullptr;
    uint8_t* planes = nullptr;

    // The game thread's: the frame it holds (none: free) and whether the
    // worker may still be decoding it.
    const uint8_t* data = nullptr;
    size_t bytes = 0;
    uint32_t sum = 0;
    bool pending = false;

    // In: the stuffed stream's length and the frame's size; out: the decode
    // result, the planes' layout and the time it took.
    size_t length = 0;
    uint32_t width = 0, height = 0;
    int result = 0;
    JpegOutputInfo info{};
    uint64_t decodeUs = 0;
};

struct Decoder {
    bool tried = false, ready = false;
    Slot slots[kSlots];
    uint32_t frames = 0, ahead = 0, failures = 0;
    uint64_t waitUs = 0, stuffUs = 0, tileUs = 0, decodeUs = 0;
};

Decoder decoder;

// The stuffed stream in slot.stream, decoded into slot.planes: the decode
// result, or a negative error. Runs on the slot's worker.
int Decode(Slot& slot) {
    const int queried = sceJpegGetOutputInfo(slot.stream, slot.length, kNoCsc, kWithDht, &slot.info);
    const JpegOutputInfo& info = slot.info;

    // Planar 4:2:0 at the frame's size: Cb and Cr half as wide and high as Y.
    const bool planar420 = info.width == slot.width && info.height == slot.height && info.pitch[0].x >= slot.width && info.pitch[0].y >= slot.height &&
                           info.pitch[1].x * 2 == info.pitch[0].x && info.pitch[1].y * 2 == info.pitch[0].y && info.pitch[2].x == info.pitch[1].x &&
                           info.pitch[2].y == info.pitch[1].y;

    if (queried < 0)
        return queried;

    if (!planar420 || info.outputSize > kBlockBytes - kStreamBytes)
        return -1;

    return sceJpegDecodeMJpegYCbCr(slot.stream, slot.length, slot.planes, info.outputSize, kWithDht, nullptr, 0);
}

// Planar `width` x `height` samples (rows `pitch` apart) into GX I8 tiles:
// 8x4 samples, 32 bytes each, row-major across the plane. Four tiles at a
// time, each row read in one 32-byte load.
void Tile(const uint8_t* source, uint32_t pitch, uint32_t width, uint32_t height, uint8_t* tiles) {
    for (uint32_t ty = 0; ty < height; ty += 4) {
        const uint8_t* rows = source + size_t(ty) * pitch;
        uint32_t tx = 0;

        for (; tx + 32 <= width; tx += 32, tiles += 128) {
            uint8x16x2_t row[4];

            for (uint32_t r = 0; r < 4; ++r)
                row[r] = vld1q_u8_x2(rows + size_t(r) * pitch + tx);

            for (uint32_t half = 0; half < 2; ++half) {
                uint8_t* left = tiles + half * 64;
                vst1q_u8(left, vcombine_u8(vget_low_u8(row[0].val[half]), vget_low_u8(row[1].val[half])));
                vst1q_u8(left + 16, vcombine_u8(vget_low_u8(row[2].val[half]), vget_low_u8(row[3].val[half])));
                vst1q_u8(left + 32, vcombine_u8(vget_high_u8(row[0].val[half]), vget_high_u8(row[1].val[half])));
                vst1q_u8(left + 48, vcombine_u8(vget_high_u8(row[2].val[half]), vget_high_u8(row[3].val[half])));
            }
        }

        for (; tx < width; tx += 8, tiles += 32) {
            for (uint32_t r = 0; r < 4; ++r)
                vst1_u8(tiles + r * 8, vld1_u8(rows + size_t(r) * pitch + tx));
        }
    }
}

int Work(SceSize, void* argument) {
    Slot& slot = **static_cast<Slot**>(argument);

    while (sceKernelWaitSema(slot.start, 1, nullptr) >= 0) {
        const uint64_t start = sceKernelGetProcessTimeWide();
        slot.result = Decode(slot);
        slot.decodeUs = sceKernelGetProcessTimeWide() - start;
        sceKernelSignalSema(slot.done, 1);
    }

    return sceKernelExitDeleteThread(0);
}

// The slot's uncached, physically contiguous buffers, its semaphores and
// its worker (a new thread's cores, 0 and 1).
bool Open(Slot& slot) {
    slot.block = sceKernelAllocMemBlock("MH3 THP JPEG", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, kBlockBytes, nullptr);
    void* base = nullptr;

    if (slot.block < 0 || sceKernelGetMemBlockBase(slot.block, &base) < 0) {
        std::printf("MH3 movie: no memory for the hardware JPEG decoder (%08x); decoding in software\n", unsigned(slot.block));
        return false;
    }

    slot.stream = static_cast<uint8_t*>(base);
    slot.planes = slot.stream + kStreamBytes;
    slot.start = sceKernelCreateSema("MH3 THP start", 0, 0, 1, nullptr);
    slot.done = sceKernelCreateSema("MH3 THP done", 0, 0, 1, nullptr);
    slot.thread = sceKernelCreateThread("MH3 THP decoder", Work, kWorkerPriority, 0x4000, 0,
                                        SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1, nullptr);
    Slot* argument = &slot;

    if (slot.start < 0 || slot.done < 0 || slot.thread < 0 || sceKernelStartThread(slot.thread, sizeof(argument), &argument) < 0) {
        std::printf("MH3 movie: no hardware JPEG decoder thread (%08x %08x %08x); decoding in software\n", unsigned(slot.start), unsigned(slot.done),
                    unsigned(slot.thread));
        return false;
    }

    return true;
}

bool Initialize() {
    decoder.tried = true;
#ifdef MH3_DEVELOPMENT
    const char* mode = std::getenv("MH3_THP_JPEG");

    if (mode && std::strcmp(mode, "off") == 0)
        return false;
#endif
    // Option 1: the buffers may be in main memory.
    const JpegInitParam param{sizeof(JpegInitParam), kSlots, 1};
    const int init = sceJpegInitMJpegWithParam(&param);

    if (init < 0) {
        std::printf("MH3 movie: hardware JPEG decoder unavailable (%08x); decoding in software\n", unsigned(init));
        return false;
    }

    for (Slot& slot : decoder.slots) {
        if (!Open(slot))
            return false;
    }

    std::printf("MH3 movie: hardware JPEG decoder on (%d frames ahead)\n", kSlots);
    return true;
}

// Tells a frame's data apart from what an earlier frame left at the same
// address.
uint32_t Sum(const uint8_t* data, size_t bytes) {
    uint32_t sum = uint32_t(bytes);

    for (size_t i = 0; i + 4 <= bytes; i += 4) {
        uint32_t word;
        std::memcpy(&word, data + i, 4);
        sum = ((sum << 5) | (sum >> 27)) + word;
    }

    return sum;
}

// The THP scan with its 0xFF bytes stuffed (0xFF00) and an EOI, in `out`: a
// standard JPEG. The scan ends at the frame's last EOI (zero padding follows
// it).
size_t Stuff(const uint8_t* data, size_t bytes, size_t scanStart, uint8_t* out) {
    size_t end = bytes;

    while (end >= scanStart + 2 && !(data[end - 2] == 0xFF && data[end - 1] == 0xD9))
        --end;

    if (end < scanStart + 2 || bytes - end > 4)
        return 0;

    end -= 2;
    std::memcpy(out, data, scanStart);
    size_t length = scanStart;

    for (size_t i = scanStart; i < end;) {
        const void* ff = std::memchr(data + i, 0xFF, end - i);
        const size_t run = (ff ? static_cast<const uint8_t*>(ff) + 1 : data + end) - (data + i);

        if (length + run + 3 > kStreamBytes)
            return 0;

        std::memcpy(out + length, data + i, run);
        length += run;
        i += run;

        if (ff)
            out[length++] = 0x00;
    }

    out[length++] = 0xFF;
    out[length++] = 0xD9;
    return length;
}

// Hands the frame to the slot's worker; false when it is no THP frame of
// this size.
bool Submit(Slot& slot, const uint8_t* data, size_t bytes, uint32_t width, uint32_t height) {
    mh3_thp_frame frame;

    if (mh3_thp_parse(data, bytes, &frame) != MH3_THP_OK || frame.width != width || frame.height != height)
        return false;

    const uint64_t start = sceKernelGetProcessTimeWide();
    slot.length = Stuff(data, bytes, size_t(frame.scan - data), slot.stream);

    if (!slot.length)
        return false;

    slot.data = data;
    slot.bytes = bytes;
    slot.sum = Sum(data, bytes);
    slot.width = width;
    slot.height = height;
    slot.pending = true;
    decoder.stuffUs += sceKernelGetProcessTimeWide() - start;
    sceKernelSignalSema(slot.start, 1);
    return true;
}

// Waits for the slot's worker.
void Collect(Slot& slot) {
    if (slot.pending) {
        sceKernelWaitSema(slot.done, 1, nullptr);
        slot.pending = false;
    }
}

void Release(Slot& slot) {
    Collect(slot);
    slot.data = nullptr;
}

Slot* Holding(const uint8_t* data, size_t bytes) {
    for (Slot& slot : decoder.slots) {
        if (slot.data == data && slot.bytes == bytes)
            return &slot;
    }

    return nullptr;
}

Slot* Free() {
    for (Slot& slot : decoder.slots) {
        if (!slot.data)
            return &slot;
    }

    return nullptr;
}

// The frames read ahead that no slot holds yet, into the free slots.
void DecodeAhead(const VitaThpFrame* ahead, size_t count, uint32_t width, uint32_t height) {
    for (size_t i = 0; i < count; ++i) {
        if (Holding(ahead[i].data, ahead[i].bytes))
            continue;

        Slot* slot = Free();

        if (!slot || !Submit(*slot, ahead[i].data, ahead[i].bytes, width, height))
            return;
    }
}

void Report() {
    if (++decoder.frames % 300)
        return;

    const double frames = decoder.frames;
    std::printf("MH3 movie: hardware JPEG %u frames (%u decoded ahead): waited %.2f ms, stuffing %.2f ms, tiling %.2f ms per frame; decode %.2f ms",
                decoder.frames, decoder.ahead, decoder.waitUs / 1000.0 / frames, decoder.stuffUs / 1000.0 / frames, decoder.tileUs / 1000.0 / frames,
                decoder.decodeUs / 1000.0 / frames);
    std::printf("\n");
}

} // namespace

bool VitaThpDecode(const uint8_t* data, size_t bytes, uint32_t width, uint32_t height, uint8_t* y, uint8_t* u, uint8_t* v, const VitaThpFrame* ahead,
                   size_t aheadCount) {
    if (!decoder.tried)
        decoder.ready = Initialize();

    if (!decoder.ready)
        return false;

    const uint64_t start = sceKernelGetProcessTimeWide();
    Slot* slot = Holding(data, bytes);

    // Decoded ahead, unless the address now holds another frame.
    if (slot && slot->sum != Sum(data, bytes)) {
        Release(*slot);
        slot = nullptr;
    }

    if (slot) {
        ++decoder.ahead;
    } else {
        // A first frame or a jump: the frames held are not the ones coming.
        for (Slot& held : decoder.slots)
            Release(held);

        slot = Free();

        if (!Submit(*slot, data, bytes, width, height))
            return false;
    }

    // Frames not handed over yet start before this one tiles.
    DecodeAhead(ahead, aheadCount, width, height);
    Collect(*slot);
    const uint64_t collected = sceKernelGetProcessTimeWide();
    decoder.waitUs += collected - start;
    decoder.decodeUs += slot->decodeUs;

    if (slot->result < 0) {
        const JpegOutputInfo& info = slot->info;

        if (decoder.failures++ < 2)
            std::printf("MH3 movie: hardware JPEG decode failed (%08x): %u bytes stuffed, colour space %x, %ux%u, output %u bytes, pitches %ux%u %ux%u "
                        "%ux%u; decoding in software\n",
                        unsigned(slot->result), unsigned(slot->length), unsigned(info.colorSpace), info.width, info.height, info.outputSize, info.pitch[0].x,
                        info.pitch[0].y, info.pitch[1].x, info.pitch[1].y, info.pitch[2].x, info.pitch[2].y);

        Release(*slot);
        decoder.ready = decoder.failures < 3;
        return false;
    }

    decoder.failures = 0;
    const JpegOutputInfo& info = slot->info;
    const uint8_t* cb = slot->planes + size_t(info.pitch[0].x) * info.pitch[0].y;
    const uint8_t* cr = cb + size_t(info.pitch[1].x) * info.pitch[1].y;
    Tile(slot->planes, info.pitch[0].x, width, height, y);
    Tile(cb, info.pitch[1].x, width / 2, height / 2, u);
    Tile(cr, info.pitch[2].x, width / 2, height / 2, v);
    Release(*slot);
    decoder.tileUs += sceKernelGetProcessTimeWide() - collected;
    DecodeAhead(ahead, aheadCount, width, height);

    Report();
    return true;
}
