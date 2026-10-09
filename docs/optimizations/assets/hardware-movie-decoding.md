# Movies on the Vita's hardware JPEG decoder

## What the original does

The game's movies are THP files: motion JPEG at 640×480, 29.97 fps, every
frame a baseline JPEG with the `0xFF00` byte stuffing removed from its scan.
The game decodes one frame per tick through `THPVideoDecode` (0x804E17A0)
into tiled I8 Y, U and V planes, which `DrawFrame` shows.

## What the port does instead

`THPVideoDecode` is native (`runtime/src/native_thp.cpp`). Everywhere it
can use a software decoder (`runtime/native/mh3_thp_video.h`); on the Vita
the frames decode on the media engine through SceJpeg
(`runtime/vita/thp_jpeg.cpp`), ahead of the game:

- The movie player keeps a ring of ten read frames (player 0x806D6B80,
  `+0xD4`: ten `{buffer, frame, ready}` entries). When the game decodes
  frame N, frames N+1 and N+2 are usually read already; the port hands them
  to two worker threads, one per hardware decoder.
- The game thread puts the byte stuffing back into a frame's stream when it
  hands the frame over (SceJpeg needs it; the re-stuffed frames decode
  bit-identically to ffmpeg's THP decoder).
- Each worker decodes into uncached, physically contiguous main memory (1 MiB
  per decoder: the stuffed stream, then the planes). The kernel only takes
  one physical range: CDRAM, or main memory when the decoder is initialized
  with option 1 (`sceJpegInitMJpegWithParam`), which also makes the kernel
  maintain the caches. Anything else fails with `0x80650053`.
- A frame is matched by address, size and a checksum of its data, so a new
  movie at the same buffer addresses is never taken for an old one. A first
  frame or a jump decodes while the game waits.
- The game thread tiles the planes into the game's with NEON, reading the
  uncached rows 32 bytes at a time (each uncached load is its own bus
  access).
- At 60 fps two packets show the same movie frame: its 450 KiB of planes
  are copied once, immutable, and shared by the packet slots until the
  frame changes (the game thread's `memcpy` samples halved).
- Anything the hardware refuses falls back to the software decoder.

`sceJpegDecodeMJpegYCbCr` takes the output buffer before the decode mode;
VitaSDK's header orders them differently.

## Why

The software decoder took about 26 ms of the game thread per frame on the
Cortex-A9, so the game ran at 30 fps during a movie. The Vita's video player
(SceAvPlayer) plays H.264 only, and the port converts the user's disc image
on the console, which has no encoder, so transcoding was not an option.

The media engine runs at 166 MHz and only the kernel can raise it: a
synchronous hardware decode (27-33 ms an opening-movie frame) was slower
than software. Decoding ahead on two decoders hides that latency.

Opening movie on the Vita:

| | Software | Hardware, synchronous | Hardware, ahead |
| --- | ---: | ---: | ---: |
| Game during the movie | 30 fps | 17-20 fps | 60 fps |
| Game thread per movie frame | 26 ms decode | 40 ms decode + 4.6 ms tiling | 0.5 ms wait + 0.5 ms stuffing + 5.6 ms tiling |
| Frames decoded ahead | | | 599 of 600 |

The planes match the software decoder's within 2 levels (the IDCTs differ).
Tiling on the workers into cached buffers left 5 ms of copying them over:
no gain.

## Applying it to another Wii game

Generic: every THP game. THP is motion JPEG, the format and the player
structure come from the SDK, and the ring of read frames exists in the
SDK's player. The stuffing fix, the physical-memory requirement, the
argument order and the decode-ahead design apply as they are.

Game-specific: where the game keeps its read-ahead ring and how many frames
it reads ahead; a game that reads only one frame ahead gains less.
