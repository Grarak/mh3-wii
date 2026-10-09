// NW4R g3d ScnMdl and ScnObj scene tasks and their material helpers, RMHP08.
// C reconstructions from the disassembly of the debug-build g3d_scnmdl.cpp
// and g3d_scnobj.cpp code the model draw dispatch (model_draw_dispatch.cpp)
// runs for every model each frame:
//
//   0x8007D600 ScnMdlCalcMat(mdl, arg, options): task 2. Per material, the
//              copies its dirty bits ask for (CopiedMatRefresh), then the
//              texture pattern (+0x130), texture SRT (+0x134) and colour
//              (+0x12C) animations of the animated materials
//   0x8007E4FC CopiedMatRefresh(mdl, mat, bits): the model's copies of the
//              material's parts named by `bits` refreshed from the resource,
//              then those bits cleared
//   0x8007BF10 CopiedMatAccess::CopiedMatAccess(this, mdl, mat): the
//              material's copies (null when the model has none)
//   0x80081A94 ScnObjG3dProc(obj, task, arg, options): the ScnObj default
//              for tasks 1 (CalcWorld), 2 (CalcMat), 4 (CalcView) and the
//              parent links 0x10002/0x10003
//   0x80069D58, 0x80069838, 0x80063EC4: AnmObjTexSrtRes, AnmObjTexPatRes and
//              AnmObjMatClrRes::IsMaterialAnimated(mat): their u16 material
//              flags (+0x14, count +0x10) have neither 0x4000 nor 0x8000
//
// The ScnMdl (scnmdl.h): +0xD4 the scene callback, u8 +0xD8 its timings, u16
// +0xDA its kinds (1 CalcWorld, 2 CalcMat, 4 CalcView), +0xE8 the ResMdl
// data, +0x12C/+0x130/+0x134 the colour, texture pattern and SRT animations,
// +0x140 the per-material dirty words, +0x14C..+0x174 the arrays of copied
// material parts (null when the model shares the resource's):
//
//   bit    copy (+offset, bytes each)   resource part        copy function
//   0x1    +0x14C 0x104 texture         ResMat +0x40         0x80094C00
//   0x2    +0x150 100   palette         ResMat +0x144        0x80095114
//   0x4    +0x154 0x248 texture SRT     ResMat +0x1A8        0x8009528C
//   0x8    +0x158 0x28  channel         ResMat +0x3F0        0x8009713C
//   0x10   +0x15C 8     misc            ResMat +0x14         0x800956FC
//   0x20   +0x160 0xC   fog             ResMat +0x1C         0x80095C28
//   0x80   +0x164 0x20  pixel           TEV block            0x800946D8
//   0x100  +0x168 0x80  TEV colours     TEV block +0x20      0x80094724
//   0x200  +0x16C 0x40  indirect        TEV block +0xA0      0x80094770
//   0x400  +0x170 0xA0  texgen          TEV block +0xE0      0x800947BC
//   0x800  +0x174 0x200 shape (ResMat +0x28 offset)          0x8009A2DC
//
// (the TEV block is ResMat + its offset at +0x3C). The originals reach each
// of these through a constructor and an accessor with a stack frame and an
// alignment assert; the ports compute the addresses and drop the debug
// asserts, calling the guest only for the copies, the animations' results
// and the callbacks.
#include "abi_bridge.h"
#include "fatal.h"
#include "g3d_native.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
#include "runtime/native/adapters/model_pose_bridge.h"

#include <cstdint>
#include <stdexcept>

