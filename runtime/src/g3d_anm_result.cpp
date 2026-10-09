// NW4R g3d ChrAnmResult helpers, RMHP08. C reconstructions from the
// disassembly of the debug-build ResNode and ChrAnmResult accessors the world
// matrix pass calls per node (G3dCalcWorld, g3d_calc_world.cpp, and the
// player's and monsters' bone code):
//
//   0x80098FD0 ResNodeChrAnmResult(node ref, result): the node's static pose
//              (bind scale, rotation and translation) as a ChrAnmResult
//   0x80098DC0 ResNodePatchChrAnmResult(node ref, result): the parts the
//              result's flags 0x80 (scale), 0x100 (rotation) and 0x200
//              (translation) ask for, taken from the node instead
//   0x8008E224 ChrAnmResultGetTranslation(result, out VEC3)
//   0x8008F1AC ChrAnmResultSetTranslation(result, VEC3)
//   0x8008E934 ChrAnmResultSetScale(result, VEC3)
//   0x8008E46C ChrAnmResultGetRotTrans(result, MTX34 out)
//   0x8008E6FC ChrAnmResultGetMtx(result, MTX34 out): with the scale
//   0x8008F440 ChrAnmResultSetMtx(result, MTX34): scale, rotation and
//              translation from a matrix
//
// A ChrAnmResult (0x4C bytes): u32 flags, VEC3 scale (+4), VEC3 rotation in
// degrees (+0x10), MTX34 rotation and translation (+0x1C, the translation in
// its last column: +0x28, +0x38, +0x48). Flags: 0x2/0x4 identity rotation and
// translation (with unit scale), 0x8 unit scale, 0x10 scale uniform, 0x20
// identity rotation, 0x40 zero translation, 0x400/0x800 the node's segment
// scale and billboard bits, 0x80000000 rotation matrix valid.
//
// The ResNode (node ref: the address of a word holding the node's data):
// u32 flags +0x14 (2 zero translation, 4 identity rotation, 8 unit scale,
// 0x10 uniform scale, 0x20 segment scale compensation, 0x40 parent's), VEC3
// scale +0x20, VEC3 rotation in degrees +0x2C, VEC3 translation +0x38.
//
// The originals assert their pointers (null, or outside MEM1/MEM2/the locked
// cache); the ports drop those debug checks. The registered bodies name only
// argument registers, so translated callers call them directly;
// MH3_PORT_CHECK=Name checks the written result.
#include "abi_bridge.h"
#include "fatal.h"
#include "g3d_native.h"
#include "memory.h"
#include "nw4r_math.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cstdint>
#include <stdexcept>

