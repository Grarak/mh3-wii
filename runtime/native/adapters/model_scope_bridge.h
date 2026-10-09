#pragma once
#include "runtime/native/mh3_model_scope.h"
#include <memory>
/* Explicit G3D texture producer state. Backend sampler bindings may be changed
 * by UI/effects; they are not a substitute for this coordinate-size owner. */
void PublishNativeModelMapSize(uint32_t map, uint16_t width, uint16_t height);
const mh3_model_texcoord_sizes& ReadNativeModelCoordinateSizes();
void PrepareNativeModelCoordinateSizes(uint32_t count);
void LoadNativeModelPipeline(uint32_t resource);
void PrepareNativeModelMaterial(uint32_t material_reference, uint32_t control, uint32_t swaps, uint32_t indirect_operator, bool ignore);
void SubmitNativeModelShape(uint32_t shape_reference, uint32_t position, uint32_t normal);

// SubmitNativeModelShape prepares on the game thread and records the draw in
// the frame packet; the render thread executes it from that packet's slot.
// A model draw (between Begin and End) asks once for the version of its pose
// arrays (model_pose_bridge.h); a shape drawn outside one asks each time.
void BeginNativeModelDraw();
void EndNativeModelDraw();
void ExecuteNativeModelDraw(uint32_t slot, uint32_t item);

// RMHP08 display-list dispatch 80076A58: material/shape resource references,
// direct position/normal matrices, dispatch flags, replacement bindings and the
// indirect operator from 80077814.
struct NativeModelDispatch {
    uint32_t material, shape, position, normal, flags, bindings, descriptor;
};

uint32_t NativeModelIndirectMatrices(uint32_t material_reference, uint32_t node_reference, uint32_t shape_reference);

// The same with the referenced material and shape already read (both nonzero
// or as the references hold them).
uint32_t NativeModelIndirectMatricesAt(uint32_t material, uint32_t shape, uint32_t material_reference, uint32_t node_reference, uint32_t shape_reference);
void DispatchNativeModel(const NativeModelDispatch& dispatch);
void TraceNativeModelDispatch(const NativeModelDispatch& dispatch);

// Draw tables (model_catalog_bridge.h): a model draw resolves its model's
// table once, then dispatches each item by its draw-stream material and shape
// ID instead of resolving its material scope and shape by address.
// MH3_DRAW_TABLE=0 turns them off; =2 (development) also resolves every
// indexed item by address and reports any difference.
struct NativeModelDrawTable;

// The table of the model whose MDL0 is at `model` (as drawn), built now if it
// has none; `hold` keeps it alive. Null when tables are off or the model has
// no certified owner (its items then dispatch by address).
NativeModelDrawTable* NativeModelDrawTableFor(uint32_t model, std::shared_ptr<NativeModelDrawTable>& hold);

// DispatchNativeModel for an item of the table's model. `replacements` are the
// item's replacement bindings (also at dispatch.bindings), or null. False
// when the table has no record for the item: dispatch it by address instead.
bool DispatchNativeModelIndexed(const NativeModelDrawTable& table, uint32_t material_id, uint32_t shape_id, const mh3_model_material_bindings* replacements,
                                const NativeModelDispatch& dispatch);
// The table's draw-stream entries for mh3_model_draw_walk_entries.
const mh3_model_draw_entries* NativeModelDrawEntries(const NativeModelDrawTable& table);

// MH3_DRAW_TABLE=2: check indexed draws against the address path.
bool NativeModelDrawTableCompare();
void ReportNativeModelDrawTableMismatch(const char* what);

// Builds the tables of every certified model that has none (loading screens).
void PrepareNativeModelDrawTables();

// Development statistics (MH3_PROFILE=prepare, MH3_VERIFY=draw-table).
void ReportNativeModelDrawTables();
