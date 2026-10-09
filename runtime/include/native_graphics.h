#pragma once
#include <cstdint>
#include "../native/mh3_draw_state.h"

struct mh3_native_texture_binding;
struct mh3_native_palette_source;
struct mh3_native_fog;
struct mh3_native_light_slots;
struct mh3_native_material_state;
struct mh3_resolved_mesh_submit;

const mh3_draw_state& GetNativeDrawState();
void GetNativeRenderViewportSize(float out[2]);
void SetNativeProjection(const float row_major[16], bool perspective);
void SetNativePositionMatrix(const float row_major[12]);
void SetNativeNormalMatrix(uint32_t selector,const float row_major[9]);
bool GetNativeNormalMatrix(uint32_t selector,float row_major[9]);
void SetNativeViewport(float x,float y,float width,float height,float near_z,float far_z);
void SetNativeScissor(int32_t x,int32_t y,int32_t width,int32_t height);
// Returns the published logical rectangle, including skipped-frame changes.
// An unpublished rectangle returns false and leaves out untouched.
bool GetNativeScissor(int32_t out[4]);
void SetNativeRaster(const mh3_raster_state& raster);
using NativeRasterObserver=void(*)(const mh3_raster_state&);
void SetNativeRasterObserver(NativeRasterObserver observer);
void RestoreNativeRenderState();
// Clear only far depth on the current admitted target. Covers the whole target
// while preserving logical state and backend raster/scissor/draw bindings.
// Skipped frames perform no GPU work, allocation, scene switch or wait.
void ClearNativeDepthTarget();
void BindNativeTexture(const mh3_native_texture_binding& binding);
const mh3_native_texture_binding* GetNativeTextureBinding(uint32_t unit);
const mh3_native_palette_source* GetNativePaletteBinding(uint32_t slot);
void BindNativePalette(uint32_t slot,const mh3_native_palette_source& palette);
void SetNativeFog(const mh3_native_fog& fog);
void SetNativeModelLights(const mh3_native_light_slots& lights);
void SetNativeTextureMatrix(uint32_t selector,const float row_major[12],bool matrix3x4);
bool GetNativeTextureMatrix(uint32_t selector,float row_major[12],bool* matrix3x4=nullptr);
void SetNativeDepthCompareBeforeTexture(bool before);
void SetNativeMaterial(const mh3_native_material_state& material);
// Atomic sparse field publication; inherited bytes and unknown-field rejection
// are identical to SetNativeMaterial, with only the selected top fields copied.
// This CPU publication runs on skipped logical frames too.
void OverlayNativeMaterial(const mh3_native_material_state& patch,uint32_t top_known=0);
const mh3_native_material_state& GetNativeMaterialState();
// Owner-only cumulative preparation counters, followed by current occupancy
// and fixed cache bytes. Read only at development reporting boundaries.
// 0..8: needs, dirty lookups, base hits, full preparations, base evictions,
//       model hits, model misses, configuration hashes, suffix hashes.
// 9..11: live base entries, live model variants, fixed cache allocation bytes.
void GetNativeMaterialPreparationStatistics(uint64_t statistics[12]);
// Owner-only cumulative program memo hits (base/model), map lookups
// (base/model), full config equality checks and memo-dropping suffix changes.
void GetNativeMaterialProgramStatistics(uint64_t statistics[6]);
inline void ApplyNativeMaterial(const mh3_native_material_state& material) { SetNativeMaterial(material); }
void SubmitNativeMaterialGeometry(const AuroraNativeMaterialGeometry& geometry);
// Direct compact resident mesh boundary. Caller supplies typed live uniforms;
// immutable vertex/index payloads remain in their admitted GPU allocation.
void SubmitNativeModelMesh(const mh3_resolved_mesh_submit& mesh);
// Called before destroying the graphics context; preserves no borrowed GPU data.
void ClearNativeMaterialPrograms();

// Create the renderer on the game thread. Native targets use only the selected
// visible dimensions, with no guest FIFO or XFB allocation/initialization.
void InitializeNativeGraphics(uint32_t width, uint32_t height,bool load_native_assets=true);

// The game thread owns both frame admission and submission. Begin polls GPU
// completion once, before draw preparation or CDRAM writes. Always end the
// logical frame, even when drawing is skipped; neither boundary waits.
bool BeginNativeGraphicsFrame();
bool NativeGraphicsFrameShouldRender();
// Cancel presentation after temporary residency pressure. Recorded scenes are
// sealed without a GPU wait; their frame slot remains leased until completion.
void CancelNativeGraphicsFrame();
void EndNativeGraphicsFrame();
