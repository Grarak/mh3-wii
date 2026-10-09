// MH3_PORT_CHECK=Name,Name... or all (development builds): checks native ports
// against the translated originals they replace. From one RAM and register
// snapshot the original runs, then (restored) the port; every word of
// MEM1/MEM2 that ends up different is reported, once per distinct first
// difference, except the guest stack below the caller's stack pointer (the two
// use different frames) and the LR save word. Only at the stock step
// (GameStep::Ticks() == 1), where a port must match the original. Host-side
// effects of the extra pass (audio, GX) are not undone. Checks are rate-limited
// (one every 500 ms, one per port every 2 s) unless MH3_PORT_CHECK_EVERY=1.
#pragma once

#include "abi_bridge.h"

#include <cstdint>
#include <initializer_list>

namespace PortCheck {
bool Enabled(const char* name);
// Runs original(ctx) and port(ctx) from the same state, reports differences,
// and leaves the port's result in place.
void Run(CpuContext* ctx, const char* name, void (*original)(CpuContext*), void (*port)(CpuContext*));

// For ports that write no guest memory but their callee stack frames: runs
// the original and then the port from the same registers on every call and
// compares the return register (r3 when gpr, f1 otherwise) bit for bit,
// reporting each distinct mismatch with the argument registers. No RAM
// snapshot, so it can check every call.
enum class Returns { Gpr3, Fpr1 };
void RunPure(CpuContext* ctx, const char* name, Returns returns, void (*original)(CpuContext*),
             void (*port)(CpuContext*));

// For ports whose only guest writes (besides their stack frames) are a few
// output buffers: runs the original and then, with the buffers restored, the
// port on every call and compares the buffers' bytes, reporting each distinct
// mismatch with the argument registers.
struct Region {
    uint32_t address, bytes;
};
void RunOutput(CpuContext* ctx, const char* name, void (*original)(CpuContext*), void (*port)(CpuContext*),
               std::initializer_list<Region> outputs);

// MH3_PORT_FUZZ=P:0xADDR,E:0xADDR,... (development): ports the game rarely
// reaches, checked from a spread of made-up states. The PortCheck::Fuzz hooks
// (the player's in PlayerStepTimers, an enemy's in EnemySight) fire once each,
// on their MH3_PORT_FUZZ_AFTER-th call (default 900), and call each listed
// function of their kind (P: player, E: enemy) through the dispatcher, so the
// ports among them must also be named in MH3_PORT_CHECK, where every one of
// these calls is checked. Player: action state (+5) 0..7 with the step counter
// (+0x20) 25 times it, countdown (+0x28) 1, 2, 40 or 400, r4 = r5 = 0 or 1
// (so no functions taking pointers there). Enemy: the step counter over 0..63, r4 = r5
// = 0 or 1. RAM, the clock and the registers are put back after each call.
// C:0xADDR (player hook): called once as (PLW+0x3C, a zero vector, 0, PLW)
// and kept, to start something rare whose updates the checks then see
// (eft045_set 0x8031EBB8: ObjectListScrollUpdate).
void Fuzz(CpuContext* ctx, char kind, uint32_t work);

// The port, checked against the original when MH3_PORT_CHECK names it.
inline void Call(CpuContext* ctx, const char* name, void (*original)(CpuContext*), void (*port)(CpuContext*)) {
    if (Enabled(name)) Run(ctx, name, original, port);
    else port(ctx);
}
} // namespace PortCheck
