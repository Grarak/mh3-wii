// NW4R g3d world-matrix bytecode, RMHP08. C reconstructions from the
// disassembly of calc_world.cpp's two passes over a model's nodes, 0x80073830
// (G3dCalcWorld) and 0x80074180 (G3dCalcSkinning). Both use the scratch
// buffer at 0x8066D300 (the guest worker its own copy): CalcWorld's
// per-matrix scales (12 bytes each, the matrix ids it wrote at +0x6000), then
// Skinning's matrices.
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
#include "guest_worker.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include "runtime/native/adapters/model_pose_bridge.h"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#ifdef MH3_DEVELOPMENT
void MH3FrameSplitCall(unsigned kind); // game_frame.cpp
#endif
namespace {

// The passes' scratch (0x18000 bytes): the game thread's at 0x8066D300, the
// guest worker's and the draw thread's their own (G3dScratch, guest_worker.h),
// so models can calculate on all three. CalcWorld keeps the scales at its
// start and the matrix ids at +0x6000.
constexpr uint32_t kMatrixIdsOffset = 0x6000;

uint32_t WorkArea() {
    return G3dScratch();
}

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

// envelopes (GPU skinning, FindNativeModelNodeViews): when not null, only the
// envelopes it flags by matrix ID.
void Skinning(uint32_t out, uint32_t flags, uint32_t mdl, uint32_t code, const std::vector<uint8_t>* envelopes) {
    const uint32_t work = WorkArea();

    for (uint8_t op; (op = Read8(code)) != 1;) {
        if (op == 3) {
            const uint32_t dst = Read16(code + 1), count = Read8(code + 3);
            code += 4;

            if (envelopes && (dst >= envelopes->size() || !(*envelopes)[dst])) {
                code += 6 * count;
                continue;
            }

            const uint32_t flagsAt = flags + 4 * dst;
            uint32_t combined = 0xF0000000u;
#if defined(__ARM_NEON)
            // Three rows of four, byte-swapped as they load and store (NEON:
            // denormals flush to zero, the sums keep the scalar order).
            float32x4_t sum0 = vdupq_n_f32(0.0f), sum1 = sum0, sum2 = sum0;
#else
            float sum[12] = {};
#endif

            for (uint32_t i = 0; i < count; ++i, code += 6) {
                const uint32_t src = Read16(code);
                const float weight = [&] {
                    const uint32_t bits = Read16(code + 2) << 16 | Read16(code + 4);
                    float f;
                    std::memcpy(&f, &bits, 4);
                    return f;
                }();
                const uint32_t m = work + 0x30 * src;
#if defined(__ARM_NEON)
                // Multiply, then add: vmlaq is contracted to a fused FMA on
                // AArch64, which rounds differently from the original.
                const uint8_t* row = GuestFlat::Host(m);
                sum0 = vaddq_f32(sum0, vmulq_n_f32(vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(row))), weight));
                sum1 = vaddq_f32(sum1, vmulq_n_f32(vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(row + 16))), weight));
                sum2 = vaddq_f32(sum2, vmulq_n_f32(vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(row + 32))), weight));
#else
                for (unsigned k = 0; k < 12; ++k)
                    sum[k] = ReadFloat(m + 4 * k) * weight + sum[k];
#endif
                combined &= Read32(flags + 4 * src);
            }

            MemoryInline::FlatWrite32(flagsAt, combined);
#if defined(__ARM_NEON)
            uint8_t* result = GuestFlat::Host(out + 0x30 * dst);
            vst1q_u8(result, vrev32q_u8(vreinterpretq_u8_f32(sum0)));
            vst1q_u8(result + 16, vrev32q_u8(vreinterpretq_u8_f32(sum1)));
            vst1q_u8(result + 32, vrev32q_u8(vreinterpretq_u8_f32(sum2)));
#else
            for (unsigned k = 0; k < 12; ++k)
                WriteFloat(out + 0x30 * dst + 4 * k, sum[k]);
