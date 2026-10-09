# The Wii system layer: IOS, OS and devices

## What the original does

Below the game, the Wii runs:

- the SDK's **OS** library on the PowerPC: threads, interrupts, exception
  vectors, alarms driven by the decrementer, `OSSleepTicks`;
- hardware devices the SDK programs directly: VI (video timing, the retrace
  interrupt), SI (controllers), EXI, AI/DSP (audio), the GX FIFO;
- **IOS** on the Starlet ARM core, reached through IPC: the disc drive
  (`/dev/di`), the NAND file system (`/dev/fs`), titles and tickets
  (`/dev/es`), the state manager (`/dev/stm/*`), USB HID, the network.

The SDK's code for all of this is part of the DOL and is translated with
the game.

## What the port does instead

The port keeps the SDK code above a narrow boundary and replaces what is
below it (high-level emulation):

- **IOS at its API.** `IOSOpen`, `IOSIoctl`, `IOSIoctlv` and their async
  forms (0x804BC500 ... 0x804BD220) are native (`runtime/src/ios.cpp`): they
  answer `/dev/di` from the disc image ([disc access](../assets/disc-access.md)),
  `/dev/fs` from host files ([saves](saves-nand.md)), the `/dev/es` queries
  the boot needs, STM events and HID enumeration. Async replies are queued
  and delivered later.
- **Interrupts are calls at pump points.** Nothing interrupts translated
  code. `PumpDevices` (`runtime/src/os_sleep.cpp`) delivers host window
  events, IOS completions, audio work, due OS alarms and the VI retrace.
  It runs where the game would wait anyway: `VIWaitForRetrace` (0x804E8050),
  `OSSleepTicks` (0x804D5320), the frame pacer and the drive wait
  (`WaitDvd`, 0x80435EA8).
- **VI retrace** (`ServiceVideoRetrace`, `runtime/src/vi_services.cpp`): at
  each due retrace on the host clock the runtime calls the game's own VI
  interrupt handler (0x804E6F30) with interrupts masked, so the SDK's
  retrace count and callbacks advance as on the Wii. Missed retraces are
  skipped, not queued.
- **OS alarms** (`runtime/src/os_alarm.cpp`): the SDK still creates,
  inserts, cancels and reschedules alarms; only the decrementer interrupt
  is replaced by a check of the alarm queue at pump points.
- **One guest thread.** There is no guest thread scheduler. `OSSleepTicks`
  stops the game if another guest thread is runnable while it sleeps; MH3
  never needs a switch there. (A second host thread does run guest code,
  the [guest worker](../threading/guest-worker.md), but only on jobs the game thread
  hands it.)
- **Boot and probes**: exception and interrupt init, memory protection,
  EXI/SI probes, SRAM (persisted to `saves/sram.bin`), the audio system
  init and console output are small natives (`runtime/src/os_boot.cpp`,
  `os_devices.cpp`).
- **The main loop's wait** (0x800401A8) is ported to C and paces on the
  host clock instead of counting retraces ([variable step](../simulation/variable-step.md)).

## Why

Emulating Starlet and the devices at register level would mean running
IOS itself and interrupting translated code at arbitrary points, which a
static translation cannot do. The game only needs the SDK-level behaviour:
replies, callbacks and retrace counts, in the right order. Delivering them
at pump points keeps every guest callback on the game thread, between
whole translated functions, where the guest's own state is consistent.

## Example

`runtime/src/vi_services.cpp`, the game's wait for the next retrace:

```cpp
extern "C" void MH3_VIWaitForRetrace(CpuContext* cpu) {
    std::this_thread::sleep_until(nextRetrace);
#ifdef MH3_HAS_AURORA
    ServiceHostGraphicsEvents();
#endif
    PumpIosCompletions(cpu);
    ServiceAudioDma(cpu);
    NativeAudio::ServiceNativeAudio(cpu);
    ServiceOsAlarms(cpu);
    ServiceVideoRetrace(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x804E8050, MH3_VIWaitForRetrace, "MH3_VIWaitForRetrace");
```

## Applying it to another Wii game

Generic: the IOS device answers, the pump points and the alarm and retrace
delivery follow the RVL SDK, which every Wii game links.

Game-specific: the SDK functions' addresses (find them by signature or
from symbols; MH3's SEL file exports many names), and whether the game
needs real thread switching. A game that sleeps or waits on a message
queue while another guest thread must run needs a scheduler for guest
threads (the inherited `runtime/include/fiber_manager.h` describes one for
host fibers; this port does not use it). Find out early: put a check like
`OSSleepTicks`'s at every blocking SDK call and run the game.
