/* Force-included into the Vita C sources. newlib's math.h maps isfinite, isnan
 * and isinf to compiler builtins only for GCC 4.4 and later; clang reports GCC
 * 4.2, so each check called __fpclassifyf (1.6% of the draw thread in the
 * Jhen fight). Its fallback definitions are guarded by #ifndef. */
#pragma once

#define isfinite(x) __builtin_isfinite(x)
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf_sign(x)
