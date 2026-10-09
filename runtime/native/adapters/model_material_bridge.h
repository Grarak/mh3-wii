#pragma once
#include "runtime/native/mh3_model_submission.h"
/* Select a typed group from certified immutable defaults/live producers.
 * Zero indirect/texgen count returns an empty patch without reading a block.
 * Pipeline address names ResTev's header; its certified payload starts+32. */
mh3_native_material_state ReadNativeModelMaterialGroup(uint32_t address,
    mh3_model_material_block kind,uint32_t count=1);
