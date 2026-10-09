#include "abi_bridge.h"
#include "fatal.h"
#include "ios_host.h"
#include "video_interface.h"
#include "audio_interface.h"
#include "gx_host.h"
#include "efb_host.h"
#include "gx_stream.h"
#include "os_alarm.h"
#include "controller_host.h"
#include "village_boot.h"
#include "self_test.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#endif

extern "C" void MH3_EXIInit(CpuContext*);
extern "C" void MH3_EXIGetID(CpuContext*);
extern "C" void MH3_SIInit(CpuContext*);
extern "C" void MH3_SISetXY(CpuContext*);
extern "C" void MH3_SIGetType(CpuContext*);
extern "C" void MH3_WUDInit(CpuContext*);
extern "C" void MH3_OSInitSram(CpuContext*);
extern "C" void MH3_UnlockSram(CpuContext*);
extern "C" void MH3_OSGetResetCode(CpuContext*);
extern "C" void MH3_IOSOpen(CpuContext*);
extern "C" void MH3_IOSClose(CpuContext*);
extern "C" void MH3_IOSRead(CpuContext*);
extern "C" void MH3_IOSWrite(CpuContext*);
extern "C" void MH3_IOSWriteAsync(CpuContext*);
extern "C" void MH3_IOSSeekAsync(CpuContext*);
extern "C" void MH3_IOSIoctlAsync(CpuContext*);
extern "C" void MH3_IOSIoctl(CpuContext*);
extern "C" void MH3_IOSIoctlv(CpuContext*);
extern "C" void MH3_IOSIoctlvAsync(CpuContext*);
extern "C" void MH3_IOSOpenAsync(CpuContext*);
void RunSleepSelfTests(CpuContext*);
void RunRsoOverlaySelfTests(CpuContext*);
void RunGxPanicSelfTests(CpuContext*);
extern "C" void PPC_WriteSpr(uint32_t, uint32_t);
extern "C" uint32_t PPC_ReadSpr(uint32_t);

namespace {
bool callbackObserved = false;
unsigned callbackDepth = 0, maxCallbackDepth = 0, callbackCount = 0;
unsigned alarmCallbacks = 0;
bool failAlarm = false;
void TestAlarmCallback(CpuContext* cpu) {
    if (cpu->gpr[3] != 0x80003000 || cpu->gpr[4] != 0x80004000 || (cpu->msr & 0x8000) || Memory::Read32(cpu->gpr[13] - 14992) != 1 || Memory::Read32(0x800000D4) == 0x80004000)
        MH3_THROW(std::runtime_error("Alarm callback context incorrect"));

    ++alarmCallbacks;
    ServiceOsAlarms(cpu); // no recursive delivery
    cpu->gpr[14] = 0;

    if (failAlarm)
        MH3_THROW(std::runtime_error("test alarm exception"));
}

void TestIosCallback(CpuContext* cpu) {
    if (static_cast<int32_t>(cpu->gpr[3]) != -106 || cpu->gpr[4] != 0x12345678)
        MH3_THROW(std::runtime_error("IOS callback arguments were not delivered"));

    callbackObserved = true;
    ++callbackCount;
    ++callbackDepth;
    maxCallbackDepth = std::max(maxCallbackDepth, callbackDepth);
    PumpIosCompletions(cpu);
    --callbackDepth;
    cpu->gpr[14] = 0xDEADBEEF;
    cpu->lr = 0;
}
} // namespace

