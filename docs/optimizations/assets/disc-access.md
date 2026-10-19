# Disc access: the image read in place, reads on a reader thread

## What the original does

The game reads its data through the SDK's DVD library, which sends reads to
IOS's `/dev/di` as `IOS_IoctlAsync` (DI command 0x71) in chunks of at most
512 KiB. The drive works while the game runs; the reply reaches the
library's callback later. MH3's file loader (`load_file_req` 0x800CEB14,
served once a frame by 0x800CE704) issues a read and polls for the reply
each frame, so its loading screens animate. Its synchronous loads
(`load_file` 0x800CECC8) spin in `WaitDvd` (0x80435EA8) until the drive is
idle. The disc is an encrypted Wii partition (AES-128-CBC sectors with a
title key from the ticket).

## What the port does instead

- **The user's image, read in place.** `RomImage` (`runtime/src/rom_image.cpp`)
  reads an ISO, RVZ or WIA image where it lies, with 64-bit positioned reads.
  A native decoder (`runtime/src/rom_decoder.cpp`) decrypts partition sectors
  and decompresses RVZ/WIA groups (Zstandard, bzip2, LZMA, LZMA2); partition
  hashes are neither rebuilt nor checked. The first launch writes only the
  boot, FST, DOL and IOS metadata (about 7 MB) to `disc/`.
- **`/dev/di` answered from the image** (`runtime/src/ios.cpp`): inquiry,
  reads, status and cover commands. A synchronous read copies straight into
  guest RAM.
- **Asynchronous reads on a reader thread** (`DiscReader`, `ios.cpp`; a
  worker core on the Vita). Replies keep their order and reach the callbacks at the next
  pump point ([system layer](../system/system-layer.md)). While no read waits, the
  reader reads the rest of the current DVD request ahead, so the next chunk
  comes from memory. `DVDCancelAsync` answers the read in flight and drops
  its data.
- **RVZ groups decode ahead.** A worker with its own codec and buffers
  decodes the group a read needs next, or the next group of a sequential
  stream, while the reading thread decodes the current one, so card reads
  and Zstandard overlap.
- **Aligned reads.** On the Vita the image is read into 64-byte aligned
  buffers at 512-byte multiple offsets, in large requests (the SDK's I/O
  guidance).
- **A read is answered once its files are converted.** Texture and model
  files are converted while the game loads them ([asset
  conversion](load-time-asset-conversion.md)); the reader holds a reply
  until the files it touches are done, so nothing that looks a converted
  pack up later has to wait. Reads also patch texture headers with the tags
  of their converted textures, in memory; the image is never modified.
- **`WaitDvd` is native.** While the drive is busy it pumps devices and
  sleeps until the reader's next reply: the retrace that refreshes the drive
  status the original polls arrives only at pump points.
- **Audio streams** are copied from the image to plain files at first read
  and prefetched on each map's loading screen ([native audio](../system/native-audio.md)).

## Why

- Extracting the partition first was slow: on the Pi 5, first start went from 8 min 42 s (decoding the 4.1 GiB
  partition to the SD card) to 0.25 s.
- Reads and conversion on other threads keep the game thread drawing loading
  screens. Pi 5 cold Moga Woods: the longest loading-screen frame went from
  115 ms to 72-84 ms; warm, the loading screen takes 0.60 s instead of
  1.15 s. Vita cold Moga Woods: the three loading screens ran 466, 2,243
  and 2,347 frames instead of 32, 89 and 109, with about the same total load
  time.
- The RVZ decode ahead: Pi 5 482 → 803-816 MiB/s for 128 and 512 KiB
  streams; Vita decoded reads 15.9 → 20.5 MiB/s (card-bound), the village
  motion archive's read 210 → 101 ms.
- The nod library the decoder replaced was a Rust dependency on every
  target; the C++ decoder removed the Rust toolchain from the build.

## Example

The DI read in `runtime/src/ios.cpp` (synchronous form):

```cpp
if (command == 0x70 || command == 0x71) {
    const uint32_t length = command == 0x70 ? 0x20 : Memory::Read32(input + 4);
    const uint32_t position = command == 0x70 ? 0 : Memory::Read32(input + 8);
    ...
    // Straight into guest RAM (bytes are bytes; no per-byte checks).
    const auto bytes = Disc().Read(uint64_t(position) * 4, length);
    std::memcpy(Memory::GetPointer(output, length), bytes.data(), length);
    RecordNativeDiscWrite(output, uint64_t(position) * 4, length);
```

`RecordNativeDiscWrite` tells the renderer's resource tracking that the
range now holds disc bytes.

## Applying it to another Wii game

Generic: the image decoder, `/dev/di` and the reader thread work for any
Wii disc. The decoder needs the Wii common keys (embedded in
`rom_decoder.cpp`, as in Dolphin and nod).

Game-specific: how the game waits for reads. A game that polls for replies
(as MH3's loader does) keeps animating; a game that blocks needs its wait
ported so that it pumps devices. The hook that delays a reply until the
read's files are converted only matters with load-time conversion; it needs
to know which byte ranges belong to which file (the FST gives that).
