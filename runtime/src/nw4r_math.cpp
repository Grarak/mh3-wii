// NW4R math and the game's small angle and vector helpers, RMHP08. C
// reconstructions from the disassembly; each original is a few float
// operations behind a stack frame and, for the u16 angles, a chain of calls
// through quantized loads and stores:
//
//   0x805014B4 SinFIdx(f1)           nw4r::math: the {sin, cos, dsin, dcos}
//   0x8050151C CosFIdx(f1)           table at 0x805744A0 (256 entries),
//                                    interpolated (fmuls, then fadds)
//   0x80501490 FrSqrt(f1)            frsqrte refined by one Newton step
//   0x80050C24 FSqrt(f1)             x * FrSqrt(x), 0 for x <= 0
//   0x80050248 U16ToFloat(r3)        (float) u16 through GQR3
//   0x80050278 U16ToFloatStore(r3, r4)  *(float*)r4 = (float) *(u16*)r3
//   0x800502A8 U16ToFloatLoad(r3)    (float) *(u16*)r3
//   0x800502B0 SinU16(r3)            SinFIdx(u16 angle * 1/256): the game's
//   0x80050218 CosU16(r3)            16-bit angles (65536 a turn)
//   0x80050130 rotMatrixX(angle, MTX34*)   m = R(angle) * m about X, Y or Z
//   0x800502E0 rotMatrixY(angle, MTX34*)   (rows 1-2, 0-2, 0-1 turned)
//   0x800503C8 rotMatrixZ(angle, MTX34*)
//   0x800509F4 cpSetRotMatrixZXY(angles, MTX34*)  identity turned by Z, X, Y
//   0x80050914 cpSetRotMatrix(angles, MTX34*)     MTX34RotXYZFIdx of them
//   0x80501C48 MTX34MultVecSR(out, m, v)          rotation part times v
//   0x804C71E0 PSVECNormalize(src, dst)
//   0x800508B4 VecNormalizeTo(dst, src)    PSVECNormalize(src, dst), returns dst
//   0x80051F44 VectorScaleWrapper(out, v, f1 s)  out = v * s (VecScale3)
//
// GQR3 is the SDK's u16 setting (type 4, no scale) for the whole game, so the
// quantized conversions are plain u16 <-> float ones (a float store
// truncating and saturating). The products and sums are float operations in
// the originals' order; the vector normalization uses the paired-single
// helpers of its translation. The registered bodies name only argument
// registers, so translated callers call them directly; MH3_PORT_CHECK=Name
// compares the result (f1, r3) or the written memory.
#include "abi_bridge.h"
#include "g3d_native.h"
#include "memory.h"
#include "nw4r_math.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cmath>
#include <cstdint>

