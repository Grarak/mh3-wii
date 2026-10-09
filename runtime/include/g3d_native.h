// Host entry points of the native NW4R g3d and matrix ports, for other ports
// that call them without going through a CpuContext.
#pragma once

#include <cstdint>

namespace G3dNative {
// nw4r_accessors.cpp
uint32_t ResDicAtOffset14(uint32_t mdl, uint32_t index); // 0x80097F7C

// mtx.cpp
void MtxCopy(uint32_t src, uint32_t dst);                 // 0x804C62C0 PSMTXCopy
void MtxConcat(uint32_t a, uint32_t b, uint32_t ab);      // 0x804C6300 PSMTXConcat
void MtxMultVec(uint32_t m, uint32_t src, uint32_t dst);  // 0x804C6F20 PSMTXMultVec
void MtxApplyScale(uint32_t out, uint32_t m, uint32_t s); // 0x805019BC MTX34Scale
void MtxConcatArray(uint32_t a, uint32_t src, uint32_t dst, uint32_t count,
                    uint32_t r13);                        // 0x804C63D0 PSMTXConcatArray
uint32_t MtxInv34(uint32_t dst, uint32_t src);            // 0x80331064 MTX34Inv
uint32_t MtxInvTranspose33(uint32_t dst, uint32_t src);   // 0x80330E78
uint32_t MtxInvTranspose34(uint32_t dst, uint32_t src);   // 0x80330F5C
void MtxTo33(uint32_t dst, uint32_t src);                 // 0x80501968 MTX34ToMTX33
void MtxRotXYZFIdx(uint32_t m, float x, float y, float z,
                   uint32_t r2);                          // 0x80501AB4 MTX34RotXYZFIdx
uint32_t WorldMtxNodeStepMayaScale(uint32_t world, uint32_t worldScale, uint32_t parent, uint32_t scale,
                                   uint32_t flags, uint32_t node); // 0x800D7814
} // namespace G3dNative