namespace {

uint32_t Read32(uint32_t a) {
    return MemoryInline::FlatRead32(a);
}

void Write32(uint32_t a, uint32_t v) {
    MemoryInline::FlatWrite32(a, v);
}

constexpr uint32_t kCallback = 0xD4, kCallbackTiming = 0xD8, kCallbackKinds = 0xDA;
constexpr uint32_t kResMdl = 0xE8, kAnmClr = 0x12C, kAnmTexPat = 0x130, kAnmTexSrt = 0x134, kDirty = 0x140;

// The copies' arrays and element sizes, by dirty bit (the table above).
struct CopiedPart {
    uint32_t bit, array, size;
};

constexpr CopiedPart kParts[] = {
    {0x1, 0x14C, 0x104}, {0x2, 0x150, 100},   {0x4, 0x154, 0x248}, {0x8, 0x158, 0x28},   {0x10, 0x15C, 8},     {0x20, 0x160, 0xC},
    {0x80, 0x164, 0x20}, {0x100, 0x168, 0x80}, {0x200, 0x16C, 0x40}, {0x400, 0x170, 0xA0}, {0x800, 0x174, 0x200},
};

// The order CalcMat tests the dirty bits in.
constexpr uint32_t kRefreshOrder[] = {0x1, 0x2, 0x200, 0x4, 0x8, 0x100, 0x10, 0x20, 0x80, 0x400, 0x800};

// The model's material dictionary (ResDic at data + *(data + 0x30)).
uint32_t MaterialDic(uint32_t data) {
    const uint32_t offset = Read32(data + 0x30);
    return offset ? data + offset : 0;
}

uint32_t MaterialCount(uint32_t data) {
    const uint32_t dic = MaterialDic(data);
    return dic ? Read32(dic + 4) : 0;
}

// MdlMaterial (0x80098864): the ResMat data of material i, 0 when none.
uint32_t Material(uint32_t data, uint32_t i) {
    const uint32_t dic = MaterialDic(data);

    if (!dic)
        return 0;

    const uint32_t offset = Read32(dic + (i + 1) * 16 + 0x14);
    return offset ? dic + offset : 0;
}

// The ResMat behind a material reference (0x8005AA20 asserts it).
uint32_t CheckedMaterial(uint32_t mat) {
    if (!mat)
        MH3_THROW(std::runtime_error("NW4R panic (g3d_resmat.h:621): ResMat is not valid"));

    return mat;
}

// The TEV block (0x8006F228): ResMat + its offset at +0x3C, 0 when none.
uint32_t TevBlock(uint32_t mat) {
    const uint32_t offset = Read32(CheckedMaterial(mat) + 0x3C);
    return offset ? mat + offset : 0;
}

// The resource part each copy refreshes from.
uint32_t ResourcePart(uint32_t mat, uint32_t bit) {
    switch (bit) {
    case 0x1:
        return CheckedMaterial(mat) + 0x40;
    case 0x2:
        return CheckedMaterial(mat) + 0x144;
    case 0x4:
        return CheckedMaterial(mat) + 0x1A8;
    case 0x8:
        return CheckedMaterial(mat) + 0x3F0;
    case 0x10:
        return CheckedMaterial(mat) + 0x14;
    case 0x20:
        return CheckedMaterial(mat) + 0x1C;
    case 0x80:
        return TevBlock(mat);
    case 0x100:
        return TevBlock(mat) + 0x20;
    case 0x200:
        return TevBlock(mat) + 0xA0;
    case 0x400:
        return TevBlock(mat) + 0xE0;
    default: { // 0x800
        const uint32_t offset = Read32(CheckedMaterial(mat) + 0x28);
        return offset ? mat + offset : 0;
    }
    }
}

// The material's part of the model's copy when it has one (array + i *
// size), else the resource's own.
uint32_t PartFor(uint32_t mdl, uint32_t mat, uint32_t i, uint32_t bit) {
    for (const CopiedPart& part : kParts) {
        if (part.bit == bit) {
            const uint32_t array = Read32(mdl + part.array);
            return array ? array + i * part.size : ResourcePart(mat, bit);
        }
    }

    return 0;
}

// CopiedMatRefresh: `slot` is a guest word for the source reference.
void Refresh(GuestCall& game, uint32_t slot, uint32_t mdl, uint32_t i, uint32_t bits) {
    const uint32_t mat = Material(Read32(mdl + kResMdl), i);

    for (const CopiedPart& part : kParts) {
        const uint32_t array = Read32(mdl + part.array);

        if (!(bits & part.bit) || !array)
            continue;

        Write32(slot, ResourcePart(mat, part.bit));
        const uint32_t dst = array + i * part.size;

        switch (part.bit) {
        case 0x1:
            game.Call<0x80094C00>({slot, dst}); // ModelTextureCopy
            break;
        case 0x2:
            game.Call<0x80095114>({slot, dst}); // ModelPaletteCopy
            break;
        case 0x4:
            game.Call<0x8009528C>({slot, dst});
            break;
        case 0x8:
            game.Call<0x8009713C>({slot, dst});
            break;
        case 0x10:
            game.Call<0x800956FC>({slot, dst});
            break;
        case 0x20:
            game.Call<0x80095C28>({slot, dst});
            break;
        case 0x80:
            game.Call<0x800946D8>({slot, dst}); // MatPixelCopy
            break;
        case 0x100:
            game.Call<0x80094724>({slot, dst}); // MatColorCopy
            break;
        case 0x200:
            game.Call<0x80094770>({slot, dst}); // MatIndirectCopy
            break;
        case 0x400:
            game.Call<0x800947BC>({slot, dst}); // MatTexgenCopy
            break;
        default:
            game.Call<0x8009A2DC>({slot, dst});
            break;
        }
    }

    const uint32_t word = Read32(mdl + kDirty) + 4 * i;
    Write32(word, Read32(word) & ~bits);
}

// IsMaterialAnimated of the three material animations.
uint32_t MaterialAnimated(uint32_t anm, uint32_t i) {
    return (MemoryInline::FlatRead16(Read32(anm + 0x14) + 2 * i) & 0xC000) == 0;
}

// The scene callback (0x8007B5C8, 0x8007B54C, 0x8007B4F0): its virtual Slot
// (0xC CalcWorld, 0x10 CalcMat, 0x14 CalcView) as (cb, timing, obj, arg,
// options) when the object has a callback of that kind for that timing.
template <uint32_t Slot, uint32_t Kind>
void SceneCallback(GuestCall& game, uint32_t obj, uint32_t timing, uint32_t arg, uint32_t options) {
    const uint32_t cb = Read32(obj + kCallback);

    if (cb && (MemoryInline::FlatRead16(obj + kCallbackKinds) & Kind) && (MemoryInline::FlatRead8(obj + kCallbackTiming) & timing))
        game.Virtual<Slot>(cb, {timing, obj, arg, options});
}

// Guest stack offsets of CalcMat's locals.
constexpr uint32_t kSlotA = 0x08, kSlotB = 0x0C, kRefreshSlot = 0x10;
constexpr uint32_t kTexPatResult = 0x40; // 0x44 bytes
constexpr uint32_t kTexSrtResult = 0x90; // 0x118 bytes
constexpr uint32_t kClrResult = 0x1B0;   // 0x5C bytes

void CalcMat(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x230);
    GuestCall game{ctx};
    const uint32_t mdl = frame.Arg(3), arg = frame.Arg(4), options = frame.Arg(5);
    SceneCallback<0x10, 2>(game, mdl, 1, arg, options);
    const uint32_t data = Read32(mdl + kResMdl);
    const uint32_t count = MaterialCount(data);

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t mat = Material(data, i);

