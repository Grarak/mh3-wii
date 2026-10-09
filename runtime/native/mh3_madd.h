#ifndef MH3_MADD_H
#define MH3_MADD_H
#include <math.h>

/* a * b + c for native matrix code. The Cortex-A9 (VFPv3/NEONv1) has no fused
 * multiply-add, so fmaf() there is glibc's software routine, about 2% of the Pi
 * game thread in Alatreon. On such a host this is a plain multiply and add
 * (vmla.f32, or vmul + vadd where the A9 scheduler splits it), rounding twice.
 * Hosts with a fused instruction keep the single rounding, so arm64/x86 fixture
 * results are unchanged. Mirrors MH3_FUSED_MADD in isa/ppc_isa_float.h. */
static inline float mh3_madd(float a, float b, float c) {
#if defined(__arm__) && !defined(__ARM_FEATURE_FMA) && !defined(MH3_EXACT_FMA)
    return a * b + c;
#else
    return fmaf(a, b, c);
#endif
}

#endif
