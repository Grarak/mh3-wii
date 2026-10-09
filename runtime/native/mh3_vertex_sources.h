#ifndef MH3_VERTEX_SOURCES_H
#define MH3_VERTEX_SOURCES_H
#include "mh3_model_resources.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {MH3_VERTEX_POSITION,MH3_VERTEX_NORMAL,MH3_VERTEX_COLOR,MH3_VERTEX_UV,
              MH3_VERTEX_FUR_POSITION} mh3_vertex_kind;
typedef struct mh3_native_vertex_source {
    uint32_t resource_address,payload_address,source_id,resource_bytes,payload_bytes;
    uint32_t count,stride,components,format,fraction,component_mode;
    uint32_t sample_index,sample_count,sample_stride;
    mh3_vertex_kind kind;
    uint64_t source_generation,binding_generation;
} mh3_native_vertex_source;
/* Reads only the resource header. Exact source numeric/color format enums are
 * retained; they are NOT GXM format enums. Bounds checks cover the last element
 * at source stride. This does not decode, inspect or hash vertex contents. */
mh3_res_status mh3_vertex_source_read(mh3_res_span resource,uint32_t address,
                                     mh3_vertex_kind kind,mh3_native_vertex_source *out);
/* MH3 80089598: sample count/stride are u32 at +0x20/+0x24; an invalid sample
 * is ABSENT. XYZ/numeric metadata matches the ordinary position array header. */
mh3_res_status mh3_vertex_fur_source_read(mh3_res_span resource,uint32_t address,
                                         uint16_t sample,mh3_native_vertex_source *out);
#ifdef __cplusplus
}
#endif
#endif
