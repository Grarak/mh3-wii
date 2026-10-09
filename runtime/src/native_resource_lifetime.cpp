// Verified CPU copy/fill/expandable-heap producers. GPU ownership retirement
// is explicit; guest heap split/coalesce and mutex helpers remain ordinary CPU
// dependencies. No resource is certified merely because its address is reused.
#include "abi_bridge.h"
#include "fatal.h"
#include "frame_split.h"
#include "guest_call.h"
#include "guest_worker.h"
#include "native_frame_packet.h"
#include "memory.h"
#include "recomp_mod_loader.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include "runtime/native/mh3_resource_lifetime.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#ifdef MH3_DEVELOPMENT
#include "dev_options.h"
#include <mutex>
#endif

namespace {
void Checked(mh3_lifetime_status status) {
    if (status != MH3_LIFETIME_OK)
        MH3_THROW(std::runtime_error("Native resource lifetime validation failed: " + std::to_string(status)));
}

bool Canonical(uint32_t address, uint32_t bytes, uint32_t* out) {
    return mh3_source_canonical_address(address, bytes, out) == MH3_RES_OK;
}

uint32_t RamAddress(uint32_t address, uint32_t bytes) {
    uint32_t result;

    if (!Canonical(address, bytes, &result))
        MH3_THROW(std::runtime_error("Native heap record is outside MEM1/MEM2"));

    return result;
}

uint32_t Link(uint32_t address) {
    return address ? RamAddress(address, 16) : 0;
}

void Put32(uint8_t* destination, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        destination[i] = uint8_t(value >> (24 - 8 * i));
}

mh3_lifetime_expheap ReadHeap(uint32_t address) {
    const uint32_t canonical = RamAddress(address, 80);
    std::array<uint8_t, 80> record;
    std::memcpy(record.data(), Memory::GetPointer(address, record.size()), record.size());
    const uint32_t begin = Memory::Read32(address + 24), end = Memory::Read32(address + 28);

    if (end < begin || !end)
        MH3_THROW(std::runtime_error("Native heap extent is invalid"));
    // Validate the entire original view, including an end at the RAM boundary.
    const uint32_t first = RamAddress(begin, end - begin);
    const uint32_t last = RamAddress(end - 1, 1) + 1;
    Put32(record.data() + 24, first);
    Put32(record.data() + 28, last);

    for (unsigned offset = 60; offset <= 72; offset += 4)
        Put32(record.data() + offset, Link(Memory::Read32(address + offset)));

    mh3_lifetime_expheap heap;
    Checked(mh3_lifetime_expheap_read(record.data(), record.size(), canonical, &heap));
    return heap;
}

mh3_lifetime_expblock ReadBlock(uint32_t address) {
    mh3_lifetime_expblock block;
    Checked(mh3_lifetime_expblock_read(Memory::GetPointer(address, 16), 16, &block));
    block.previous = Link(block.previous);
    block.next = Link(block.next);
    return block;
}

mh3_lifetime_status ReadBlockCallback(void*, uint32_t address, mh3_lifetime_expblock* out) {
    *out = ReadBlock(address);
    return MH3_LIFETIME_OK;
}

class HeapLock {
  public:
    HeapLock(CpuContext* cpu, uint32_t heap) : call_{cpu}, heap_(heap) {
        Memory::GetPointer(heap, 80);
        locked_ = (Memory::Read32(heap + 56) & 4) != 0;

        if (locked_)
            call_(0x804D25A0, {heap_ + 32});
    }

    ~HeapLock() noexcept(false) {
        if (locked_)
            call_(0x804D2680, {heap_ + 32});
    }

    HeapLock(const HeapLock&) = delete;
    HeapLock& operator=(const HeapLock&) = delete;

