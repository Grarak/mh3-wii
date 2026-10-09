// GlareFilter (MH3's bloom), RMHP08. C reconstruction of GlareFilter::draw
// (0x80056F88, vtable slot at 0x8001F4B4) and GlareFilter::brightPass
// (0x80057354), decompiled with Ghidra (scripts/dol2elf.py -> ELF import).
//
// The game's draw():
//   1. if the game is not paused and the capture is armed, SceneCapture::capture
//      (0x8005876C) copies the 640x448 EFB into sceneCapture+0x134;
//   2. brightPass(): draws `levels` successively halved downscales of the scene
//      into the EFB corner (GXCopyTex with mip box filter), waits in GXDrawDone,
//      rewrites every pixel of the smallest level through GXPeekARGB/GXPokeARGB
//      (luma = (77R + 150G + 29B) >> 8, darkened below +0x28 by +0x30, brightened
//      above +0x2C by +0x34, clamped, >> +0x18) and copies it to the glare texture;
//   3. SceneCapture::drawBack (0x80058870) redraws the captured scene over the
//      corner it scribbled on;
//   4. composites the glare texture over the frame with the filter's blend mode.
//
// Bloom is always off here: the CPU pixel pass is a mid-frame GPU stall plus
// ~9k EFB accesses per frame, and its GXDrawDone would stop frameskip. The
// composite quad is dropped too. Every other step still runs, because they
// leave GX state behind (VAT 0 = s16 pos / rgba8 colour / f32 uv, vertex
// descriptors, Z mode, cull, one TEV stage, channel control, projection, copy
// filter) that later game draws inherit; skipping the whole function (clearing
// `enabled`) breaks unrelated meshes and UI.
#include "abi_bridge.h"
#include "memory.h"
#include <initializer_list>

