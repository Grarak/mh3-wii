// NW4R g3d world-matrix bytecode, RMHP08. C reconstructions from the
// disassembly of calc_world.cpp's two passes over a model's nodes, 0x80073830
// (G3dCalcWorld) and 0x80074180 (G3dCalcSkinning). Both use the scratch
// buffer at 0x8066D300: CalcWorld's per-matrix scales (12 bytes each, the
// matrix ids it wrote at +0x6000), then Skinning's matrices.
//
// G3dCalcWorld(world, flags, code, base, mdl, anmObj, callback, rootFlags)
// runs the "NodeTree" byte code (looked up by name when code is 0):
//
//   01                      end
//   02 nn nn pp pp          node n under matrix p: its ChrAnmResult (from
//                           anmObj's virtual +0x38, or the node's static one),
//                           then world[m], scale[m], flags[m] from the
//                           parent's (the Maya segment-scale step when the
//                           model's scaling rule is 1), m the node's matrix id
//   06 dd dd ss ss          world/scale/flags[d] = those of s
//
// and finally applies each written matrix's scale when it is not (1, 1, 1).
//
// G3dCalcSkinning(out, flags, mdl, code) runs "NodeMix":
//
//   01                      end
//   03 dd dd nn [ss ss ww ww ww ww] * nn
//                           out[d] = sum of work[s] * w (w a float), and
//                           flags[d] = 0xF0000000 & each flags[s]
//   05 dd dd ii ii          work[d] = out[d] * node(i).invBind (node + 0xA0)
//
// then, by the model info's envelope matrix mode (u8 at info + 0x23), sets
// (1: | 0x30000000) or clears (2: & ~0x20000000) the flags of the view
// matrices past the node matrices. The original goes through a chain of
// debug-build accessors (each a call with a stack frame and an assert) per
// node; the ports read the resource directly and call the guest only for the
// animation, the callbacks and the rarely taken paths.
#include "abi_bridge.h"
#include "g3d_native.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include "runtime/native/adapters/model_pose_bridge.h"

#include <cstdint>
#include <cstring>
#include <utility>

#ifdef MH3_DEVELOPMENT
void MH3FrameSplitCall(unsigned kind); // game_frame.cpp
#endif
namespace {

constexpr uint32_t kWorkMatrices = 0x8066D300u;
constexpr uint32_t kScales = 0x8066D300u, kMatrixIds = 0x80673300u;

uint32_t Read32(uint32_t a) {
    return MemoryInline::FlatRead32(a);
}

uint32_t Read16(uint32_t a) {
    return MemoryInline::FlatRead16(a);
}

uint32_t Read8(uint32_t a) {
    return MemoryInline::FlatRead8(a);
}

float ReadFloat(uint32_t a) {
    const uint32_t bits = Read32(a);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

void WriteFloat(uint32_t a, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    MemoryInline::FlatWrite32(a, bits);
}

// The model's node dictionary (ResDic at data + *(data + 0x14)), 0 when none.
uint32_t NodeDic(uint32_t mdl) {
    const uint32_t data = Read32(mdl);
    const uint32_t offset = Read32(data + 0x14);
    return offset ? data + offset : 0;
}

void Skinning(uint32_t out, uint32_t flags, uint32_t mdl, uint32_t code) {
    for (uint8_t op; (op = Read8(code)) != 1;) {
        if (op == 3) {
            const uint32_t dst = Read16(code + 1), count = Read8(code + 3);
            code += 4;
            const uint32_t flagsAt = flags + 4 * dst;
            MemoryInline::FlatWrite32(flagsAt, 0xF0000000u);
            float sum[12];

            for (float& s : sum)
                s = 0.0f;

            for (uint32_t i = 0; i < count; ++i, code += 6) {
                const uint32_t src = Read16(code);
                const float weight = [&] {
                    const uint32_t bits = Read16(code + 2) << 16 | Read16(code + 4);
                    float f;
                    std::memcpy(&f, &bits, 4);
                    return f;
                }();
                const uint32_t m = kWorkMatrices + 0x30 * src;

                for (unsigned k = 0; k < 12; ++k)
                    sum[k] = ReadFloat(m + 4 * k) * weight + sum[k];

                MemoryInline::FlatWrite32(flagsAt, Read32(flagsAt) & Read32(flags + 4 * src));
            }

            for (unsigned k = 0; k < 12; ++k)
                WriteFloat(out + 0x30 * dst + 4 * k, sum[k]);
        } else {
            // 5 (anything else is an assert in the original).
            const uint32_t dst = Read16(code + 1);
            const uint32_t node = G3dNative::ResDicAtOffset14(mdl, Read16(code + 3));
            G3dNative::MtxConcat(out + 0x30 * dst, node + 0xA0, kWorkMatrices + 0x30 * dst);
            code += 5;
        }
    }

    const uint32_t info = Read32(mdl) + 0x4C;
    const uint32_t mode = Read8(info + 0x23);

    if (mode != 1 && mode != 2)
        return;

    const uint32_t dic = NodeDic(mdl);
    const uint32_t nodes = dic ? Read32(dic + 4) : 0;
    const uint32_t matrices = Read32(info + 0x1C);
    const uint32_t first = matrices - (Read32(info + Read32(info + 0x24)) - nodes);

    for (uint32_t i = first; i < matrices; ++i) {
        const uint32_t f = Read32(flags + 4 * i);
        MemoryInline::FlatWrite32(flags + 4 * i, mode == 1 ? f | 0x30000000u : f & ~0x20000000u);
    }
}

} // namespace

extern "C" void func_80074180(CpuContext* ctx);
extern "C" void func_80097DA4(CpuContext* ctx);

static void SkinningPort(CpuContext* ctx) {
    uint32_t code = ctx->gpr[6];

    if (code == 0) {
        // ResMdl::GetResByteCode(mdl, "NodeMix"): once per model, not per node.
        const uint32_t r1 = ctx->gpr[1], r3 = ctx->gpr[3], r4 = ctx->gpr[4], r5 = ctx->gpr[5];
        ctx->gpr[1] = r1 - 0xB0;
        MemoryInline::FlatWrite32(ctx->gpr[1], r1);
        ctx->gpr[3] = r5;
        ctx->gpr[4] = ctx->gpr[13] - 0x7C48;
        func_80097DA4(ctx);
        code = ctx->gpr[3];
        ctx->gpr[1] = r1;
        ctx->gpr[3] = r3;
        ctx->gpr[4] = r4;
        ctx->gpr[5] = r5;

        if (code == 0)
            return;
    }

    Skinning(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], code);
}

