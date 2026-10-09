// Immutable copies of the native state a draw consumes, split by component so
// a frame packet stores each one only when it changed. The game thread records
// draws against its logical state; the render thread restores each changed
// component into its own (thread-local) logical state before drawing.
#pragma once
#include "../native/mh3_draw_state.h"
#include "../native/mh3_material_state.h"
#include "../native/mh3_model_state.h"
#include <array>
#include <cstdint>

// Per-thread change counters. A setter bumps its component whenever it may
// have changed; captures compare contents before storing a new copy.
enum NativeStateComponent : unsigned {
    NATIVE_STATE_DRAW,
    NATIVE_STATE_MATERIAL,
    NATIVE_STATE_LIGHTING,
    NATIVE_STATE_POST,
    NATIVE_STATE_TEXTURES,
    NATIVE_STATE_COMPONENTS
};
extern thread_local constinit uint32_t nativeStateRevision[NATIVE_STATE_COMPONENTS];

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
// Render thread: restore into the logical state and apply it to the backend.
void RestoreNativeDrawSnapshot(const NativeDrawSnapshot& snapshot);
void RestoreNativeMaterialSnapshot(const NativeMaterialSnapshot& snapshot);
void RestoreNativeLightingSnapshot(const NativeLightingSnapshot& snapshot);
void RestoreNativePostSnapshot(const NativePostSnapshot& snapshot);
void RestoreNativeTextureSnapshot(const NativeTextureSnapshot& snapshot);
// Drop the render thread's backend texture bindings so the next texture
// restore rebinds every unit (after the backend changed its own bindings).
void ForgetNativeTextureBindings();