namespace {

// GlareFilter layout
enum : uint32_t {
    kEnabled = 0x00,      // u8
    kBlendSrc = 0x05,     // u8 GXBlendFactor
    kBlendDst = 0x06,     // u8 GXBlendFactor
    kTexture = 0x08,      // u32 glare texture (RGBA8)
    kLevels = 0x10,       // s32 downscale passes
    kTexWidth = 0x14,     // u16 result size
    kTexHeight = 0x16,    // u16
    kStartWidth = 0x1C,   // s16 first downscale size
    kStartHeight = 0x1E,  // s16
};

constexpr uint32_t kSceneCapture = 0x8066D178;
constexpr uint32_t kSceneCaptureArmed = kSceneCapture + 0x11C;
constexpr uint32_t kSmallData2 = 0x8079FF60; // r2
constexpr uint32_t kZero = kSmallData2 - 0x7E58; // f32 0.0
constexpr uint32_t kOne = kSmallData2 - 0x7E54;  // f32 1.0

// Game / SDK functions called by the glare filter.
enum : uint32_t {
    IsPaused = 0x80046F70,             // DAT_8065aa90 < 0
    SceneCapture_capture = 0x8005876C,
    SceneCapture_drawBack = 0x80058870,
    GetScreenOrigin = 0x8005854C,      // vec2 from 0x8066D298
    GetScreenSize = 0x8005853C,        // vec2 from 0x8066D2A0
    SetTexCoordGen = 0x80057348,       // inline GXSetTexCoordGen wrapper
    GXPosition2s16 = 0x80057330,       // inline FIFO writes
    GXColor4u8 = 0x80057308,
    GXTexCoord2f32 = 0x800572F8,
    GXEnd = 0x800572F4,
    GXSetVtxDesc = 0x804B54C0,
    GXClearVtxDesc = 0x804B5AD0,
    GXSetVtxAttrFmt = 0x804B5B10,
    GXSetNumTexGens = 0x804B6400,
    GXPixModeSync = 0x804B68B0,
    GXBegin = 0x804B6EF0,
    GXSetCullMode = 0x804B71E0,
    GXSetTexCopySrc = 0x804B7420,
    GXSetTexCopyDst = 0x804B7490,
    GXSetCopyFilter = 0x804B7970,
    GXCopyTex = 0x804B7CC0,
    GXSetNumChans = 0x804B8500,
    GXSetChanCtrl = 0x804B8530,
    GXInitTexObj = 0x804B86A0,
    GXInitTexObjLOD = 0x804B8900,
    GXLoadTexObj = 0x804B8DA0,
    GXInvalidateTexAll = 0x804B8FA0,
    GXSetTevDirect = 0x804B9920,
    GXSetTevOp = 0x804B99E0,
    GXSetTevOrder = 0x804B9F20,
    GXSetNumTevStages = 0x804BA080,
    GXSetBlendMode = 0x804BA550,
    GXSetZMode = 0x804BA600,
    GXSetProjection = 0x804BAA90,
    GXLoadPosMtxImm = 0x804BAB90,
    GXSetCurrentMtx = 0x804BACB0,
    PSMTXIdentity = 0x804C6290,
    C_MTXOrtho = 0x804C7110,
    DCInvalidateRange = 0x804CCC90,
};

// Calls into guest code with integer args in r3.. and float args in f1..
struct Guest {
    CpuContext* cpu;
    uint32_t operator()(uint32_t fn, std::initializer_list<uint32_t> ints = {},
                        std::initializer_list<double> floats = {}) {
        uint32_t r = 3, f = 1;
        for (uint32_t v : ints) cpu->gpr[r++] = v;
        for (double v : floats) cpu->fpr[f++].d = v;
        InvokeIndirectCpu(fn, cpu);
        return cpu->gpr[3];
    }
};

// Guest stack scratch shared by draw() and brightPass().
struct Frame {
    uint32_t texObj, viewMtx, projMtx, vec;
};

// The quad state block brightPass, drawBack and the composite all set up.
void SetupQuadState(Guest& gx, const Frame& fr, float right, float bottom) {
    const float zero = Memory::ReadFloat32(kZero), one = Memory::ReadFloat32(kOne);
    gx(GXInitTexObjLOD, {fr.texObj, 1, 1, 0, 0, 0}, {zero, zero, zero});
    gx(C_MTXOrtho, {fr.projMtx}, {zero, bottom, zero, right, zero, one});
    gx(GXSetProjection, {fr.projMtx, 1});
    gx(PSMTXIdentity, {fr.viewMtx});
    gx(GXLoadPosMtxImm, {fr.viewMtx, 0});
    gx(GXSetCurrentMtx, {0});
    gx(GXSetCullMode, {0});
    gx(GXSetZMode, {1, 7, 0});
    gx(GXSetNumTexGens, {1});
    gx(SetTexCoordGen, {0, 1, 4, 0x3C});
    gx(GXLoadTexObj, {fr.texObj, 0});
    gx(GXSetNumTevStages, {1});
    gx(GXSetTevDirect, {0});
    gx(GXSetTevOp, {0, 0});
    gx(GXSetTevOrder, {0, 0, 0, 4});
    gx(GXSetNumChans, {1});
    gx(GXSetChanCtrl, {4, 0, 0, 1, 0, 0, 2});
    gx(GXClearVtxDesc);
    gx(GXSetVtxDesc, {9, 1});
    gx(GXSetVtxDesc, {0xB, 1});
    gx(GXSetVtxDesc, {0xD, 1});
    gx(GXSetVtxAttrFmt, {0, 9, 0, 3, 0});
    gx(GXSetVtxAttrFmt, {0, 0xB, 1, 5, 0});
    gx(GXSetVtxAttrFmt, {0, 0xD, 1, 4, 0});
}

void DrawQuad(Guest& gx, int16_t w, int16_t h) {
    const float zero = Memory::ReadFloat32(kZero), one = Memory::ReadFloat32(kOne);
    const struct { int16_t x, y; float u, v; } corners[] = {
        {0, 0, zero, zero}, {w, 0, one, zero}, {w, h, one, one}, {0, h, zero, one}};
    gx(GXBegin, {0x80, 0, 4});
    for (const auto& c : corners) {
        gx(GXPosition2s16, {uint32_t(c.x), uint32_t(c.y)});
        gx(GXColor4u8, {0xFF, 0xFF, 0xFF, 0xFF});
        gx(GXTexCoord2f32, {}, {c.u, c.v});
    }
    gx(GXEnd);
}

void BrightPass(Guest& gx, const Frame& fr, uint32_t self) {
    gx(GetScreenOrigin, {fr.vec});
    const float screenW = Memory::ReadFloat32(fr.vec), screenH = Memory::ReadFloat32(fr.vec + 4);
    const uint32_t texture = Memory::Read32(self + kTexture);
    int16_t w = int16_t(Memory::Read16(self + kStartWidth));
    int16_t h = int16_t(Memory::Read16(self + kStartHeight));
    uint16_t srcW = uint16_t(w << 1), srcH = uint16_t(h << 1);
    uint16_t dstW = uint16_t(w), dstH = uint16_t(h);

    // Each level box-filters the previous one (EFB 2w x 2h -> w x h texture)
    // and draws it back into the EFB corner for the next level.
    gx(GXSetCopyFilter, {0, 0, 0, 0});
    const int32_t levels = int32_t(Memory::Read32(self + kLevels));
    for (int32_t level = 0; level < levels; ++level) {
        gx(GXSetTexCopySrc, {0, 0, srcW, srcH});
        gx(GXSetTexCopyDst, {dstW, dstH, 6, 1});
        gx(DCInvalidateRange, {texture, uint32_t(dstW) * dstH * 4});
        gx(GXCopyTex, {texture, 0});
        gx(GXPixModeSync);
        gx(GXInvalidateTexAll);
        gx(GXInitTexObj, {fr.texObj, texture, dstW, dstH, 6, 0, 0, 0});
        SetupQuadState(gx, fr, screenW, screenH);
        gx(GXSetBlendMode, {1, 1, 0, 0});
        DrawQuad(gx, w, h);
        srcW >>= 1, srcH >>= 1, dstW >>= 1, dstH >>= 1;
        w /= 2, h /= 2;
    }

    // Skipped: GXDrawDone and the CPU threshold pass over the smallest level.
    gx(GXSetTexCopySrc, {0, 0, srcW, srcH});
    gx(GXSetTexCopyDst, {srcW, srcH, 6, 0});
    gx(DCInvalidateRange, {texture, uint32_t(srcW) * srcH * 4});
    gx(GXCopyTex, {texture, 0});
    gx(GXInvalidateTexAll);
    const uint32_t renderMode = Memory::Read32(gx.cpu->gpr[13] - 0x46B8);
    gx(GXSetCopyFilter, {Memory::Read8(renderMode + 0x19), renderMode + 0x1A, 1, renderMode + 0x32});
    Memory::Write16(self + kTexWidth, srcW);
    Memory::Write16(self + kTexHeight, srcH);
}

} // namespace