extern "C" void MH3_G3dCalcSkinning(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("G3dCalcSkinning");

    if (check) [[unlikely]] {
        PortCheck::Run(ctx, "G3dCalcSkinning", func_80074180, SkinningPort);
        return;
    }
#endif
    SkinningPort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80074180, MH3_G3dCalcSkinning, "G3dCalcSkinning");

extern "C" void func_80073830(CpuContext* ctx);
extern "C" void func_80098FD0(CpuContext* ctx);
extern "C" void func_800D7D88(CpuContext* ctx);
extern "C" void func_80073F64(CpuContext* ctx);
extern "C" void func_80073D98(CpuContext* ctx);
extern "C" void func_80073D44(CpuContext* ctx);
extern "C" void func_8008E224(CpuContext* ctx);
extern "C" void func_80098DC0(CpuContext* ctx);
extern "C" void func_80073FCC(CpuContext* ctx);
extern "C" void func_8008F1AC(CpuContext* ctx);

namespace {

// The original's frame (0xF0 bytes) and the locals the guest calls see.
constexpr uint32_t kFrame = 0xF0;
constexpr uint32_t kTemp8 = 0x08, kTempC = 0x0C, kTemp10 = 0x10, kNode = 0x1C;
constexpr uint32_t kVecA = 0x20, kVecB = 0x2C, kResult = 0x38, kResultBytes = 0x4C;

void CalcWorld(CpuContext* ctx) {
    GuestFrame frame(ctx, kFrame);
    const uint32_t world = frame.Arg(3), flags = frame.Arg(4), base = frame.Arg(6), mdl = frame.Arg(7);
    const uint32_t anmObj = frame.Arg(8), callback = frame.Arg(9), rootFlags = frame.Arg(10);
    auto call = [&](void (*fn)(CpuContext*), std::initializer_list<uint32_t> ints) {
        uint32_t r = 3;

        for (uint32_t v : ints)
            ctx->gpr[r++] = v;

        fn(ctx);
        return ctx->gpr[3];
    };

    uint32_t code = frame.Arg(5);

    if (code == 0 && (code = call(func_80097DA4, {mdl, 0x8058E938u})) == 0)
        return; // "NodeTree"
    const uint32_t info = Read32(mdl) + 0x4C;
    const bool maya = Read32(info + 8) == 1;
    const bool segmentScale = (rootFlags & 0x08000000u) != 0;
    const float one = ReadFloat(0x80798280u);

    for (uint32_t k = 0; k < 12; k += 4)
        WriteFloat(kScales + k, one);

    G3dNative::MtxCopy(base, world);
    MemoryInline::FlatWrite32(flags, rootFlags);

    const uint32_t result = frame[kResult];
    uint32_t count = 0;

    for (uint8_t op; (op = Read8(code)) != 1; code += 5) {
        const uint32_t a = Read16(code + 1), b = Read16(code + 3);

        if (op != 2) {
            // 6 (anything else is an assert in the original).
            MemoryInline::FlatWrite32(kMatrixIds + 4 * count++, a);
            MemoryInline::FlatWrite32(flags + 4 * a, Read32(flags + 4 * b));
            G3dNative::MtxCopy(world + 0x30 * b, world + 0x30 * a);

            for (uint32_t k = 0; k < 12; k += 4)
                MemoryInline::FlatWrite32(kScales + 12 * a + k, Read32(kScales + 12 * b + k));

            continue;
        }

        const uint32_t nodeIndex = a, parent = b;
        const uint32_t node = G3dNative::ResDicAtOffset14(mdl, nodeIndex);
        MemoryInline::FlatWrite32(frame[kNode], node);
        const uint32_t id = node ? Read32(node + 0x10) : 0;
        MemoryInline::FlatWrite32(kMatrixIds + 4 * count++, id);
        const uint32_t nodeWorld = world + 0x30 * id, nodeScale = kScales + 12 * id;
        const uint32_t parentFlags = Read32(flags + 4 * parent);

        uint32_t chr = 0;
        bool own = false;

        if (anmObj) {
            const uint32_t nodeId = node ? Read32(node + 0xC) : 0;
            GuestCall game{ctx};
            chr = game.Virtual<0x38>(anmObj, {result, nodeId});
        }

        if (!anmObj || Read32(chr) == 0) {
            call(func_80098FD0, {frame[kNode], result});
            chr = result;
            own = true;
        }

        auto toResult = [&] {
            if (chr == result)
                return;

            for (uint32_t k = 0; k < kResultBytes; k += 4)
                MemoryInline::FlatWrite32(result + k, Read32(chr + k));

            chr = result;
        };

        if (nodeIndex != 0 && segmentScale && !own) {
            toResult();
            call(func_8008E224, {result, frame[kVecB]});
            MemoryInline::FlatWrite32(result, Read32(result) | 0x200);
            call(func_80098DC0, {frame[kNode], result});
            call(func_8008E224, {result, frame[kVecA]});
            call(func_80073FCC, {frame[kVecA], frame[kVecB]});
            call(func_8008F1AC, {result, frame[kVecA]});
        }

        if (callback) {
            toResult();

            if (Read16(callback + 6) == nodeIndex && (Read8(callback + 4) & 1) != 0) {
                MemoryInline::FlatWrite32(frame[kTemp10], Read32(mdl));
                call(func_80073F64, {callback, nodeIndex, result, frame[kTemp10]});
            }
        }

        const uint32_t flagsAt = flags + 4 * id;
        const uint32_t stepped = maya ? G3dNative::WorldMtxNodeStepMayaScale(nodeWorld, nodeScale, world + 0x30 * parent, kScales + 12 * parent, parentFlags, chr)
                                      : call(func_800D7D88, {nodeWorld, nodeScale, world + 0x30 * parent, kScales + 12 * parent, parentFlags, chr});
        MemoryInline::FlatWrite32(flagsAt, (stepped & 0xFFFFFF00u) | (node ? Read32(node + 0x18) : 0));

        if (callback && Read16(callback + 6) == nodeIndex && (Read8(callback + 4) & 2) != 0) {
            MemoryInline::FlatWrite32(frame[kTempC], Read32(mdl));
            call(func_80073D98, {callback, nodeIndex, nodeWorld, nodeScale, flagsAt, frame[kTempC]});
        }
    }

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t id = Read32(kMatrixIds + 4 * i);
        const uint32_t s = kScales + 12 * id;

        if (ReadFloat(s) != one || ReadFloat(s + 4) != one || ReadFloat(s + 8) != one)
            G3dNative::MtxApplyScale(world + 0x30 * id, world + 0x30 * id, s);
    }

