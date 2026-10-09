// Host forms of the SDK's paired-single vector math for native ports, with the
// translation's float helpers so they round as the translated code does.
#pragma once

#include "memory.h"
#include "ppc_runtime.h"

#include <cstdint>

namespace PsVec {

// PSVECMag (0x804C7230) of (x, y, z): sqrt(x*x + y*y + z*z) as the
// paired-single code computes it (z*z + x*x, then + y*y), the frsqrte
// estimate refined by one Newton step; 0 for a zero vector.
inline float Mag(float x, float y, float z) {
    const double xy = PPC_PsMulInline(PpcPackPairedInline(x, y), PpcPackPairedInline(x, y));
    const double zz = PPC_PsFromScalarInline(z);
    const double sq = PPC_PsToScalarInline(PPC_PsSum0Inline(PPC_PsMaddInline(zz, zz, xy), xy, xy));

    if (sq == 0.0)
        return float(sq);

    const double e = PPC_Frsqrte(sq);
    const double refine = PPC_Fnmsubs(PpcFmulsInline(e, e), sq, 3.0); // 3 - e*e*sq
    return float(PpcFmulsInline(sq, PpcFmulsInline(refine, PpcFmulsInline(e, 0.5))));
}

// The distance |b - a| of two guest VEC3s (0x80050F58: VectorSubtractWrapper,
// then VecMagWrapper).
inline float Distance(uint32_t a, uint32_t b) {
    return Mag(MemoryInline::FlatReadFloat32(b) - MemoryInline::FlatReadFloat32(a), MemoryInline::FlatReadFloat32(b + 4) - MemoryInline::FlatReadFloat32(a + 4),
               MemoryInline::FlatReadFloat32(b + 8) - MemoryInline::FlatReadFloat32(a + 8));
}

} // namespace PsVec
