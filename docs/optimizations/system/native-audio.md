# Native audio instead of AX and the DSP

## What the original does

Wii audio runs in three layers:

- the game's **sound engine** (Capcom's own driver), called from the AX
  frame callback (0x800E86FC) every 3 ms audio frame: it walks the active
  sound objects, ticks envelopes, 3D panning and distance volume, refills
  streams from the disc, and sets voice parameters;
- the SDK's **MIX** (dB settings to voice volumes), SEQ and SYN libraries;
- **AX**, which keeps a parameter block per voice (AXPB) in main memory and,
  every frame, sends a command list to the **DSP**. A DSP microcode decodes
  ADPCM and PCM, resamples with polyphase coefficients from the DSP's
  coefficient ROM, applies envelopes and filters, mixes the buses and runs
  aux effects; the result reaches the audio interface (AI) by DMA at 32 kHz.

## What the port does instead

All three layers are native C, and there is no DSP
([native audio](native-audio.md)):

- **Sound engine** (`runtime/src/audio/sound_engine.cpp`,
  `sound_stream.cpp`): 109 entry points, decompiled and ported. Its objects,
  records and queues stay in guest memory, where the game's thread creates
  them.
- **MIX, SEQ, SYN** (`sdk_mix.cpp`, `sdk_synseq.cpp`): MIX skips settled
  channels, and a setter that repeats a channel's stored value leaves it
  settled (the engine sets pan and volume every 3 ms, mostly to the values
  they have); the Wii Remote speaker sends are not computed.
- **Voices** are 96 host records (`native_audio_api.cpp`) holding what the
  mixer reads from an AXPB. The SDK's allocation rules are kept (priority,
  stealing, drop callbacks). Guest code still names a voice by the address
  its AXVPB had, which is never dereferenced. The AX entry points the game
  still calls are thin wrappers over these records.
- **Frame driver** (`native_audio_frame.cpp`, game thread): at each pump
  point it produces 3 ms ticks until it is a measured lead ahead of the
  mixer, sends each tick's changed voice fields to a command ring, takes
  state, envelope and position back, and runs the engine's frame callback.
- **Mixer** (`native_audio_mixer.cpp`, an audio thread on a Vita worker core):
  ADPCM/PCM decode, 4-tap polyphase or linear resampling, envelope, lowpass
  and biquad filters, main and aux buses, the AXFX ReverbHi effect, 32 kHz
  stereo out. The Vita output thread blocks in `sceAudioOutOutput`, which is
  the clock.

Approximations: the resampler uses Hann-windowed sinc tables instead of the
DSP's coefficient ROM, aux returns happen in the same frame, a stopped
voice fades over one frame, the initial time delay and the Wii Remote
speaker are not mixed, and surround is folded into left and right.

## Why

The first port emulated AX and the DSP at a high level (WiiCompiled's
mixer, run on a worker). The DSP command lists, the AXPB sync through guest
memory and the guest callbacks cost the game thread time, and on the Vita
the recording underflowed. Measured:

- Mixing natively on core 0 instead of emulating the DSP: Vita desert
  quest (10057/6) 14.4-14.9 → 20.3 fps with continuous audio; the mixer took
  18.5% of core 0 (commit "Mix AX voices natively on core 0 instead of
  emulating the DSP").
- Dropping AX: game-thread audio on arm64 Linux 2.94 → 1.07 ms per second of
  play (commit "Drop AX: native voices driven by the sound engine and MIX").
- One-copy disc reads and skipping settled MIX channels: the Vita's sound
  frame median 33 → 4.5 ms/s in the Alatreon fight
  ([native audio](native-audio.md)).
- Keeping channels settled when a setter repeats their value: the Vita's
  game-thread audio service in the Alatreon fight 42-49 → 26 ms/s (voice
  updates 7-9 → 1.7, the sound frame 34-40 → 23), about 0.3 ms per game frame
  (commit "Keep mixer channels settled when a setter repeats their value").

The host mix matched the AX/DSP emulation's when it replaced it (mean
-15.3 versus -14.8 dB, the same spectrogram structure).

## Example

The order of work per tick, from the header of
`runtime/src/audio/native_audio_frame.cpp`:

```text
For each tick:
  1. the voices: the changed fields of the voices set since the last tick
     go to the mixer, tagged with the tick; those voices' fields the game
     did not set get the mixer's state, envelope volume and current address
     back; freed voices are stopped;
  2. the frame callback the game registered with AXRegisterCallback: the
     sound engine's frame (native), or a guest callback.
The aux callbacks are not called: the mixer runs the reverb natively.
```

## Applying it to another Wii game

Generic: the voice records, the frame driver, the mixer and the ReverbHi
effect implement what AX and the standard DSP microcode do for any game
that uses AX. A game that drives AX directly (without its own engine) could
use them with only the AX wrappers.

Game-specific: the sound engine above AX. MH3's is Capcom's; other games use
NW4R's sound library (`nw4r::snd`), the SDK's SEQ/SYN, or their own. Porting
the engine is optional: keeping it translated works, at the cost of guest
calls every 3 ms. Check a port with the engine's own entry points as port
checks (`MH3_PORT_CHECK=...,originals` here) and by comparing dumped mixes
(`MH3_CAPTURE=audio=FILE`).