    if (callback && (Read8(callback + 4) & 4) != 0) {
        MemoryInline::FlatWrite32(frame[kTemp8], Read32(mdl));
        call(func_80073D44, {callback, world, frame[kTemp8]});
    }
}

} // namespace

extern "C" void MH3_G3dCalcWorld(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    MH3FrameSplitCall(0);
#endif
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("G3dCalcWorld");

    if (check) [[unlikely]] {
        PortCheck::Run(ctx, "G3dCalcWorld", func_80073830, CalcWorld);
        return;
    }
#endif
    CalcWorld(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80073830, MH3_G3dCalcWorld, "G3dCalcWorld");

// 0x800711FC (G3dCalcViewLc, calcview.cpp's CalcView_LC): the view matrices
// (view * model), their normal matrices and texture matrices, 40 at a time
// through the locked cache.
//
// G3dCalcViewLc(viewPos, nrm, model, attrib, count, view, mdl, tex): per
// chunk, view * model into LC buffer A; a billboard matrix (attrib & 0xFF, 1-6)
// through the table at 0x8056FDF8, keeping view * model * inverse(model) of a
// billboard that has children (node + 0x60) in the scratch at 0x8066D300; a
// node with flag 0x400 (under a billboard) is its billboard parent's scratch *
// its model matrix. A is DMA'd to viewPos. With nrm: per matrix the inverse
// transpose of A (or A itself when attrib bit 29 says it has no scale), the
// 3x3 into B for nrm and, with tex, the 3x4 with a zero translation into C for
// tex. Buffers A-C alternate with a second set between chunks. The DMA copies
// whole 32-byte blocks, as the runtime's locked-cache DMA does.
namespace {

constexpr uint32_t kLockedCache = 0xE0000000u;
constexpr uint32_t kBillboards = 0x8056FDF8u;

void LcStore(uint32_t dst, uint32_t cache, uint32_t blocks) {
    const uint32_t size = (blocks ? blocks : 128) * 32;
    const uint32_t destination = dst & 0x1FFFFFE0u;
    void* output = Memory::GetPointer(destination, size);
    const void* input = Memory::GetPointer(cache & ~31u, size);
    RetireNativeResourceOwner(destination, size, NativeSourceEpochProducer::ViewLc);
    std::memcpy(output, input, size);
}

// The bytes LcStore would overwrite, retired before the results are written
// there directly.
void LcRetire(uint32_t dst, uint32_t blocks) {
    RetireNativeResourceOwner(dst & 0x1FFFFFE0u, (blocks ? blocks : 128) * 32, NativeSourceEpochProducer::ViewLc);
}

// The node of matrix id: the model info's matrix-to-node table (-1 when none).
int32_t MatrixNode(uint32_t info, uint32_t id) {
    const uint32_t table = info + Read32(info + 0x24);
    return int32_t(Read32(table + 4 + 4 * id));
}

void CalcViewLc(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x100);
    const uint32_t viewPos = frame.Arg(3), nrm = frame.Arg(4), model = frame.Arg(5), attrib = frame.Arg(6);
    const uint32_t count = frame.Arg(7), view = frame.Arg(8), mdl = frame.Arg(9), tex = frame.Arg(10);
    const uint32_t r13 = frame.Arg(13);

    if (count == 0)
        return;

    uint32_t a = kLockedCache + 0x800, b = kLockedCache + 0x1000, c = kLockedCache + 0x1800;
    uint32_t a1 = kLockedCache + 0x2800, b1 = kLockedCache + 0x3000, c1 = kLockedCache + 0x3800;

    // The original builds each batch of 40 in the locked cache and DMAs it out
    // in 32-byte blocks. With 32-byte aligned outputs (as the DMA needs; the
    // batches stay aligned) the results go straight to them instead; only the
    // bytes past the last matrix of a batch, which the DMA fills with stale
    // cache contents, keep their old values.
    const bool direct = ((viewPos | nrm | tex) & 31) == 0;
    const uint32_t info = Read32(mdl) + 0x4C;
    const float zero = ReadFloat(0x80798268u);
    const uint32_t inverse = frame[0x58];
    GuestCall game{ctx};

    for (uint32_t first = 0; first < count; first += 40) {
        const uint32_t n = count - first > 40 ? 40 : count - first;
        const uint32_t blocksA = (n * 0x30 + 31) >> 5, blocksB = (n * 0x24 + 31) >> 5;

        if (direct) {
            a = viewPos + 0x30 * first;
            b = nrm + 0x24 * first;
            c = tex + 0x30 * first;
            LcRetire(a, blocksA);

            if (nrm) {
                LcRetire(b, blocksB);

                if (tex)
                    LcRetire(c, blocksA);
            }
        }

        if (n > 1)
            G3dNative::MtxConcatArray(view, model + 0x30 * first, a, n, r13);
        else
            G3dNative::MtxConcat(view, model + 0x30 * first, a);

        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t id = first + k, attr = Read32(attrib + 4 * id), mtx = a + 0x30 * k;
            const uint32_t billboard = attr & 0xFF;

            if (billboard != 0) {
                MemoryInline::FlatWrite32(frame[0x30], Read32(mdl));
                game(Read32(kBillboards + 4 * billboard), {mtx, model, (attr >> 29) & 1, view, frame[0x30], id});
                const uint32_t node = G3dNative::ResDicAtOffset14(mdl, uint32_t(MatrixNode(info, id)));

                if (node == 0 || Read32(node + 0x60) == 0)
                    continue;
                // A billboard with children: what they are relative to.
                const uint32_t scratch = kWorkMatrices + 0x30 * id;

                if (G3dNative::MtxInv34(inverse, model + 0x30 * id) == 1) {
                    G3dNative::MtxConcat(mtx, inverse, scratch);
                } else {
                    for (unsigned w = 0; w < 12; ++w)
                        WriteFloat(scratch + 4 * w, w % 5 == 0 ? 1.0f : 0.0f);

                    for (uint32_t t = 0xC; t < 0x30; t += 0x10)
                        MemoryInline::FlatWrite32(scratch + t, Read32(view + t));
                }

                continue;
            }

            const int32_t nodeId = MatrixNode(info, id);

            if (nodeId < 0)
                continue;

            const uint32_t node = G3dNative::ResDicAtOffset14(mdl, uint32_t(nodeId));

            if (node == 0 || (Read32(node + 0x14) & 0x400) == 0)
                continue;

            const uint32_t parent = G3dNative::ResDicAtOffset14(mdl, Read32(node + 0x1C));
            const uint32_t parentId = parent ? Read32(parent + 0x10) : 0;
            G3dNative::MtxConcat(kWorkMatrices + 0x30 * parentId, model + 0x30 * id, mtx);
        }

        if (!direct)
            LcStore(viewPos + 0x30 * first, a, blocksA);

        if (nrm) {
            for (uint32_t k = 0; k < n; ++k) {
                const uint32_t id = first + k, mtx = a + 0x30 * k, n33 = b + 0x24 * k, t34 = c + 0x30 * k;

                if ((Read32(attrib + 4 * id) & 0x20000000u) != 0) {
                    if (tex) {
                        G3dNative::MtxCopy(mtx, t34);

                        for (uint32_t t = 0xC; t < 0x30; t += 0x10)
                            WriteFloat(t34 + t, zero);
                    }

                    G3dNative::MtxTo33(n33, mtx);
                } else if (tex) {
                    G3dNative::MtxInvTranspose34(t34, mtx);
                    G3dNative::MtxTo33(n33, t34);
                } else {
                    G3dNative::MtxInvTranspose33(n33, mtx);
                }
            }

            if (!direct) {
                LcStore(nrm + 0x24 * first, b, blocksB);

                if (tex)
                    LcStore(tex + 0x30 * first, c, blocksA);
            }
        }

        std::swap(a, a1);
        std::swap(b, b1);
        std::swap(c, c1);
    }
}

} // namespace

extern "C" void func_800711FC(CpuContext* ctx);
extern "C" void MH3_G3dCalcViewLc(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    MH3FrameSplitCall(3);
#endif
    const uint32_t position = ctx->gpr[3], normal = ctx->gpr[4], environment = ctx->gpr[10];
    const uint32_t count = ctx->gpr[7], model_reference = ctx->gpr[9];
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("G3dCalcViewLc");

    if (check) [[unlikely]] {
        PortCheck::Run(ctx, "G3dCalcViewLc", func_800711FC, CalcViewLc);
        PublishNativeModelPose(position, normal, environment, count, model_reference);
        return;
    }
#endif
    CalcViewLc(ctx);
    PublishNativeModelPose(position, normal, environment, count, model_reference);
}
REGISTER_NATIVE_FUNCTION_AS(0x800711FC, MH3_G3dCalcViewLc, "G3dCalcViewLc");