void RunDeviceSelfTests(CpuContext* cpu) {
    {
        // Single rounding under FPSCR[NI]: a result below 2^-126 before rounding
        // flushes to a signed zero, even one that rounds up to the smallest
        // normal; without NI it rounds normally.
        const uint32_t saved = MkwGetHostFpControl();
        volatile double below = 0x1p-126 * (1 - 0x1p-30), tiny = 0x1p-130, smallest = 0x1p-126, one = 1.5;
        auto bits = [](float f) {
            uint32_t b;
            std::memcpy(&b, &f, 4);
            return b;
        };

        MkwApplyHostNiMode(4);
        const bool ni = bits(PpcForceSingleValueInline(below)) == 0 && bits(PpcForceSingleValueInline(-below)) == 0x80000000u && bits(PpcForceSingleValueInline(tiny)) == 0 &&
                        bits(PpcForceSingleValueInline(smallest)) == 0x00800000u && PpcForceSingleValueInline(one) == 1.5f &&
                        std::isnan(PpcForceSingleValueInline(std::numeric_limits<double>::quiet_NaN()));
        MkwApplyHostNiMode(0);
        const bool ieee = bits(PpcForceSingleValueInline(below)) == 0x00800000u && bits(PpcForceSingleValueInline(tiny)) == 0x00080000u;
        MkwRestoreHostMxcsr(saved);

        if (!ni)
            MH3_THROW(std::runtime_error("Single rounding under FPSCR[NI] did not flush pre-round subnormals"));

        if (!ieee)
            MH3_THROW(std::runtime_error("Single rounding without FPSCR[NI] flushed"));
    }

    {
        // Every alias of a RAM byte is the same shared-memory byte, including
        // accesses straddling a host page.
        for (uint32_t base : {0u, 0x10000000u}) {
            const uint32_t address = base + 0xFFFFD;
            Memory::Write64(address, 0x0123456789ABCDEFull);

            for (uint32_t alias : {0x80000000u, 0xC0000000u}) {
                if (MemoryInline::FlatLoad<uint64_t>(address | alias) != 0x0123456789ABCDEFull || Memory::Read64(address | alias) != 0x0123456789ABCDEFull)
                    MH3_THROW(std::runtime_error("Flat RAM alias/page straddle failed"));
            }
        }

        for (uint32_t address : {0x817FFFFCu, 0x93FFFFFCu, 0x48000000u, 0xC8000000u, 0xCC008000u}) {
            if (Memory::Contains(address, 8))
                MH3_THROW(std::runtime_error("Terminal RAM or MMIO reported as mapped"));
        }

        if (CheckRejections && !Rejects<Memory::AccessViolation>([] { Memory::Read64(0x817FFFFC); }))
            MH3_THROW(std::runtime_error("Cross-allocation RAM read accepted"));
    }

    RunVideoTimingSelfTests();
    RunControllerSelfTests(cpu);
    RunRsoOverlaySelfTests(cpu);
    {
        constexpr uint32_t start = 0x81700004, end = 0x81701008;
        Memory::Write32(start, 0);
        Memory::Write32(end - 4, 0);
        Memory::Write32(end, 0);
        RecompMod::RegisterExecutableRange(start, end, "lifetime test");
        RecompMod::RegisterExecutableRange(start, end, "duplicate registration");
        RecompMod::RegisterExecutableRange(end, end + 4, "shared-page neighbor");
        const auto rejectsWrite = [](uint32_t address) {
            return Rejects([&] { RecompMod::CheckExecutableWrite(address, 4, 0x5A5A5A5A); });
        };

        for (auto alias : {0u, 0x80000000u, 0xC0000000u}) {
            if (CheckRejections && !rejectsWrite(((end - 4) & 0x3FFFFFFF) | alias))
                MH3_THROW(std::runtime_error("Executable final-word/alias protection failed"));
        }

        RecompMod::CheckExecutableWrite(start, 4, 0); // Identical loader writes remain valid.
        RecompMod::VerifyExecutableRanges();
        Memory::Write32(start + 4, 1);

        if (CheckRejections && !Rejects([] { RecompMod::VerifyExecutableRanges(); }))
            MH3_THROW(std::runtime_error("Code checksum missed a store to translated code"));

        RecompMod::UnregisterExecutableRange(start, end);
        Memory::Write32(start, 1);
        Memory::Write32(end - 4, 1);

        if (CheckRejections && !rejectsWrite(end))
            MH3_THROW(std::runtime_error("Unlink removed neighboring code protection"));

        RecompMod::RegisterExecutableRange(start, end, "relinked image");

        if (CheckRejections && !rejectsWrite(start + 4))
            MH3_THROW(std::runtime_error("Reload did not restore code protection"));

        RecompMod::VerifyExecutableRanges(); // A relinked image takes a fresh checksum.
        RecompMod::UnregisterExecutableRange(start, end);
        RecompMod::UnregisterExecutableRange(end, end + 4);

        // Rejected with a runtime error; the entry's logic error fails the test.
        RawDispatchRecord stale{0x80868F30, [](CpuContext*) { MH3_THROW(std::logic_error("Stale RSO entry executed")); }, 0, false};

        if (CheckRejections && !Rejects([&] { TryDispatchRawCpuTarget(&stale, cpu); }))
            MH3_THROW(std::runtime_error("Unlinked RSO dispatch was accepted"));

        RecompMod::CheckRsoExecution(0x8086907C); // Verification prolog remains reachable.
    }

    {
        uint16_t x = 0, y = 0;

        for (auto base : {0x08000000u, 0x88000000u, 0xC8000000u})
            if (!DecodeEfbColorAddress(base + 527 * 4096 + 639 * 4, x, y) || x != 639 || y != 527)
                MH3_THROW(std::runtime_error("EFB coordinate/alias decoding failed"));
        for (auto bad : {0x48000000u, 0xC8400000u, 0xC8000001u, 0x80000000u})
            if (DecodeEfbColorAddress(bad, x, y))
                MH3_THROW(std::runtime_error("Invalid EFB address accepted"));
        // CPU EFB reads (including out-of-bounds coordinates) now terminate;
        // RunGxPanicSelfTests isolates and validates that fatal path below.
        if (ConvertEfbArgb(0xAB123456, 0, 2) != 0xFF123456 || ConvertEfbArgb(0xAB123456, 1, 2) != 0xAA103455 || ConvertEfbArgb(0xAB123456, 2, 2) != 0xFF103452 ||
            ConvertEfbArgb(0xAB123456, 1, 0) != 0x00103455 || ConvertEfbArgb(0xAB123456, 1, 1) != 0xFF103455)
            MH3_THROW(std::runtime_error("EFB pixel format/alpha conversion failed"));
        for (auto modes : {std::pair{3u, 0u}, std::pair{0u, 3u}}) {
            if (CheckRejections && !Rejects([&] { ConvertEfbArgb(0, modes.first, modes.second); }))
                MH3_THROW(std::runtime_error("Unsupported EFB mode accepted"));
        }
    }

    {
        AudioInterfaceState ai;
        ai.Reset(0);

        if (ai.Read(0, 1000000) != 0x42 || ai.Read(8, 1000000))
            MH3_THROW(std::runtime_error("AI reset/stopped state failed"));

        ai.Write(12, 48, 1000000);
        ai.Write(0, 0x43, 1000000);

        // Many sub-sample reads must retain fractional elapsed time.
        for (uint64_t t = 1000100; t <= 2000000; t += 100)
            ai.Read(8, t);

        if (ai.Read(8, 2000000) != 48 || !(ai.Read(0, 2000000) & 8))
            MH3_THROW(std::runtime_error("AI 48 kHz clock/interrupt failed"));

        ai.Write(0, 0x49, 2000000); // clear interrupt and switch stream to 32 kHz
        if (ai.Read(8, 3000000) != 80 || (ai.Read(0, 3000000) & 8))
            MH3_THROW(std::runtime_error("AI rate switch/interrupt acknowledge failed"));

        ai.Write(0, 0x40, 3000000);

        if (ai.Read(8, 9000000) != 80)
            MH3_THROW(std::runtime_error("AI stop failed"));

        ai.Write(0, 0x62, 9000000);

        if (ai.Read(8, 9000000) || ai.Read(0, 9000000) != 0x42)
            MH3_THROW(std::runtime_error("AI counter reset failed"));

        ai.Write(8, 0xFFFFFFF0, 9000000);
        ai.Write(12, 0x10, 9000000);
        ai.Write(0, 0x43, 9000000);

        if (ai.Read(8, 10000000) != 0x20 || !(ai.Read(0, 10000000) & 8))
            MH3_THROW(std::runtime_error("AI counter wrap failed"));

        Memory::Write32(0xCD006C04, 0x1234);

        if (Memory::Read32(0xCC006C04) != 0x1234 || Memory::Read32(0x0D006C04) != 0x1234)
            MH3_THROW(std::runtime_error("AI register aliases failed"));

        uint32_t value;

        if (ReadAudioRegister32(0x4D006C00, value) || ReadAudioRegister32(0xCD006C01, value) || ReadAudioRegister32(0xCD006C10, value))
            MH3_THROW(std::runtime_error("AI accepted an invalid register"));
    }

    {
        const auto saved = *cpu;
        cpu->gpr[13] = 0x8079B2E0;
        Memory::Write32(cpu->gpr[13] - 0x3700, 0);
        MH3_WUDInit(cpu);

        if (cpu->gpr[3] != 1 || Memory::Read8(0x80761668) != 3)
            MH3_THROW(std::runtime_error("WUD host startup state failed"));

        MH3_WUDInit(cpu);

        if (cpu->gpr[3] != 0)
            MH3_THROW(std::runtime_error("WUD startup is not idempotent"));

        *cpu = saved;
        Memory::Write32(0xCD0000C0, 0x100);

        if (Memory::Read32(0x0D0000C0) != 0x100)
            MH3_THROW(std::runtime_error("Sensor-bar GPIO alias failed"));

        Memory::Write32(0xCD0000C0, 0);

        if (CheckRejections && !Rejects([] { Memory::Write32(0xCD0000C0, 2); }))
            MH3_THROW(std::runtime_error("Unknown GPIO output was accepted"));
    }

    for (uint32_t channel = 0; channel < 4; ++channel) {
        cpu->gpr[3] = channel;
        Memory::Write32(0x8062A8B8 + channel * 4, 0x80);
        MH3_SIGetType(cpu);

        if (cpu->gpr[3] != 8 || Memory::Read32(0x8062A8B8 + channel * 4) != 8)
            MH3_THROW(std::runtime_error("SI absent-device result/state failed"));
    }

    // The locked cache is independent storage, including at its last byte.
    Memory::Write32(0xE0000000, 0x12345678);
    MemoryInline::FlatWriteRam32(0xE0003FFC, 0xABCDEF01);

    if (Memory::Read32(0xE0000000) != 0x12345678 || Memory::Read32(0xE0003FFC) != 0xABCDEF01 || Memory::Contains(0xE0003FFC, 8) || Memory::Contains(0xE0004000, 1) || Memory::Contains(0x20000000, 4) ||
        Memory::Contains(0xA0000000, 4))
        MH3_THROW(std::runtime_error("Locked-cache mapping/bounds failed"));
    {
        const auto saved = *cpu;
        std::array<uint8_t, 4096> pattern;

        for (size_t i = 0; i < pattern.size(); ++i)
            pattern[i] = uint8_t(i * 37 + i / 32);

        std::copy(pattern.begin(), pattern.end(), Memory::GetPointer(0x80020000, pattern.size()));
        PPC_WriteSpr(922, 0x00020000); // zero length means 128 cache lines
        Memory::Write8(0xE0001000, 0xFF);
        PPC_WriteSpr(923, 0xE0001000); // programming without a trigger does not copy
        if (Memory::Read8(0xE0001000) != 0xFF)
            MH3_THROW(std::runtime_error("DMA setup changed cache"));

        PPC_WriteSpr(923, 0xE0001012); // trigger RAM -> locked cache
        if (!std::equal(pattern.begin(), pattern.end(), Memory::GetPointer(0xE0001000, 4096)) || PPC_ReadSpr(923) != 0xE0001010 || PPC_ReadSpr(922) != 0x00020000)
            MH3_THROW(std::runtime_error("Locked-cache DMA load/completion failed"));
        // Both length fields matter: 7 lines, to MEM2, with bytes beyond the
        // destination kept intact. DMA preserves guest byte order.
        Memory::Write8(0x90020100, 0xEE);
        PPC_WriteSpr(922, 0x10020021); // aligned address 0x10020020, length upper=1
        PPC_WriteSpr(923, 0xE000100E); // 7 lines, cache -> RAM
        if (!std::equal(pattern.begin(), pattern.begin() + 224, Memory::GetPointer(0x90020020, 224)) || Memory::Read8(0x90020100) != 0xEE || (PPC_ReadSpr(923) & 2))
            MH3_THROW(std::runtime_error("Locked-cache DMA store/length failed"));
        // Reject the whole request before writing any destination bytes.
        const auto expectRejected = [&](uint32_t upper, uint32_t lower) {
            PPC_WriteSpr(922, upper);

            if (CheckRejections && !Rejects([&] { PPC_WriteSpr(923, lower); }))
                MH3_THROW(std::runtime_error("Invalid locked-cache DMA accepted"));
        };

        expectRejected(0x00020000, 0xE0003FF2); // 4096 bytes past cache end
        expectRejected(0x017FFFE0, 0xE000101A); // 2 lines past MEM1 end
        expectRejected(0x0C008000, 0xE0001016); // MMIO is not RAM
        expectRejected(0x80020000, 0xE0001016); // effective address is not physical
        PPC_WriteSpr(920, 0x0F000000);

        if (PPC_ReadSpr(920) & 0x0F000000)
            MH3_THROW(std::runtime_error("DMA queue count is writable"));

        PPC_WriteSpr(922, 0);
        PPC_WriteSpr(923, 0);
        *cpu = saved;
    }

    if (Memory::Read16(0xCC002002) != 0x0101)
        MH3_THROW(std::runtime_error("VI initial PAL mode failed"));

    Memory::Write16(0xCC002048, 0x2828);

    if (Memory::Read16(0x0C002048) != 0x2828)
        MH3_THROW(std::runtime_error("VI register alias/storage failed"));

    const uint16_t line = Memory::Read16(0xCC00202C);
    const uint16_t sample = Memory::Read16(0xCC00202E);

    if (line < 1 || line > 625 || sample < 1 || sample > 864)
        MH3_THROW(std::runtime_error("VI beam position out of PAL bounds"));

    BeginVideoRetrace();

    if (Memory::Read16(0xCC00202C) != 1 || !(Memory::Read16(0xCC002030) & 0x8000) || VideoRetracePeriodNs() != 20000000)
        MH3_THROW(std::runtime_error("VI first PAL retrace latch failed"));

    Memory::Write16(0xCC002030, Memory::Read16(0xCC002030) & 0x7FFF);
    EndVideoRetrace();
    BeginVideoRetrace();

    if (Memory::Read16(0xCC00202C) != 313 || !(Memory::Read16(0xCC002034) & 0x8000) || (Memory::Read16(0xCC002030) & 0x8000))
        MH3_THROW(std::runtime_error("VI second-field/acknowledgement failed"));

    EndVideoRetrace();
    Memory::Write16(0xCC002002, 5); // enabled, non-interlaced NTSC
    BeginVideoRetrace();
    EndVideoRetrace();
    BeginVideoRetrace();

    if (Memory::Read16(0xCC00202C) != 1 || VideoRetracePeriodNs() != 16683333)
        MH3_THROW(std::runtime_error("VI non-interlaced retrace failed"));

    EndVideoRetrace();
    ResetVideoInterface();
    uint16_t unused;

    if (ReadVideoRegister16(0xCC003000, unused) || ReadVideoRegister16(0xCC002001, unused))
        MH3_THROW(std::runtime_error("VI intercepted an unrelated or unaligned register"));
    // Replaces the old emulated-register mask/alias checks: every GX register
    // alias, SDK call and FIFO writer must now terminate before graphics work.
    RunGxPanicSelfTests(cpu);
    RunVillageBootSelfTests();
    GuestGxStream stream;
    std::vector<uint8_t> delivered;
    unsigned packets = 0;
    auto sink = [&](std::span<const uint8_t> packet) {
        ++packets;
        delivered.insert(delivered.end(), packet.begin(), packet.end());
    };

    const std::array<uint8_t, 15> commands{0x08, 0x50, 0, 0, 0, 1, 0x10, 0, 0, 0x10, 0, 0x3F, 0x80, 0, 0};
    stream.Write(std::span(commands).first(5), sink);

    if (packets || stream.PendingBytes() != 5)
        MH3_THROW(std::runtime_error("GX decoded a partial command"));

    stream.Write(std::span(commands).subspan(5, 5), sink);

    if (packets != 1 || stream.PendingBytes() != 4)
        MH3_THROW(std::runtime_error("GX command framing failed"));

    stream.Write(std::span(commands).subspan(10), sink);

    if (packets != 2 || stream.PendingBytes() || !std::equal(delivered.begin(), delivered.end(), commands.begin(), commands.end()))
        MH3_THROW(std::runtime_error("GX stream changed command bytes"));

    const uint8_t guestPointer[]{0x08, 0xA0, 0x80, 0, 0, 0};

    if (CheckRejections && (!Rejects([&] { stream.Write(guestPointer, sink); }) || packets != 2))
        MH3_THROW(std::runtime_error("GX forwarded an unconverted guest pointer"));

    GuestGxStream translatedStream;
    delivered.clear();
    const uint8_t arrayCommand[]{0x08, 0xA3, 0x10, 0, 0x12, 0x34};

    // Host pointers travel as a big-endian 64-bit field; 32-bit hosts zero-extend them.
    constexpr uintptr_t kHostArrayProbe = sizeof(uintptr_t) == 8 ? static_cast<uintptr_t>(0x0123456789ABCDEFULL) : static_cast<uintptr_t>(0x89ABCDEFu);
    translatedStream.Write(arrayCommand, sink, [](uint32_t guest) -> GuestGxStream::ArrayView {
        if (guest != 0x10001234)
            MH3_THROW(std::runtime_error("GX array address byte order failed"));

        return {kHostArrayProbe, 0x1234};
    });
    std::vector<uint8_t> expected{0x50, 0, 0x13};

    for (unsigned i = 0; i < 8; ++i)
        expected.push_back(uint8_t(uint64_t(kHostArrayProbe) >> (56 - 8 * i)));

    expected.insert(expected.end(), {0, 0, 0x12, 0x34, 0});

    if (delivered != expected)
        MH3_THROW(std::runtime_error("GX host array command encoding failed"));

    {
        std::array<uint8_t, 32> inner{}, outer{};
        const uint8_t setup[]{0x08, 0x50, 0, 0, 2, 0, 0x61, 0x43, 0, 0, 1};
        std::copy(std::begin(setup), std::end(setup), inner.begin());
        const std::array<uint8_t, 9> callInner{0x40, 0, 0, 0x20, 0, 0, 0, 0, 32};
        std::copy(callInner.begin(), callInner.end(), outer.begin());
        auto resolve = [&](uint32_t address) -> GuestGxStream::ArrayView {
            if (address == 0x1000)
                return {reinterpret_cast<uintptr_t>(outer.data()), outer.size()};

            if (address == 0x2000)
                return {reinterpret_cast<uintptr_t>(inner.data()), inner.size()};

            MH3_THROW(std::runtime_error("Unexpected display-list address"));
        };

        const std::array<uint8_t, 9> callOuter{0x40, 0, 0, 0x10, 0, 0, 0, 0, 32};
        GuestGxStream lists;
        std::vector<std::vector<uint8_t>> packets;
        auto receive = [&](auto bytes) {
            packets.emplace_back(bytes.begin(), bytes.end());
        };

        lists.Write(std::span(callOuter).first(4), receive, resolve);

        if (!packets.empty())
            MH3_THROW(std::runtime_error("Partial display-list call executed"));

        lists.Write(std::span(callOuter).subspan(4), receive, resolve);
        const uint8_t draw[]{0x90, 0, 1, 0xAB, 0xCD};
        lists.Write(draw, receive, resolve);

        // Commands arrive in runs: the delivered bytes are the inner list's
        // setup (and its padding) followed by the draw.
        std::vector<uint8_t> delivered;

        for (const auto& packet : packets)
            delivered.insert(delivered.end(), packet.begin(), packet.end());

        if (lists.PixelFormat() != 1 || lists.PendingBytes() || !std::equal(std::begin(setup), std::end(setup), delivered.begin()) ||
            packets.back() != std::vector<uint8_t>(std::begin(draw), std::end(draw)))
            MH3_THROW(std::runtime_error("Nested display-list state/order failed"));
        for (auto& packet : packets)
            if (packet[0] == 0x40)
                MH3_THROW(std::runtime_error("Guest display-list pointer reached Aurora"));
        auto rejectList = [&](auto resolver) {
            GuestGxStream invalid;

            if (CheckRejections && !Rejects([&] { invalid.Write(callOuter, [](auto) {}, resolver); }))
                MH3_THROW(std::runtime_error("Invalid display list accepted"));
        };

        rejectList([&](uint32_t) -> GuestGxStream::ArrayView { return {reinterpret_cast<uintptr_t>(outer.data()), 8}; });
        outer.fill(0);
        outer.back() = 0x08;
        rejectList(resolve); // Truncated CP packet.
        outer.fill(0);
        std::copy(callOuter.begin(), callOuter.end(), outer.begin());
        rejectList(resolve); // Self-recursion.
    }

    {
        std::array<uint8_t, 256> matrices;

        for (unsigned i = 0; i < matrices.size(); ++i)
            matrices[i] = i ^ 0xA5;

        const unsigned destinations[]{0, 0x400, 0x78, 0x600};
        const unsigned lengths[]{12, 9, 8, 16};

        for (unsigned slot = 0; slot < 4; ++slot) {
            GuestGxStream indexed;
            unsigned accessible = matrices.size();
            auto resolve = [&](uint32_t address) -> GuestGxStream::ArrayView {
                if (address != 0x1000)
                    MH3_THROW(std::runtime_error("Indexed XF base address mismatch"));

                return {reinterpret_cast<uintptr_t>(matrices.data()), accessible};
            };

            std::vector<uint8_t> last;
            auto receive = [&](auto bytes) {
                last.assign(bytes.begin(), bytes.end());
            };

            const uint8_t base[]{0x08, uint8_t(0xAC + slot), 0, 0, 0x10, 0};
            const uint8_t stride[]{0x08, uint8_t(0xBC + slot), 0, 0, 0, 64};
            indexed.Write(base, receive, resolve);
            indexed.Write(stride, receive, resolve);
            const uint8_t command[]{uint8_t(0x20 + slot * 8), 0, 2, uint8_t(((lengths[slot] - 1) << 4) | (destinations[slot] >> 8)), uint8_t(destinations[slot])};
            indexed.Write(std::span(command).first(3), receive, resolve);

            if (last != std::vector<uint8_t>(std::begin(stride), std::end(stride)))
                MH3_THROW(std::runtime_error("Partial indexed XF command executed"));

            indexed.Write(std::span(command).subspan(3), receive, resolve);

            if (last.size() != 5 + lengths[slot] * 4 || last[0] != 0x10 || last[2] != lengths[slot] - 1 || last[3] != destinations[slot] >> 8 || last[4] != (destinations[slot] & 255) ||
                !std::equal(last.begin() + 5, last.end(), matrices.begin() + 128))
                MH3_THROW(std::runtime_error("Indexed XF expansion/endian/stride failed"));
            accessible = 128 + lengths[slot] * 4 - 1;

            if (CheckRejections && !Rejects([&] { indexed.Write(command, receive, resolve); }))
                MH3_THROW(std::runtime_error("Indexed XF exceeded source bounds"));
        }

        GuestGxStream invalid;
        const uint8_t partial[]{0x20, 0, 0, 0xB0, 1};
        const auto matrixRam = [&](uint32_t) -> GuestGxStream::ArrayView { return {reinterpret_cast<uintptr_t>(matrices.data()), matrices.size()}; };

        if (CheckRejections && !Rejects([&] { invalid.Write(partial, [](auto) {}, matrixRam); }))
            MH3_THROW(std::runtime_error("Unsupported partial XF matrix accepted"));
    }

    auto vertexCase = [&](uint32_t lo, uint32_t hi, uint32_t a, uint32_t b, uint32_t c, size_t stride, uint16_t count, uint8_t opcode) {
        GuestGxStream vertices;
        unsigned draws = 0, nops = 0;
        std::vector<uint8_t> packet(3 + count * stride, 0x61);
        packet[0] = opcode;
        packet[1] = count >> 8;
        packet[2] = count;
        auto receive = [&](std::span<const uint8_t> command) {
            // Commands arrive in runs: a NOP can follow the draw in its packet.
            if (command[0] >= 0x80) {
                if (command.size() < packet.size() || !std::equal(packet.begin(), packet.end(), command.begin()))
                    MH3_THROW(std::runtime_error("GX vertex packet size/data changed"));

                ++draws;
                nops += unsigned(std::count(command.begin() + packet.size(), command.end(), 0));
            } else if (!command[0])
                nops += unsigned(command.size());
        };

        auto setCp = [&](uint8_t reg, uint32_t value) {
            const uint8_t command[]{8, reg, uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};

            for (auto byte : command)
                vertices.Write(std::span(&byte, 1), receive);
        };

        auto resolveFramingArray = [](uint32_t) -> GuestGxStream::ArrayView {
            return {0x10000000, 0x1000000};
        };

        const auto fmt = opcode & 7;
        setCp(0x50, lo);
        setCp(0x60, hi);
        setCp(0x70 + fmt, a);
        setCp(0x80 + fmt, b);
        setCp(0x90 + fmt, c);
        vertices.Write(std::span(packet).first(2), receive, resolveFramingArray);
        vertices.Write(std::span(packet).subspan(2, packet.size() - 3), receive, resolveFramingArray);

        if (draws || vertices.PendingBytes() != packet.size() - 1)
            MH3_THROW(std::runtime_error("GX emitted an incomplete vertex packet"));

        const uint8_t tail[]{packet.back(), 0};
        vertices.Write(tail, receive, resolveFramingArray);

        if (draws != 1 || nops != 1 || vertices.PendingBytes())
            MH3_THROW(std::runtime_error("GX vertex packet framing lost the next command"));
    };

    for (unsigned opcode = 0x80; opcode <= 0xBF; ++opcode)
        vertexCase(0x101 | (1 << 9) | (1 << 13), 1, 9 | (5 << 14) | (5 << 21), 0, 0, 22, 4, opcode);
    // Nine matrix bytes, index8 position/color, three index16 normals,
    // and eight index16 texture coordinates.
    vertexCase(0x1FF | (2 << 9) | (3 << 11) | (2 << 13), 0xFFFF, 0x80000200, 0, 0, 33, 3, 0x97);

    // Mixed direct formats crossing all three VAT words, including TEX4's split field.
    vertexCase(0x1FF | (1 << 9) | (1 << 11) | (1 << 13) | (1 << 15), 0x5555, (3 << 1) | (1 << 10) | (1 << 14) | (2 << 18) | (8 << 21), 1 | (4 << 9) | (9 << 18) | (5u << 27),
               (0 << 5) | (9 << 14) | (4 << 23), 54, 3, 0xA3);
    // More than 1 MiB, and the full 16-bit vertex count, with direct XYZ+NBT floats.
    vertexCase((1 << 9) | (1 << 11), 0, 9 | (1 << 9) | (4 << 10), 0, 0, 48, 65535, 0x90);
    vertexCase(0, 0, 0, 0, 0, 0, 0, 0xBF);
    {
        GuestGxStream arrays;
        std::array<uint32_t, 12> sizes{};
        uint32_t available = 64 * 1024 * 1024;
        unsigned draws = 0;
        unsigned arrayPackets = 0;
        auto resolve = [&](uint32_t address) -> GuestGxStream::ArrayView {
            if (address < 0x1000 || address >= 0x1C00 || (address & 255))
                MH3_THROW(std::runtime_error("Draw array base mismatch"));

            return {uintptr_t(0x10000000) + address, available};
        };

        auto receive = [&](std::span<const uint8_t> packet) {
            if (packet[0] == 0x50) {
                ++arrayPackets;
                const unsigned slot = packet[2] - 0x10;

                if (slot >= 12)
                    MH3_THROW(std::runtime_error("Unexpected draw array slot"));

                sizes[slot] = (uint32_t(packet[11]) << 24) | (uint32_t(packet[12]) << 16) | (uint32_t(packet[13]) << 8) | packet[14];
            } else if (packet[0] >= 0x80)
                ++draws;
        };

        auto cp = [&](uint8_t reg, uint32_t value) {
            const uint8_t packet[]{8, reg, uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
            arrays.Write(packet, receive, resolve);
        };

        for (unsigned slot = 0; slot < 12; ++slot)
            cp(0xA0 + slot, 0x1000 + slot * 256);

        cp(0xB0, 12);
        cp(0xB1, 12);
        cp(0xB2, 4);
        cp(0xB4, 8);
        cp(0x50, 1 | (3 << 9) | (3 << 11) | (2 << 13));
        cp(0x60, 3);
        cp(0x70, 9 | 0x80000200u | (4 << 10) | (5 << 14) | (9 << 21));
        const uint8_t vertices[]{0x90, 0, 2, 0, 0, 100, 0, 1, 0, 7, 0, 3, 2, 0, 5, 3, 0, 1, 0, 9, 0, 2, 0, 4, 0, 0, 1};
        sizes.fill(0);
        arrays.Write(vertices, receive, resolve);

        if (draws != 1 || sizes[0] != 1212 || sizes[1] != 120 || sizes[2] != 12 || sizes[4] != 48)
            MH3_THROW(std::runtime_error("Per-draw indexed array extents/NBT3 failed"));

        const unsigned firstArrayPackets = arrayPackets;
        arrays.Write(vertices, receive, resolve);

        if (draws != 2 || arrayPackets != firstArrayPackets)
            MH3_THROW(std::runtime_error("Unchanged draw redundantly rebound vertex arrays"));

        for (unsigned slot : {3, 5, 6, 7, 8, 9, 10, 11})
            if (sizes[slot])
                MH3_THROW(std::runtime_error("Unused draw array was uploaded"));
        // A later draw that reads less of the same array reuses the bound prefix
        // (every rebind is another upload)...
        auto smaller = std::vector<uint8_t>(std::begin(vertices), std::end(vertices));
        smaller[5] = 2;
        arrays.Write(smaller, receive, resolve);

        if (draws != 3 || arrayPackets != firstArrayPackets)
            MH3_THROW(std::runtime_error("Smaller draw extent redundantly rebound vertex arrays"));
        // ...but a base-register write never keeps its RAM-region size: the next
        // draw binds only what it reads.
        cp(0xA0, 0x1000);
        sizes.fill(0);
        arrays.Write(smaller, receive, resolve);

        if (sizes[0] != 36 || draws != 4)
            MH3_THROW(std::runtime_error("Draw array extent did not shrink"));

        cp(0x50, 2 << 11);
        cp(0x60, 0);
        cp(0x70, (1 << 9) | (3 << 10));
        cp(0xB1, 20);
        cp(0xA1, 0x1100);
        const uint8_t nbt[]{0x90, 0, 1, 4};
        arrays.Write(nbt, receive, resolve);

        if (sizes[1] != 98)
            MH3_THROW(std::runtime_error("Single-index NBT extent failed"));

        available = 97;
        cp(0xA1, 0x1100); // resolved again at the base write

        if (CheckRejections && (!Rejects([&] { arrays.Write(nbt, receive, resolve); }) || draws != 5))
            MH3_THROW(std::runtime_error("Out-of-RAM draw was emitted"));
    }

    {
        GuestGxStream skipped;
        skipped.coalesce = false;
        bool drop = true;
        skipped.skipDraw = [&] {
            return drop;
        };

        unsigned draws = 0, arrayPackets = 0, texturePackets = 0, traces = 0;
        uint32_t lastArraySize = 0;
        std::array<uint8_t, 32> textureBytes{};
        const uint8_t draw[]{0x90, 0, 1, 0, 3}; // Index 3 into the position array.
        const uint8_t list[]{8, 0xB0, 0, 0, 0, 16, 0x61, 0x43, 0, 0, 3, 0x90, 0, 1, 0, 3};
        auto receive = [&](std::span<const uint8_t> packet) {
            if (packet[0] >= 0x80)
                ++draws;

            if (packet[0] == 0x50 && packet[2] == 0x10) {
                ++arrayPackets;
                lastArraySize = (uint32_t(packet[11]) << 24) | (uint32_t(packet[12]) << 16) | (uint32_t(packet[13]) << 8) | packet[14];
            }

            if (packet[0] == 0x50 && packet[2] == 0x30)
                ++texturePackets;
        };

        auto resolve = [&](uint32_t address) -> GuestGxStream::ArrayView {
            if (address == 0x1000)
                return {0x10000000, 64};

            if (address == 0x2000)
                return {reinterpret_cast<uintptr_t>(list), sizeof(list)};

            if (address == 0x3000)
                return {reinterpret_cast<uintptr_t>(textureBytes.data()), textureBytes.size()};

            MH3_THROW(std::runtime_error("Skipped draw resolved unexpected RAM"));
        };

        auto cp = [&](uint8_t reg, uint32_t value) {
            const uint8_t command[]{8, reg, uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
            skipped.Write(command, receive, resolve);
        };

        auto bp = [&](uint8_t reg, uint32_t value) {
            const uint8_t command[]{0x61, reg, uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
            skipped.Write(command, receive, resolve);
        };

        cp(0xA0, 0x1000);
        cp(0xB0, 12);
        cp(0x50, 3 << 9);
        cp(0x70, 9);
        bp(0x88, (7 << 10) | 7);
        bp(0x94, 0x3000 >> 5);
        skipped.traceDraw = [&](uint32_t, uint32_t) {
            ++traces;
        };

        const auto before = GuestGxStream::skippedDrawCount;
        const uint8_t call[]{0x40, 0, 0, 0x20, 0, 0, 0, 0, sizeof(list)};
        skipped.Write(call, receive, resolve);
        skipped.Write(std::span(draw).first(2), receive, resolve);
        skipped.Write(std::span(draw).subspan(2), receive, resolve);

        if (draws || texturePackets || traces || arrayPackets != 1 || skipped.PendingBytes() || skipped.PixelFormat() != 3 || GuestGxStream::skippedDrawCount - before != 2)
            MH3_THROW(std::runtime_error("Skipped nested/fragmented GX draws lost state or prepared resources"));

        drop = false;
        skipped.Write(draw, receive, resolve);

        if (draws != 1 || traces != 1 || texturePackets != 1 || arrayPackets != 2 || lastArraySize != 60)
            MH3_THROW(std::runtime_error("Visible GX draw did not use deferred texture and display-list array state"));
    }

    auto textureCase = [&](uint32_t format, uint32_t width, uint32_t height, bool mips, uint32_t bytes, bool shouldPass, uint32_t guestAddress = 0) {
        if (!CheckRejections && !shouldPass)
            return; // its rejection would stop the process

        GuestGxStream textureStream;
        GuestGxStream::InvalidateTextureHashes(); // the hashes are process-wide
        std::vector<uint8_t> metadata;
        std::vector<uint8_t> ownedBytes(bytes);
        auto textureBytes = guestAddress ? std::span(Memory::GetPointer(guestAddress, bytes), bytes) : std::span(ownedBytes);
        std::fill(textureBytes.begin(), textureBytes.end(), 0);
        auto textureSink = [&](std::span<const uint8_t> packet) {
            if (packet[0] == 0x50)
                metadata.assign(packet.begin(), packet.end());
        };

        auto resolve = [&](uint32_t address) -> GuestGxStream::ArrayView {
            if (address != 0x100000)
                MH3_THROW(std::runtime_error("GX texture address scaling failed"));

            return {reinterpret_cast<uintptr_t>(textureBytes.data()), bytes};
        };

        auto bpWrite = [&](uint8_t reg, uint32_t value) {
            const uint8_t command[]{0x61, reg, uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
            textureStream.Write(command, textureSink, resolve);
        };

        // Texture metadata goes out before the next draw (an empty one here).
        auto draw = [&] {
            const uint8_t command[]{0x90, 0, 0};
            textureStream.Write(command, textureSink, resolve);
        };

        const bool passed = !Rejects([&] {
            bpWrite(0x80, mips ? 0xC0 : 0x80);
            bpWrite(0x88, (format << 20) | ((height - 1) << 10) | (width - 1));

            // A masked width update must preserve format/height, and its mask
            // must not leak into the following image-address write.
            bpWrite(0xFE, 0x3FF);
            bpWrite(0x88, 0xF00000 | (width - 1));
            bpWrite(0x94, 0x100000 >> 5);
            draw();
        });

        if (passed != shouldPass)
            MH3_THROW(std::runtime_error("GX texture footprint validation failed"));

        if (passed && (metadata.size() != 37 || metadata[2] != 0x30 || metadata[15] != width || metadata[19] != height || metadata[23] != format || metadata[28] != mips))
            MH3_THROW(std::runtime_error("GX texture metadata encoding failed"));

        if (passed) {
            const auto version = std::vector<uint8_t>(metadata.begin() + 33, metadata.end());
            const uint8_t repeat[]{0x61, 0x94, 0, 0x80, 0};
            textureStream.Write(repeat, textureSink, resolve);
            draw();

            if (!std::equal(version.begin(), version.end(), metadata.begin() + 33))
                MH3_THROW(std::runtime_error("Unchanged GX texture lost its cache identity"));

            bpWrite(0x66, 0xFFFF00);
            textureStream.Write(repeat, textureSink, resolve);
            draw();

            if (!std::equal(version.begin(), version.end(), metadata.begin() + 33))
                MH3_THROW(std::runtime_error("Unchanged GX texture lost its identity at TMEM invalidation"));

            textureBytes[0] ^= 0xFF;

            // Unchanged until the texture cache is invalidated (TMEM keeps it).
            textureStream.Write(repeat, textureSink, resolve);
            draw();

            if (!std::equal(version.begin(), version.end(), metadata.begin() + 33))
                MH3_THROW(std::runtime_error("GX texture was hashed again before an invalidation"));

            bpWrite(0x66, 0xFFFF00); // GXInvalidateTexAll
            textureStream.Write(repeat, textureSink, resolve);
            draw();

            if (std::equal(version.begin(), version.end(), metadata.begin() + 33))
                MH3_THROW(std::runtime_error("Rewritten GX texture kept a stale cache identity"));
            // A copy into other bytes keeps the hash; one over the texture drops it.
            const auto rewritten = std::vector<uint8_t>(metadata.begin() + 33, metadata.end());
            const auto base = reinterpret_cast<uintptr_t>(textureBytes.data());
            textureBytes[0] ^= 0xFF;
            GuestGxStream::InvalidateTextureHashes(base + bytes, 64);
            textureStream.Write(repeat, textureSink, resolve);
            draw();

            if (!std::equal(rewritten.begin(), rewritten.end(), metadata.begin() + 33))
                MH3_THROW(std::runtime_error("A copy elsewhere dropped a GX texture hash"));

            GuestGxStream::InvalidateTextureHashes(base + bytes - 1, 1);
            textureStream.Write(repeat, textureSink, resolve);
            draw();

            if (!std::equal(version.begin(), version.end(), metadata.begin() + 33))
                MH3_THROW(std::runtime_error("A copy over a GX texture kept its hash"));
        }
    };

    textureCase(4, 4, 4, false, 32, true);
    textureCase(6, 5, 5, false, 255, false);
    textureCase(6, 5, 5, false, 256, true);
    textureCase(14, 9, 9, false, 128, true);
    textureCase(6, 4, 4, true, 191, false);
    textureCase(6, 4, 4, true, 192, true);
    textureCase(8, 9, 8, false, 64, true);
    textureCase(8, 9, 8, false, 63, false);
    textureCase(9, 8, 5, false, 64, true);
    textureCase(10, 4, 4, false, 32, true);
    {
        GuestGxStream paletteStream;
        std::array<uint8_t, 512> source{};
        std::vector<uint8_t> textureBytes(32768);
        const uint8_t paletteList[]{0x61, 0x64, 0, 0x80, 0, 0x61, 0x65, 0, 6, 0};
        source[0] = 0xF8;
        source[1] = 0x00;
        uint32_t available = source.size();
        std::vector<std::vector<uint8_t>> packets;
        auto receive = [&](std::span<const uint8_t> p) {
            packets.emplace_back(p.begin(), p.end());
        };

        auto resolve = [&](uint32_t address) -> GuestGxStream::ArrayView {
            if (address == 0x100000)
                return {reinterpret_cast<uintptr_t>(source.data()), available};

            if (address == 0x200000)
                return {reinterpret_cast<uintptr_t>(textureBytes.data()), uint32_t(textureBytes.size())};

            if (address == 0x300000)
                return {reinterpret_cast<uintptr_t>(paletteList), sizeof(paletteList)};

            MH3_THROW(std::runtime_error("Unexpected TLUT source address"));
        };

        auto write = [&](uint8_t reg, uint32_t value) {
            const uint8_t command[]{0x61, reg, uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};

            // Fragmentation must preserve the upload's command boundary.
            paletteStream.Write({command, 2}, receive, resolve);
            paletteStream.Write({command + 2, 3}, receive, resolve);
        };

        const uint8_t draw[]{0x90, 0, 3}; // Three vertices with no enabled attributes.
        auto drawPalette = [&] {
            paletteStream.Write(draw, receive, resolve);
        };

        auto palettePackets = [&] {
            std::vector<std::vector<uint8_t>> result;

            for (const auto& p : packets)
                if (p[0] == 0x50 && p[2] == 0x31)
                    result.push_back(p);
            return result;
        };

        auto word = [](const auto& p, unsigned at) {
            return (uint32_t(p[at]) << 24) | (uint32_t(p[at + 1]) << 16) | (uint32_t(p[at + 2]) << 8) | p[at + 3];
        };

        write(0x64, 0x100000 >> 5);
        write(0x65, (16 << 10) | 0x200); // 512-byte C8 palette at TMEM 0x40000.
        source[0] = 0;                   // The DMA must already have taken its snapshot.
        write(0xA8, 9 << 20);
        write(0xB4, 0x200000 >> 5);
        write(0xB8, (1 << 10) | 0x200);
        write(0x28, 64 | 4); // TEV stage zero samples texture unit four.
        drawPalette();
        auto p = palettePackets();

        if (p.size() != 1 || p[0].size() != 26 || p[0][3] != 4 || word(p[0], 12) != 1 || p[0][16] != 1 || p[0][17] != 0)
            MH3_THROW(std::runtime_error("GX TLUT slot/format/entry encoding failed"));

        uintptr_t pointer = 0;

        for (unsigned i = 4; i < 12; ++i)
            pointer = (pointer << 8) | p[0][i];

        if (reinterpret_cast<const uint8_t*>(pointer)[0] != 0xF8)
            MH3_THROW(std::runtime_error("GX TLUT did not snapshot guest RAM"));

        drawPalette();

        if (palettePackets().size() != 1)
            MH3_THROW(std::runtime_error("GX TLUT cache did not retain unchanged binding"));

        write(0x65, (1 << 10) | 0x200); // Partial overlapping update keeps the tail.
        drawPalette();
        p = palettePackets();

        if (p.size() != 2 || word(p[0], 22) == word(p[1], 22) || reinterpret_cast<const uint8_t*>(pointer)[0] != 0)
            MH3_THROW(std::runtime_error("GX TLUT overlapping reload was not rebound"));

        for (unsigned format : {0u, 2u}) {
            write(0xB8, (format << 10) | 0x200);
            drawPalette();

            if (word(palettePackets().back(), 12) != format)
                MH3_THROW(std::runtime_error("GX TLUT format rebind failed"));
        }

        source[0] = 0xAB;
        const uint8_t call[]{0x40, 0, 0x30, 0, 0, 0, 0, 0, sizeof(paletteList)};
        paletteStream.Write(call, receive, resolve);
        drawPalette();

        if (reinterpret_cast<const uint8_t*>(pointer)[0] != 0xAB || palettePackets().size() != 5)
            MH3_THROW(std::runtime_error("GX TLUT display-list state did not reach its caller"));

        const auto count = palettePackets().size();
        write(0x65, (16 << 10) | 0x200);
        drawPalette();

        if (palettePackets().size() != count)
            MH3_THROW(std::runtime_error("Identical GX TLUT reload redundantly rebound the palette"));
        // Restore the original bytes and format after intervening uploads.
        // Reuse its content version, while still publishing changed TMEM.
        source[0] = 0xF8;
        write(0x65, (16 << 10) | 0x200);
        write(0xB8, (1 << 10) | 0x200);
        drawPalette();

        if (word(palettePackets().back(), 22) != word(p[0], 22) || reinterpret_cast<const uint8_t*>(pointer)[0] != 0xF8)
            MH3_THROW(std::runtime_error("GX TLUT A/B/A reload lost the original content identity"));

        bool skipPaletteDraw = true;
        paletteStream.skipDraw = [&] {
            return skipPaletteDraw;
        };

        const auto beforeSkip = palettePackets().size();
        source[0] = 0x55;
        write(0x65, (16 << 10) | 0x200);
        drawPalette();

        if (palettePackets().size() != beforeSkip)
            MH3_THROW(std::runtime_error("Skipped GX draw prepared a palette conversion"));

        source[0] = 0x99; // The skipped frame's DMA snapshot must survive RAM reuse.
        skipPaletteDraw = false;
        drawPalette();

        if (palettePackets().size() != beforeSkip + 1 || reinterpret_cast<const uint8_t*>(pointer)[0] != 0x55)
            MH3_THROW(std::runtime_error("Skipped GX frame lost the next visible draw's palette snapshot"));

        auto mustReject = [&](auto operation) {
            if (CheckRejections && !Rejects(operation))
                MH3_THROW(std::runtime_error("Invalid GX TLUT operation accepted"));
        };

        write(0xB8, 0x300); // A different, never-loaded TMEM range.
        mustReject(drawPalette);

        // Use a fresh stream after a rejected command, since pending FIFO bytes
        // deliberately remain available for diagnosis.
        GuestGxStream invalid;
        const uint8_t shortLoad[]{0x61, 0x64, 0, 0x80, 0, 0x61, 0x65, 0, 8, 0};
        available = 32;
        mustReject([&] { invalid.Write(shortLoad, receive, resolve); });
    }

    auto copyCase = [&](uint32_t room, uint32_t scale, uint32_t trigger, bool shouldPass, uint32_t expectedHeight = 528) {
        if (!CheckRejections && !shouldPass)
            return; // its rejection would stop the process

        GuestGxStream copyStream;
        unsigned copies = 0;
        auto resolveCopy = [&](uint32_t address) -> GuestGxStream::ArrayView {
            if (address != 0x200000)
                MH3_THROW(std::runtime_error("GX copy address scaling failed"));

            return {0x12345678, room};
        };

        auto copied = [&](const GuestGxStream::DisplayCopy& copy) {
            if (copy.destination != 0x12345678 || copy.width != 640 || copy.height != expectedHeight || !copy.clear)
                MH3_THROW(std::runtime_error("GX copy metadata incorrect"));

            ++copies;
        };

        auto writeBp = [&](uint8_t reg, uint32_t value) {
            const uint8_t command[]{0x61, reg, uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
            copyStream.Write(command, [](auto) {}, resolveCopy, copied);
        };

        const bool passed = !Rejects([&] {
            writeBp(0x4A, (527 << 10) | 639);
            writeBp(0x4B, 0x200000 >> 5);
            writeBp(0x4D, 40);
            writeBp(0x4E, scale);
            writeBp(0x52, trigger);
        });

        if (passed != shouldPass || copies != unsigned(shouldPass))
            MH3_THROW(std::runtime_error("GX display-copy bounds/dispatch failed"));
    };

    copyCase(640 * 528 * 2, 256, 0x4800, true);
    copyCase(640 * 528 * 2 - 1, 256, 0x4800, false);
    copyCase(640 * 264 * 2, 128, 0x4800, true, 264);
    copyCase(640 * 264 * 2 - 1, 128, 0x4800, false, 264);
    copyCase(640 * 1055 * 2, 128, 0x4C00, true, 1055);
    copyCase(640 * 1055 * 2 - 1, 128, 0x4C00, false, 1055);
    copyCase(640 * 703 * 2, 192, 0x4C00, true, 703);
    copyCase(640 * 352 * 2, 384, 0x4C00, true, 352);
    copyCase(640 * 2, 0, 0x4800, true, 1);
    copyCase(640 * 528 * 2, 0, 0x4C00, false);

    // Removed texture-copy commands must never access guest RAM or emit a
    // copy callback, even with unsupported formats/invalid destinations.
    // Their clear rectangle remains an independent observable operation.
    for (const bool withClear : {false, true}) {
        GuestGxStream stream;
        unsigned clears = 0;
        auto bpWrite = [&](uint8_t reg, uint32_t value) {
            const uint8_t command[]{0x61, reg, uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
            stream.Write(
                command, [](auto) {}, [](uint32_t) -> GuestGxStream::ArrayView { MH3_THROW(std::runtime_error("Removed EFB texture copy touched guest RAM")); }, {},
                [&](const GuestGxStream::EfbClear& clear) {
                    if (clear.x != 4 || clear.y != 8 || clear.sourceWidth != 10 || clear.sourceHeight != 9)
                        MH3_THROW(std::runtime_error("EFB clear rectangle changed"));

                    ++clears;
                });
        };

        bpWrite(0x43, 3);
        bpWrite(0x49, (8 << 10) | 4);
        bpWrite(0x4A, (8 << 10) | 9);
        bpWrite(0x4B, 0xffffff);
        bpWrite(0x4D, 0);
        bpWrite(0x52, withClear ? 0x800 : 0);

        if (clears != unsigned(withClear))
            MH3_THROW(std::runtime_error("EFB clear-only dispatch failed"));
    }

    MH3_OSGetResetCode(cpu);

    if (cpu->gpr[3] != 0)
        MH3_THROW(std::runtime_error("Cold boot reset code failed"));

    Memory::Write32(0x8074F740, 1);
    Memory::Write32(0x8074F744, 0x1234);
    MH3_OSGetResetCode(cpu);

    if (cpu->gpr[3] != 0x80001234)
        MH3_THROW(std::runtime_error("Saved reset code failed"));

    Memory::Write32(0x8074F740, 0);
    MH3_EXIInit(cpu);
    cpu->gpr[5] = 0x80001100;
    Memory::Write32(cpu->gpr[5], 0xFFFFFFFF);
    MH3_EXIGetID(cpu);

    if (cpu->gpr[3] != 0 || Memory::Read32(cpu->gpr[5]) != 0 || Memory::Read32(0x80003040 + 9 * 4) != 0x804B1310)
        MH3_THROW(std::runtime_error("EXI absence/interrupt-state test failed"));

    MH3_SIInit(cpu);
    Memory::Write32(0x8062A8A4, 0xFC123411);
    cpu->gpr[3] = 0x70;
    cpu->gpr[4] = 3;
    MH3_SISetXY(cpu);

    if (cpu->gpr[3] != 0xFC700311 || Memory::Read32(0x8062A8A4) != 0xFC700311)
        MH3_THROW(std::runtime_error("SI polling-rate guest state failed"));

    Memory::Write32(0x8062A8A4, 0);

    if (Memory::Read32(0x80797934) != 1 || Memory::Read32(0x80759B28) != 0xFFFFFFFF)
        MH3_THROW(std::runtime_error("SI initialization test failed"));

    // Never read or modify the user's actual saved settings during tests.
    struct TestDirectory {
        std::filesystem::path path;
        bool hadOriginal;
        std::string original;
        TestDirectory() {
            const char* value = std::getenv("MH3_USER_DIR");
            hadOriginal = value != nullptr;

            if (value)
                original = value;
#ifdef __vita__
            const auto testRoot = std::filesystem::path(original).parent_path() / ".port-tests";
            std::filesystem::create_directories(testRoot);
            std::string pattern = (testRoot / ("mh3-sram-test-" + std::to_string(sceKernelGetProcessId()))).string();

            if (!std::filesystem::create_directory(pattern))
                MH3_THROW(std::runtime_error("SRAM test directory already exists"));
#else
            const auto testRoot = std::filesystem::temp_directory_path();
            std::string pattern = (testRoot / "mh3-sram-test-XXXXXX").string();

            if (!mkdtemp(pattern.data()))
                MH3_THROW(std::runtime_error("Unable to create SRAM test directory"));
#endif
            path = pattern;
            setenv("MH3_USER_DIR", pattern.c_str(), 1);
        }

        ~TestDirectory() {
            if (hadOriginal)
                setenv("MH3_USER_DIR", original.c_str(), 1);
            else
                unsetenv("MH3_USER_DIR");

            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } directory;
    MH3_OSInitSram(cpu);

    if (Memory::Read8(0x8074FB13) != 0x0C || Memory::Read32(0x8074FB4C) != 1)
        MH3_THROW(std::runtime_error("Default SRAM test failed"));

    Memory::Write8(0x8074FB12, 2);
    Memory::Write32(0x8074FB44, 1);
    cpu->gpr[3] = 1;
    cpu->gpr[4] = 0;
    MH3_UnlockSram(cpu);

    if (!(cpu->msr & 0x8000) || Memory::Read32(0x8074FB48) != 0)
        MH3_THROW(std::runtime_error("SRAM unlock/interrupt-restore test failed"));

    Memory::Write8(0x8074FB12, 0);
    MH3_OSInitSram(cpu);

    if (Memory::Read8(0x8074FB12) != 2 || Memory::Read16(0x8074FB00) != 0x020C || Memory::Read16(0x8074FB02) != 0xFDF0)
        MH3_THROW(std::runtime_error("SRAM persistence/checksum test failed"));

    const auto nand = directory.path / "nand";
    std::filesystem::create_directories(nand / "shared2");
    {
        std::ofstream file(nand / "shared2/settings");
        file << "ABCD";
    }

    auto open = [&](const std::string& path, uint32_t mode) {
        for (size_t i = 0; i <= path.size(); ++i)
            Memory::Write8(0x80001200 + i, i == path.size() ? 0 : path[i]);

        cpu->gpr[3] = 0x80001200;
        cpu->gpr[4] = mode;
        MH3_IOSOpen(cpu);
        return static_cast<int32_t>(cpu->gpr[3]);
    };

    int32_t fd = open("/shared2/settings", 3);

    if (fd < 0)
        MH3_THROW(std::runtime_error("IOS file open failed"));

    cpu->gpr[3] = fd;
    cpu->gpr[4] = 0x80001300;
    cpu->gpr[5] = 2;
    MH3_IOSRead(cpu);

    if (cpu->gpr[3] != 2 || Memory::Read16(0x80001300) != 0x4142)
        MH3_THROW(std::runtime_error("IOS file read failed"));

    Memory::Write16(0x80001300, 0x5859);
    cpu->gpr[3] = fd;
    cpu->gpr[4] = 0x80001300;
    cpu->gpr[5] = 2;
    MH3_IOSWrite(cpu);

    if (cpu->gpr[3] != 2)
        MH3_THROW(std::runtime_error("IOS file write failed"));

    cpu->gpr[3] = fd;
    MH3_IOSClose(cpu);
    {
        std::ifstream file(nand / "shared2/settings");
        std::string text;
        file >> text;

        if (text != "ABXY")
            MH3_THROW(std::runtime_error("IOS file persistence failed"));
    }

    fd = open("/shared2/settings", 1);

    if (fd < 0)
        MH3_THROW(std::runtime_error("IOS read-only open failed"));

    cpu->gpr[3] = fd;
    cpu->gpr[4] = 0x80001300;
    cpu->gpr[5] = 2;
    MH3_IOSWrite(cpu);

    if (static_cast<int32_t>(cpu->gpr[3]) != -102)
        MH3_THROW(std::runtime_error("IOS read-only mode was not enforced"));

    cpu->gpr[3] = fd;
    MH3_IOSClose(cpu);

    if (open("/../escape", 1) != -4 || open("//etc/passwd", 1) != -4 || open("/missing", 1) != -106)
        MH3_THROW(std::runtime_error("IOS path/error handling failed"));
#ifndef __vita__ // Vita's exFAT mount cannot contain symlinks.
    std::filesystem::create_directory_symlink(directory.path, nand / "link");
#endif
    if (open("/link/sram.bin", 1) >= 0)
        MH3_THROW(std::runtime_error("IOS NAND symlink isolation failed"));

    std::filesystem::create_directories(nand / "usage/sub");
    {
        std::ofstream file(nand / "usage/sub/data");
        file << std::string(0x4001, 'x');
    }

    { std::ofstream file(nand / "usage/empty"); }
    fd = open("/dev/fs", 0);
    auto usage = [&](const std::string& path, bool asynchronous = false) {
        for (size_t i = 0; i <= path.size(); ++i)
            Memory::Write8(0x80001200 + i, i == path.size() ? 0 : path[i]);

        Memory::Write32(0x80001500, 0x80001200);
        Memory::Write32(0x80001504, 64);
        Memory::Write32(0x80001508, 0x80001400);
        Memory::Write32(0x8000150C, 4);
        Memory::Write32(0x80001510, 0x80001404);
        Memory::Write32(0x80001514, 4);
        cpu->gpr[3] = fd;
        cpu->gpr[4] = 12;
        cpu->gpr[5] = 1;
        cpu->gpr[6] = 2;
        cpu->gpr[7] = 0x80001500;
        cpu->gpr[8] = 0x81700000;
        cpu->gpr[9] = 0x12345678;

        if (asynchronous)
            MH3_IOSIoctlvAsync(cpu);
        else
            MH3_IOSIoctlv(cpu);

        return static_cast<int32_t>(cpu->gpr[3]);
    };

    if (usage("/usage") || Memory::Read32(0x80001400) != 2 || Memory::Read32(0x80001404) != 4)
        MH3_THROW(std::runtime_error("NAND directory cluster/inode accounting failed"));

    if (usage("/usage", true))
        MH3_THROW(std::runtime_error("Asynchronous usage queue failed"));

    auto successfulUsage = TakeIosCompletion();

    if (!successfulUsage || successfulUsage->result != 0)
        MH3_THROW(std::runtime_error("Asynchronous usage lost its success result"));

    if (usage("/missing", true))
        MH3_THROW(std::runtime_error("Asynchronous ioctlv queue failed"));

    auto usageCompletion = TakeIosCompletion();

    if (!usageCompletion || usageCompletion->result != -106 || usageCompletion->callback != 0x81700000 || usageCompletion->argument != 0x12345678)
        MH3_THROW(std::runtime_error("Asynchronous ioctlv completion lost result/arguments"));

    if (usage("/../usage") != -4 || usage("//etc") != -4 || usage("/link") >= 0)
        MH3_THROW(std::runtime_error("NAND usage followed an unsafe path"));
#ifndef __vita__
    std::filesystem::create_directory_symlink(directory.path, nand / "usage/sub/link");

    if (usage("/usage") != -102)
        MH3_THROW(std::runtime_error("NAND usage followed a nested symlink"));
#endif
    auto createEntry = [&](const std::string& path, bool directoryEntry, bool async = false) {
        for (uint32_t i = 0; i < 0x4C; ++i)
            Memory::Write8(0x80001800 + i, 0);

        for (size_t i = 0; i < path.size(); ++i)
            Memory::Write8(0x80001806 + i, path[i]);

        Memory::Write8(0x80001846, 3);
        Memory::Write8(0x80001847, 1);
        Memory::Write8(0x80001849, 0x20);
        cpu->gpr[3] = fd;
        cpu->gpr[4] = directoryEntry ? 3 : 9;
        cpu->gpr[5] = 0x80001800;
        cpu->gpr[6] = 0x4C;
        cpu->gpr[7] = cpu->gpr[8] = 0;
        cpu->gpr[9] = 0x81700000;
        cpu->gpr[10] = 0x5678;

        if (async)
            MH3_IOSIoctlAsync(cpu);
        else
            MH3_IOSIoctl(cpu);

        return static_cast<int32_t>(cpu->gpr[3]);
    };

    if (createEntry("/tmp/mh3-test", true, true))
        MH3_THROW(std::runtime_error("NAND mkdir enqueue failed"));

    auto created = TakeIosCompletion();

    if (!created || created->result || created->callback != 0x81700000 || created->argument != 0x5678 || !std::filesystem::is_directory(nand / "tmp/mh3-test"))
        MH3_THROW(std::runtime_error("NAND mkdir callback/storage failed"));

    if (createEntry("/tmp/mh3-test", true) != -105 || createEntry("/absent/sub", true) != -106 || createEntry("/../outside", true) != -101 || createEntry("/link/outside", true) >= 0)
        MH3_THROW(std::runtime_error("NAND mkdir error/isolation failed"));

    if (createEntry("/tmp/mh3-test/save", false))
        MH3_THROW(std::runtime_error("NAND file creation failed"));

    {
        std::ofstream file(nand / "tmp/mh3-test/save");
        file << "saved";
    }

    if (createEntry("/tmp/mh3-test/save", false) != -105 || std::filesystem::file_size(nand / "tmp/mh3-test/save") != 5)
        MH3_THROW(std::runtime_error("NAND creation overwrote existing data"));

    cpu->gpr[3] = fd;
    MH3_IOSClose(cpu);
    fd = open("/dev/fs", 0);
    cpu->gpr[3] = fd;
    cpu->gpr[4] = 6;
    cpu->gpr[5] = 0x80001806;
    cpu->gpr[6] = 64;
    cpu->gpr[7] = 0x80001900;
    cpu->gpr[8] = 0x4C;
    MH3_IOSIoctl(cpu);

    if (cpu->gpr[3] || Memory::Read32(0x80001900) != 0x1000 || Memory::Read16(0x80001904) != 0x3038 || Memory::Read32(0x80001946) != 0x03010020)
        MH3_THROW(std::runtime_error("NAND metadata did not persist across descriptor reopening"));

    const int32_t saveFd = open("/tmp/mh3-test/save", 3);

    if (saveFd < 0)
        MH3_THROW(std::runtime_error("Created save could not be opened"));

    Memory::Write32(0x80001300, 0x54535421);
    cpu->gpr[3] = saveFd;
    cpu->gpr[4] = 0x80001300;
    cpu->gpr[5] = 4;
    cpu->gpr[6] = 0x81700000;
    cpu->gpr[7] = 0x1234;
    MH3_IOSWriteAsync(cpu);
    auto writeResult = TakeIosCompletion();

    if (cpu->gpr[3] || !writeResult || writeResult->result != 4 || writeResult->argument != 0x1234)
        MH3_THROW(std::runtime_error("Asynchronous save write failed"));

    auto seek = [&](uint32_t offset, uint32_t mode) {
        cpu->gpr[3] = saveFd;
        cpu->gpr[4] = offset;
        cpu->gpr[5] = mode;
        cpu->gpr[6] = 0x81700000;
        cpu->gpr[7] = 0x1234;
        MH3_IOSSeekAsync(cpu);
        auto completion = TakeIosCompletion();

        if (cpu->gpr[3] || !completion)
            MH3_THROW(std::runtime_error("Asynchronous save seek was not queued"));

        return completion->result;
    };

    if (seek(uint32_t(-2), 2) != 3)
        MH3_THROW(std::runtime_error("Save seek from end failed"));

    cpu->gpr[3] = saveFd;
    cpu->gpr[4] = 0x80001300;
    cpu->gpr[5] = 2;
    MH3_IOSRead(cpu);

    if (cpu->gpr[3] != 2 || Memory::Read16(0x80001300) != 0x2164 || seek(6, 0) != -101)
        MH3_THROW(std::runtime_error("Save seek/read or EOF bounds failed"));

    cpu->gpr[3] = saveFd;
    cpu->gpr[4] = 11;
    cpu->gpr[7] = 0x80001900;
    cpu->gpr[8] = 8;
    MH3_IOSIoctl(cpu);

    if (cpu->gpr[3] || Memory::Read32(0x80001900) != 5 || Memory::Read32(0x80001904) != 5)
        MH3_THROW(std::runtime_error("Save file size/offset query failed"));

    cpu->gpr[3] = saveFd;
    MH3_IOSClose(cpu);
    auto rename = [&](const std::string& from, const std::string& to) {
        for (unsigned i = 0; i < 128; ++i)
            Memory::Write8(0x80001800 + i, 0);

        for (size_t i = 0; i < from.size(); ++i)
            Memory::Write8(0x80001800 + i, from[i]);

        for (size_t i = 0; i < to.size(); ++i)
            Memory::Write8(0x80001840 + i, to[i]);

        cpu->gpr[3] = fd;
        cpu->gpr[4] = 8;
        cpu->gpr[5] = 0x80001800;
        cpu->gpr[6] = 128;
        MH3_IOSIoctl(cpu);
        return static_cast<int32_t>(cpu->gpr[3]);
    };

    if (rename("/tmp/mh3-test/save", "/../outside") != -101 || rename("/tmp/mh3-test/save", "/link/outside") >= 0 || rename("/tmp/mh3-test/save", "/tmp/mh3-test/moved"))
        MH3_THROW(std::runtime_error("NAND rename or destination isolation failed"));

    cpu->gpr[3] = fd;
    cpu->gpr[4] = 6;
    cpu->gpr[5] = 0x80001840;
    cpu->gpr[6] = 64;
    cpu->gpr[7] = 0x80001900;
    cpu->gpr[8] = 0x4C;
    MH3_IOSIoctl(cpu);

    if (cpu->gpr[3] || Memory::Read32(0x80001946) != 0x03010020 || std::filesystem::exists(nand / "tmp/mh3-test/save") || std::filesystem::file_size(nand / "tmp/mh3-test/moved") != 5)
        MH3_THROW(std::runtime_error("NAND rename lost data or metadata"));
    // ISFS rename replaces an existing file (saves move their temporary over it).
    {
        std::ofstream(nand / "tmp/mh3-test/old") << "old";
        std::ofstream(nand / "tmp/mh3-test/new") << "newer";
    }

    if (rename("/tmp/mh3-test/new", "/tmp/mh3-test/old") || std::filesystem::exists(nand / "tmp/mh3-test/new") || std::filesystem::file_size(nand / "tmp/mh3-test/old") != 5 ||
        std::filesystem::exists(nand / "tmp/mh3-test/old.replaced"))
        MH3_THROW(std::runtime_error("NAND rename did not replace an existing file"));
    for (unsigned i = 0; i < 74; ++i)
        Memory::Write8(0x80001800 + i, Memory::Read8(0x80001900 + i));

    Memory::Write8(0x80001846, 1); // owner may read, but no longer write
    cpu->gpr[3] = fd;
    cpu->gpr[4] = 5;
    cpu->gpr[5] = 0x80001800;
    cpu->gpr[6] = 0x4C;
    MH3_IOSIoctl(cpu);

    if (cpu->gpr[3] || open("/tmp/mh3-test/moved", 2) != -102 || open("/tmp/mh3-test", 1) != -101)
        MH3_THROW(std::runtime_error("NAND metadata permissions/directory open were not enforced"));

    auto erase = [&](const std::string& path) {
        for (unsigned i = 0; i < 64; ++i)
            Memory::Write8(0x80001800 + i, i < path.size() ? path[i] : 0);

        cpu->gpr[3] = fd;
        cpu->gpr[4] = 7;
        cpu->gpr[5] = 0x80001800;
        cpu->gpr[6] = 64;
        MH3_IOSIoctl(cpu);
        return static_cast<int32_t>(cpu->gpr[3]);
    };

    const int32_t heldFd = open("/tmp/mh3-test/moved", 1);

    if (heldFd < 0 || erase("/tmp/mh3-test") != -111 || !std::filesystem::exists(nand / "tmp/mh3-test/moved"))
        MH3_THROW(std::runtime_error("NAND deleted an in-use subtree"));

    cpu->gpr[3] = heldFd;
    MH3_IOSClose(cpu);
#ifndef __vita__
    std::filesystem::create_directory_symlink(directory.path, nand / "tmp/mh3-test/link");

    if (erase("/tmp/mh3-test") != -102 || !std::filesystem::exists(nand / "tmp/mh3-test/moved"))
        MH3_THROW(std::runtime_error("NAND delete did not reject a symlink before changing data"));

    std::filesystem::remove(nand / "tmp/mh3-test/link");
#endif
    if (erase("/") != -101 || erase("/../escape") != -101 || erase("/tmp/mh3-test") || std::filesystem::exists(nand / "tmp/mh3-test") || erase("/tmp/mh3-test") != -106)
        MH3_THROW(std::runtime_error("NAND recursive delete/error handling failed"));

    cpu->gpr[3] = fd;
    MH3_IOSClose(cpu);
    fd = open("/dev/usb/hid", 0);
    const auto hidRequest = [&](uint32_t command, uint32_t outputSize = 0x600) {
        cpu->gpr[3] = fd;
        cpu->gpr[4] = command;
        cpu->gpr[5] = 0;
        cpu->gpr[6] = 0;
        cpu->gpr[7] = 0x80002000;
        cpu->gpr[8] = outputSize;
        cpu->gpr[9] = 0x804D6320;
        cpu->gpr[10] = 0xA55A;
        MH3_IOSIoctlAsync(cpu);
    };

    hidRequest(6, 0x20);
    auto hidCompletion = TakeIosCompletion();

    if (cpu->gpr[3] || !hidCompletion || hidCompletion->result != 0x40001 || hidCompletion->argument != 0xA55A)
        MH3_THROW(std::runtime_error("USB HID version completion failed"));

    Memory::Write32(0x80002004, 0x12345678);
    hidRequest(0, 4);

    if (int32_t(cpu->gpr[3]) != -4 || TakeIosCompletion())
        MH3_THROW(std::runtime_error("USB HID accepted bad enumeration buffer"));

    hidRequest(0);
    hidCompletion = TakeIosCompletion();

    if (cpu->gpr[3] || !hidCompletion || hidCompletion->result || Memory::Read32(0x80002000) != 0xFFFFFFFF || Memory::Read32(0x80002004) != 0x12345678)
        MH3_THROW(std::runtime_error("USB HID empty enumeration failed"));

    hidRequest(0);

    if (cpu->gpr[3] || TakeIosCompletion())
        MH3_THROW(std::runtime_error("USB HID subscription must remain pending"));

    hidRequest(0);

    if (int32_t(cpu->gpr[3]) != -8 || TakeIosCompletion())
        MH3_THROW(std::runtime_error("USB HID replaced an outstanding subscription"));

    hidRequest(7, 0);
    const auto hidCancelled = TakeIosCompletion();
    const auto hidShutdown = TakeIosCompletion();

    if (cpu->gpr[3] || !hidCancelled || hidCancelled->result != -1 || !hidShutdown || hidShutdown->result)
        MH3_THROW(std::runtime_error("USB HID shutdown did not cancel the subscription"));

    hidRequest(0);
    hidCompletion = TakeIosCompletion();

    if (!hidCompletion || hidCompletion->result)
        MH3_THROW(std::runtime_error("USB HID did not enumerate after shutdown"));

    hidRequest(0);

    if (TakeIosCompletion())
        MH3_THROW(std::runtime_error("USB HID subscription completed without a change"));

    cpu->gpr[3] = fd;
    cpu->gpr[4] = 2;
    cpu->gpr[5] = 0x80002000;
    cpu->gpr[6] = 32;
    MH3_IOSIoctl(cpu);

    if (int32_t(cpu->gpr[3]) != -6)
        MH3_THROW(std::runtime_error("USB HID accepted a transfer to an absent device"));

    cpu->gpr[3] = fd;
    MH3_IOSClose(cpu);
    hidCompletion = TakeIosCompletion();

    if (cpu->gpr[3] || !hidCompletion || hidCompletion->result != -1 || TakeIosCompletion())
        MH3_THROW(std::runtime_error("USB HID close did not release the pending request"));

    fd = open("/dev/stm/immediate", 0);
    cpu->gpr[3] = fd;
    cpu->gpr[4] = 0x5001;
    cpu->gpr[5] = 0x80001400;
    cpu->gpr[6] = 20;
    cpu->gpr[7] = 0;
    cpu->gpr[8] = 0;
    cpu->gpr[9] = 0x804D6320;
    cpu->gpr[10] = 0xA5;
    MH3_IOSIoctlAsync(cpu);
    auto dimming = TakeIosCompletion();

    if (cpu->gpr[3] || !dimming || dimming->result || dimming->callback != 0x804D6320 || dimming->argument != 0xA5)
        MH3_THROW(std::runtime_error("STM async dimming completion failed"));

    cpu->gpr[3] = fd;
    MH3_IOSClose(cpu);
    fd = open("/dev/stm/eventhook", 0);

    if (fd < 0)
        MH3_THROW(std::runtime_error("STM event device open failed"));

    Memory::Write32(0x80001400, 0xDEADBEEF);
    cpu->gpr[3] = fd;
    cpu->gpr[4] = 0x1000;
    cpu->gpr[7] = 0x80001400;
    cpu->gpr[8] = 0x20;
    cpu->gpr[9] = 0x804D6320;
    cpu->gpr[10] = 0;
    MH3_IOSIoctlAsync(cpu);

    if (cpu->gpr[3] || Memory::Read32(0x80001400) != 0xDEADBEEF)
        MH3_THROW(std::runtime_error("STM event subscription must remain pending"));

    cpu->gpr[3] = fd;
    MH3_IOSIoctlAsync(cpu);
    auto completion = TakeIosCompletion();

    if (cpu->gpr[3] || !completion || completion->result != -8)
        MH3_THROW(std::runtime_error("STM duplicate subscription completion failed"));

    TriggerStmEvent(0x20000);
    completion = TakeIosCompletion();

    if (!completion || completion->callback != 0x804D6320 || completion->result || Memory::Read32(0x80001400) != 0x20000 || TakeIosCompletion())
        MH3_THROW(std::runtime_error("STM event delivery failed"));

    cpu->gpr[3] = fd;
    MH3_IOSClose(cpu);
    fd = open("/dev/es", 0);
    Memory::Write32(0x80001500, 0x80001600);
    Memory::Write32(0x80001504, 8);
    cpu->gpr[3] = fd;
    cpu->gpr[4] = 0x20;
    cpu->gpr[5] = 0;
    cpu->gpr[6] = 1;
    cpu->gpr[7] = 0x80001500;
    MH3_IOSIoctlv(cpu);

    if (cpu->gpr[3] || Memory::Read64(0x80001600) != 0x00010000524D4850ull)
        MH3_THROW(std::runtime_error("ES title identity failed"));

    Memory::Write32(0x80001508, 0x80001700);
    Memory::Write32(0x8000150C, 30);
    cpu->gpr[3] = fd;
    cpu->gpr[4] = 0x1D;
    cpu->gpr[5] = 1;
    cpu->gpr[6] = 1;
    cpu->gpr[7] = 0x80001500;
    MH3_IOSIoctlv(cpu);

    if (cpu->gpr[3] || std::string(reinterpret_cast<char*>(Memory::GetPointer(0x80001700, 30))) != "/title/00010000/524d4850/data")
        MH3_THROW(std::runtime_error("ES data directory failed"));

    for (uint32_t i = 0; i < 0x2A4; ++i)
        Memory::Write8(0x80001800 + i, 0);

    Memory::Write32(0x80001800, 0x00010001);

    for (uint32_t i = 0; i < 0xD4; ++i)
        Memory::Write8(0x800019D0 + i, i);

    Memory::Write32(0x80001500, 0x80001800);
    Memory::Write32(0x80001504, 0x2A4);
    Memory::Write32(0x80001508, 0x80001B00);
    Memory::Write32(0x8000150C, 0xD8);
    cpu->gpr[3] = fd;
    cpu->gpr[4] = 0x1B;
    cpu->gpr[5] = 1;
    cpu->gpr[6] = 1;
    cpu->gpr[7] = 0x80001500;
    MH3_IOSIoctlv(cpu);

    if (cpu->gpr[3] || Memory::Read32(0x80001B00))
        MH3_THROW(std::runtime_error("ES ticket view header failed"));

    for (uint32_t i = 0; i < 0xD4; ++i)
        if (Memory::Read8(0x80001B04 + i) != i)
            MH3_THROW(std::runtime_error("ES ticket view contents failed"));
    cpu->gpr[3] = fd;
    MH3_IOSClose(cpu);

    // A missing file is an asynchronous operation result, not a queue failure.
    open("/missing", 1); // also writes the test path into guest memory
    cpu->gpr[3] = 0x80001200;
    cpu->gpr[4] = 1;
    cpu->gpr[5] = 0x804DB9B0;
    cpu->gpr[6] = 0x12345678;
    MH3_IOSOpenAsync(cpu);
    auto fileCompletion = TakeIosCompletion();

    if (cpu->gpr[3] || !fileCompletion || fileCompletion->result != -106 || fileCompletion->callback != 0x804DB9B0 || fileCompletion->argument != 0x12345678)
        MH3_THROW(std::runtime_error("IOS asynchronous error completion failed"));
    // Test-only registration; never included in the production guest map.
    AbiTrampoline<decltype(TestIosCallback)> testCallback(0x81700000, "IOS callback test", TestIosCallback, FunctionKind::Native, false, 0, 0, 0, TestIosCallback);
    AbiTrampoline<decltype(TestAlarmCallback)> testAlarm(0x81700010, "OS alarm test", TestAlarmCallback, FunctionKind::Native, false, 0, 0, 0, TestAlarmCallback);
    TranslatedFunctionRegistry::Finalize();

    for (unsigned i = 0; i < 2; ++i) {
        cpu->gpr[3] = 0x80001200;
        cpu->gpr[4] = 1;
        cpu->gpr[5] = 0x81700000;
        cpu->gpr[6] = 0x12345678;
        MH3_IOSOpenAsync(cpu);
    }

    cpu->gpr[14] = 0x1234;
    cpu->lr = 0x5678;
    PumpIosCompletions(cpu);

    if (!callbackObserved || callbackCount != 2 || maxCallbackDepth != 1 || cpu->gpr[14] != 0x1234 || cpu->lr != 0x5678 || TakeIosCompletion())
        MH3_THROW(std::runtime_error("IOS callback CPU-state preservation failed"));

    RunSleepSelfTests(cpu);
    const auto alarmSaved = *cpu;
    cpu->gpr[1] = 0x817FF000;
    cpu->msr |= 0x8000;
    Memory::Write32(0x800000D4, 0x80004000);
    Memory::Write32(0x800000C0, 0x4000);
    Memory::Write32(cpu->gpr[13] - 14992, 0);
    const auto queue = cpu->gpr[13] - 0x3B10;
    auto queueAlarm = [&](uint64_t period) {
        for (uint32_t i = 0; i < 0x30; i += 4)
            Memory::Write32(0x80003000 + i, 0);

        Memory::Write32(queue, 0x80003000);
        Memory::Write32(queue + 4, 0x80003000);
        Memory::Write32(0x80003000, 0x81700010);
        Memory::Write64(0x80003018, period);

        if (period) {
            // Start three periods ago. A zero start can put the next deadline
            // just microseconds away and logging can legitimately expire it
            // again during ServiceOsAlarms. Keep a full period for assertions.
            const auto saved = *cpu;
            InvokeIndirectCpu(0x804D5400, cpu);
            const uint64_t now = (uint64_t(cpu->gpr[3]) << 32) | cpu->gpr[4];
            *cpu = saved;
            Memory::Write64(0x80003020, now - 3 * period);
        }
    };

    queueAlarm(0);
    ServiceOsAlarms(cpu);

    if (alarmCallbacks != 1 || Memory::Read32(queue) || Memory::Read32(queue + 4) || Memory::Read32(0x80003000) || cpu->gpr[14] != 0x1234 || !(cpu->msr & 0x8000) ||
        Memory::Read32(0x800000D4) != 0x80004000 || Memory::Read32(0x800000C0) != 0x4000 || Memory::Read32(cpu->gpr[13] - 14992))
        MH3_THROW(std::runtime_error("One-shot alarm dequeue/context restoration failed"));
    queueAlarm(60'750'000); // one second; InsertAlarm must skip expired periods
    ServiceOsAlarms(cpu);

    if (alarmCallbacks != 2 || Memory::Read32(queue) != 0x80003000 || Memory::Read32(0x80003000) != 0x81700010 || !Memory::Read64(0x80003008))
        MH3_THROW(std::runtime_error("Periodic alarm was not reinserted: callbacks=" + std::to_string(alarmCallbacks) + " queue=" + std::to_string(Memory::Read32(queue)) +
                                 " callback=" + std::to_string(Memory::Read32(0x80003000)) + " deadline=" + std::to_string(Memory::Read64(0x80003008))));
    ServiceOsAlarms(cpu);

    if (alarmCallbacks != 2)
        MH3_THROW(std::runtime_error("Periodic alarm fired before its next deadline"));

    cpu->gpr[3] = 0x80003000;
    InvokeIndirectCpu(0x804CBE80, cpu); // original OSCancelAlarm remains interoperable
    if (Memory::Read32(queue) || Memory::Read32(queue + 4) || Memory::Read32(0x80003000))
        MH3_THROW(std::runtime_error("Original alarm cancellation failed"));

    // A failing callback (it still counts) leaves the service unlocked.
    if (CheckRejections) {
        queueAlarm(0);
        failAlarm = true;
        const bool alarmRejected = Rejects([&] { ServiceOsAlarms(cpu); });
        failAlarm = false;

        if (!alarmRejected || Memory::Read32(0x800000D4) != 0x80004000 || Memory::Read32(cpu->gpr[13] - 14992))
            MH3_THROW(std::runtime_error("Alarm exception context restoration failed"));
    }

    queueAlarm(0);
    ServiceOsAlarms(cpu);

    if (alarmCallbacks != (CheckRejections ? 4 : 3))
        MH3_THROW(std::runtime_error("Alarm service remained locked after exception"));

    *cpu = alarmSaved;
}