  private:
    GuestCall call_;
    uint32_t heap_;
    bool locked_;
};

void RetireWrite(uint32_t address, uint32_t bytes, NativeSourceEpochProducer producer) {
    if (!bytes)
        return;

    uint32_t canonical;

    if (Canonical(address, bytes, &canonical))
        RetireNativeResourceOwner(canonical, bytes, producer);
    else if (address < 0xe0000000u || uint64_t(address) + bytes > 0xe0004000ull)
        MH3_THROW(std::runtime_error("Native write is outside RAM/locked cache"));
}

void PrepareWrite(uint32_t address, uint32_t bytes, NativeSourceEpochProducer producer) {
    RecompMod::CheckExecutableWrite(address, bytes, 0);
    RetireWrite(address, bytes, producer);
}

void CopyBlocks(uint32_t destination, uint32_t source, uint32_t bytes) {
    Checked(mh3_lifetime_validate_copy32(destination, source, bytes));

    if (!bytes)
        return;

    void* dst = Memory::GetPointer(destination, bytes);
    const void* src = Memory::GetPointer(source, bytes);
    RecompMod::CheckExecutableWrite(destination, bytes, 0);
    uint32_t canonical_dst, canonical_src;
    const bool ram_dst = Canonical(destination, bytes, &canonical_dst);
    const bool ram_src = Canonical(source, bytes, &canonical_src);

    if (ram_dst && ram_src) {
        mh3_lifetime_copy_kind kind;
        Checked(mh3_lifetime_forward_copy_kind(canonical_dst, canonical_src, bytes, &kind));

        if (kind == MH3_LIFETIME_COPY_SOURCE)
            RecordNativeResourceCopy(canonical_dst, canonical_src, bytes, NativeSourceEpochProducer::Copy32);
        else if (kind == MH3_LIFETIME_COPY_UNKNOWN)
            RetireNativeResourceOwner(canonical_dst, bytes, NativeSourceEpochProducer::Copy32Unknown);
    } else
        RetireWrite(destination, bytes, NativeSourceEpochProducer::Copy32Unknown);

    Checked(mh3_lifetime_copy32(dst, bytes, src, bytes, bytes));
}

void Fill(uint32_t destination, uint8_t value, uint32_t bytes) {
    if (!bytes)
        return;

    void* pointer = Memory::GetPointer(destination, bytes);
    PrepareWrite(destination, bytes, NativeSourceEpochProducer::Fill);
    Checked(mh3_lifetime_fill(pointer, bytes, value, bytes));
}

uint32_t CreateHeap(CpuContext* cpu, uint32_t address, uint32_t bytes, uint32_t options) {
    mh3_lifetime_range region;
    Checked(mh3_lifetime_expheap_create_plan(address, bytes, &region));

    if (!region.bytes)
        return 0;

    Memory::GetPointer(region.address, region.bytes);
    RamAddress(region.address, region.bytes);
    PrepareWrite(region.address, region.bytes, NativeSourceEpochProducer::HeapCreate);
    GuestFrame frame(cpu, 0x40);
    GuestCall call{cpu};
    call(0x804C1F20, {region.address, 0x45585048, region.address + 80, region.address + region.bytes, options});
    const uint32_t begin = Memory::Read32(region.address + 24), end = Memory::Read32(region.address + 28);
    Checked(mh3_lifetime_expheap_initialize(Memory::GetPointer(region.address, 80), 80, Memory::GetPointer(begin, 16), 16, begin, end));
    return region.address;
}

uint32_t DestroyHeap(CpuContext* cpu, uint32_t address) {
    const auto heap = ReadHeap(address);
    PrepareWrite(heap.address, heap.end - heap.address, NativeSourceEpochProducer::HeapDestroy);
    GuestFrame frame(cpu, 0x40);
    GuestCall{cpu}(0x804C20E0, {address});
    return address;
}

uint32_t Allocate(CpuContext* cpu, uint32_t address, uint32_t bytes, int32_t alignment) {
    GuestFrame frame(cpu, 0x60);
    HeapLock lock(cpu, address);
    GuestCall call{cpu};
    ReadHeap(address);
    const uint32_t result = call(alignment < 0 ? 0x804C2560 : 0x804C2480, {address, mh3_lifetime_expheap_request_size(bytes), mh3_lifetime_alignment_magnitude(alignment)});

    if (result) {
        const auto heap = ReadHeap(address);
        const uint32_t payload = RamAddress(result, 1);
        const auto block = ReadBlock(payload - 16);
        mh3_lifetime_range region;
        Checked(mh3_lifetime_expblock_extent(&heap, payload, &block, &region));

        // The allocator may absorb a short fragment: retire its actual extent,
        // including the header/alignment padding, rather than the request size.
        RetireNativeResourceOwner(region.address, region.bytes, NativeSourceEpochProducer::HeapAllocate);
    }

    return result;
}

#ifdef MH3_DEVELOPMENT
// MH3_TRACE=draw-writes: the game thread's heap frees and memory fills while
// the overlapped draw runs beside it, kept for ReportDrawSideWrites.
struct DrawSideWrite {
    char kind = 0; // 'f' free, 's' fill
    uint32_t begin = 0, bytes = 0, lr = 0, caller = 0;
    uint64_t packet = 0;
};

std::mutex drawSideMutex;
std::array<DrawSideWrite, 4096> drawSideWrites;
size_t drawSideCount = 0;

void RecordDrawSideWrite(CpuContext* cpu, char kind, uint32_t begin, uint32_t bytes) {
    static const bool enabled = TraceOptions().Has("draw-writes");

    if (!enabled || PpcThread::CurrentThread() != PpcThread::Game || !DrawThread::Thread().Pending())
        return;

    const uint32_t sp = Memory::Read32(cpu->gpr[1]);
    const uint32_t caller = sp >= 0x80000000u && sp < 0x94000000u ? Memory::Read32(sp + 4) : 0;
    std::lock_guard lock(drawSideMutex);
    drawSideWrites[drawSideCount++ % drawSideWrites.size()] = {kind, begin, bytes, cpu->lr, caller, NativeFramePacketSequence()};
}
#else
void RecordDrawSideWrite(CpuContext*, char, uint32_t, uint32_t) {}
#endif

// A logic step that runs beside the overlapped draw (game_frame.cpp) must not
// free what the draw still calls into: a model the next step destroys is
// still in the prepared buckets. Such a free first waits for the draw, before
// the heap lock, which the draw thread may need.
void WaitForDrawBeforeFree(CpuContext* cpu, uint32_t payload, uint32_t bytes) {
    if (PpcThread::CurrentThread() != PpcThread::Game || !DrawThread::Thread().Pending() || !SceneQueue::HoldsPrepared(payload, payload + bytes))
        return;

#ifdef MH3_DEVELOPMENT
    // The first few waits with the guest call chain of the free.
    static unsigned reported = 0;

    if (reported < 8) {
        ++reported;
        std::fprintf(stderr, "MH3 draw wait: free of %08x (%u bytes) from %08x", payload, bytes, cpu->lr);
        uint32_t sp = cpu->gpr[1];

        for (unsigned depth = 0; depth < 6 && sp >= 0x80000000u && sp < 0x94000000u; ++depth) {
            sp = Memory::Read32(sp);

            if (sp < 0x80000000u || sp >= 0x94000000u)
                break;

            std::fprintf(stderr, " <- %08x", Memory::Read32(sp + 4));
        }

        std::fprintf(stderr, "\n");
    }
#endif
    DrawThread::Thread().Wait();
}

void Free(CpuContext* cpu, uint32_t address, uint32_t payload) {
    if (!payload)
        return;

    const uint32_t canonical_payload = RamAddress(payload, 1);

    // The caller's block: its size is its own; the links are read under the
    // lock below.
    const uint32_t bytes = ReadBlock(canonical_payload - 16).size;
    RecordDrawSideWrite(cpu, 'f', payload, bytes);
    WaitForDrawBeforeFree(cpu, payload, bytes);
    GuestFrame frame(cpu, 0x60);
    HeapLock lock(cpu, address);
    GuestCall call{cpu};
    const auto heap = ReadHeap(address);
    const auto block = ReadBlock(canonical_payload - 16);
    mh3_lifetime_free_plan plan;
    Checked(mh3_lifetime_expheap_free_plan(&heap, canonical_payload, &block, &plan));

    // Addresses are canonical for bounds/retirement; compatibility link values
    // retain the original cached/uncached/physical pointer bits.
    const uint32_t previous = Memory::Read32(payload - 8), next = Memory::Read32(payload - 4);
    Memory::GetPointer(plan.links[0].address, 4);
    Memory::GetPointer(plan.links[1].address, 4);
    PrepareWrite(plan.retired.address, plan.retired.bytes, NativeSourceEpochProducer::HeapFree);
    Memory::Write32(plan.links[0].address, next);
    Memory::Write32(plan.links[1].address, previous);
    const uint32_t padding = (block.settings >> 8) & 127;
    Memory::Write32(frame[0x20], payload - 16 - padding);
    Memory::Write32(frame[0x24], payload + block.size);
    call(0x804C2630, {address + 60, frame[0x20]});
}

uint32_t Available(CpuContext* cpu, uint32_t address, int32_t alignment) {
    GuestFrame frame(cpu, 0x40);
    HeapLock lock(cpu, address);
    const auto heap = ReadHeap(address);
    uint32_t bytes;
    Checked(mh3_lifetime_expheap_available(&heap, alignment, ReadBlockCallback, nullptr, &bytes));
    return bytes;
}

uint32_t AllocatorAllocate(CpuContext* cpu, uint32_t allocator, uint32_t bytes) {
    GuestFrame frame(cpu, 0x40);
    return GuestCall{cpu}(Memory::Read32(Memory::Read32(allocator)), {allocator, bytes});
}

void AllocatorFree(CpuContext* cpu, uint32_t allocator, uint32_t payload) {
    GuestFrame frame(cpu, 0x40);
    GuestCall{cpu}(Memory::Read32(Memory::Read32(allocator) + 4), {allocator, payload});
}

uint32_t SystemAllocate(CpuContext* cpu, uint32_t bytes) {
    return bytes ? Allocate(cpu, Memory::Read32(0x80796C48), bytes, 8) : 0;
}

void SystemFree(CpuContext* cpu, uint32_t payload) {
    if (payload)
        Free(cpu, Memory::Read32(0x80796C48), payload);
}
} // namespace

