// Immutable copy of the native shading state a model mesh draw consumes:
// typed material, raster, bound textures/palettes, lights, fog, post texture
// matrices and the early-depth flag. The model draw prepare step captures it
// after material preparation; the execute step restores it before drawing,
// so preparation and drawing no longer need to run back to back.
#pragma once
#include "../native/mh3_draw_state.h"
#include "../native/mh3_material_state.h"
#include "../native/mh3_model_state.h"
#include <array>
#include <cstdint>

struct NativeShadeSnapshot {
    mh3_native_material_state material;
    mh3_raster_state raster;
    std::array<mh3_native_texture_binding, 8> textures;
    std::array<mh3_native_palette_source, 20> palettes;
    mh3_native_light_slots lights;
    std::array<mh3_shader_light, 8> preparedLights;
    mh3_native_fog fog;
    mh3_shader_fog preparedFog;
    std::array<std::array<float, 12>, 8> post;
    uint8_t textureValid, postKnown, post3x4;
    bool fogKnown, depthBeforeTexture;
};
void CaptureNativeShadeSnapshot(NativeShadeSnapshot& out);
void RestoreNativeShadeSnapshot(const NativeShadeSnapshot& snapshot);
// native_texture_state.cpp: bound texture/palette records.
void CaptureNativeTextureBindings(NativeShadeSnapshot& out);
void RestoreNativeTextureBindings(const NativeShadeSnapshot& snapshot);
