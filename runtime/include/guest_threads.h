// Guest OS threads: the SDK's OSThread scheduler with each guest thread on a
// host thread of its own (guest_threads.cpp). One guest thread runs at a time,
// as on the Wii's single core: SelectThread (0x804D4360) hands a baton from
// the thread it switches out to the one it selects. The game thread runs the
// boot thread and does all device work (PumpDevices), also while every guest
// thread waits (the idle loop).
//
// Offline the game never runs a second thread, and none of this changes what
// it does: OSSleepTicks keeps its plain pumping wait until a second guest
// thread has run. The online library starts threads (sNetworkLibraryWii, DWC,
// NHTTP) that sleep, wait for IOS replies and wake each other.
#pragma once

#include "isa/ppc_isa_context.h"

#include <chrono>
#include <cstdint>
#include <functional>

namespace GuestThreads {
using Clock = std::chrono::steady_clock;

// Whether a guest thread other than the boot thread has run.
bool Started();

// Interrupt boundary (the end of PumpDevices, on the game thread): wakes the
// threads whose sleeps or waits are over and switches to a woken thread of
// higher priority, as the SDK does when an interrupt returns.
void Service(CpuContext* cpu);

// The frame pacer's idle time: when other threads are runnable, the calling
// thread lets them run until all of them wait or `budget` has passed. The
// game advances without retrace waits, which on the Wii were where its
// lower-priority threads ran.
void Donate(CpuContext* cpu, std::chrono::nanoseconds budget);

// OSSleepTicks: puts the calling guest thread to sleep until `deadline`.
// False when it cannot switch threads (the scheduler is disabled, or the call
// comes from an interrupt callback's context); the caller then waits itself.
bool SleepUntil(CpuContext* cpu, Clock::time_point deadline);

// A blocking IOS request: the calling guest thread waits until `ready`
// returns true, which the game thread asks at every device pump. Other guest
// threads run meanwhile. Without thread switching (see SleepUntil) it pumps
// devices itself until then.
void BlockUntil(CpuContext* cpu, const std::function<bool()>& ready);

// Whether the calling host thread is the game thread (the boot thread's,
// which does device work), not another guest thread's.
bool OnGameThread();

// Wakes the game thread's idle loop early (a host helper finished work a
// waiter asks about).
void Notify();
} // namespace GuestThreads