namespace {

constexpr uint32_t kSinCosTable = 0x805744A0u;
constexpr uint32_t kFIdxPeriod = 0x8079F960u; // 65536.0f (r2 - 1536)
constexpr uint32_t kU16ToFIdx = 0x80798090u;  // 1/256 (r2 - 32464)
constexpr uint32_t kSqrtZero = 0x8079809Cu;   // 0.0f (r2 - 32452)
constexpr uint32_t kHalf = 0x8079F954u;       // 0.5f (r2 - 1548)
constexpr uint32_t kThree = 0x8079F958u;      // 3.0f (r2 - 1544)

float ReadFloat(uint32_t a) {
    return MemoryInline::FlatReadFloat32(a);
}

void WriteFloat(uint32_t a, float v) {
    MemoryInline::FlatWriteFloat32(a, v);
}

// The table entry and fraction of |fidx|: the magnitude wrapped while above
// a period, cut to u16 by the quantized store (truncating, saturating).
struct TableIndex {
    uint32_t entry;
    float fraction;
};

TableIndex Index(float fidx) {
    const float period = ReadFloat(kFIdxPeriod);
    float a = std::fabs(fidx);

    while (a > period)
        a -= period;

    const uint32_t whole = a >= 65535.0f ? 65535u : a > 0.0f ? uint32_t(a) : 0u;
    return {kSinCosTable + 16 * (whole & 0xFF), a - float(whole)};
}

float SinFIdx(float fidx) {
    const TableIndex i = Index(fidx);
    const float value = ReadFloat(i.entry) + i.fraction * ReadFloat(i.entry + 8);
    return fidx < 0.0f ? -value : value;
}

float CosFIdx(float fidx) {
    const TableIndex i = Index(fidx);
    return ReadFloat(i.entry + 4) + i.fraction * ReadFloat(i.entry + 12);
}

float SinU16(uint32_t angle) {
    return SinFIdx(ReadFloat(kU16ToFIdx) * float(angle & 0xFFFF));
}

float CosU16(uint32_t angle) {
    return CosFIdx(ReadFloat(kU16ToFIdx) * float(angle & 0xFFFF));
}

float FrSqrt(double x) {
    const double e = PPC_Frsqrte(x);
    const double refine = PPC_Fnmsubs(PpcFmulsInline(e, e), x, ReadFloat(kThree)); // 3 - e*e*x
    return float(PpcFmulsInline(refine, PpcFmulsInline(e, ReadFloat(kHalf))));
}

float FSqrt(double x) {
    const float zero = ReadFloat(kSqrtZero);

    if (x <= zero)
        return zero;

    return float(PpcFmulsInline(x, FrSqrt(x)));
}

// Two rows of the matrix turned: for each column, a' = c a - s b and
// b' = s a + c b (Sub first) or a' = s b + c a, b' = c b - s a.
void TurnRows(uint32_t a, uint32_t b, float s, float c) {
    for (uint32_t k = 0; k < 12; k += 4) {
        const float x = ReadFloat(a + k), y = ReadFloat(b + k);
        WriteFloat(a + k, c * x - s * y);
        WriteFloat(b + k, s * x + c * y);
    }
}

void RotMatrixX(uint32_t angle, uint32_t m) {
    TurnRows(m + 0x10, m + 0x20, SinU16(angle), CosU16(angle));
}

void RotMatrixY(uint32_t angle, uint32_t m) {
    // Rows 0 and 2 the other way round: row 0 = s row2 + c row0.
    TurnRows(m + 0x20, m, SinU16(angle), CosU16(angle));
}

void RotMatrixZ(uint32_t angle, uint32_t m) {
    TurnRows(m, m + 0x10, SinU16(angle), CosU16(angle));
}

// cpSetRotMatrixZXY: identity, then turned by Z, X and Y (u32 angles at
// +8, +0 and +4, 16-bit units).
void ZXY(uint32_t angles, uint32_t m) {
    for (uint32_t k = 0; k < 0x30; k += 4)
        MemoryInline::FlatWrite32(m + k, k == 0x00 || k == 0x14 || k == 0x28 ? 0x3F800000u : 0u);

    RotMatrixZ(MemoryInline::FlatRead32(angles + 8), m);
    RotMatrixX(MemoryInline::FlatRead32(angles), m);
    RotMatrixY(MemoryInline::FlatRead32(angles + 4), m);
}

// MTX34MultVecSR (0x80501C48): out = the rotation part of m times v, each
// row (r0 v.x + r1 v.y) + r2 v.z in float.
void MultVecSR(uint32_t out, uint32_t m, uint32_t v) {
    const float x = ReadFloat(v), y = ReadFloat(v + 4), z = ReadFloat(v + 8);
    float r[3];

    for (uint32_t i = 0; i < 3; ++i)
        r[i] = (ReadFloat(m + 0x10 * i) * x + ReadFloat(m + 0x10 * i + 4) * y) + ReadFloat(m + 0x10 * i + 8) * z;

    WriteFloat(out, r[0]);
    WriteFloat(out + 4, r[1]);
    WriteFloat(out + 8, r[2]);
}

// cpSetRotMatrix (0x80050914): MTX34RotXYZFIdx of the u32 angles (16-bit
// units) at `angles`, each to degrees (0x800500A8: c1 * angle / c2) and then
// to FIdx (* k).
void RotMatrixXYZ(uint32_t angles, uint32_t m) {
    const float c1 = ReadFloat(0x80798080u), c2 = ReadFloat(0x80798084u), k = ReadFloat(0x80798094u); // r2 - 32480, - 32476, - 32460
    float degrees[3];

    for (uint32_t i = 0; i < 3; ++i)
        degrees[i] = (c1 * float(MemoryInline::FlatRead32(angles + 4 * i) & 0xFFFF)) / c2;

    G3dNative::MtxRotXYZFIdx(m, k * degrees[0], k * degrees[1], k * degrees[2], 0x8079FF60u);
}

void VecNormalize(uint32_t src, uint32_t dst) {
    const float x = ReadFloat(src), y = ReadFloat(src + 4), z = ReadFloat(src + 8);
    const double xy = PPC_PsMulInline(PpcPackPairedInline(x, y), PpcPackPairedInline(x, y));
    const double zz = PPC_PsFromScalarInline(z);
    const double sq = PPC_PsToScalarInline(PPC_PsSum0Inline(PPC_PsMaddInline(zz, zz, xy), xy, xy));
    const double e = PPC_Frsqrte(sq);
    const double refine = PPC_Fnmsubs(PpcFmulsInline(e, e), sq, ReadFloat(0x8079F774u)); // 3 - e*e*sq (r2 - 2028)
    const double scale = PpcFmulsInline(refine, PpcFmulsInline(e, ReadFloat(0x8079F770u))); // 0.5 (r2 - 2032)
    WriteFloat(dst, float(PpcFmulsInline(x, scale)));
    WriteFloat(dst + 4, float(PpcFmulsInline(y, scale)));
    WriteFloat(dst + 8, float(PpcFmulsInline(z, scale)));
}

} // namespace

