// Deferred model preparation: the G3D material, primitive and matrix-row
// setup of one model dispatch (80076220, 800879DC, 80087B34), executed by the
// render thread against its own state instead of by the game thread.
//
// The game thread captures only the dispatch's inputs: the material's resource
// records (genmode, texture/palette objects, channels, texture SRT), frozen
// immutable versions of its typed material blocks, the selected lights' masks,
// the indirect operator and the shape's matrix inputs. Fog and light-set
// selection stay on the game thread (they publish the lighting snapshot).
// The render thread replays the original step order against a host mirror of
// the G3DState CPU caches and the Render owner's material, texture and post
// matrix state. Nothing on the render thread reads guest memory.
#pragma once
#include "native_owner.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include "runtime/native/mh3_model_pose.h"
#include "runtime/native/mh3_model_scope.h"
#include "runtime/native/mh3_texture_matrices.h"
#include <cstdint>
#include <memory>

// MH3_RENDER_PREP: 0 prepares on the game thread (the original path),
// 1 (default) defers preparation to the render thread, 2 (development) runs
// the original path and checks a Shadow execution of the captured inputs
// against it for every dispatch.
enum class ModelPrepMode : uint8_t { GameThread, Deferred, Verify };
ModelPrepMode NativeModelPrepMode();

struct ModelPrepInputs {
    uint32_t flags = 0;      // dispatch flags (2: no primitive setup, 4: front cull, 8: lights off)
    uint32_t invalidate = 0; // G3DState::Invalidate fields since the previous capture
    bool material = false;   // the material steps run (scope present and not ignored)
    bool visible = false;    // the shape draws; otherwise only the material applies
    bool selectors = false;  // game-side selector state changed: replace the mirror's
    uint16_t present = 0;    // MH3_MAT_* resource slots with a record
    mh3_model_matrix_selectors selector_state{};
    // Indirect operator from 80077814 (dispatch descriptor), when nonzero.
    bool indirect_operator = false;
    mh3_indirect_output op{};
    uint8_t depth_before_texture = 0;
    uint8_t genmode[8]{};
    uint8_t palette_objects[100]{}, texture_objects[260]{};
    mh3_native_palette_source palettes[8]{};
    mh3_native_texture_binding textures[8]{};
    uint32_t pipeline_address = 0;
    // Frozen copies stay alive while the game thread holds them and are
    // deleted only after the packets recorded meanwhile have rendered.
    const NativeFrozenMaterialBlock *pipeline = nullptr, *pixel = nullptr, *colors = nullptr, *indirect = nullptr, *texgen = nullptr;
    uint8_t channels[40]{};
    uint32_t light_masks[4]{};
    uint8_t ambient[4]{};
    mh3_native_texture_srt srt{};
    mh3_matrix34 maps[8]{};
    uint32_t map_sources[8]{};
    uint8_t shape_cache[12]{};
    // Shape matrix inputs (80087B34).
    int32_t matrix_id = 0;
    bool has_normal = false, has_binding = false;
};

// Host mirror of the G3DState CPU caches a dispatch reads and writes, with
// the guest layout so the portable C cache producers run unchanged.
struct ModelPrepMirror {
    static constexpr uint32_t BaseA = 0x80685300, SizeA = 0x240; // genmode .. view sources
    static constexpr uint32_t BaseB = 0x80796db0, SizeB = 0x20;  // sync .. depth compare
    uint8_t a[SizeA]{}, b[SizeB]{};
    uint8_t unused[48]{}; // fog/light/view caches stay on the game thread
    uint32_t pipeline_address = 0;
    uint64_t pipeline_revision = 0;
    uint8_t coordinate_map_known = 0;
    mh3_model_texcoord_sizes map_sizes{}, coordinate_sizes{};
    mh3_model_matrix_selectors selectors{};
    mh3_res_mut_span At(uint32_t address, size_t size);
    void Invalidate(uint32_t fields);
};
// The render thread's mirror.
ModelPrepMirror& RenderModelPrepMirror();
// Render thread: a snapshot restore replaced draw, material, texture or post
// state behind the mirror's back; forget every cache (G3DState::Invalidate).
void ForgetNativeModelPrepCaches();

// Game thread capture helpers (model_render_prep.cpp).
// Reads the material's resource records and freezes its typed blocks.
void CaptureModelPrepMaterial(ModelPrepInputs& in, const mh3_model_material_scope& scope);
// Pending G3DState::Invalidate fields since the previous capture.
uint32_t TakeNativeModelPrepInvalidate();
void NoteNativeModelPrepInvalidate(uint32_t fields);

// Executes a captured dispatch against owner (Render on the render thread,
// Shadow on the game thread) and its mirror. Produces the shape matrix input
// and coordinate sizes of the draw when in.visible.
struct ModelPrepShapeResult {
    mh3_shape_matrix_input input{};
    mh3_model_texcoord_sizes coordinate_sizes{};
};
void ExecuteModelPrep(NativeOwner owner, ModelPrepMirror& mirror, const ModelPrepInputs& in, const mh3_matrix34* direct_position, const mh3_matrix34* direct_normal,
                      ModelPrepShapeResult* shape);
