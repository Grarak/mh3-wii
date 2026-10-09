// A monster's hit shapes from its bones, RMHP08. C reconstructions from the
// disassembly:
//
//   0x8029D8F8 EnemyPartShape(em, part, capsule, sphere): one body part's hit
//              shape in world space. Joint 1000: a sphere about the monster's
//              position (+0x188), its offset turned by the monster's angles
//              (+0x1BC); otherwise the joint's world matrix places a sphere
//              (mode 0) or a capsule (modes 1 and 2; mode 2's second end on
//              the second joint). Returns 1 for a capsule
//   0x800E0A78 CharJointMtx(ch, joint, out): the joint's world matrix (the
//              model's +0xEC array by the node's matrix id; nothing without
//              a model)
//   0x80135994 get_joint_wmat_em(em, joint, out): the monster's (em + 0x24)
//   0x801359A4 get_em_scale(em): +0x1CC * +0x1D0
//   0x800514A4 mulVecMat(VEC3* v, MTX34* m): v = the rotation part of m
//              times v
//
// The part (from the monster's data): s16 joints +0/+2, u16 mode +4, radius
// +0x10, offsets +0x14 and +0x20. The capsule: two ends and a radius (7
// floats); the sphere: a center and a radius.
#include "abi_bridge.h"
#include "g3d_native.h"
#include "memory.h"
#include "nw4r_math.h"
#include "port_check.h"

#include <cstdint>

namespace {

uint32_t Read32(uint32_t a) {
    return MemoryInline::FlatRead32(a);
}

float ReadFloat(uint32_t a) {
    return MemoryInline::FlatReadFloat32(a);
}

void WriteFloat(uint32_t a, float v) {
    MemoryInline::FlatWriteFloat32(a, v);
}

void JointMtx(uint32_t ch, uint32_t joint, uint32_t out) {
    const uint32_t model = Read32(ch + 0x118);

    if (!model)
        return;

    const uint32_t node = G3dNative::ResDicAtOffset14(ch + 0x114, joint);
    const uint32_t id = node ? Read32(node + 0x10) : 0; // the original asserts the node
    G3dNative::MtxCopy(Read32(model + 0xEC) + id * 0x30, out);
}

float Scale(uint32_t em) {
    return ReadFloat(em + 0x1CC) * ReadFloat(em + 0x1D0);
}

// mulVecMat: v through a copy (scratch, 12 bytes).
void MulVecMat(uint32_t v, uint32_t m, uint32_t scratch) {
    for (uint32_t k = 0; k < 12; k += 4)
        MemoryInline::FlatWrite32(scratch + k, Read32(v + k));

    Nw4rMath::MultVecSR(v, m, scratch);
}

// v (turned) + the matrix's translation, into out.
void PlaceAt(uint32_t out, uint32_t v, uint32_t m) {
    for (uint32_t k = 0; k < 3; ++k)
        WriteFloat(out + 4 * k, ReadFloat(v + 4 * k) + ReadFloat(m + 0xC + 0x10 * k));
}

// The original's frame (0x70 below r1): offsets +0x14 (a) and +8 (b), the
// matrix +0x20; mulVecMat's copy below that.
uint32_t PartShape(uint32_t r1, uint32_t em, uint32_t part, uint32_t capsule, uint32_t sphere) {
    const uint32_t sp = r1 - 0x70, a = sp + 0x14, b = sp + 8, m = sp + 0x20, scratch = sp - 0x20 + 8;

    for (uint32_t k = 0; k < 12; k += 4)
        MemoryInline::FlatWrite32(a + k, Read32(part + 0x14 + k));

    const int32_t joint = int16_t(MemoryInline::FlatRead16(part));

    if (joint == 1000) {
        WriteFloat(sphere + 0xC, ReadFloat(part + 0x10) * Scale(em));

        for (uint32_t k = 0; k < 12; k += 4)
            MemoryInline::FlatWrite32(sphere + k, Read32(em + 0x188 + k));

        Nw4rMath::RotMatrixXYZ(em + 0x1BC, m);
        MulVecMat(a, m, scratch);

        for (uint32_t k = 0; k < 12; k += 4)
            WriteFloat(sphere + k, ReadFloat(sphere + k) + ReadFloat(a + k));

        return 0;
    }

    JointMtx(em + 0x24, uint32_t(joint), m);
    const uint32_t mode = MemoryInline::FlatRead16(part + 4);

    if (mode - 1 <= 1) {
        WriteFloat(capsule + 0x18, ReadFloat(part + 0x10) * Scale(em));

        for (uint32_t k = 0; k < 12; k += 4)
            MemoryInline::FlatWrite32(b + k, Read32(part + 0x20 + k));

        MulVecMat(a, m, scratch);
        PlaceAt(capsule, a, m);

        if (mode == 2)
            JointMtx(em + 0x24, uint32_t(int32_t(int16_t(MemoryInline::FlatRead16(part + 2)))), m);

        MulVecMat(b, m, scratch);
        PlaceAt(capsule + 0xC, b, m);
        return 1;
    }

    if (mode == 0) {
        WriteFloat(sphere + 0xC, ReadFloat(part + 0x10) * Scale(em));
        MulVecMat(a, m, scratch);
        PlaceAt(sphere, a, m);
    }

    return 0;
}

void PartShapePort(CpuContext* p) {
    p->gpr[3] = PartShape(p->gpr[1], p->gpr[3], p->gpr[4], p->gpr[5], p->gpr[6]);
}

} // namespace

