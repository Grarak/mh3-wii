#pragma once
#include "runtime/native/mh3_model_scope.h"
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
// Each model draw call starts its own pose copies.
void BeginNativeModelDraw();
void ExecuteNativeModelDraw(uint32_t slot, uint32_t item);
void TraceNativeModelDispatch(struct CpuContext* cpu);