        for (const uint32_t bit : kRefreshOrder) {
            if (Read32(Read32(mdl + kDirty) + 4 * i) & bit)
                Refresh(game, frame[kRefreshSlot], mdl, i, bit);
        }

        auto setDirty = [&](uint32_t bits) {
            const uint32_t word = Read32(mdl + kDirty) + 4 * i;
            Write32(word, Read32(word) | bits);
        };

        // Texture pattern: ModelTextureReplace(&texture, &palette, result).
        if (const uint32_t anm = Read32(mdl + kAnmTexPat); anm && MaterialAnimated(anm, i)) {
            game.Call<0x8006F58C>({frame[kTexPatResult]});
            const uint32_t result = game.Virtual<0x38>(anm, {frame[kTexPatResult], i});
            Write32(frame[kSlotA], PartFor(mdl, mat, i, 0x1));
            Write32(frame[kSlotB], PartFor(mdl, mat, i, 0x2));
            game.Call<0x80069A2C>({frame[kSlotA], frame[kSlotB], result});
            setDirty(0x3);
        }

        // Texture SRT: 0x8006E39C(&srt, &indirect, result).
        if (const uint32_t anm = Read32(mdl + kAnmTexSrt); anm && MaterialAnimated(anm, i)) {
            const uint32_t result = game.Virtual<0x38>(anm, {frame[kTexSrtResult], i});
            Write32(frame[kSlotA], PartFor(mdl, mat, i, 0x4));
            Write32(frame[kSlotB], PartFor(mdl, mat, i, 0x200));
            game.Call<0x8006E39C>({frame[kSlotA], frame[kSlotB], result});
            setDirty(0x204);
        }

