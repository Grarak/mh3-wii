#ifndef MH3_SYSTEM_FATAL_H
#define MH3_SYSTEM_FATAL_H
#include "mh3_model_resources.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Portable OSFatal diagnostic. The message must contain a NUL within its
 * checked span; both colors contain four RGBA bytes. Required includes NUL.
 * A sizing call uses an empty output and returns RANGE with required set.
 * Invalid/short inputs never change output. Overlapping output is supported. */
mh3_res_status mh3_system_fatal_format(mh3_res_span foreground, mh3_res_span background, mh3_res_span message, mh3_res_mut_span output, size_t *required);

#ifdef __cplusplus
}
#endif
#endif