#endif
        } else {
            // 5 (anything else is an assert in the original).
            const uint32_t dst = Read16(code + 1);
            const uint32_t node = G3dNative::ResDicAtOffset14(mdl, Read16(code + 3));
            G3dNative::MtxConcat(out + 0x30 * dst, node + 0xA0, work + 0x30 * dst);
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

static void SkinningFor(CpuContext* ctx, const std::vector<uint8_t>* envelopes) {
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

    Skinning(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], code, envelopes);
}

static void SkinningPort(CpuContext* ctx) {
    SkinningFor(ctx, nullptr);
}

extern "C" void MH3_G3dCalcSkinning(CpuContext* ctx) {
    // GPU skinning (docs/gpu-skinning.md): a model whose shapes read no
    // envelopes per vertex needs only those its rigid shapes are drawn with.
    if (const auto views = FindNativeModelNodeViews(Read32(ctx->gpr[5]))) {
        if (!views->envelopes.empty())
            SkinningFor(ctx, &views->envelopes);

        return;
    }
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
extern "C" void func_800D7D88(CpuContext* ctx);
extern "C" void func_80073D44(CpuContext* ctx);

namespace {

// The original's frame (0xF0 bytes) and the locals the guest calls see.
constexpr uint32_t kFrame = 0xF0;
constexpr uint32_t kTemp8 = 0x08, kTempC = 0x0C, kTemp10 = 0x10, kNode = 0x1C;
constexpr uint32_t kVecA = 0x20, kVecB = 0x2C, kResult = 0x38, kResultBytes = 0x4C;
constexpr uint32_t kCalcArgs = 0x90; // the CalcWorldCallbackArgs of the 0x2 callback

// PSVECAdd (0x80073FCC's): a += b, float lanes.
void AddVec3(uint32_t a, uint32_t b) {
    for (uint32_t k = 0; k < 12; k += 4)
        WriteFloat(a + k, ReadFloat(a + k) + ReadFloat(b + k));
}

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
    const uint32_t scales = WorkArea(), ids = scales + kMatrixIdsOffset;
    const uint32_t info = Read32(mdl) + 0x4C;
    const bool maya = Read32(info + 8) == 1;
    const bool segmentScale = (rootFlags & 0x08000000u) != 0;
    const float one = ReadFloat(0x80798280u);

    for (uint32_t k = 0; k < 12; k += 4)
        WriteFloat(scales + k, one);

    G3dNative::MtxCopy(base, world);
    MemoryInline::FlatWrite32(flags, rootFlags);

    const uint32_t result = frame[kResult];
    uint32_t count = 0;

    for (uint8_t op; (op = Read8(code)) != 1; code += 5) {
        const uint32_t a = Read16(code + 1), b = Read16(code + 3);

        if (op != 2) {
            // 6 (anything else is an assert in the original).
            MemoryInline::FlatWrite32(ids + 4 * count++, a);
            MemoryInline::FlatWrite32(flags + 4 * a, Read32(flags + 4 * b));
            G3dNative::MtxCopy(world + 0x30 * b, world + 0x30 * a);

            for (uint32_t k = 0; k < 12; k += 4)
                MemoryInline::FlatWrite32(scales + 12 * a + k, Read32(scales + 12 * b + k));

            continue;
        }

        const uint32_t nodeIndex = a, parent = b;
        const uint32_t node = G3dNative::ResDicAtOffset14(mdl, nodeIndex);
        MemoryInline::FlatWrite32(frame[kNode], node);
        const uint32_t id = node ? Read32(node + 0x10) : 0;
        MemoryInline::FlatWrite32(ids + 4 * count++, id);
        const uint32_t nodeWorld = world + 0x30 * id, nodeScale = scales + 12 * id;
        const uint32_t parentFlags = Read32(flags + 4 * parent);

        uint32_t chr = 0;
        bool own = false;

        if (anmObj) {
            const uint32_t nodeId = node ? Read32(node + 0xC) : 0;
            GuestCall game{ctx};
            chr = game.Virtual<0x38>(anmObj, {result, nodeId});
        }

        if (!anmObj || Read32(chr) == 0) {
            G3dNative::ResNodeChrAnmResult(frame[kNode], result);
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
            // The animated translation plus the node's own (0x200 asks
            // ResNodePatchChrAnmResult for it).
            toResult();
            G3dNative::ChrAnmResultGetTranslation(result, frame[kVecB]);
            MemoryInline::FlatWrite32(result, Read32(result) | 0x200);
            G3dNative::ResNodePatchChrAnmResult(frame[kNode], result);
            G3dNative::ChrAnmResultGetTranslation(result, frame[kVecA]);
            AddVec3(frame[kVecA], frame[kVecB]);
            G3dNative::ChrAnmResultSetTranslation(result, frame[kVecA]);
        }

        if (callback) {
            toResult();

            // 0x80073F64: the callback's ExecCallbackA (vtable +0xC) with the
            // node's ChrAnmResult before its world matrix.
            if (Read16(callback + 6) == nodeIndex && (Read8(callback + 4) & 1) != 0) {
                MemoryInline::FlatWrite32(frame[kTemp10], Read32(mdl));
                GuestCall game{ctx};
                game.Virtual<0xC>(Read32(callback), {result, frame[kTemp10], callback});
            }
        }

        const uint32_t flagsAt = flags + 4 * id;
        const uint32_t stepped = maya ? G3dNative::WorldMtxNodeStepMayaScale(nodeWorld, nodeScale, world + 0x30 * parent, scales + 12 * parent, parentFlags, chr)
                                      : call(func_800D7D88, {nodeWorld, nodeScale, world + 0x30 * parent, scales + 12 * parent, parentFlags, chr});
        MemoryInline::FlatWrite32(flagsAt, (stepped & 0xFFFFFF00u) | (node ? Read32(node + 0x18) : 0));

        // 0x80073D98: ExecCallbackB (vtable +0x10) with {world, scale,
        // flags} of the node's matrix after it.
        if (callback && Read16(callback + 6) == nodeIndex && (Read8(callback + 4) & 2) != 0) {
            MemoryInline::FlatWrite32(frame[kTempC], Read32(mdl));
            MemoryInline::FlatWrite32(frame[kCalcArgs], nodeWorld);
            MemoryInline::FlatWrite32(frame[kCalcArgs + 4], nodeScale);
            MemoryInline::FlatWrite32(frame[kCalcArgs + 8], flagsAt);
            GuestCall game{ctx};
            game.Virtual<0x10>(Read32(callback), {frame[kCalcArgs], frame[kTempC], callback});
        }
    }

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t id = Read32(ids + 4 * i);
        const uint32_t s = scales + 12 * id;

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

// The blocks LcStore would overwrite for an output array of `bytes`, retired
// at once before the results are written there directly (the chunks' blocks
// together, one catalog call instead of one per chunk).
void LcRetire(uint32_t dst, uint32_t bytes) {
    const uint32_t start = dst & 0x1FFFFFE0u, end = ((dst & 0x1FFFFFFFu) + bytes + 31) & ~31u;
    RetireNativeResourceOwner(start, end - start, NativeSourceEpochProducer::ViewLc);
}

// The node of matrix id: the model info's matrix-to-node table (-1 when none).
int32_t MatrixNode(uint32_t info, uint32_t id) {
    const uint32_t table = info + Read32(info + 0x24);
    return int32_t(Read32(table + 4 + 4 * id));
}

// CalcView_LC, or with lockedCache false the plain CalcView (0x80070884,
// the same arguments), which writes every result in place.
void CalcView(CpuContext* ctx, bool lockedCache) {
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
    const bool direct = !lockedCache || ((viewPos | nrm | tex) & 31) == 0;
    const uint32_t info = Read32(mdl) + 0x4C;
    const float zero = ReadFloat(0x80798268u);
    const uint32_t inverse = frame[0x58];
    const uint32_t work = WorkArea();
    GuestCall game{ctx};

    // GPU skinning (docs/gpu-skinning.md): a model whose shapes read no
    // envelopes per vertex gets only the views its draws read. Its outputs
    // retire as a whole pass's would.
    const auto views = direct ? FindNativeModelNodeViews(Read32(mdl)) : nullptr;

    if (direct) {
        LcRetire(viewPos, count * 0x30);

        if (nrm) {
            LcRetire(nrm, count * 0x24);

            if (tex)
                LcRetire(tex, count * 0x30);
        }
    }

    const auto chunk = [&](uint32_t first, uint32_t n) {
        const uint32_t blocksA = (n * 0x30 + 31) >> 5, blocksB = (n * 0x24 + 31) >> 5;

        if (direct) {
            a = viewPos + 0x30 * first;
            b = nrm + 0x24 * first;
            c = tex + 0x30 * first;
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
                const uint32_t scratch = work + 0x30 * id;

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
            G3dNative::MtxConcat(work + 0x30 * parentId, model + 0x30 * id, mtx);
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
    };

    if (!views) {
        for (uint32_t first = 0; first < count; first += 40)
            chunk(first, count - first > 40 ? 40 : count - first);

        return;
    }

    for (const auto& [begin, end] : views->runs) {
        const uint32_t last = end < count ? end : count;

        for (uint32_t first = begin; first < last; first += 40)
            chunk(first, last - first > 40 ? 40 : last - first);
    }
}

void CalcViewLc(CpuContext* ctx) {
    CalcView(ctx, true);
}

void CalcViewPlain(CpuContext* ctx) {
    CalcView(ctx, false);
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
    const uint32_t world = ctx->gpr[5], camera = ctx->gpr[8];
    CalcViewLc(ctx);
    PublishNativeModelPose(position, normal, environment, count, model_reference);
    PublishSkinView(position, world, camera, count, model_reference);
}
REGISTER_NATIVE_FUNCTION_AS(0x800711FC, MH3_G3dCalcViewLc, "G3dCalcViewLc");

// 0x80070884, the plain CalcView, which 0x8007FADC (ScnMdlSimple's view
// task) runs when the locked cache is not granted, as it never is now.
extern "C" void func_80070884(CpuContext* ctx);
extern "C" void MH3_G3dCalcViewPlain(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    MH3FrameSplitCall(3);
#endif
    const uint32_t position = ctx->gpr[3], normal = ctx->gpr[4], environment = ctx->gpr[10];
    const uint32_t count = ctx->gpr[7], model_reference = ctx->gpr[9];
    const uint32_t world = ctx->gpr[5], camera = ctx->gpr[8];
    PortCheck::Call(ctx, "G3dCalcViewPlain", func_80070884, CalcViewPlain);
    PublishNativeModelPose(position, normal, environment, count, model_reference);
    PublishSkinView(position, world, camera, count, model_reference);
}
REGISTER_NATIVE_FUNCTION_AS(0x80070884, MH3_G3dCalcViewPlain, "G3dCalcViewPlain");

// 0x80502DD0, g3d's locked-cache acquire: under the OS mutex at 0x8076313C,
// 1 (the mutex kept until 0x80502E40) while the byte at 0x80763138 says the
// cache is enabled. Never granted here: ScnMdlSimple's world, skinning and
// view passes (0x8007F480, 0x8007F7E0, 0x8007FADC) then work in main memory
// instead of staging matrices in the cache and copying them out, and the
// game thread and the guest worker, one guest thread to the mutex, cannot
// share the cache.
extern "C" void MH3_LockedCacheAcquire(CpuContext* ctx) {
    ctx->gpr[3] = 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80502DD0, MH3_LockedCacheAcquire, "LockedCacheAcquire");