namespace {

constexpr uint32_t kR2 = 0x8079FF60u;
constexpr uint32_t kOne = 0x80798408u;     // 1.0f (r2 - 31576)
constexpr uint32_t kZero = 0x8079840Cu;    // 0.0f
constexpr uint32_t kZeroTrans = 0x80798390u; // 0.0f, ChrAnmResult's zero translation
constexpr uint32_t kDegToFIdx = 0x807983B4u; // 256 / 360 (r2 - 31660)

// The node's flags (+0x14) and its parts.
constexpr uint32_t kNodeFlags = 0x14, kNodeScale = 0x20, kNodeRotation = 0x2C, kNodeTranslation = 0x38;
constexpr uint32_t kNodeZeroTrans = 0x2, kNodeIdentityRot = 0x4, kNodeUnitScale = 0x8, kNodeUniformScale = 0x10;
constexpr uint32_t kNodeSegmentScale = 0x20, kNodeParentSegment = 0x40;

// The result's parts and flags.
constexpr uint32_t kScale = 0x04, kRotation = 0x10, kMatrix = 0x1C;
constexpr uint32_t kTransX = 0x28, kTransY = 0x38, kTransZ = 0x48;
constexpr uint32_t kFlagRotTransIdentity = 0x4, kFlagScaleOne = 0x8, kFlagScaleUniform = 0x10;
constexpr uint32_t kFlagRotIdentity = 0x20, kFlagTransZero = 0x40, kFlagMatrixValid = 0x80000000u;

uint32_t Read32(uint32_t a) {
    return MemoryInline::FlatRead32(a);
}

void Write32(uint32_t a, uint32_t v) {
    MemoryInline::FlatWrite32(a, v);
}

float ReadFloat(uint32_t a) {
    return MemoryInline::FlatReadFloat32(a);
}

[[noreturn]] void Panic(const char* what) {
    MH3_THROW(std::runtime_error(what));
}

void CopyVec3(uint32_t dst, uint32_t src) {
    Write32(dst, Read32(src));
    Write32(dst + 4, Read32(src + 4));
    Write32(dst + 8, Read32(src + 8));
}

void FillVec3(uint32_t dst, uint32_t bits) {
    Write32(dst, bits);
    Write32(dst + 4, bits);
    Write32(dst + 8, bits);
}

// PSMTXIdentity.
void Identity(uint32_t m) {
    const uint32_t one = 0x3F800000u;

    for (uint32_t k = 0; k < 0x30; k += 4)
        Write32(m + k, k == 0x00 || k == 0x14 || k == 0x28 ? one : 0u);
}

// AnmChrRotationMtxFromDegrees (0x8008C4E8): MTX34RotXYZFIdx of the degrees
// times 256 / 360 (fmuls each).
void RotationFromDegrees(uint32_t m, uint32_t degrees) {
    const float k = ReadFloat(kDegToFIdx);
    G3dNative::MtxRotXYZFIdx(m, k * ReadFloat(degrees), k * ReadFloat(degrees + 4), k * ReadFloat(degrees + 8), kR2);
}

// Both rotation and translation identity: 4, and 2 too with a unit scale.
uint32_t WithIdentityBits(uint32_t flags) {
    if ((flags & kFlagRotIdentity) && (flags & kFlagTransZero))
        return flags & kFlagScaleOne ? flags | 6 : flags | kFlagRotTransIdentity;

    return flags;
}

void ChrAnmResult(uint32_t ref, uint32_t result) {
    if (!result)
        Panic("NW4R panic (g3d_resnode.cpp:126): Pointer must not be NULL (pResult)");

    const uint32_t node = Read32(ref);

    if (!node)
        Panic("NW4R panic (g3d_resnode.cpp:127): Failed assertion IsValid()");

    const uint32_t nodeFlags = Read32(node + kNodeFlags);
    uint32_t flags = 0;

    if (nodeFlags & kNodeUnitScale) {
        flags = kFlagScaleOne | kFlagScaleUniform;
        FillVec3(result + kScale, Read32(kOne));
    } else {
        if (nodeFlags & kNodeUniformScale)
            flags = kFlagScaleUniform;

        CopyVec3(result + kScale, node + kNodeScale);
    }

    if (nodeFlags & kNodeIdentityRot) {
        Identity(result + kMatrix);
        flags |= kFlagRotIdentity;
    } else {
        CopyVec3(result + kRotation, node + kNodeRotation);
        RotationFromDegrees(result + kMatrix, node + kNodeRotation);
    }

    if (nodeFlags & kNodeZeroTrans) {
        flags |= kFlagTransZero;
    } else {
        Write32(result + kTransX, Read32(node + kNodeTranslation));
        Write32(result + kTransY, Read32(node + kNodeTranslation + 4));
        Write32(result + kTransZ, Read32(node + kNodeTranslation + 8));
    }

    flags = WithIdentityBits(flags) | kFlagMatrixValid | 1;

    if (nodeFlags & kNodeSegmentScale)
        flags |= 0x400;

    if (nodeFlags & kNodeParentSegment)
        flags |= 0x800;

    Write32(result, flags);
}

void PatchChrAnmResult(uint32_t ref, uint32_t result) {
    if (!result)
        Panic("NW4R panic (g3d_resnode.cpp:32): Pointer must not be NULL (pResult)");

    const uint32_t node = Read32(ref);

    if (!node)
        Panic("NW4R panic (g3d_resnode.cpp:33): Failed assertion IsValid()");

    const uint32_t nodeFlags = Read32(node + kNodeFlags);
    uint32_t flags = Read32(result);

    // 0x80: the node's scale.
    if (flags & 0x80) {
        if (nodeFlags & kNodeUnitScale) {
            FillVec3(result + kScale, Read32(kOne));
            flags |= kFlagScaleOne | kFlagScaleUniform;
        } else {
            flags = nodeFlags & kNodeUniformScale ? flags | kFlagScaleUniform : flags & ~kFlagScaleUniform;
            CopyVec3(result + kScale, node + kNodeScale);
            flags &= ~0xAu;
        }
    }

    // 0x100: the node's rotation, keeping the result's translation.
    if (flags & 0x100) {
        const uint32_t x = Read32(result + kTransX), y = Read32(result + kTransY), z = Read32(result + kTransZ);

        if (nodeFlags & kNodeIdentityRot) {
            Identity(result + kMatrix);
            flags |= kFlagRotIdentity;
        } else {
            RotationFromDegrees(result + kMatrix, node + kNodeRotation);
            flags &= ~0x26u;
        }

        Write32(result + kTransX, x);
        Write32(result + kTransY, y);
        Write32(result + kTransZ, z);
        flags |= kFlagMatrixValid;
    }

    // 0x200: the node's translation.
    if (flags & 0x200) {
        if (nodeFlags & kNodeZeroTrans) {
            const uint32_t zero = Read32(kZero);
            Write32(result + kTransZ, zero);
            Write32(result + kTransY, zero);
            Write32(result + kTransX, zero);
            flags |= kFlagTransZero;
        } else {
            Write32(result + kTransX, Read32(node + kNodeTranslation));
            Write32(result + kTransY, Read32(node + kNodeTranslation + 4));
            Write32(result + kTransZ, Read32(node + kNodeTranslation + 8));
            flags &= ~0x46u;
        }
    }

    Write32(result, WithIdentityBits(flags) & ~0x380u);
}

void GetTranslation(uint32_t result, uint32_t out) {
    if (Read32(result) & kFlagTransZero) {
        FillVec3(out, Read32(kZeroTrans));
        return;
    }

    Write32(out, Read32(result + kTransX));
    Write32(out + 4, Read32(result + kTransY));
    Write32(out + 8, Read32(result + kTransZ));
}

void SetTranslation(uint32_t result, uint32_t t) {
    const float zero = ReadFloat(kZeroTrans);
    const uint32_t flags = Read32(result);

    if (zero == ReadFloat(t) && zero == ReadFloat(t + 4) && zero == ReadFloat(t + 8)) {
        uint32_t set = kFlagTransZero;

        if (flags & kFlagRotIdentity)
            set = flags & kFlagScaleOne ? 0x46 : 0x44;

        Write32(result, flags | set);
    } else {
        Write32(result, flags & ~0x46u);
    }

    Write32(result + kTransX, Read32(t));
    Write32(result + kTransY, Read32(t + 4));
    Write32(result + kTransZ, Read32(t + 8));
}

// ChrAnmResult::SetScale (0x8008E934): the scale (+4) and its flags: 0x18
// (and 2 with an identity rotation and translation, 4) when it is (1, 1, 1),
// else 0x10 when uniform, 0x2/0x8/0x10 cleared first.
void SetScale(uint32_t result, uint32_t scale) {
    const float one = ReadFloat(0x807983A8u); // r2 - 31672
    const float x = ReadFloat(scale), y = ReadFloat(scale + 4), z = ReadFloat(scale + 8);
    uint32_t flags = Read32(result);

    if (one == x && one == y && one == z) {
        flags |= kFlagScaleOne | kFlagScaleUniform;

        if (flags & kFlagRotTransIdentity)
            flags |= 2;
    } else {
        flags &= ~0x1Au;

        if (x == y && y == z)
            flags |= kFlagScaleUniform;
    }

    Write32(result, flags);
    CopyVec3(result + kScale, scale);
}

// 0x8008F728: the length of column i of the matrix (FSqrt of m[4+i]^2,
// then + m[i]^2 and + m[8+i]^2 by fmadds).
float ColumnLength(uint32_t m, uint32_t i) {
    const double y = ReadFloat(m + 0x10 + 4 * i), x = ReadFloat(m + 4 * i), z = ReadFloat(m + 0x20 + 4 * i);
    const double sum = PPC_Fmadds(z, z, PPC_Fmadds(x, x, PpcFmulsInline(y, y)));
    return Nw4rMath::FSqrt(sum);
}

// ChrAnmResult::SetRotTrans from a matrix with scale (0x8008F440): the
// columns' lengths as the scale (SetScale), the matrix divided by them
// (unless the scale is one) as the rotation and translation; the matrix
// flag (0x80000000) cleared. `scratch` is 12 guest bytes for the scales.
void SetMtx(uint32_t result, uint32_t m, uint32_t scratch) {
    const float sz = ColumnLength(m, 2), sy = ColumnLength(m, 1), sx = ColumnLength(m, 0);
    MemoryInline::FlatWriteFloat32(scratch, sx);
    MemoryInline::FlatWriteFloat32(scratch + 4, sy);
    MemoryInline::FlatWriteFloat32(scratch + 8, sz);
    SetScale(result, scratch);

    if (Read32(result) & kFlagScaleOne) {
        G3dNative::MtxCopy(m, result + kMatrix);
    } else {
        const float iz = Nw4rMath::FastReciprocal(sz), iy = Nw4rMath::FastReciprocal(sy), ix = Nw4rMath::FastReciprocal(sx);
        MemoryInline::FlatWriteFloat32(scratch, ix);
        MemoryInline::FlatWriteFloat32(scratch + 4, iy);
        MemoryInline::FlatWriteFloat32(scratch + 8, iz);
        G3dNative::MtxApplyScale(result + kMatrix, m, scratch);
    }

    Write32(result, Read32(result) & ~kFlagMatrixValid);
}

// ChrAnmResult::GetRotTrans (0x8008E46C): the rotation and translation
// matrix, identity parts by the flags.
void GetRotTrans(uint32_t result, uint32_t out) {
    const uint32_t flags = Read32(result);

    if (flags & kFlagRotIdentity) {
        Identity(out);

        if (!(flags & kFlagTransZero)) {
            Write32(out + 0x0C, Read32(result + kTransX));
            Write32(out + 0x1C, Read32(result + kTransY));
            Write32(out + 0x2C, Read32(result + kTransZ));
        }

        return;
    }

    G3dNative::MtxCopy(result + kMatrix, out);

    if (flags & kFlagTransZero) {
        const uint32_t zero = Read32(0x80798390u); // r2 - 31696
        Write32(out + 0x0C, zero);
        Write32(out + 0x1C, zero);
        Write32(out + 0x2C, zero);
    }
}

// ChrAnmResult::GetMtx (0x8008E6FC): GetRotTrans, scaled unless the scale is
// one.
void GetMtx(uint32_t result, uint32_t out) {
    GetRotTrans(result, out);

    if (!(Read32(result) & kFlagScaleOne))
        G3dNative::MtxApplyScale(out, out, result + kScale);
}

} // namespace

