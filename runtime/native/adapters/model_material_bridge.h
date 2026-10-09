#pragma once
#include "runtime/native/mh3_model_submission.h"
/* Publish an admitted, borrowed typed resource group directly. The source is
 * consumed immediately; no aggregate patch or pointer is retained. Zero
 * indirect/texgen count does not read source. Pixel capability failures leave
 * both material and raster unchanged. */
/* OverlayNativeMaterial for model producers that declare only top-level
 * counts/cull (genmode), the two channels, or the indirect matrices. */
void OverlayNativeMaterialTop(uint32_t top_known, uint32_t texgens, uint32_t channels, uint32_t tevs, uint32_t indirect, uint32_t cull);
void OverlayNativeMaterialChannels(const mh3_pack_channel channels[2]);
void OverlayNativeMaterialIndirectMatrices(const mh3_pack_indirect_matrix matrices[3]);
void OverlayNativeModelMaterialGroup(const mh3_native_material_state& source, mh3_model_material_block kind, uint32_t count = 1);
