#pragma once
// FPSCR[NI] (non-IEEE flush-to-zero) modeled on the host FP environment, plus
// the thread-local mirror of that state the hot paths read instead of MXCSR/FPCR.

#include "ppc_isa_config.h"

#include <cstdint>

// Software-flushing Gekko's single-precision denormals per op roughly doubled the THP IDCT
// kernel's cycle count, so instead the runtime mirrors guest FPSCR[NI] into the host FP control
// register's flush-to-zero bit(s) wherever FPSCR can change (PPC_Mtfs*, fiber context switches,
// CpuContextScope), making per-op flushes free. Accepted deviations (same trade Dolphin makes):
// flush-to-zero also flushes double denormals unlike real NI, and a pre-round-flush edge near
// FLT_MIN rounds via cvtsd2ss (or its AArch64 equivalent) instead.
#if defined(__x86_64__)
// MXCSR: FTZ (bit 15, flushes arithmetic *output* denormals) and DAZ (bit 6, treats denormal
// *inputs* as zero) are two separate bits on x86; both are set/cleared together here.
inline constexpr uint32_t kMkwFpControlFlushToZeroBits = (1u << 15) | (1u << 6); // FTZ | DAZ
#elif defined(__aarch64__)
// FPCR: a single FZ bit (bit 24) flushes both single- and double-precision denormals, both as
// inputs and outputs - actually a cleaner match to the "flush everything" trade above than x86's
// two-bit combo, not an extra deviation.
inline constexpr uint32_t kMkwFpControlFlushToZeroBits = (1u << 24); // FZ
#elif defined(__arm__)
// AArch32 FPSCR: FZ (bit 24) flushes VFP single and double denormals, inputs and outputs, the
// same trade as AArch64's FPCR.FZ.
inline constexpr uint32_t kMkwFpControlFlushToZeroBits = (1u << 24); // FZ
#elif defined(__wasm__)
// WebAssembly has no FP control register and always keeps denormals: the web build holds the
// bit in software, so the NI mirror below stays consistent, but nothing is flushed (an accepted
// deviation; NI mode is rare).
inline constexpr uint32_t kMkwFpControlFlushToZeroBits = (1u << 24);
inline uint32_t g_mkwWasmFpControl = 0;
#else
#error "ppc_isa_fpenv.h has no host FP control register mapping for this architecture"
#endif

// Mirrors the FP control register of the threads running guest code. The guest worker
// (guest_worker.h) runs with the game thread's FPSCR, so both write the same NI state.
inline bool g_mkwHostNiActive = false;

// Same state in the form PpcForceSingleValueInline consumes: the pre-round subnormal threshold
// while NI is active, 0.0 (identity, `|value| < 0.0` is always false) otherwise, so that path
// needs no branch. Every writer of g_mkwHostNiActive must write this beside it in agreement.
inline constexpr double kMkwNiFlushThreshold = 0x1p-126; // 0x3810000000000000
inline double g_mkwNiFlushThreshold = 0.0;

// Host FP control register access (MXCSR on x86_64, FPCR on AArch64), abstracted so
// MkwApplyHostNiMode/MkwRestoreHostMxcsr and CpuContextScope (ppc_isa_context.h) don't each need
// their own per-arch branch.
inline uint32_t MkwGetHostFpControl() noexcept {
#if defined(__x86_64__)
    return _mm_getcsr();
#elif defined(__aarch64__)
    uint64_t fpcr = 0;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    return static_cast<uint32_t>(fpcr);
#elif defined(__arm__)
    uint32_t fpscr = 0;
    __asm__ __volatile__("vmrs %0, fpscr" : "=r"(fpscr));
    return fpscr;
#elif defined(__wasm__)
    return g_mkwWasmFpControl;
#endif
}

inline void MkwSetHostFpControl(uint32_t value) noexcept {
#if defined(__x86_64__)
    _mm_setcsr(value);
#elif defined(__aarch64__)
    // Read-modify-write the full 64-bit FPCR: only the low 32 bits are architecturally defined
    // today, but a bare 32-bit write would zero the (reserved) upper half of the real register.
    uint64_t fpcr = 0;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr = (fpcr & ~static_cast<uint64_t>(0xFFFFFFFFu)) | value;
    __asm__ __volatile__("msr fpcr, %0" ::"r"(fpcr));
#elif defined(__arm__)
    __asm__ __volatile__("vmsr fpscr, %0" ::"r"(value));
#elif defined(__wasm__)
    g_mkwWasmFpControl = value;
#endif
}

inline void MkwApplyHostNiMode(uint32_t fpscr) noexcept {
    const uint32_t csr = MkwGetHostFpControl();
    const bool wantNi = (fpscr & 0x4u) != 0;
    const uint32_t want = wantNi ? (csr | kMkwFpControlFlushToZeroBits) : (csr & ~kMkwFpControlFlushToZeroBits);

    if (want != csr)
        MkwSetHostFpControl(want);
    // `want` has every flush-to-zero bit set or every one clear, so this is exactly
    // `(MkwGetHostFpControl() & kMkwFpControlFlushToZeroBits) != 0` after the write - the
    // mirror cannot disagree with the register even if the incoming value held only some of
    // those bits.
    g_mkwHostNiActive = wantNi;
    g_mkwNiFlushThreshold = wantNi ? kMkwNiFlushThreshold : 0.0;
}

/// <summary>
/// Restores a previously captured MXCSR/FPCR value and re-derives the mirror from it. Every raw
/// restore has to go through here; a bare MkwSetHostFpControl would leave the mirror describing
/// the FP environment that was just replaced.
/// </summary>
inline void MkwRestoreHostMxcsr(uint32_t csr) noexcept {
    MkwSetHostFpControl(csr);
    const bool niActive = (csr & kMkwFpControlFlushToZeroBits) != 0;
    g_mkwHostNiActive = niActive;
    g_mkwNiFlushThreshold = niActive ? kMkwNiFlushThreshold : 0.0;
}
