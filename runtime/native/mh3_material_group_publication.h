#ifndef MH3_MATERIAL_GROUP_PUBLICATION_H
#define MH3_MATERIAL_GROUP_PUBLICATION_H
#include "mh3_material_state.h"
#include "mh3_model_submission.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Internal, admitted typed producers only. Equivalent field effects to
 * fields_select(source,block,count,patch), then overlay(destination,patch,0).
 * No aggregate patch, candidate, allocation, or retained pointer. Destination
 * and source must be disjoint or exactly equal. Existing checked selectors and
 * overlays remain the arbitrary-input boundary. NDEBUG removes contracts.
 *
 * Returns whether authored values changed in the previous successful shader's
 * consumed structural fields. Color/konst and indirect matrices are uniforms;
 * they never set this bit. Passing NULL consumed skips comparisons. Known-mask
 * additions still publish, including equal values; the result is not a source
 * revision, a publication mask, or a test of whether any material byte changed.
 * The borrowed requirements pointer is read during this call only.
 *
 * Count is ignored for pixel/colors/pipeline. Texgen's nonzero count must equal
 * source->texgen_count; pipeline requires source->tev_count != 0. Indirect or
 * texgen count0 returns0 without reading source (which may then be NULL).
 * All top fields, classification, channels, clip, unknown bytes and padding
 * inherit. A caller must finish fallible pixel/raster/capability preflight
 * before invoking this mutating function. */
int mh3_material_group_publish_unchecked(mh3_native_material_state *destination,
    const mh3_native_material_state *source,mh3_model_material_block block,
    uint32_t count,const mh3_material_requirements *consumed);

/* Only the first19 pixel fields belong to the resource pixel group. Merge them
 * into a small preview for the existing raster/capability preflight; current's
 * classification, fields19+, unknown bytes and padding survive. Exact alias of
 * output with either input is supported; otherwise records must be disjoint.
 * No aggregate material is constructed. Mutation of the material/raster is
 * still the caller's later commit. */
void mh3_material_group_pixel_preview_unchecked(const mh3_pack_pixel_inputs *current,
    const mh3_pack_pixel_inputs *source,mh3_pack_pixel_inputs *out);

#ifdef __cplusplus
}
#endif
#endif