extern "C" void MH3_GlareFilterDraw(CpuContext* ctx) {
    const CpuContext saved = *ctx;
    const uint32_t self = ctx->gpr[3];
    // Guest stack scratch below the caller's frame (keep the back chain aligned).
    const uint32_t sp = (saved.gpr[1] - 0x100) & ~0xFu;
    const Frame fr{sp + 0x10, sp + 0x30, sp + 0x60, sp + 0xA0};
    ctx->gpr[1] = sp;
    Memory::Write32(sp, saved.gpr[1]);
    Guest gx{ctx};

    if (gx(IsPaused) == 0 && Memory::Read8(kSceneCaptureArmed) != 0) {
        gx(SceneCapture_capture, {kSceneCapture, 0});
    }
    if (Memory::Read8(self + kEnabled) != 0) {
        BrightPass(gx, fr, self);
        gx(SceneCapture_drawBack, {kSceneCapture, 0});

        gx(GetScreenOrigin, {fr.vec});
        gx(GetScreenSize, {fr.vec});
        const float screenW = Memory::ReadFloat32(fr.vec), screenH = Memory::ReadFloat32(fr.vec + 4);
        gx(GXInitTexObj, {fr.texObj, Memory::Read32(self + kTexture), Memory::Read16(self + kTexWidth),
                          Memory::Read16(self + kTexHeight), 6, 0, 0, 0});
        SetupQuadState(gx, fr, screenW, screenH);
        gx(GXSetBlendMode, {1, Memory::Read8(self + kBlendSrc), Memory::Read8(self + kBlendDst), 0});
        // (The composite quad is not drawn.)
    }

    // Restore callee-saved state; draw() returns nothing.
    const uint32_t r3 = ctx->gpr[3];
    *ctx = saved;
    ctx->gpr[3] = r3;
}
REGISTER_NATIVE_FUNCTION_AS(0x80056F88, MH3_GlareFilterDraw, "GlareFilter::draw");