float Nw4rMath::SinU16(uint32_t angle) {
    return ::SinU16(angle);
}

float Nw4rMath::CosU16(uint32_t angle) {
    return ::CosU16(angle);
}

float Nw4rMath::FSqrt(double x) {
    return ::FSqrt(x);
}

float Nw4rMath::FrSqrt(double x) {
    return ::FrSqrt(x);
}

void Nw4rMath::RotMatrixX(uint32_t angle, uint32_t m) {
    ::RotMatrixX(angle, m);
}

void Nw4rMath::RotMatrixY(uint32_t angle, uint32_t m) {
    ::RotMatrixY(angle, m);
}

void Nw4rMath::RotMatrixZ(uint32_t angle, uint32_t m) {
    ::RotMatrixZ(angle, m);
}

void Nw4rMath::RotMatrixZXY(uint32_t angles, uint32_t m) {
    ZXY(angles, m);
}

void Nw4rMath::RotMatrixXYZ(uint32_t angles, uint32_t m) {
    ::RotMatrixXYZ(angles, m);
}

void Nw4rMath::MultVecSR(uint32_t out, uint32_t m, uint32_t v) {
    ::MultVecSR(out, m, v);
}

// f1 -> f1 ports, checked with RunPure.
#ifdef MH3_DEVELOPMENT
#define MH3_FLOAT_CHECK(Name, Addr, Setup)                                                                                                                                                             \
    const bool check = PortCheck::Enabled(#Name);                                                                                                                                                      \
    if (check) [[unlikely]] {                                                                                                                                                                          \
        CpuContext* c = TryGetCpuContext();                                                                                                                                                            \
        c->gpr[1] = r1;                                                                                                                                                                                \
        c->gpr[2] = 0x8079FF60u;                                                                                                                                                                       \
        Setup;                                                                                                                                                                                         \
        PortCheck::RunPure(c, #Name, PortCheck::Returns::Fpr1, func_##Addr, Name##Port);                                                                                                               \
        return c->fpr[1].d;                                                                                                                                                                            \
    }
#define MH3_OUTPUT_CHECK(Name, Addr, Out, Bytes, Setup)                                                                                                                                                \
    const bool check = PortCheck::Enabled(#Name);                                                                                                                                                      \
    if (check) [[unlikely]] {                                                                                                                                                                          \
        CpuContext* c = TryGetCpuContext();                                                                                                                                                            \
        c->gpr[1] = r1;                                                                                                                                                                                \
        c->gpr[2] = 0x8079FF60u;                                                                                                                                                                       \
        Setup;                                                                                                                                                                                         \
        PortCheck::RunOutput(c, #Name, func_##Addr, Name##Port, {{Out, Bytes}});                                                                                                                       \
        return;                                                                                                                                                                                        \
    }
#else
#define MH3_FLOAT_CHECK(Name, Addr, Setup) (void)r1;
#define MH3_OUTPUT_CHECK(Name, Addr, Out, Bytes, Setup) (void)r1;
#endif

// SinFIdx, CosFIdx, FrSqrt, FSqrt: f1 -> f1.
#define MH3_FLOAT_PORT(Addr, Name, Fn)                                                                                                                                                                 \
    extern "C" void func_##Addr(CpuContext* ctx);                                                                                                                                                      \
    static void Name##Port(CpuContext* p) {                                                                                                                                                            \
        p->fpr[1].d = Fn(float(p->fpr[1].d));                                                                                                                                                          \
    }                                                                                                                                                                                                  \
    static double Name##Checked(uint32_t r1, double f1) {                                                                                                                                              \
        MH3_FLOAT_CHECK(Name, Addr, c->fpr[1].d = f1)                                                                                                                                                  \
        return Fn(float(f1));                                                                                                                                                                          \
    }

MH3_FLOAT_PORT(805014B4, SinFIdx, SinFIdx)
extern "C" void MH3_SinFIdx(CpuContext* ctx) {
    ctx->fpr[1].d = SinFIdxChecked(ctx->gpr[1], ctx->fpr[1].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x805014B4, MH3_SinFIdx, "SinFIdx");

MH3_FLOAT_PORT(8050151C, CosFIdx, CosFIdx)
extern "C" void MH3_CosFIdx(CpuContext* ctx) {
    ctx->fpr[1].d = CosFIdxChecked(ctx->gpr[1], ctx->fpr[1].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050151C, MH3_CosFIdx, "CosFIdx");

// FrSqrt and FSqrt take the full double (frsqrte reads all of f1).
extern "C" void func_80501490(CpuContext* ctx);
static void FrSqrtPort(CpuContext* p) {
    p->fpr[1].d = FrSqrt(p->fpr[1].d);
}

static double FrSqrtChecked(uint32_t r1, double f1) {
    MH3_FLOAT_CHECK(FrSqrt, 80501490, c->fpr[1].d = f1)
    return FrSqrt(f1);
}

extern "C" void MH3_FrSqrt(CpuContext* ctx) {
    ctx->fpr[1].d = FrSqrtChecked(ctx->gpr[1], ctx->fpr[1].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x80501490, MH3_FrSqrt, "FrSqrt");

extern "C" void func_80050C24(CpuContext* ctx);
static void FSqrtPort(CpuContext* p) {
    p->fpr[1].d = FSqrt(p->fpr[1].d);
}

static double FSqrtChecked(uint32_t r1, double f1) {
    MH3_FLOAT_CHECK(FSqrt, 80050C24, c->fpr[1].d = f1)
    return FSqrt(f1);
}

extern "C" void MH3_FSqrt(CpuContext* ctx) {
    ctx->fpr[1].d = FSqrtChecked(ctx->gpr[1], ctx->fpr[1].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x80050C24, MH3_FSqrt, "FSqrt");

// r3 -> f1: the u16 conversions and the u16-angle sine and cosine.
#define MH3_U16_PORT(Addr, Name, Fn)                                                                                                                                                                   \
    extern "C" void func_##Addr(CpuContext* ctx);                                                                                                                                                      \
    static void Name##Port(CpuContext* p) {                                                                                                                                                            \
        p->fpr[1].d = Fn(p->gpr[3]);                                                                                                                                                                   \
    }                                                                                                                                                                                                  \
    static double Name##Checked(uint32_t r1, uint32_t r3) {                                                                                                                                            \
        MH3_FLOAT_CHECK(Name, Addr, c->gpr[3] = r3)                                                                                                                                                    \
        return Fn(r3);                                                                                                                                                                                 \
    }

static float U16ToFloat(uint32_t value) {
    return float(value & 0xFFFF);
}

static float U16ToFloatLoad(uint32_t address) {
    return float(MemoryInline::FlatRead16(address));
}

MH3_U16_PORT(80050248, U16ToFloat, U16ToFloat)
extern "C" void MH3_U16ToFloat(CpuContext* ctx) {
    ctx->fpr[1].d = U16ToFloatChecked(ctx->gpr[1], ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80050248, MH3_U16ToFloat, "U16ToFloat");

MH3_U16_PORT(800502A8, U16ToFloatLoad, U16ToFloatLoad)
extern "C" void MH3_U16ToFloatLoad(CpuContext* ctx) {
    ctx->fpr[1].d = U16ToFloatLoadChecked(ctx->gpr[1], ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800502A8, MH3_U16ToFloatLoad, "U16ToFloatLoad");

MH3_U16_PORT(800502B0, SinU16, SinU16)
extern "C" void MH3_SinU16(CpuContext* ctx) {
    ctx->fpr[1].d = SinU16Checked(ctx->gpr[1], ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800502B0, MH3_SinU16, "SinU16");

MH3_U16_PORT(80050218, CosU16, CosU16)
extern "C" void MH3_CosU16(CpuContext* ctx) {
    ctx->fpr[1].d = CosU16Checked(ctx->gpr[1], ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80050218, MH3_CosU16, "CosU16");

// (r3, r4) ports writing memory, checked with RunOutput.
#define MH3_STORE_PORT(Addr, Name, Call, Out, Bytes)                                                                                                                                                   \
    extern "C" void func_##Addr(CpuContext* ctx);                                                                                                                                                      \
    static void Name##Port(CpuContext* p) {                                                                                                                                                            \
        [[maybe_unused]] const uint32_t r3 = p->gpr[3], r4 = p->gpr[4];                                                                                                                                \
        Call;                                                                                                                                                                                          \
    }                                                                                                                                                                                                  \
    static void Name##Checked(uint32_t r1, uint32_t r3, uint32_t r4) {                                                                                                                                 \
        MH3_OUTPUT_CHECK(Name, Addr, Out, Bytes, (c->gpr[3] = r3, c->gpr[4] = r4))                                                                                                                     \
        Call;                                                                                                                                                                                          \
    }

MH3_STORE_PORT(80050278, U16ToFloatStore, WriteFloat(r4, U16ToFloatLoad(r3)), r4, 4)
extern "C" void MH3_U16ToFloatStore(CpuContext* ctx) {
    U16ToFloatStoreChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80050278, MH3_U16ToFloatStore, "U16ToFloatStore");

MH3_STORE_PORT(80050130, RotMatrixX, RotMatrixX(r3, r4), r4, 0x30)
extern "C" void MH3_RotMatrixX(CpuContext* ctx) {
    RotMatrixXChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80050130, MH3_RotMatrixX, "RotMatrixX");

MH3_STORE_PORT(800502E0, RotMatrixY, RotMatrixY(r3, r4), r4, 0x30)
extern "C" void MH3_RotMatrixY(CpuContext* ctx) {
    RotMatrixYChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800502E0, MH3_RotMatrixY, "RotMatrixY");

MH3_STORE_PORT(800503C8, RotMatrixZ, RotMatrixZ(r3, r4), r4, 0x30)
extern "C" void MH3_RotMatrixZ(CpuContext* ctx) {
    RotMatrixZChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800503C8, MH3_RotMatrixZ, "RotMatrixZ");

MH3_STORE_PORT(804C71E0, PSVECNormalize, VecNormalize(r3, r4), r4, 12)
extern "C" void MH3_PSVECNormalize(CpuContext* ctx) {
    PSVECNormalizeChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x804C71E0, MH3_PSVECNormalize, "PSVECNormalize");

MH3_STORE_PORT(800508B4, VecNormalizeTo, VecNormalize(r4, r3), r3, 12)
extern "C" void MH3_VecNormalizeTo(CpuContext* ctx) {
    VecNormalizeToChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800508B4, MH3_VecNormalizeTo, "VecNormalizeTo");

MH3_STORE_PORT(800509F4, CpSetRotMatrixZXY, ZXY(r3, r4), r4, 0x30)
extern "C" void MH3_CpSetRotMatrixZXY(CpuContext* ctx) {
    CpSetRotMatrixZXYChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800509F4, MH3_CpSetRotMatrixZXY, "CpSetRotMatrixZXY");

MH3_STORE_PORT(80050914, CpSetRotMatrix, RotMatrixXYZ(r3, r4), r4, 0x30)
extern "C" void MH3_CpSetRotMatrix(CpuContext* ctx) {
    CpSetRotMatrixChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80050914, MH3_CpSetRotMatrix, "CpSetRotMatrix");

// (out r3, m r4, v r5).
extern "C" void func_80501C48(CpuContext* ctx);
static void MultVecSRPort(CpuContext* p) {
    MultVecSR(p->gpr[3], p->gpr[4], p->gpr[5]);
}

static void MultVecSRChecked([[maybe_unused]] uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("MTX34MultVecSR")) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        c->gpr[5] = r5;
        PortCheck::RunOutput(c, "MTX34MultVecSR", func_80501C48, MultVecSRPort, {{r3, 12}});
        return;
    }
#endif
    MultVecSR(r3, r4, r5);
}

extern "C" void MH3_MTX34MultVecSR(CpuContext* ctx) {
    MultVecSRChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80501C48, MH3_MTX34MultVecSR, "MTX34MultVecSR");

// VectorScaleWrapper (r3 out, r4 v, f1 s): the lanes times s, in float.
extern "C" void func_80051F44(CpuContext* ctx);
static void VecScale(uint32_t out, uint32_t v, float s) {
    const float x = ReadFloat(v) * s, y = ReadFloat(v + 4) * s, z = ReadFloat(v + 8) * s;
    WriteFloat(out, x);
    WriteFloat(out + 4, y);
    WriteFloat(out + 8, z);
}

static void VecScalePort(CpuContext* p) {
    VecScale(p->gpr[3], p->gpr[4], float(p->fpr[1].d));
}

static void VecScaleChecked([[maybe_unused]] uint32_t r1, uint32_t r3, uint32_t r4, double f1) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("VectorScaleWrapper")) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        c->fpr[1].d = f1;
        PortCheck::RunOutput(c, "VectorScaleWrapper", func_80051F44, VecScalePort, {{r3, 12}});
        return;
    }
#endif
    VecScale(r3, r4, float(f1));
}

extern "C" void MH3_VectorScaleWrapper(CpuContext* ctx) {
    VecScaleChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->fpr[1].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x80051F44, MH3_VectorScaleWrapper, "VectorScaleWrapper");
