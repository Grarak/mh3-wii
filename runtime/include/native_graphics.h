#pragma once
#include <cstdint>
#include "../native/mh3_draw_state.h"

struct mh3_native_texture_binding;
struct mh3_native_palette_source;
struct mh3_native_fog;
struct mh3_native_light_slots;
struct mh3_native_material_state;

const mh3_draw_state& GetNativeDrawState();
void SetNativeProjection(const float row_major[16], bool perspective);
void SetNativePositionMatrix(const float row_major[12]);
void SetNativeViewport(float x,float y,float width,float height,float near_z,float far_z);
void SetNativeScissor(int32_t x,int32_t y,int32_t width,int32_t height);
void SetNativeRaster(const mh3_raster_state& raster);
void BindNativeTexture(const mh3_native_texture_binding& binding);
void BindNativePalette(uint32_t slot,const mh3_native_palette_source& palette);
void SetNativeFog(const mh3_native_fog& fog);
void SetNativeModelLights(const mh3_native_light_slots& lights);
void SetNativeTextureMatrix(uint32_t selector,const float row_major[12],bool matrix3x4);
void SetNativeDepthCompareBeforeTexture(bool before);
void SetNativeMaterial(const mh3_native_material_state& material);
const mh3_native_material_state& GetNativeMaterialState();
inline void ApplyNativeMaterial(const mh3_native_material_state& material) { SetNativeMaterial(material); }
void SubmitNativeMaterialGeometry(const AuroraNativeMaterialGeometry& geometry);

// Create the renderer on the game thread. Native targets use only the selected
// visible dimensions, with no guest FIFO or XFB allocation/initialization.
void InitializeNativeGraphics(uint32_t width, uint32_t height);

// The game thread owns both frame admission and submission. Begin polls GPU
// completion once, before draw preparation or CDRAM writes. Always end the
// logical frame, even when drawing is skipped; neither boundary waits.
bool BeginNativeGraphicsFrame();
bool NativeGraphicsFrameShouldRender();
void EndNativeGraphicsFrame();
