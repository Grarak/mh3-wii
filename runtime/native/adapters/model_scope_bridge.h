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
// Between Begin and End, SubmitNativeModelShape only prepares: each draw keeps
// its shading snapshot and pose, and End submits them in order.
void BeginNativeModelCapture();
void EndNativeModelCapture();
void TraceNativeModelDispatch(struct CpuContext* cpu);
