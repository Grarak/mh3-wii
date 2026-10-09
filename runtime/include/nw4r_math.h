// Host entry points of the NW4R math and angle ports (nw4r_math.cpp) for other
// native ports.
#pragma once

#include "ppc_runtime.h"

#include <cstdint>

namespace Nw4rMath {

float SinU16(uint32_t angle);                // 0x800502B0
float CosU16(uint32_t angle);                // 0x80050218
float FSqrt(double x);                       // 0x80050C24
float FrSqrt(double x);                      // 0x80501490
void RotMatrixX(uint32_t angle, uint32_t m); // 0x80050130
void RotMatrixY(uint32_t angle, uint32_t m); // 0x800502E0
void RotMatrixZ(uint32_t angle, uint32_t m); // 0x800503C8
void RotMatrixZXY(uint32_t angles, uint32_t m); // 0x800509F4 cpSetRotMatrixZXY
void RotMatrixXYZ(uint32_t angles, uint32_t m); // 0x80050914 cpSetRotMatrix
void MultVecSR(uint32_t out, uint32_t m, uint32_t v); // 0x80501C48 MTX34MultVecSR

// FastReciprocal (0x80061110): fres refined by one Newton step, in paired
// arithmetic.
inline float FastReciprocal(double x) {
    const double estimate = PPC_Fres(PPC_PsFromScalarInline(x));
    const double twice = PPC_PsAddInline(estimate, estimate);
    const double squared = PPC_PsMulInline(estimate, estimate);
    return float(PPC_PsToScalarInline(PPC_PsNmsubInline(PPC_PsFromScalarInline(x), squared, twice)));
}

} // namespace Nw4rMath
