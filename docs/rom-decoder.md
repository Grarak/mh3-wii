# Disc image decoder

`RomImage` (`runtime/src/rom_image.cpp`) reads the European disc image in
place through four C functions (`runtime/vita/rom_decoder.h`): open with a
64-bit positioned-read callback, the disc and partition sizes, a read, close.
`runtime/src/rom_decoder.cpp` implements them in C++ for every target (arm64
and arm32 Linux, Vita); no build needs a Rust toolchain.

| Image | Handling |
| --- | --- |
| RVZ, WIA | Zstandard, bzip2, LZMA, LZMA2 or no compression; RVZ packing with its junk generator. WIA Purge is rejected, as nod 1.4.4 rejected it. |
| ISO | AES-128-CBC partition sectors with the title key from the ticket (Wii common key). An ISO with `no_partition_encryption` set (`nodtool convert --decrypt`) has plain sectors; `RomImage` reads it without the decoder. |
| CISO, GCZ, WBFS, TGC, NFS | Rejected with a message to convert the image. The launcher only lists `.iso`, `.rvz` and `.wia` files. |

Reads return what the nod 1.4.4 bridge returned for the same calls. A flat
read gives the game partition's data, 0x7c00 bytes per sector. A disc read
gives partition sectors decrypted: an ISO sector with its decrypted hash
block, a WIA/RVZ sector with a zeroed hash block. Hashes are never rebuilt,
so the exception lists are skipped. The WIA/RVZ header hashes are checked.
RVZ/WIA chunks above 2 MiB and tables larger than the old bridge allowed are
rejected before anything is allocated. Up to four decoded groups are kept (1
MiB; one group for 2 MiB chunks), so interleaved readers don't decode the
same group again. Each group is one positioned read of the image.

## Read-ahead

`RomImage` starts one worker thread (`mh3_rom_read_ahead`) with its own codec
and buffers. When the reading thread is about to decode a group itself (no
cached slot and not at the worker), it first hands the next group to the
worker if the read needs it or the read continues where the previous one
ended (a sequential stream, such as the DVD layer's 128 KiB pieces). The
reader takes the worker's group when it gets there, waiting while it is
decoded, and decodes the one after itself: the two alternate, so two groups
decode at once and one's image read overlaps the other's decompression. A
group queued and not begun gives way to the next one queued, or is decoded by
the reader itself. Disc (raw) reads never use the worker.

On the Vita the worker runs on core 3 with the other workers (the DVD reader
thread that calls the decoder among them), below the default priority so the
guest worker and the reader run first.

Checked with a standalone comparison of two decoders, with and without the
worker: the whole 4 GiB game partition in 512 KiB reads and 20,000 random
reads, byte for byte equal. Pi 5, warm page cache, sequential flat reads in
MiB/s:

| Read size | Without | With the worker |
| --- | ---: | ---: |
| 512 KiB | 482 | 803 |
| 128 KiB stream | 482 | 816 |
| 32 KiB stream | 633 | 1,047 |

Vita (`MH3_PROFILE=disc`, Alatreon quest): decoded 512 KiB reads 15.9 ->
20.5 MiB/s, the image's share of the time 34% -> 68% (the card's 10-11.5
MiB/s is now the limit); the village motion archive's 512 KiB read 210 -> 101
ms. The quest's load time stayed 11.6 s: it is not bound by decoding.

Codecs: Aurora's zstd 1.5.7, libbzip2 1.0.8 (`runtime/third_party/bzip2`) and
the LZMA SDK 25.01 decoders (`runtime/third_party/lzma`), all unmodified. An
LZMA stream decodes straight into its group buffer, which serves as the
dictionary, so a large dictionary size costs no memory. AES-128 (32-bit
tables built at compile time) and SHA-1 are local.

Every build decodes; there is no option to leave the decoder out.

## Vita runtime helpers

The Vita program's runtime helpers come from these sources:

- The EABI memory helpers (`__aeabi_memcpy`, `memmove`, `memset`, `memclr` and
  their 4/8 forms) come from `runtime/vita/aeabi_memory.c`, a C port of the
  compiler-builtins 0.1.160 routines: scalar word loops in the generic
  bodies, a 16-byte NEON loop in the aligned copy and set (it changes
  `d16/d17`), the 4/8 and move forms sharing one body each. The SDK's would
  be a byte loop for move and a scalar loop for set. On an idle Pi 5, ns per
  call at 4 KiB / 32 KiB, aligned:

  | | C port | SDK |
  | --- | --- | --- |
  | `__aeabi_memcpy4/8` | 167 / 1,294 | |
  | `__aeabi_memcpy` | 781 / 6,137 | |
  | `__aeabi_memmove` | 773 / 6,215 | 3,191 / 25,478 |
  | `__aeabi_memset` | 446 / 3,439 | 253 / 2,004 |
  | `__aeabi_memset4/8` | 112 / 868 | |
- Integer division and long/float conversions (`__aeabi_uidiv`,
  `__aeabi_uidivmod`, `__aeabi_ldivmod`, `__aeabi_f2lz`, ...) come from libgcc;
  `floor`, `ceil`, `round`, `trunc`, `fmod`, `sqrt` and their float forms,
  `fabs`, `fmin` and `fmax` from newlib's libm. These have exactly specified
  results.
- The C helpers have ordinary CFI for snapshot unwinding.

## Verification

A harness linked to either implementation hashed (XXH64 per 256 MiB) every
byte each returned: the whole flat partition (4,286,328,832 bytes), the whole
disc-level stream (4,699,979,776 bytes) and 40,000 random flat reads (1 B to
256 KiB). They were identical for:

- the European RVZ (zstd 19, 128 KiB chunks; 17,287 of its 35,859 groups are
  stored uncompressed, 13,569 packed) and the US RVZ;
- a Redump-verified encrypted ISO, and nodtool 2.0 conversions to bzip2 RVZ,
  LZMA2 RVZ and LZMA WIA (2 MiB chunks).

The decrypted ISO gives the same partition as the RVZ. `MH3 --rom` writes a
`disc/` identical to the nod build's, on arm64 Linux and on the Pi (arm32
build), and Alatreon ran from the RVZ on arm64 Linux. SHA-1 matches `sha1sum` for
lengths 0–299 and AES the FIPS-197 vector.

Decode speed on the Pi 5, arm32 Cortex-A9 code, one thread, 1 MiB reads,
alternating runs (3 each):

| | nod 1.4.4 | C++ |
| --- | --- | --- |
| RVZ zstd, first GiB of the partition (page cache) | 191–193 MB/s | 203–207 MB/s |
| RVZ zstd, 4,000 random reads | 111 MB/s | 120 MB/s |
| RVZ zstd, first GiB of the disc-level stream | 151 MB/s | 234 MB/s |
| Encrypted ISO, 384 MiB of the partition (image in memory) | 45–46 MB/s | 108–117 MB/s |
| Encrypted ISO, 3,000 random reads (image in memory) | 37 MB/s | 91 MB/s |

On 32-bit ARM, nod's `aes` crate uses its bit-sliced software form; the
decoder's table form is most of the ISO gain.
