// Material uniforms in GPU memory (docs/optimizations/rendering/material-uniform-blocks.md).
//
// The fixed shading programs read a material from two uniform blocks:
//  - Material (static, mh3_shade_gpu_material, 784 bytes): every coefficient
//    of the classified shading equation as a static factor and a pick of at
//    most one animated palette entry, plus the BASIC selects/equations and the
//    MODULATE scale/bias. Made once per classified structure (a material
//    record) and kept in persistent GPU memory (GXM: CDRAM; GLES: one UBO).
//  - Animated (mh3_shade_gpu_animated, 272 bytes): the raw values that change
//    at run time: TEV colour registers and konst colours, the alpha compare
//    range, the role LOD bias and each role's post texture SRT (scale,
//    sin/cos, translation). MODULATE programs derive their coefficients from
//    it with uniform-only code (GXM's secondary program); the mesh vertex
//    program builds the post texture matrix of a pure-SRT role from it.
// BASIC programs do not evaluate picks (SGX secondary attribute budget): a
// BASIC record with animated picks gets a per-draw resolved Material copy.
// Blend state is part of a GXM fragment program: each program keeps one
// patched variant per blend/output state (PatchedProgram::bind).
//
// Today the render thread derives both blocks from the Render owner's
// material state. The functions below let a packet item reference them
// instead:
//
//  Game thread, per material record (once):
//    ref = AcquireNativeMaterialGpu(state)          // classify + bake
//    defaults = CreateNativeMaterialGpuAnimated(v)   // the record's own values
//  Game thread, per packet:
//    BeginNativeMaterialGpuPacket(sequence)          // before the first write
//    per animated draw:
//      auto* a = WriteNativeMaterialGpuAnimated(&item.animated);
//      fill *a (mh3_shade_gpu_palette / _alpha_range / _srt_from_slot ...)
//    other draws: item.animated = defaults
//  Render thread, per item, before SubmitNativeModelMesh:
//    SetNativeMaterialGpuDraw(item.material, item.animated, item.srt_roles)
//  Render thread, after the packet's draws:
//    EndNativeMaterialGpuPacket(sequence)
#pragma once
#include "native_owner.h"
#include "../native/mh3_shade.h"
#include "../native/mh3_texture_matrices.h"
#include <cstddef>
#include <cstdint>

// ---- Material records (static section) ----
// A classified structure: the TEV pipeline, swap tables and alpha compare
// functions of a material state (mh3_shade_structure_key). 0 is none.
struct NativeMaterialGpuRef {
    uint32_t index = 0;
};

// Any thread. Classifies and bakes the structure of `state` the first time it
// is seen and returns its record, valid for the session (records are never
// evicted; a quest uses a few dozen). The render thread uploads the Material
// block at the record's first draw. Returns none when the table (4096) is
// full: draw such materials without references.
NativeMaterialGpuRef AcquireNativeMaterialGpu(const mh3_native_material_state& state);

// What a writer of the record's Animated sections needs (immutable).
struct NativeMaterialGpuInfo {
    bool valid = false;
    bool supported = false;   // false: no GPU form (two animated factors), draw without references
    bool dynamic = false;     // a coefficient picks an animated palette entry
    bool cpu_resolve = false; // picks the program does not evaluate: the render thread reads the palette back
    uint8_t model = 0, textures = 0, alpha_test = 0;

    // Texture role -> texture coordinate generator (0xff unused). The role's
    // post SRT is the SRT slot its generator's post matrix row selects
    // (texgen POST_MATRIX_ROW / 3).
    uint8_t texture_coordinate[3]{0xff, 0xff, 0xff};
    uint64_t structure_key = 0;
};

NativeMaterialGpuInfo DescribeNativeMaterialGpu(NativeMaterialGpuRef material);

// ---- Animated sections ----
// A persistent section (bit 31) or an offset into the producer ring.
struct NativeMaterialGpuAnimatedRef {
    uint32_t value = UINT32_MAX;
};

// Any thread: a persistent section with these values (e.g. a material
// record's own colours, alpha compare, LOD bias and SRT); uploaded by the
// render thread at first use. Release returns it after the render thread's
// next EndNativeMaterialGpuPacket; no item recorded later may use it.
NativeMaterialGpuAnimatedRef CreateNativeMaterialGpuAnimated(const mh3_shade_gpu_animated& values);
void ReleaseNativeMaterialGpuAnimated(NativeMaterialGpuAnimatedRef animated);

// Producer ring (8 segments of 64 KiB, 240 sections each): GPU-visible memory
// the game thread writes directly (CDRAM on the Vita, no packet copy).
// Begin: game thread, before the first write of packet `sequence`; reuses the
// segment of the packet 8 sequences earlier once the render thread ended it
// (or a later packet) and the GPU completed it, waiting otherwise.
void BeginNativeMaterialGpuPacket(uint64_t sequence);

// Game thread: a write-only section of the current packet (fill all of it);
// *animated identifies it for the packet item. On the Vita it is
// write-combined CDRAM: build the section in a local struct and copy it with
// one memcpy; never read it back. Null when the ring is not created yet
// (before the first EndNativeMaterialGpuPacket) or the segment is full: keep
// the values another way for that draw.
mh3_shade_gpu_animated* WriteNativeMaterialGpuAnimated(NativeMaterialGpuAnimatedRef* animated);

// Render thread: after the draws of packet `sequence` were submitted (also
// creates the ring and retires released persistent sections). Mandatory for
// every rendered packet: Begin stops waiting for an unended segment after 1 s
// and that packet writes no sections.
void EndNativeMaterialGpuPacket(uint64_t sequence);

// ---- Drawing with references ----
// Render thread: the next SubmitNativeModelMesh uses these blocks instead of
// the classification and Animated section derived from the Render owner's
// material state (channels, texgen inputs, textures and raster still come
// from it). srt_roles: roles whose post texture matrix is the section's pure,
// non-identity SRT (built by the vertex program); other roles use the post
// matrices of the Render owner.
void SetNativeMaterialGpuDraw(NativeMaterialGpuRef material, NativeMaterialGpuAnimatedRef animated, uint8_t srt_roles);

// ---- Render-side inputs ----
// Render/Shadow owner: the SRT parameters behind the post texture matrices a
// texture SRT step applied (`applied` bits of slots 0..7, after the
// SetNativeTextureMatrix calls). A slot whose matrix is exactly its XSI SRT
// (no mapping, effect skipped) is built by the mesh vertex program from them;
// any other post matrix publication forgets the slot's parameters.
void PublishNativeTextureSrt(const mh3_native_texture_srt& srt, uint32_t applied, NativeOwner owner);

// Render thread counters (development statistics).
struct NativeMaterialGpuStatistics {
    uint64_t baked = 0, bake_unsupported = 0, bake_space = 0;
    uint64_t animated_reused = 0, animated_equal = 0, animated_written = 0;
    uint64_t resolved_reused = 0, resolved_written = 0;
    uint64_t ring_waits = 0, ring_full = 0, ring_timeouts = 0;
    size_t persistent_bytes = 0;
};

NativeMaterialGpuStatistics ReadNativeMaterialGpuStatistics();