extern "C" void func_8029D8F8(CpuContext* ctx);
static uint32_t PartShapeChecked(uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("EnemyPartShape")) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[2] = 0x8079FF60u;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        c->gpr[5] = r5;
        c->gpr[6] = r6;
        PortCheck::RunOutput(c, "EnemyPartShape", PortCheck::Returns::Gpr3, func_8029D8F8, PartShapePort, {{r5, 0x1C}, {r6, 0x10}});
        return c->gpr[3];
    }
#endif
    return PartShape(r1, r3, r4, r5, r6);
}

extern "C" void MH3_EnemyPartShape(CpuContext* ctx) {
    ctx->gpr[3] = PartShapeChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8029D8F8, MH3_EnemyPartShape, "EnemyPartShape");

// CharJointMtx (r3 ch, r4 joint, r5 out) and the monster's wrapper.
extern "C" void func_800E0A78(CpuContext* ctx);
static void JointMtxPort(CpuContext* p) {
    JointMtx(p->gpr[3], p->gpr[4], p->gpr[5]);
}

static void JointMtxChecked([[maybe_unused]] uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("CharJointMtx")) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        c->gpr[5] = r5;
        PortCheck::RunOutput(c, "CharJointMtx", func_800E0A78, JointMtxPort, {{r5, 0x30}});
        return;
    }
#endif
    JointMtx(r3, r4, r5);
}

extern "C" void MH3_CharJointMtx(CpuContext* ctx) {
    JointMtxChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800E0A78, MH3_CharJointMtx, "CharJointMtx");

extern "C" void MH3_EnemyJointMtx(CpuContext* ctx) {
    JointMtxChecked(ctx->gpr[1], ctx->gpr[3] + 0x24, ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80135994, MH3_EnemyJointMtx, "EnemyJointMtx");

// mulVecMat (r3 v, r4 m); its copy of v below the stack pointer, as the
// original's frame keeps it.
extern "C" void func_800514A4(CpuContext* ctx);
static void MulVecMatPort(CpuContext* p) {
    MulVecMat(p->gpr[3], p->gpr[4], p->gpr[1] - 0x20 + 8);
}

static void MulVecMatChecked(uint32_t r1, uint32_t r3, uint32_t r4) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("MulVecMat")) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        PortCheck::RunOutput(c, "MulVecMat", func_800514A4, MulVecMatPort, {{r3, 12}});
        return;
    }
#endif
    MulVecMat(r3, r4, r1 - 0x20 + 8);
}

extern "C" void MH3_MulVecMat(CpuContext* ctx) {
    MulVecMatChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800514A4, MH3_MulVecMat, "MulVecMat");
