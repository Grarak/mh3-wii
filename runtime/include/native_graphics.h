#pragma once
#include <cstdint>
#include "../native/mh3_draw_state.h"
#include "native_owner.h"

struct mh3_native_texture_binding;
struct mh3_native_palette_source;
struct mh3_native_fog;
struct mh3_native_light_slots;
struct mh3_native_material_state;
struct mh3_model_matrix_selectors;
struct mh3_resolved_mesh_submit;

// Game unless named: the render thread passes NativeOwner::Render.
const mh3_draw_state& GetNativeDrawState(NativeOwner owner = NativeOwner::Game);

// Render thread: the restored viewport in target pixels.
void GetNativeRenderViewportSize(float out[2]);
void SetNativeProjection(const float row_major[16], bool perspective);
void SetNativePositionMatrix(const float row_major[12], NativeOwner owner = NativeOwner::Game);
void SetNativeNormalMatrix(uint32_t selector, const float row_major[9], NativeOwner owner = NativeOwner::Game);
bool GetNativeNormalMatrix(uint32_t selector, float row_major[9]);
void SetNativeViewport(float x, float y, float width, float height, float near_z, float far_z);
void SetNativeScissor(int32_t x, int32_t y, int32_t width, int32_t height);

// Returns the published logical rectangle, including skipped-frame changes.
// An unpublished rectangle returns false and leaves out untouched.
bool GetNativeScissor(int32_t out[4], NativeOwner owner = NativeOwner::Game);
void SetNativeRaster(const mh3_raster_state& raster);

// Render/Shadow: logical raster publication of deferred model preparation.
void SetNativeRaster(const mh3_raster_state& raster, NativeOwner owner);

// Render thread: apply the Render owner's raster to the backend before a draw.
void ApplyNativeRenderRaster();
using NativeRasterObserver = void (*)(const mh3_raster_state&);
void SetNativeRasterObserver(NativeRasterObserver observer);

// Clear only far depth on the current admitted target. Covers the whole target
// while preserving logical state and backend raster/scissor/draw bindings.
// Skipped frames perform no GPU work, allocation, scene switch or wait.
void ClearNativeDepthTarget();

// Game: records a binding; the render thread binds the backend on restore.
void BindNativeTexture(const mh3_native_texture_binding& binding, NativeOwner owner = NativeOwner::Game);
const mh3_native_texture_binding* GetNativeTextureBinding(uint32_t unit, NativeOwner owner = NativeOwner::Game);
const mh3_native_palette_source* GetNativePaletteBinding(uint32_t slot);
void BindNativePalette(uint32_t slot, const mh3_native_palette_source& palette, NativeOwner owner = NativeOwner::Game);
void SetNativeFog(const mh3_native_fog& fog);

// Publishes a selected light set. owner/revision identify the producer's slots
// (model_scene_state.cpp): while both are unchanged since the previous
// publication, only the masks and the ambient colour can differ.
void SetNativeModelLightSelection(const mh3_native_light_slots& lights, const void* owner, uint64_t revision);
void SetNativeTextureMatrix(uint32_t selector, const float row_major[12], bool matrix3x4, NativeOwner owner = NativeOwner::Game);
bool GetNativeTextureMatrix(uint32_t selector, float row_major[12], bool* matrix3x4 = nullptr, NativeOwner owner = NativeOwner::Game);
void SetNativeDepthCompareBeforeTexture(bool before, NativeOwner owner = NativeOwner::Game);
void SetNativeMaterial(const mh3_native_material_state& material);

// Internal sparse publication from typed renderer producers; only declared
// fields replace sticky inherited state. DEBUG asserts producer domains and
// dependencies; external assets retain checked admission. Raster capability
// failures leave the material unchanged. Runs on skipped logical frames too.
void OverlayNativeMaterial(const mh3_native_material_state& patch, uint32_t top_known = 0);
const mh3_native_material_state& GetNativeMaterialState(NativeOwner owner = NativeOwner::Game);

// Per-draw colour values of the Game material without a whole-material
// publication (effect particles; the frame packet captures them as their own
// small component, NATIVE_STATE_COLORS): TEV colour register index (0..3) or
// konst colour index, set known with these components. Equal to publishing a
// copy of the material with only that register changed.
void SetNativeMaterialColor(bool konst, uint32_t index, const uint8_t rgba[4]);

// The two alpha-compare references, when the material's comparisons and
// operation are known and equal to these (otherwise false: nothing changed,
// publish the whole material instead).
bool SetNativeMaterialAlphaReferences(uint32_t reference0, uint32_t reference1, uint32_t compare0, uint32_t compare1, uint32_t operation);

