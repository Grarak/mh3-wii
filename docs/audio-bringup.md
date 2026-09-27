# Audio bring-up

The original AI initializer at `0x8046DC00` now completes and boot reaches AX/DSP
initialization. This is register/clock support, not audible output. PCM device
output, DSP command processing, and interrupt delivery are still unfinished.

`AudioInterfaceState` implements the four AI registers at offsets 0, 4, 8, and
12 from `0x0C006C00`/`0x0D006C00`, including cached/uncached aliases. Reset selects
48 kHz streaming and 32 kHz DSP DMA, with playback stopped. The stream counter
uses monotonic elapsed time and retains fractional samples between reads.
Control writes support rate changes, counter reset, and write-one-to-clear
interrupt status. The comparator latches status across counter wrap. This does
not yet assert the guest PI interrupt line. Volume and comparator registers are
retained for the future output/event bridge.

Register semantics were checked against
[Dolphin AudioInterface.cpp](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/AudioInterface.cpp).
Deterministic self-tests cover both rates, fine-grained reads, stopping, reset,
counter wrap, interrupt acknowledgement, aliases, and invalid addresses.

The next observed stop was DSP control register `0xCC00500A`, reached through
`800E8794 -> 8046EAB0 -> 80470930 -> 80470840 -> 804A5600`.
The SDK banners identify AI, AX, and DSP revision `0x4302_145`.

WiiCompiled provides an AX DSP HLE mixer in `runtime/src/hle/audio/ax_mix.cpp`,
with shared helpers in `ax_internal.h`, `ax_memory.cpp`, and `ax_mix_kernels.h`.
Its game-specific addresses must be adapted and its PB/command layout verified
before reuse. MH3's `0x80470840` is the AX DSP-task setup boundary. Disassembly
establishes the following fields; these are evidence for the next integration,
now used by the native bindings:

| Field | MH3 value |
| --- | --- |
| DSP task | `0x806FDA60` |
| IRAM source | `0x80611240` |
| DRAM source | `0x806FDAC0` |
| DRAM length / DSP address | `0x40` / `0xCD2` |
| Init / resume callbacks | `0x804707B0` / `0x804707C0` |
| Done / request callbacks | `0x80470820` / `0x80470830` |
| IRAM length | halfword at `r13 - 0x50FC` |
| Init / resume vector | halfwords at `r13 - 0x5100` / `r13 - 0x50FE` |
| AX DSP-ready / related flag | words at `r13 - 0x3EB0` / `r13 - 0x3EB4` |

The native mixer is now integrated in `runtime/src/audio`. MH3's 8 KiB DSP
program hashes to `0x4CC52064`, selecting the mixer's known `SkipUpdatesOnly` PB
layout and new AX command layout. Both startup and task configuration reject
other DSP programs. Mixing is synchronous during bring-up. Mailbox functions
and `__AXOutInitDSP` are native boundaries; game-side AX state setup remains
translated.

`MH3 --audio-self-test` loads the actual DOL data, validates the DSP binding, and
sends a synthetic command list through the mailbox. All 96 stereo output frames
are compared with known L/R ramps, including big-endian storage and Wii R/L
channel order. The test also checks startup/sync/yield mails and MEM2 bounds.
The shared MEM2 size constant was corrected from 128 MiB to the runtime's actual
64 MiB allocation; otherwise the mixer could not resolve its MEM2 base.

Build scripts validate the local coefficient file at `Assets/DSP/dsp_coef.bin`,
copying it from the existing sibling WiiCompiled checkout only if absent. The
file remains ignored, as do game assets. `WIICOMPILED_AUDIO_ROOT` can select a
different local source checkout. Source attribution is in `toolchain/ORIGIN.md`.

Boot then passed DSP-task setup and reached `AIInitDMA` (`0x8046DAF0`), called
from `0x80470930`. It stopped at `0xCC005030` while programming buffer `0x806FD5E0`,
length `0x180` (96 stereo PCM frames). The DMA/output bridge below now passes that setup boundary.


Audio DMA entry points `8046DAF0`, `8046DB70`, `8046DB90`, `8046DBB0`,
`8046DBC0`, and `8046DBE0` now manage the buffer, enable state, remaining bytes,
and register queries. Address/length masks match the SDK register encoding.
The bridge uses elapsed monotonic time and the AI DMA sample-rate bit. A
384-byte buffer takes 3 ms at 32 kHz or 2 ms at 48 kHz. Catch-up is bounded to
16 blocks per service call, retaining remaining elapsed time. Each completion
joins the mixer, queues PCM, invokes the guest DMA callback with interrupts
masked, then services AX resume callbacks. A reentrancy guard and full CPU-state
restoration protect guest execution.

Service points currently are VIWaitForRetrace, OSSleepTicks, and AIGetDMABytesLeft. General
thread scheduling and audio servicing during other long waits remain incomplete.
The SDL output converts big-endian Wii R/L frames to host L/R frames, accepts
32/48 kHz, and bounds queued latency at 250 ms with an explicit drop diagnostic.
DMA callback stack switching to a dedicated SDK stack is not implemented.

`--audio-self-test` now also verifies block timing, partial bytes remaining,
carry across blocks, stop behavior, and conversion of the mixer's 96-frame
stereo ramp. `build/metal/MH3 --audio-output-check` passed against the actual
macOS SDL output device with 100 ms of silence. This proves device opening and
queue submission, not audible game output.

The scaled-copy and timed-wait bridges now pass the initial DMA service boundary.
`logs/alarm-boot.log` confirms SDL device opening at 32 kHz, three logged DMA
completions with guest callbacks, two retraces, and original OS alarm callbacks.
The subsequent graphics bridge renders the wrist-strap screen and stops at a
NAND directory-creation request with LR `0x804B26AC`.
This verifies startup callback execution, not audible music or gameplay audio.