        // Colours: 0x8006418C(&tevColors, &channel, result).
        if (const uint32_t anm = Read32(mdl + kAnmClr); anm && MaterialAnimated(anm, i)) {
            Write32(frame[kSlotA], PartFor(mdl, mat, i, 0x100));
            Write32(frame[kSlotB], PartFor(mdl, mat, i, 0x8));
            const uint32_t result = game.Virtual<0x38>(anm, {frame[kClrResult], i});
            game.Call<0x8006418C>({frame[kSlotA], frame[kSlotB], result});
            setDirty(0x108);
        }
    }

    SceneCallback<0x10, 2>(game, mdl, 4, arg, options);
}

void CopiedMatRefresh(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    Refresh(game, frame[kSlotA], frame.Arg(3), frame.Arg(4), frame.Arg(5));
}

// CopiedMatAccess: {mdl, mat, the eleven copies' parts} (0 for each when
// the model has none, or when the material does not exist).
void CopiedMatAccessCtor(uint32_t self, uint32_t mdl, uint32_t i) {
    const bool valid = mdl && Material(Read32(mdl + kResMdl), i) != 0;
    Write32(self, valid ? mdl : 0);
    Write32(self + 4, i);
    uint32_t at = self + 8;

    for (const CopiedPart& part : kParts) {
        const uint32_t array = valid ? Read32(mdl + part.array) : 0;
        Write32(at, array ? array + i * part.size : 0);
        at += 4;
    }
}

void ScnObjG3dProc(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t obj = frame.Arg(3), task = frame.Arg(4), options = frame.Arg(6);
    uint32_t arg = frame.Arg(5);

    switch (task) {
    case 1: {
        // CalcWorld: the callbacks around the object's virtual +0x30
        // (obj, options, &arg), which may change arg.
        SceneCallback<0xC, 1>(game, obj, 1, arg, options);
        Write32(frame[8], arg);
        game.Virtual<0x30>(obj, {options, frame[8]});
        arg = Read32(frame[8]);
        SceneCallback<0xC, 1>(game, obj, 2, arg, options);
        SceneCallback<0xC, 1>(game, obj, 4, arg, options);
        break;
    }
    case 2:
        SceneCallback<0x10, 2>(game, obj, 1, arg, options);
        SceneCallback<0x10, 2>(game, obj, 4, arg, options);
        break;
    case 4:
        // CalcView: view (+0x6C) = camera (options) * world (+0x3C).
        SceneCallback<0x14, 4>(game, obj, 1, arg, options);
        G3dNative::MtxConcat(options, obj + 0x3C, obj + 0x6C);
        SceneCallback<0x14, 4>(game, obj, 2, arg, options);
        SceneCallback<0x14, 4>(game, obj, 4, arg, options);
        break;
    case 0x10003:
        Write32(obj + 4, 0); // the parent link cleared
        break;
    case 0x10002:
        if (Read32(obj + 4))
            MH3_THROW(std::runtime_error("NW4R panic (g3d_scnobj.cpp:492): Failed assertion !GetParent()"));

        Write32(obj + 4, options);
        break;
    default:
        break;
    }
}

