// MH3_PORT_CHECK=Name,Name... or all (development builds):
// checks native ports against the translated originals they replace. From one RAM and register
// snapshot the original runs, then (restored) the port; every word of
// MEM1/MEM2 that ends up different is reported, once per distinct first
// difference, except the guest stack below the caller's stack pointer (the two
// use different frames) and the LR save word. Only at the stock step
// (GameStep::Ticks() == 1), where a port must match the original. Host-side
// effects of the extra pass (audio, GX) are not undone. Checks are rate-limited
// (one every 500 ms, one per port every 2 s) unless the `every` option is set.
#pragma once

#include "abi_bridge.h"

#include <atomic>
#include <cstdint>
#include <initializer_list>

namespace PortCheck {
// Whether MH3_PORT_CHECK is set at all: -1 until the first test asks, then 0
// or 1. The ports test their checks on every call, so the usual answer is
// one relaxed byte load; a function-local static would test its guard
// instead, which on ARM32 is a dmb barrier per call.
inline std::atomic<int8_t> g_set{-1};
bool SetSlow();
inline bool Set() {
    const int8_t set = g_set.load(std::memory_order_relaxed);

    if (set >= 0) [[likely]]
        return set != 0;
    return SetSlow();
}

bool EnabledSlow(const char* name);

// Whether MH3_PORT_CHECK names the port (or `all`). Cheap enough to test per
// call, so callers need no static copy.
inline bool Enabled(const char* name) {
    return Set() && EnabledSlow(name);
}

// The `originals` option: the sound engine's entry points not named run their
// translated originals, so a named port is reached from original callers.
bool OriginalsSlow();
inline bool Originals() {
    return Set() && OriginalsSlow();
}

// Runs original(ctx) and port(ctx) from the same state, reports differences,
// and leaves the port's result in place.
void Run(CpuContext* ctx, const char* name, void (*original)(CpuContext*), void (*port)(CpuContext*));

// Run that also compares the return register (r3, or f1).
enum class Returns;
void Run(CpuContext* ctx, const char* name, Returns returns, void (*original)(CpuContext*), void (*port)(CpuContext*));

// For ports that write no guest memory but their callee stack frames: runs
// the original and then the port from the same registers on every call and
// compares the return register (r3 when gpr, f1 otherwise) bit for bit,
// reporting each distinct mismatch with the argument registers. No RAM
// snapshot, so it can check every call.
enum class Returns {
    Gpr3,
    Fpr1
};

void RunPure(CpuContext* ctx, const char* name, Returns returns, void (*original)(CpuContext*), void (*port)(CpuContext*));

// For ports whose only guest writes (besides their stack frames) are a few
// output buffers: runs the original and then, with the buffers restored, the
// port on every call and compares the buffers' bytes, reporting each distinct
// mismatch with the argument registers.
struct Region {
    uint32_t address, bytes;
};

void RunOutput(CpuContext* ctx, const char* name, void (*original)(CpuContext*), void (*port)(CpuContext*), std::initializer_list<Region> outputs);

// RunOutput that also compares the return register.
void RunOutput(CpuContext* ctx, const char* name, Returns returns, void (*original)(CpuContext*), void (*port)(CpuContext*), std::initializer_list<Region> outputs);

// MH3_PORT_CHECK=...,P:0xADDR,E:0xADDR,... (development): ports the game rarely
// reaches, checked from a spread of made-up states. The PortCheck::Fuzz hooks
// (the player's in PlayerStepTimers, an enemy's in EnemySight) fire once each,
// on their fuzz-after-th call (default 900), and call each listed function of
// their kind (P: player, E: enemy) through the dispatcher, so the ports among
// them must also be named, where every one of these calls is checked. Player: action state (+5) 0..7 with the step counter
// (+0x20) 25 times it, countdown (+0x28) 1, 2, 40 or 400, r4 = r5 = 0 or 1
// (so no functions taking pointers there). Enemy: the step counter over 0..63, r4 = r5
// = 0 or 1. RAM, the clock and the registers are put back after each call.
// C:0xADDR (player hook): called once as (PLW+0x3C, a zero vector, 0, PLW)
// and kept, to start something rare whose updates the checks then see
// (eft045_set 0x8031EBB8: ObjectListScrollUpdate).
// A:0xADDR[:KIND] (player hook): an enemy action (work), run once on an active
// enemy of that kind (work +3), one in the current area if any, from
// fuzz-after on, over samples of its state, substate, count, motion frame (or
// the fuzz-frames=V/V/... ones), the event demo clock (or the
// fuzz-demo=V/V/... steps) and the step counter: actions only a
// first-encounter demo reaches (monster_intros.cpp) and others of kinds the
// arena lacks.
void FuzzSlow(CpuContext* ctx, char kind, uint32_t work);
inline void Fuzz(CpuContext* ctx, char kind, uint32_t work) {
    if (Set()) [[unlikely]]
        FuzzSlow(ctx, kind, work);
}

// The port, checked against the original when MH3_PORT_CHECK names it.
inline void Call(CpuContext* ctx, const char* name, void (*original)(CpuContext*), void (*port)(CpuContext*)) {
    if (Enabled(name))
        Run(ctx, name, original, port);
    else
        port(ctx);
}

// Call for ports with a result: the check compares the return register too.
inline void Call(CpuContext* ctx, const char* name, Returns returns, void (*original)(CpuContext*), void (*port)(CpuContext*)) {
    if (Enabled(name))
        Run(ctx, name, returns, original, port);
    else
        port(ctx);
}

// The check of a precise native: its registered body names only constant-index
// registers, so translated callers sync just those
// (docs/optimizations/cpu/native-function-ports.md), and it must not hand them its context.
// It passes r1 instead; the check runs on the thread's context and leaves the
// port's result there. Whether the check ran.
bool RunPreciseSlow(const char* name, uint32_t r1, bool pure, Returns returns, void (*original)(CpuContext*), void (*port)(CpuContext*));

inline bool RunPrecise(const char* name, uint32_t r1, void (*original)(CpuContext*), void (*port)(CpuContext*)) {
    return Enabled(name) && RunPreciseSlow(name, r1, false, Returns::Gpr3, original, port);
}

// RunPure of a precise native.
inline bool RunPurePrecise(const char* name, uint32_t r1, Returns returns, void (*original)(CpuContext*), void (*port)(CpuContext*)) {
    return Enabled(name) && RunPreciseSlow(name, r1, true, returns, original, port);
}
} // namespace PortCheck
