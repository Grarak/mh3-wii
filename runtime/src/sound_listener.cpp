// 3D sound listener math, RMHP08: where a source sits relative to the
// listener, for the per-voice update (0x800E6378).
//
//   0x800E8F5C (listener mtx r3, world position r4, local out r5, angle out
//   r6, distance out r7): PSMTXMultVec(mtx, position, local), then the angle
//   atan2f(local.x, -local.z) and the distance sqrtf(x*x + y*y + z*z), each
//   stored as an f32. The squares and sums are single precision, as the
//   original's fmuls/fadds.
#include "abi_bridge.h"
#include "g3d_native.h"
#include "memory.h"

#include <cmath>

extern "C" void MH3_SoundListenerPolar(CpuContext* ctx) {
    const uint32_t local = ctx->gpr[5], angle = ctx->gpr[6], distance = ctx->gpr[7];
    G3dNative::MtxMultVec(ctx->gpr[3], ctx->gpr[4], local);
    const float x = MemoryInline::FlatReadFloat32(local);
    const float y = MemoryInline::FlatReadFloat32(local + 4);
    const float z = MemoryInline::FlatReadFloat32(local + 8);
    MemoryInline::FlatWriteFloat32(angle, float(std::atan2(double(x), -double(z))));
    const float xy = x * x + y * y;
    MemoryInline::FlatWriteFloat32(distance, float(std::sqrt(double(z * z + xy))));
}
REGISTER_NATIVE_FUNCTION_AS(0x800E8F5C, MH3_SoundListenerPolar, "SoundListenerPolar");
