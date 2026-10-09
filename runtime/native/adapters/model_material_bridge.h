#pragma once
#include "runtime/native/mh3_model_submission.h"
/* Publish an admitted, borrowed typed resource group directly. The source is
 * consumed immediately; no aggregate patch or pointer is retained. Zero
 * indirect/texgen count does not read source. Pixel capability failures leave
 * both material and raster unchanged. */
void OverlayNativeModelMaterialGroup(const mh3_native_material_state& source,
    mh3_model_material_block kind,uint32_t count=1);