// The owner's selected model light slots (the last SetNativeModelLights or restore).
const mh3_native_light_slots& GetNativeModelLights(NativeOwner owner = NativeOwner::Game);

// Explicit current-matrix producer: updates row/dual fields directly and marks
// consumed shader fields dirty only when those fields actually change.
void SetNativeModelMatrixSelectors(const mh3_model_matrix_selectors& selectors, NativeOwner owner = NativeOwner::Game);
inline void ApplyNativeMaterial(const mh3_native_material_state& material) {
    SetNativeMaterial(material);
}

// Game: records the geometry into the frame packet.
void SubmitNativeMaterialGeometry(const AuroraNativeMaterialGeometry& geometry);

// Render thread: draws recorded geometry with the restored Render state.
void DrawNativeMaterialGeometry(const AuroraNativeMaterialGeometry& geometry);

// Render thread: compact effect quads of one record (frame packet Kind::Quads).
void DrawNativeQuads(const AuroraNativeQuadVertex* vertices, uint32_t count, const float normal[3], const uint8_t color[4]);

// Render thread: direct compact resident mesh boundary. The caller supplies
// the Render owner's material; immutable vertex/index payloads remain in their
// admitted GPU allocation.
void SubmitNativeModelMesh(const mh3_resolved_mesh_submit& mesh);

// Called before destroying the graphics context; preserves no borrowed GPU data.
void ClearNativeMaterialPrograms();

// Create the renderer on the game thread. Native targets use only the selected
// visible dimensions, with no guest FIFO or XFB allocation/initialization.
void InitializeNativeGraphics(uint32_t width, uint32_t height, bool load_native_assets = true);

// Game thread: the game's 16:9 mode is anamorphic, a 640x448 EFB the display
// stretches. Every host renders for the Vita's 960x544 display (other hosts
// scale it into their window): widescreen fills it with the target, scaled
// per axis; 4:3 keeps the target's own aspect (777x544, pillarboxed).
// Applies at the next rendered frame.
void SetNativeDisplayWidescreen(bool widescreen);

// The aspect of the picture the widescreen target shows on this host.
float NativeWidescreenAspect();

// Opens the native texture and model assets of MH3_DISC_DIR: prepared packs,
// or the load-time asset cache. Done by InitializeNativeGraphics unless told not to.
void OpenNativeAssets();

// The game thread records each frame into a packet; the render thread draws
// and presents it (native_frame_packet.h). Begin admits recording unless the
// window cannot present; it never waits on the GPU. Always end the logical
// frame, even when drawing is skipped. On the render thread, ShouldRender and
// Cancel refer to the frame being drawn.
bool BeginNativeGraphicsFrame();
bool NativeGraphicsFrameShouldRender();
void CancelNativeGraphicsFrame();
void EndNativeGraphicsFrame();

// The overlapped draw (the default, docs/pipelined-draw.md):
// frame begin only latches admission; the game thread begins the packet at
// the handoff (BeginOverlappedNativeGraphicsFrame) and gives it to the draw
// thread (NativeFrameSubmittedToDraw), which ends the frame. At the frame end
// EndOverlappedNativeGraphicsFrame says whether the draw thread has it (true:
// skip EndNativeGraphicsFrame); for a frame that drew nothing it begins the
// empty packet the end then publishes.
void SetNativeFrameOverlap(bool enabled);
void BeginOverlappedNativeGraphicsFrame();

// The game thread's overlapped logic step, before its packet begins: the
// draw thread may still be recording the previous packet, so a record made
// now must wait for the next one (QueueNativeSceneClone records a scene clone
// as it begins).
bool NativeGraphicsLogicPhase();
void QueueNativeSceneClone(const void* destination, const void* source);
void NativeFrameSubmittedToDraw();
bool EndOverlappedNativeGraphicsFrame();

// The game thread or the render thread: each owns its logical draw state.
bool OnNativeGraphicsThread();

#ifdef MH3_DEVELOPMENT
// The scene's temporal feedback (0x80055FBC, native_scene_feedback.cpp), since
// the last call: enabled passes, those that drew the previous frame's image
// back over the scene, and the last pass's mode, alpha and amount.
struct TemporalFeedbackStatistics {
    uint64_t calls = 0, previous = 0;
    uint8_t mode = 0, alpha = 0, amount = 0;
};

TemporalFeedbackStatistics TakeTemporalFeedbackStatistics();
#endif