// ScnMdlSimple::CalcWorld (0x8007F480): the callbacks around the object's
// virtual +0x30, then G3dCalcWorld over the model's NodeTree. The original
// stages the matrices in the locked cache for 9 to 341 of them, which g3d's
// LockedCacheAcquire (0x80502DD0) never grants here, so they go to main
// memory (+0xEC) and the "in the cache" bit (+0x104 & 1) is cleared.
void SimpleCalcWorld(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x50);
    GuestCall game{ctx};
    const uint32_t mdl = frame.Arg(3), options = frame.Arg(5);
    uint32_t arg = frame.Arg(4);
    SceneCallback<0xC, 1>(game, mdl, 1, arg, options);
    Write32(frame[8], arg);
    game.Virtual<0x30>(mdl, {options, frame[8]});
    arg = Read32(frame[8]);
    SceneCallback<0xC, 1>(game, mdl, 2, arg, options);
    Write32(mdl + 0x104, Read32(mdl + 0x104) & ~1u);

    // The root flags: by the model's scale (+0xDC, +0xE0, +0xE4: uniform
    // and one, uniform, or not), and the segment scale rule asked through
    // virtual +0x24 (0x20001).
    uint32_t rootFlags = 0xF0000000u;
    const float sx = MemoryInline::FlatReadFloat32(mdl + 0xDC), sy = MemoryInline::FlatReadFloat32(mdl + 0xE0);
    game.Virtual<0x24>(mdl, {0x20001, frame[0x1C]});

    if (sx == sy && sy == MemoryInline::FlatReadFloat32(mdl + 0xE4))
        rootFlags = MemoryInline::FlatReadFloat32(0x8079FF60u - 31804) == sx ? rootFlags : rootFlags & 0x3FFFFFFFu;
    else
        rootFlags &= 0x0FFFFFFFu;

    if (Read32(frame[0x1C]))
        rootFlags |= 0x08000000u;

    // The CalcWorld callback {object, timing, node} (0x8007F738) when set.
    uint32_t callback = 0;

    if (const uint32_t object = Read32(mdl + 0x11C)) {
        callback = frame[0x20];
        Write32(callback, object);
        MemoryInline::FlatWrite8(callback + 4, MemoryInline::FlatRead8(mdl + 0x120));
        MemoryInline::FlatWrite16(callback + 6, MemoryInline::FlatRead16(mdl + 0x122));
    }

    Write32(frame[0x10], Read32(mdl + kResMdl));
    game.Call<0x80073830>({Read32(mdl + 0xEC), Read32(mdl + 0xF0), Read32(mdl + 0x108), mdl + 0x3C, frame[0x10], Read32(mdl + 0x124), callback, rootFlags});
}

// ScnMdl's CalcWorld task (0x8007D4E0): SimpleCalcWorld, the envelope
// matrices (0x8007E860) when the model asks for them (+0x13C: 1 set, 2
// clear), the visibility animation (+0x128) on the model's own copy (+0x148,
// then marked: +0x13C | 1) or the resource, and the final callback.
void CalcWorldTask(CpuContext* ctx) {
    const uint32_t mdl = ctx->gpr[3], arg = ctx->gpr[4], options = ctx->gpr[5];
    SimpleCalcWorld(ctx);
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};

    if (!(Read32(mdl + 0x13C) & 2) && (Read32(mdl + 0x13C) & 1))
        game.Call<0x8007E860>({mdl});

    if (const uint32_t anm = Read32(mdl + 0x128)) {
        if (const uint32_t copy = Read32(mdl + 0x148)) {
            Write32(frame[0xC], Read32(mdl + kResMdl));
            game.Call<0x8006EDE8>({copy, frame[0xC], anm});
            Write32(mdl + 0x13C, Read32(mdl + 0x13C) | 1);
        } else {
            Write32(frame[8], Read32(mdl + kResMdl));
            game.Call<0x8006ED18>({frame[8], anm});
        }
    }

    SceneCallback<0xC, 1>(game, mdl, 4, arg, options);
}