void G3dNative::ChrAnmResultGetMtx(uint32_t result, uint32_t out) {
    GetMtx(result, out);
}

void G3dNative::ChrAnmResultSetMtx(uint32_t result, uint32_t m, uint32_t scratch) {
    SetMtx(result, m, scratch);
}

void G3dNative::ResNodeChrAnmResult(uint32_t ref, uint32_t result) {
    ChrAnmResult(ref, result);
}

void G3dNative::ResNodePatchChrAnmResult(uint32_t ref, uint32_t result) {
    PatchChrAnmResult(ref, result);
}

void G3dNative::ChrAnmResultGetTranslation(uint32_t result, uint32_t out) {
    GetTranslation(result, out);
}

void G3dNative::ChrAnmResultSetTranslation(uint32_t result, uint32_t t) {
    SetTranslation(result, t);
}

// Ports of (r3, r4) that write `bytes` at r3 (Out = 3) or r4 (Out = 4),
// checked with RunOutput.
#ifdef MH3_DEVELOPMENT
#define MH3_RESULT_CHECK(Name, Addr, Out, Bytes)                                                                                                                                                       \
    const bool check = PortCheck::Enabled(#Name);                                                                                                                                                      \
    if (check) [[unlikely]] {                                                                                                                                                                          \
        CpuContext* c = TryGetCpuContext();                                                                                                                                                            \
        c->gpr[1] = r1;                                                                                                                                                                                \
        c->gpr[2] = kR2;                                                                                                                                                                               \
        c->gpr[3] = r3;                                                                                                                                                                                \
        c->gpr[4] = r4;                                                                                                                                                                                \
        PortCheck::RunOutput(c, #Name, func_##Addr, Name##Port, {{Out, Bytes}});                                                                                                                       \
        return;                                                                                                                                                                                        \
    }
#else
#define MH3_RESULT_CHECK(Name, Addr, Out, Bytes) (void)r1;
#endif

#define MH3_RESULT_PORT(Addr, Name, Fn, Out, Bytes)                                                                                                                                                    \
    extern "C" void func_##Addr(CpuContext* ctx);                                                                                                                                                      \
    static void Name##Port(CpuContext* p) {                                                                                                                                                            \
        Fn(p->gpr[3], p->gpr[4]);                                                                                                                                                                      \
    }                                                                                                                                                                                                  \
    static void Name##Checked(uint32_t r1, uint32_t r3, uint32_t r4) {                                                                                                                                 \
        MH3_RESULT_CHECK(Name, Addr, Out, Bytes)                                                                                                                                                       \
        Fn(r3, r4);                                                                                                                                                                                    \
    }

