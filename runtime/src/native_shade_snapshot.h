// Immutable copies of the native state a draw consumes, split by component so
// a frame packet stores each one only when it changed. The game thread records
// draws against the Game owner's logical state; the render thread restores
// each changed component into the Render owner's state before drawing.
#pragma once
#include "../native/mh3_draw_state.h"
#include "../native/mh3_material_state.h"
#include "../native/mh3_model_state.h"
#include "native_owner.h"
#include <array>
#include <cstdint>

// Change counters per owner. A setter bumps its component whenever it may
// have changed; captures compare contents before storing a new copy.
enum NativeStateComponent : unsigned {
    NATIVE_STATE_DRAW,
    NATIVE_STATE_MATERIAL,
    NATIVE_STATE_LIGHTING,
    NATIVE_STATE_POST,
    NATIVE_STATE_TEXTURES,
    // The material's colour values (NativeColorSnapshot): a material change
    // advances it too; SetNativeMaterialColor and SetNativeMaterialAlpha-
    // References advance only it.
    NATIVE_STATE_COLORS,
    NATIVE_STATE_COMPONENTS
};

extern constinit uint32_t nativeStateRevision[NativeOwnerCount][NATIVE_STATE_COMPONENTS];

// Advances when the handoff copies Game's material store over Draw's with its
// revisions (CopyNativeMaterialStoreForDraw): Draw's material revision can
// then repeat an earlier value for other contents.
extern constinit uint32_t nativeMaterialStoreCopies;

// native_render_state.cpp: projection, position, viewport, raster, scissor.
struct NativeDrawSnapshot {
    mh3_draw_state draw;
    std::array<int32_t, 4> scissor;
    bool viewportKnown, scissorKnown, rasterKnown;
};

// native_material.cpp
struct NativeMaterialSnapshot {
    mh3_native_material_state material;
    bool depthBeforeTexture;
};

// The values a draw changes without changing the material (effect particles):
// TEV colour registers, konst colours and the two alpha-compare references.
// Restored after the material snapshot, whose copies of them it replaces.
// No padding: snapshots are compared bytewise.
struct NativeColorSnapshot {
    std::array<mh3_pack_tev_color, 4> color, konst;
    std::array<uint32_t, 2> alphaReference;
    uint32_t alphaKnown; // bit 0: reference 0, bit 1: reference 1
};

static_assert(sizeof(NativeColorSnapshot) == 2 * 4 * sizeof(mh3_pack_tev_color) + 12);
struct NativeLightingSnapshot {
    mh3_native_light_slots lights;
    std::array<mh3_shader_light, 8> preparedLights;
    mh3_native_fog fog;
    mh3_shader_fog preparedFog;
    bool fogKnown;
};

struct NativePostSnapshot {
    std::array<std::array<float, 12>, 8> post;
    uint8_t known, matrix3x4;
};

// native_texture_state.cpp: bound texture/palette records.
struct NativeTextureSnapshot {
    std::array<mh3_native_texture_binding, 8> textures;
    std::array<mh3_native_palette_source, 20> palettes;
    uint8_t valid;
};

void CaptureNativeDrawSnapshot(NativeDrawSnapshot& out);
void CaptureNativeMaterialSnapshot(NativeMaterialSnapshot& out);
void CaptureNativeLightingSnapshot(NativeLightingSnapshot& out);
void CaptureNativePostSnapshot(NativePostSnapshot& out);
void CaptureNativeTextureSnapshot(NativeTextureSnapshot& out);

// True when the Game texture state is the snapshot's (no copy).
bool NativeTextureSnapshotCurrent(const NativeTextureSnapshot& snapshot);
void CaptureNativeColorSnapshot(NativeColorSnapshot& out);

// Render thread: restore into the logical state and apply it to the backend.
void RestoreNativeDrawSnapshot(const NativeDrawSnapshot& snapshot);
void RestoreNativeMaterialSnapshot(const NativeMaterialSnapshot& snapshot);
void RestoreNativeLightingSnapshot(const NativeLightingSnapshot& snapshot);
void RestoreNativePostSnapshot(const NativePostSnapshot& snapshot);
void RestoreNativeTextureSnapshot(const NativeTextureSnapshot& snapshot);

// After the material snapshot of the same draw, if any.
void RestoreNativeColorSnapshot(const NativeColorSnapshot& snapshot);

// The colour values into a material, as the restore does.
void ApplyNativeColorSnapshot(mh3_native_material_state& material, const NativeColorSnapshot& snapshot);

// Drop the render thread's backend texture bindings so the next texture
// restore rebinds every unit (after the backend changed its own bindings).
void ForgetNativeTextureBindings();

// A guest heap freed (or handed out again) the canonical range: the cached
// textures whose images were in it go once the packets recorded before it
// have drawn. Any thread; the game thread queues them as each packet begins
// (QueueNativeTextureRetirements).
void RetireNativeTextureSources(uint32_t address, uint32_t bytes);
void QueueNativeTextureRetirements();

// Deferred model preparation (model_render_prep.h): an owner with nothing known: no material fields, texture bindings or
// palettes, the default raster (a model dispatch's compile starts Shadow there).
void ResetNativeMaterialOwner(NativeOwner owner);
void ResetNativeTextureBindings(NativeOwner owner);
void ResetNativeDrawRaster(NativeOwner owner);

// Replaces the owner's material fields (its post state stays).
void SetNativeMaterialOwnerState(const mh3_native_material_state& state, NativeOwner owner);

// Render thread: the Render owner's material state is `draw` (a compiled
// model dispatch's draw material, model_render_prep.cpp) from now on: its
// publications write it and the draws read it; nothing is copied. A full
// material restore ends the redirect.
void RedirectNativeRenderMaterial(mh3_native_material_state* draw);

// Render thread: copies a redirected material into the owner's own and ends
// the redirect (before a partial write, or when `draw` goes; with `draw`
// only while redirected to it).
void OwnNativeRenderMaterial(const mh3_native_material_state* draw = nullptr);

// Render thread: whether the Render owner's material is `draw`.
bool NativeRenderMaterialRedirected(const mh3_native_material_state* draw);

// Republishes the owner's raster from its material's pixel fields and cull
// mode (as a skipped pixel or genmode publication would have).
void PublishNativeMaterialRaster(NativeOwner owner);