// ScnMdlSimple::CalcView (0x8007FADC): the next of the model's view buffers
// (u8 +0x101 counts modulo u8 +0x100), the view matrix (+0x6C) = camera
// (options) * world (+0x3C), then G3dCalcViewPlain (0x80070884; the locked
// cache is never granted, see SimpleCalcWorld) into that buffer's positions
// (+0xF4), normals (+0xF8) and environment matrices (+0xFC) (each buffer the
// u16 +0x102 matrices rounded up to 32 bytes; ModelViewPosition and the
// others, model_pose.cpp, give them).
void SimpleCalcView(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const uint32_t mdl = frame.Arg(3), arg = frame.Arg(4), options = frame.Arg(5);
    const int32_t count = MemoryInline::FlatRead8(mdl + 0x100);
    const int32_t next = int32_t(MemoryInline::FlatRead8(mdl + 0x101)) + 1;
    const uint32_t buffer = uint8_t(count ? next % count : next); // divw by 0: a 0 quotient
    MemoryInline::FlatWrite8(mdl + 0x101, uint8_t(buffer));
    SceneCallback<0x14, 4>(game, mdl, 1, arg, options);
    G3dNative::MtxConcat(options, mdl + 0x3C, mdl + 0x6C);
    SceneCallback<0x14, 4>(game, mdl, 2, arg, options);

    // The arrays through the model pose's getters (0x80080C68, 0x80080C0C,
    // 0x80080BC0), which also declare the scene to the native pose.
    const uint32_t environment = NativeModelViewArray(mdl, 2);
    const uint32_t matrices = MemoryInline::FlatRead16(mdl + 0x102);
    const uint32_t normals = NativeModelViewArray(mdl, 1);
    const uint32_t positions = NativeModelViewArray(mdl, 0);
    Write32(frame[8], Read32(mdl + kResMdl));
    game.Call<0x80070884>({positions, normals, Read32(mdl + 0xEC), Read32(mdl + 0xF0), matrices, options, frame[8], environment});
    SceneCallback<0x14, 4>(game, mdl, 4, arg, options);
}

} // namespace

extern "C" void func_8007F480(CpuContext* ctx);
extern "C" void MH3_ScnMdlSimpleCalcWorld(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("ScnMdlSimpleCalcWorld")) [[unlikely]] {
        PortCheck::Run(ctx, "ScnMdlSimpleCalcWorld", func_8007F480, SimpleCalcWorld);
        return;
    }
#endif
    SimpleCalcWorld(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007F480, MH3_ScnMdlSimpleCalcWorld, "ScnMdlSimpleCalcWorld", kNativeKeepsNonvolatile);

extern "C" void func_8007D4E0(CpuContext* ctx);
extern "C" void MH3_ScnMdlCalcWorldTask(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("ScnMdlCalcWorldTask")) [[unlikely]] {
        PortCheck::Run(ctx, "ScnMdlCalcWorldTask", func_8007D4E0, CalcWorldTask);
        return;
    }
#endif
    CalcWorldTask(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007D4E0, MH3_ScnMdlCalcWorldTask, "ScnMdlCalcWorldTask", kNativeKeepsNonvolatile);

extern "C" void func_8007FADC(CpuContext* ctx);
extern "C" void MH3_ScnMdlSimpleCalcView(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("ScnMdlSimpleCalcView")) [[unlikely]] {
        PortCheck::Run(ctx, "ScnMdlSimpleCalcView", func_8007FADC, SimpleCalcView);
        return;
    }
#endif
    SimpleCalcView(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007FADC, MH3_ScnMdlSimpleCalcView, "ScnMdlSimpleCalcView", kNativeKeepsNonvolatile);

extern "C" void func_8007D600(CpuContext* ctx);
extern "C" void MH3_ScnMdlCalcMat(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("ScnMdlCalcMat")) [[unlikely]] {
        PortCheck::Run(ctx, "ScnMdlCalcMat", func_8007D600, CalcMat);
        return;
    }
#endif
    CalcMat(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007D600, MH3_ScnMdlCalcMat, "ScnMdlCalcMat", kNativeKeepsNonvolatile);

extern "C" void func_8007E4FC(CpuContext* ctx);
extern "C" void MH3_CopiedMatRefresh(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("CopiedMatRefresh")) [[unlikely]] {
        PortCheck::Run(ctx, "CopiedMatRefresh", func_8007E4FC, CopiedMatRefresh);
        return;
    }
#endif
    CopiedMatRefresh(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007E4FC, MH3_CopiedMatRefresh, "CopiedMatRefresh", kNativeKeepsNonvolatile);