MH3_RESULT_PORT(80098FD0, ResNodeChrAnmResult, ChrAnmResult, r4, 0x4C)
extern "C" void MH3_ResNodeChrAnmResult(CpuContext* ctx) {
    ResNodeChrAnmResultChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80098FD0, MH3_ResNodeChrAnmResult, "ResNodeChrAnmResult");

MH3_RESULT_PORT(80098DC0, ResNodePatchChrAnmResult, PatchChrAnmResult, r4, 0x4C)
extern "C" void MH3_ResNodePatchChrAnmResult(CpuContext* ctx) {
    ResNodePatchChrAnmResultChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80098DC0, MH3_ResNodePatchChrAnmResult, "ResNodePatchChrAnmResult");

MH3_RESULT_PORT(8008E224, ChrAnmResultGetTranslation, GetTranslation, r4, 12)
extern "C" void MH3_ChrAnmResultGetTranslation(CpuContext* ctx) {
    ChrAnmResultGetTranslationChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8008E224, MH3_ChrAnmResultGetTranslation, "ChrAnmResultGetTranslation");

MH3_RESULT_PORT(8008F1AC, ChrAnmResultSetTranslation, SetTranslation, r3, 0x4C)
extern "C" void MH3_ChrAnmResultSetTranslation(CpuContext* ctx) {
    ChrAnmResultSetTranslationChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8008F1AC, MH3_ChrAnmResultSetTranslation, "ChrAnmResultSetTranslation");

