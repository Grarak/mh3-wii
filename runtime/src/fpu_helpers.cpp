#include "ppc_runtime.h"
#include "memory.h"

#include <cmath>
#include <cstdint>

namespace {

// Broadway Manual 12.2: FEX (bit 1) and VX (bit 2) are status summaries derived
// from the other bits; mtfsf/mtfsfi cannot set them explicitly.
constexpr uint32_t kFpscrReadOnlySummaryMask = 0x60000000u;

double RoundNearestEven(double value) {
    const double floor_value = std::floor(value);
    const double frac = value - floor_value;
    if (frac < 0.5) {
        return floor_value;
    }
    if (frac > 0.5) {
        return floor_value + 1.0;
    }

    const double half = floor_value / 2.0;
    return half == std::floor(half) ? floor_value : floor_value + 1.0;
}
}

// CurrentCpuContext() aborts on a null context (isa/ppc_isa_context.h), so it
// never returns nullptr - none of the helpers below guard against one.
extern "C" void PPC_Mtfsf(uint32_t fieldMask, double source) {
    CpuContext* cpu = CurrentCpuContext();

    fieldMask &= 0xFFu;

    // A mask of 0 means update nothing. Do not force it to 0xFF.
    if (fieldMask == 0) {
        return;
    }

    // Broadway Manual 12.2: "The low-order 32 bits of frB are placed into the
    // FPSCR..." - on a little-endian host that is the low word of the FPR.
    const uint32_t incoming = PPC_FprLowWordInline(source);

    if (fieldMask == 0xFFu) {
        cpu->fpscr = (cpu->fpscr & kFpscrReadOnlySummaryMask) |
                     (incoming & ~kFpscrReadOnlySummaryMask);
        MkwApplyHostNiMode(cpu->fpscr);
        return;
    }

    for (int field = 0; field < 8; ++field) {
        // PowerPC bit numbering is reversed relative to typical host masks.
        if ((fieldMask & (1u << (7 - field))) == 0) {
            continue;
        }

        // Field 0 is bits 0-3 (MSB of FPSCR); field 7 is bits 28-31.
        const int shift = (7 - field) * 4;
        uint32_t mask = 0xFu << shift;

        if (field == 0) {
            mask &= ~kFpscrReadOnlySummaryMask;
        }

        cpu->fpscr = (cpu->fpscr & ~mask) | (incoming & mask);
    }
    MkwApplyHostNiMode(cpu->fpscr);
}

extern "C" void PPC_Mtfsb1(uint32_t bit) {
    CpuContext* cpu = CurrentCpuContext();

    bit &= 31u;

    // Broadway Manual p479: bits 1 and 2 (FEX and VX) cannot be explicitly set.
    if (bit == 1 || bit == 2) {
        return;
    }

    const uint32_t shift = 31u - bit;
    cpu->fpscr |= (1u << shift);
    MkwApplyHostNiMode(cpu->fpscr);
}

extern "C" void PPC_Mtfsb0(uint32_t bit) {
    CpuContext* cpu = CurrentCpuContext();

    bit &= 31u;

    const uint32_t shift = 31u - bit;
    cpu->fpscr &= ~(1u << shift);
    MkwApplyHostNiMode(cpu->fpscr);
}

extern "C" void PPC_Mtfsfi(uint32_t field, uint32_t value) {
    CpuContext* cpu = CurrentCpuContext();

    field &= 7u;
    const uint32_t shift = (7u - field) * 4u;
    uint32_t mask = 0xFu << shift;
    if (field == 0) {
        mask &= ~kFpscrReadOnlySummaryMask;
    }

    cpu->fpscr = (cpu->fpscr & ~mask) | ((value & 0xFu) << shift);
    MkwApplyHostNiMode(cpu->fpscr);
}

extern "C" double PPC_Mffs() {
    // Broadway Manual 12.2: the FPSCR is placed into the low-order bits of frD;
    // the high-order bits are architecturally undefined and left zero here.
    return PpcBitCastToDoubleInline(static_cast<uint64_t>(CurrentCpuContext()->fpscr));
}

// PPC_Fctiwz is defined inline in ppc_runtime.h. It is the single most emitted
// float helper in the translation (1,051 sites) and this build has no LTO, so
// an out-of-line definition made every fctiwz a full caller-saved register
// barrier around two compares and a cvttsd2si. PPC_Fctiw below shares the same
// isa pack/clamp inlines so the two cannot drift apart.

extern "C" double PPC_Fctiw(double value) {
    const uint32_t rn = CurrentCpuContext()->fpscr & 0x3u;

    double rounded = value;
    switch (rn) {
    case 0:
        rounded = RoundNearestEven(value);
        break;
    case 1:
        rounded = std::trunc(value);
        break;
    case 2:
        rounded = std::ceil(value);
        break;
    case 3:
        rounded = std::floor(value);
        break;
    }

    return PpcPackIntegerWordInline(PpcClampIntegerWordInline(rounded));
}

// stfiwx stores the low 32 bits of an FPR as raw integer data with no
// floating-point conversion - the store half of the fctiwz -> stfiwx idiom.
extern "C" void PPC_Stfiwx(uint32_t addr, double fprValue) {
    Memory::Write32(addr, PPC_FprLowWordInline(fprValue));
}