extern "C" void func_80081A94(CpuContext* ctx);
extern "C" void MH3_ScnObjG3dProc(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("ScnObjG3dProc")) [[unlikely]] {
        PortCheck::Run(ctx, "ScnObjG3dProc", func_80081A94, ScnObjG3dProc);
        return;
    }
#endif
    ScnObjG3dProc(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80081A94, MH3_ScnObjG3dProc, "ScnObjG3dProc", kNativeKeepsNonvolatile);

// CopiedMatAccess's constructor returns this; checked with RunOutput.
extern "C" void func_8007BF10(CpuContext* ctx);
static void CopiedMatAccessPort(CpuContext* p) {
    CopiedMatAccessCtor(p->gpr[3], p->gpr[4], p->gpr[5]);
}

static void CopiedMatAccessChecked([[maybe_unused]] uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("CopiedMatAccess")) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        c->gpr[5] = r5;
        PortCheck::RunOutput(c, "CopiedMatAccess", func_8007BF10, CopiedMatAccessPort, {{r3, 0x34}});
        return;
    }
#endif
    CopiedMatAccessCtor(r3, r4, r5);
}

extern "C" void MH3_CopiedMatAccess(CpuContext* ctx) {
    CopiedMatAccessChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007BF10, MH3_CopiedMatAccess, "CopiedMatAccess");

// IsMaterialAnimated (r3 anm, r4 material) -> r3, checked with RunPure.
#ifdef MH3_DEVELOPMENT
#define MH3_ANIMATED_CHECK(Name, Addr)                                                                                                                                                                 \
    if (PortCheck::Enabled(#Name)) [[unlikely]] {                                                                                                                                                      \
        CpuContext* c = TryGetCpuContext();                                                                                                                                                            \
        c->gpr[1] = r1;                                                                                                                                                                                \
        c->gpr[3] = r3;                                                                                                                                                                                \
        c->gpr[4] = r4;                                                                                                                                                                                \
        PortCheck::RunPure(c, #Name, PortCheck::Returns::Gpr3, func_##Addr, MaterialAnimatedPort);                                                                                                     \
        return c->gpr[3];                                                                                                                                                                              \
    }
#else
#define MH3_ANIMATED_CHECK(Name, Addr) (void)r1;
#endif

static void MaterialAnimatedPort(CpuContext* p) {
    p->gpr[3] = MaterialAnimated(p->gpr[3], p->gpr[4]);
}

extern "C" void func_80069D58(CpuContext* ctx);
static uint32_t TexSrtAnimatedChecked(uint32_t r1, uint32_t r3, uint32_t r4) {
    MH3_ANIMATED_CHECK(AnmTexSrtMatAnimated, 80069D58)
    return MaterialAnimated(r3, r4);
}

extern "C" void MH3_AnmTexSrtMatAnimated(CpuContext* ctx) {
    ctx->gpr[3] = TexSrtAnimatedChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80069D58, MH3_AnmTexSrtMatAnimated, "AnmTexSrtMatAnimated");

extern "C" void func_80069838(CpuContext* ctx);
static uint32_t TexPatAnimatedChecked(uint32_t r1, uint32_t r3, uint32_t r4) {
    MH3_ANIMATED_CHECK(AnmTexPatMatAnimated, 80069838)
    return MaterialAnimated(r3, r4);
}

extern "C" void MH3_AnmTexPatMatAnimated(CpuContext* ctx) {
    ctx->gpr[3] = TexPatAnimatedChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80069838, MH3_AnmTexPatMatAnimated, "AnmTexPatMatAnimated");

extern "C" void func_80063EC4(CpuContext* ctx);
static uint32_t MatClrAnimatedChecked(uint32_t r1, uint32_t r3, uint32_t r4) {
    MH3_ANIMATED_CHECK(AnmMatClrMatAnimated, 80063EC4)
    return MaterialAnimated(r3, r4);
}

extern "C" void MH3_AnmMatClrMatAnimated(CpuContext* ctx) {
    ctx->gpr[3] = MatClrAnimatedChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80063EC4, MH3_AnmMatClrMatAnimated, "AnmMatClrMatAnimated");