MH3_RESULT_PORT(8008E934, ChrAnmResultSetScale, SetScale, r3, 0x10)
extern "C" void MH3_ChrAnmResultSetScale(CpuContext* ctx) {
    ChrAnmResultSetScaleChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8008E934, MH3_ChrAnmResultSetScale, "ChrAnmResultSetScale");

// SetMtx keeps the scales in its own frame (the original's: r1 - 0x50 + 8).
static void SetMtxAt(uint32_t r1, uint32_t result, uint32_t m) {
    SetMtx(result, m, r1 - 0x50 + 8);
}

extern "C" void func_8008F440(CpuContext* ctx);
static void ChrAnmResultSetMtxPort(CpuContext* p) {
    SetMtxAt(p->gpr[1], p->gpr[3], p->gpr[4]);
}

static void ChrAnmResultSetMtxChecked(uint32_t r1, uint32_t r3, uint32_t r4) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("ChrAnmResultSetMtx")) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[2] = kR2;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        PortCheck::RunOutput(c, "ChrAnmResultSetMtx", func_8008F440, ChrAnmResultSetMtxPort, {{r3, 0x4C}});
        return;
    }
#endif
    SetMtxAt(r1, r3, r4);
}

extern "C" void MH3_ChrAnmResultSetMtx(CpuContext* ctx) {
    ChrAnmResultSetMtxChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8008F440, MH3_ChrAnmResultSetMtx, "ChrAnmResultSetMtx");

MH3_RESULT_PORT(8008E46C, ChrAnmResultGetRotTrans, GetRotTrans, r4, 0x30)
extern "C" void MH3_ChrAnmResultGetRotTrans(CpuContext* ctx) {
    ChrAnmResultGetRotTransChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8008E46C, MH3_ChrAnmResultGetRotTrans, "ChrAnmResultGetRotTrans");

MH3_RESULT_PORT(8008E6FC, ChrAnmResultGetMtx, GetMtx, r4, 0x30)
extern "C" void MH3_ChrAnmResultGetMtx(CpuContext* ctx) {
    ChrAnmResultGetMtxChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8008E6FC, MH3_ChrAnmResultGetMtx, "ChrAnmResultGetMtx");