void FillNativeGuestMemory(uint32_t destination, uint8_t value, uint32_t bytes) {
    Fill(destination, value, bytes);
}

#ifdef MH3_DEVELOPMENT
// The recorded draw-side frees and fills that cover [begin, end), and the
// last few of all (MH3_TRACE=draw-writes).
void ReportDrawSideWrites(uint32_t begin, uint32_t end) {
    std::lock_guard lock(drawSideMutex);
    const size_t kept = std::min(drawSideCount, drawSideWrites.size());
    std::fprintf(stderr, "MH3 draw-side writes: %zu recorded, packet now %llu\n", drawSideCount, static_cast<unsigned long long>(NativeFramePacketSequence()));

    for (size_t i = drawSideCount - kept; i < drawSideCount; ++i) {
        const auto& w = drawSideWrites[i % drawSideWrites.size()];
        const bool covers = w.begin < end && begin < w.begin + w.bytes;

        if (covers || i + 8 >= drawSideCount)
            std::fprintf(stderr, "MH3 draw-side %s: %c %08x+%x from %08x <- %08x packet %llu\n", covers ? "HIT" : "last", w.kind, w.begin, w.bytes, w.lr, w.caller,
                         static_cast<unsigned long long>(w.packet));
    }
}
#endif

extern "C" void MH3_Copy32Blocks(CpuContext* cpu) {
    CopyBlocks(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}

extern "C" void MH3_Zero32Blocks(CpuContext* cpu) {
    const uint32_t destination = cpu->gpr[3], bytes = cpu->gpr[4];
    Checked(mh3_lifetime_validate_zero32(destination, bytes));

    if (bytes && (cpu->gqr[0] & 7))
        MH3_THROW(std::runtime_error("ZeroMemory32ByteBlocks requires GQR0 float store format"));

    RecordDrawSideWrite(cpu, 's', destination, bytes);
    Fill(destination, 0, bytes);
}

extern "C" void MH3_FillMemory(CpuContext* cpu) {
    RecordDrawSideWrite(cpu, 's', cpu->gpr[3], cpu->gpr[5]);
    Fill(cpu->gpr[3], uint8_t(cpu->gpr[4]), cpu->gpr[5]);
}

extern "C" void MH3_Memset(CpuContext* cpu) {
    const uint32_t destination = cpu->gpr[3];
    RecordDrawSideWrite(cpu, 's', destination, cpu->gpr[5]);
    Fill(destination, uint8_t(cpu->gpr[4]), cpu->gpr[5]);
    cpu->gpr[3] = destination;
}

extern "C" void MH3_CreateExpHeap(CpuContext* cpu) {
    cpu->gpr[3] = CreateHeap(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}

extern "C" void MH3_DestroyExpHeap(CpuContext* cpu) {
    cpu->gpr[3] = DestroyHeap(cpu, cpu->gpr[3]);
}

extern "C" void MH3_AllocExpHeap(CpuContext* cpu) {
    cpu->gpr[3] = Allocate(cpu, cpu->gpr[3], cpu->gpr[4], int32_t(cpu->gpr[5]));
}

extern "C" void MH3_FreeExpHeap(CpuContext* cpu) {
    Free(cpu, cpu->gpr[3], cpu->gpr[4]);
}

extern "C" void MH3_AvailableExpHeap(CpuContext* cpu) {
    cpu->gpr[3] = Available(cpu, cpu->gpr[3], int32_t(cpu->gpr[4]));
}

extern "C" void MH3_AllocatorAllocThunk(CpuContext* cpu) {
    const uint32_t allocator = cpu->gpr[3], bytes = cpu->gpr[4];
    cpu->gpr[3] = Allocate(cpu, Memory::Read32(allocator + 4), bytes, int32_t(Memory::Read32(allocator + 8)));
}

extern "C" void MH3_AllocatorFreeThunk(CpuContext* cpu) {
    const uint32_t allocator = cpu->gpr[3], payload = cpu->gpr[4];
    Free(cpu, Memory::Read32(allocator + 4), payload);
}

extern "C" void MH3_AllocatorAlloc(CpuContext* cpu) {
    cpu->gpr[3] = AllocatorAllocate(cpu, cpu->gpr[3], cpu->gpr[4]);
}

extern "C" void MH3_AllocatorFree(CpuContext* cpu) {
    AllocatorFree(cpu, cpu->gpr[3], cpu->gpr[4]);
}

extern "C" void MH3_WorkMemFree(CpuContext* cpu) {
    const uint32_t payload = cpu->gpr[3];
    GuestFrame frame(cpu, 0x40);
    AllocatorFree(cpu, 0x8065AAF4, payload);
    Memory::Write32(0x8065AAC8, Available(cpu, Memory::Read32(0x8065AAF0), 4));
}

extern "C" void MH3_ResMemFree(CpuContext* cpu) {
    const uint32_t payload = cpu->gpr[3], service = Memory::Read32(0x80796E30);
    GuestFrame frame(cpu, 0x40);
    AllocatorFree(cpu, service + 0x38, payload);
    Memory::Write32(service + 0x14, Available(cpu, Memory::Read32(service + 0x2c), 4));
}

extern "C" void MH3_HeapAvailable4(CpuContext* cpu) {
    cpu->gpr[3] = Available(cpu, cpu->gpr[3], 4);
}

extern "C" void MH3_SystemAlloc(CpuContext* cpu) {
    cpu->gpr[3] = SystemAllocate(cpu, cpu->gpr[3]);
}

extern "C" void MH3_SystemFree(CpuContext* cpu) {
    SystemFree(cpu, cpu->gpr[3]);
}

REGISTER_NATIVE_FUNCTION_AS(0x8009A7AC, MH3_Copy32Blocks, "NativeCopy32ByteBlocks");
REGISTER_NATIVE_FUNCTION_AS(0x8009A974, MH3_Zero32Blocks, "NativeZero32ByteBlocks");
REGISTER_NATIVE_FUNCTION_AS(0x8000429C, MH3_FillMemory, "NativeFillMemory");
REGISTER_NATIVE_FUNCTION_AS(0x80004350, MH3_Memset, "NativeResourceMemset");
REGISTER_NATIVE_FUNCTION_AS(0x804C27A0, MH3_CreateExpHeap, "NativeCreateExpHeap");
REGISTER_NATIVE_FUNCTION_AS(0x804C2850, MH3_DestroyExpHeap, "NativeDestroyExpHeap");
REGISTER_NATIVE_FUNCTION_AS(0x804C2880, MH3_AllocExpHeap, "NativeAllocExpHeap");
REGISTER_NATIVE_FUNCTION_AS(0x804C2930, MH3_FreeExpHeap, "NativeFreeExpHeap");
REGISTER_NATIVE_FUNCTION_AS(0x804C2A00, MH3_AvailableExpHeap, "NativeAvailableExpHeap");
REGISTER_NATIVE_FUNCTION_AS(0x804C2AE0, MH3_AllocatorAllocThunk, "NativeAllocatorAllocThunk");
REGISTER_NATIVE_FUNCTION_AS(0x804C2AF0, MH3_AllocatorFreeThunk, "NativeAllocatorFreeThunk");
REGISTER_NATIVE_FUNCTION_AS(0x804C2B00, MH3_AllocatorAlloc, "NativeAllocatorAlloc");
REGISTER_NATIVE_FUNCTION_AS(0x804C2B10, MH3_AllocatorFree, "NativeAllocatorFree");
REGISTER_NATIVE_FUNCTION_AS(0x800CF878, MH3_WorkMemFree, "NativeWorkMemFree");
REGISTER_NATIVE_FUNCTION_AS(0x800D4CC8, MH3_ResMemFree, "NativeResMemFree");
REGISTER_NATIVE_FUNCTION_AS(0x800CF680, MH3_HeapAvailable4, "NativeWorkHeapAvailable");
REGISTER_NATIVE_FUNCTION_AS(0x800D3190, MH3_HeapAvailable4, "NativeResourceHeapAvailable");
REGISTER_NATIVE_FUNCTION_AS(0x80040484, MH3_SystemAlloc, "NativeSystemAlloc");
REGISTER_NATIVE_FUNCTION_AS(0x800404DC, MH3_SystemAlloc, "NativeOperatorNew");
REGISTER_NATIVE_FUNCTION_AS(0x80040520, MH3_SystemAlloc, "NativeOperatorNewArray");
REGISTER_NATIVE_FUNCTION_AS(0x800404C4, MH3_SystemFree, "NativeSystemFree");
REGISTER_NATIVE_FUNCTION_AS(0x80040564, MH3_SystemFree, "NativeOperatorDelete");
REGISTER_NATIVE_FUNCTION_AS(0x800405B0, MH3_SystemFree, "NativeOperatorDeleteArray");
