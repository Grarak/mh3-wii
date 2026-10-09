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
                    uint32_t r13);                      // 0x804C63D0 PSMTXConcatArray
uint32_t MtxInv34(uint32_t dst, uint32_t src);          // 0x80331064 MTX34Inv
uint32_t MtxInvTranspose33(uint32_t dst, uint32_t src); // 0x80330E78
uint32_t MtxInvTranspose34(uint32_t dst, uint32_t src); // 0x80330F5C
void MtxTo33(uint32_t dst, uint32_t src);               // 0x80501968 MTX34ToMTX33
void MtxRotXYZFIdx(uint32_t m, float x, float y, float z,
                   uint32_t r2);                                                                                                         // 0x80501AB4 MTX34RotXYZFIdx

// g3d_calc_world.cpp: the scene walk's view passes, recorded on the walking
// thread while enabled and run into their view arrays before the scene draws.
void DeferViewPasses(bool enabled);
void RunDeferredViewPasses();

// g3d_calc_world.cpp: billboard kinds 1 (0x8006F79C) and 3 (0x8006FC20) on
// mtx; false for the other kinds and in MH3_PORT_CHECK runs, which call the
// table at 0x8056FDF8 instead. mdl holds the ResMdl.
bool Billboard(uint32_t kind, uint32_t mtx, uint32_t model, uint32_t uniform, uint32_t mdl, uint32_t id);

// g3d_anm_result.cpp
void ResNodeChrAnmResult(uint32_t ref, uint32_t result);        // 0x80098FD0
void ResNodePatchChrAnmResult(uint32_t ref, uint32_t result);   // 0x80098DC0
void ChrAnmResultGetTranslation(uint32_t result, uint32_t out); // 0x8008E224
void ChrAnmResultSetTranslation(uint32_t result, uint32_t t);   // 0x8008F1AC
void ChrAnmResultGetMtx(uint32_t result, uint32_t out);         // 0x8008E6FC

// 0x8008F440; scratch: 12 guest bytes
void ChrAnmResultSetMtx(uint32_t result, uint32_t m, uint32_t scratch);

uint32_t WorldMtxNodeStepMayaScale(uint32_t world, uint32_t worldScale, uint32_t parent, uint32_t scale, uint32_t flags, uint32_t node); // 0x800D7814
} // namespace G3dNative
