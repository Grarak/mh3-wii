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
// Deferred model preparation (model_render_prep.h): copy one owner's logical
// material/post, texture and raster state into another (MH3_RENDER_PREP=2
// starts its Shadow from the Game state) and compare the results.
void CopyNativeMaterialOwner(NativeOwner from, NativeOwner to);
void CopyNativeTextureBindings(NativeOwner from, NativeOwner to);
void CopyNativeDrawRaster(NativeOwner from, NativeOwner to);
bool EqualNativeTextureBindings(NativeOwner a, NativeOwner b);
#ifdef MH3_DEVELOPMENT
#include <string>
// Empty when equal; otherwise the differing parts.
std::string CompareNativeMaterialOwners(NativeOwner a, NativeOwner b);
#endif
