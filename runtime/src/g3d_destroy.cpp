// NW4R g3d object destructors, RMHP08: native ports (Ghidra decompiles of
// the DOL, checked against the translated code) of every destructor that
// reaches G3dObj::~G3dObj. Each is its class's virtual destructor (vtable
// +0x10, r4 > 0: delete) and the next class's base destructor (r4 = 0).
//
//   0x8007B338 G3dObj::~G3dObj        vtable 0x8058F4D8; frees itself through
//                                      its allocator (+0x8, 0x8007B3A4)
//   0x8008141C ScnObj::~ScnObj         vtable 0x8058FCBC; still attached
//                                      (+0x4) panics; deletes its callback
//                                      (+0xD4) when it owns it (u8 +0xD9 1)
//   0x8007BAA8 ScnLeaf::~ScnLeaf
//   0x80080FE0 ScnMdlSimple::~...      vtable 0x8058FB00; deletes its owned
//                                      callback (+0x11C, u8 +0x121 1), then
//                                      RemoveAnmObj (+0x38) for each bound
//                                      animation (+0x124..+0x134)
//   0x8007EECC ScnMdl::~ScnMdl         vtable 0x8058F7E8; RemoveAnmObj for
//                                      its own animation (+0x138)
//   0x80082630 ScnGroup::~ScnGroup     vtable 0x8058FC48; removes (+0x38) its
//                                      children, last first, until +0xE4 is 0
//   0x80083558 ScnRoot::~ScnRoot       vtable 0x8058FE30; tells its camera
//                                      callback (+0x2888, +0xC: 0x10003)
//   0x8005D3E8 AnmObj::~AnmObj, and the animation classes 0x8005E5F0,
//   0x8005E64C (vtable 0x8058C518), 0x80063C64, 0x80063DCC, 0x800657CC,
//   0x800660E4, 0x8006AAB0, 0x8006AB0C (vtable 0x8058DD28), 0x8006E8CC,
//   0x8006EA2C. 0x8005E64C and 0x8006AB0C check that the object is in RAM
//   (else panic) and detach it from its model (+0x44).
//
// The overlapped draw (docs/optimizations/threading/pipelined-draw.md) walks the scene the previous
// logic step prepared while the game thread runs the next step. An area
// change, or a monster leaving, destroys models the draw still holds: their
// destructors put the base vtable in place, whose draw virtual is null, and
// the draw thread called 0 ("Missing translated target 0x00000000", devkit,
// Flooded Forest 0->1 and 9->7). The heap's free waited for the
// draw (native_resource_lifetime.cpp), but only after the destructors ran.
// Now a destroy waits at its start, on the game thread, while the draw holds
// the object or the model an animation is bound to (the object's parent,
// +0x4): at most the rest of one draw, on the frames that destroy a drawn
// object.
#include "abi_bridge.h"
#include "frame_split.h"
#include "guest_call.h"
#include "guest_worker.h"
#include "memory.h"

#include <cstdint>
#include <cstdio>

namespace {
uint32_t Read32(uint32_t address) {
    return Memory::Read32(address);
}

uint8_t Read8(uint32_t address) {
    return Memory::Read8(address);
}

void Write32(uint32_t address, uint32_t value) {
    Memory::Write32(address, value);
}

constexpr uint32_t Panic = 0x80501130; // nw4r::db::Panic(file, line, format, ...)
constexpr uint32_t Free = 0x8007B3A4;  // G3dObj's allocator free (allocator, block)

bool Drawn(uint32_t object) {
    return object && SceneQueue::HoldsPrepared(object, object + 1);
}

void WaitForDraw(CpuContext* cpu, uint32_t self) {
    if (!self || PpcThread::CurrentThread() != PpcThread::Game || !DrawThread::Thread().Pending())
        return;

    if (!Drawn(self) && !Drawn(Read32(self + 4)))
        return;

#ifdef MH3_DEVELOPMENT
    static unsigned reported = 0;

    if (reported < 8) {
        ++reported;
        std::fprintf(stderr, "MH3 draw wait: destroy of %08x (vtable %08x) from %08x\n", self, Read32(self), cpu->lr);
    }
#else
    (void)cpu;
#endif
    DrawThread::Thread().Wait();
}

// The class's delete (operator delete, 0x8005D444) is empty: G3dObj's
// destructor freed the object.
uint32_t G3dObj(GuestCall& game, uint32_t self, int16_t) {
    if (!self)
        return self;

    Write32(self, 0x8058F4D8);
    game.Call<Free>({Read32(self + 8), self});
    return self;
}

// Destroyed while still in a group or scene: nw4r panics.
void Detached(GuestCall& game, uint32_t self, uint32_t file, uint32_t line, uint32_t format) {
    if (Read32(self + 4))
        game.Call<Panic>({file, line, format});
}

uint32_t ScnObj(GuestCall& game, uint32_t self, int16_t) {
    if (!self)
        return self;

    Write32(self, 0x8058FCBC);
    Detached(game, self, 0x8058FB98, 0x61, 0x8058FBA8);

    if (const uint32_t callback = Read32(self + 0xD4); callback && Read8(self + 0xD9) == 1)
        game.Virtual<0x8>(callback, {1});

    return G3dObj(game, self, 0);
}

uint32_t ScnLeaf(GuestCall& game, uint32_t self, int16_t) {
    return self ? ScnObj(game, self, 0) : self;
}

uint32_t ScnMdlSimple(GuestCall& game, uint32_t self, int16_t) {
    if (!self)
        return self;

    Write32(self, 0x8058FB00);
    Detached(game, self, 0x8058F860, 0x437, 0x8058FADC);

    if (const uint32_t callback = Read32(self + 0x11C); Read8(self + 0x121) == 1 && callback)
        game.Virtual<0x8>(callback, {1});

    for (uint32_t slot = 0x124; slot <= 0x134; slot += 4)
        if (const uint32_t anm = Read32(self + slot))
            game.Virtual<0x38>(self, {anm});

    return ScnLeaf(game, self, 0);
}

uint32_t ScnMdl(GuestCall& game, uint32_t self, int16_t) {
    if (!self)
        return self;

    Write32(self, 0x8058F7E8);
    Detached(game, self, 0x8058F560, 0x65B, 0x8058F7C4);

    if (const uint32_t anm = Read32(self + 0x138))
        game.Virtual<0x38>(self, {anm});

    return ScnMdlSimple(game, self, 0);
}

uint32_t ScnGroup(GuestCall& game, uint32_t self, int16_t) {
    if (!self)
        return self;

    Write32(self, 0x8058FC48);
    Detached(game, self, 0x8058FB98, 0x34D, 0x8058FBA8);

    // Clear (0x800826CC): Remove(size - 1) until empty (0x8007F120,
    // 0x8007F0C0); the children are detached, not destroyed.
    while (const uint32_t size = Read32(self + 0xE4))
        game.Virtual<0x38>(self, {size - 1});

    return ScnObj(game, self, 0);
}

uint32_t ScnRoot(GuestCall& game, uint32_t self, int16_t) {
    if (!self)
        return self;

    Write32(self, 0x8058FE30);

    if (const uint32_t callback = Read32(self + 0x2888))
        game.Virtual<0xC>(callback, {0x10003, 0, self});

    // The member at +0x2878 (0x800834A8 with -1) deletes nothing.
    return ScnGroup(game, self, 0);
}

uint32_t AnmObj(GuestCall& game, uint32_t self, int16_t) {
    return self ? G3dObj(game, self, 0) : self;
}

uint32_t AnmObj5E5F0(GuestCall& game, uint32_t self, int16_t) {
    return self ? AnmObj(game, self, 0) : self;
}

// NW4R's pointer check before the object detaches from its model (+0x44)
// always passes in the port; the pointer is folded to its cached alias.
void Release(GuestCall& game, uint32_t self) {
    game.Virtual<0x44>(GuestFlat::CachedAlias(self));
}

uint32_t AnmObj5E64C(GuestCall& game, uint32_t self, int16_t) {
    if (!self)
        return self;

    Write32(self, 0x8058C518);
    Release(game, self);
    return AnmObj5E5F0(game, self, 0);
}

uint32_t AnmObj63C64(GuestCall& game, uint32_t self, int16_t) {
    return self ? AnmObj5E64C(game, self, 0) : self;
}

uint32_t AnmObj63DCC(GuestCall& game, uint32_t self, int16_t) {
    return self ? AnmObj5E5F0(game, self, 0) : self;
}

uint32_t AnmObj657CC(GuestCall& game, uint32_t self, int16_t) {
    return self ? G3dObj(game, self, 0) : self;
}

uint32_t AnmObj660E4(GuestCall& game, uint32_t self, int16_t) {
    return self ? AnmObj657CC(game, self, 0) : self;
}

uint32_t AnmObj6AAB0(GuestCall& game, uint32_t self, int16_t) {
    return self ? AnmObj(game, self, 0) : self;
}

uint32_t AnmObj6AB0C(GuestCall& game, uint32_t self, int16_t) {
    if (!self)
        return self;

    Write32(self, 0x8058DD28);
    Release(game, self);
    return AnmObj6AAB0(game, self, 0);
}

uint32_t AnmObj6E8CC(GuestCall& game, uint32_t self, int16_t) {
    return self ? AnmObj6AB0C(game, self, 0) : self;
}

uint32_t AnmObj6EA2C(GuestCall& game, uint32_t self, int16_t) {
    return self ? AnmObj6AAB0(game, self, 0) : self;
}

// The guest entry (r3 the object, r4 the delete flag; returns the object):
// the draw wait, then the destructor chain.
template <uint32_t (*Destructor)(GuestCall&, uint32_t, int16_t)>
void Destroy(CpuContext* cpu) {
    const uint32_t self = cpu->gpr[3];
    WaitForDraw(cpu, self);
    {
        GuestFrame frame(cpu, 0x20);
        GuestCall game{cpu};
        Destructor(game, self, int16_t(frame.Arg(4)));
    }

    cpu->gpr[3] = self;
}
} // namespace

extern "C" {
void MH3_G3dObjDtor(CpuContext* cpu) { Destroy<G3dObj>(cpu); }
void MH3_ScnObjDtor(CpuContext* cpu) { Destroy<ScnObj>(cpu); }
void MH3_ScnLeafDtor(CpuContext* cpu) { Destroy<ScnLeaf>(cpu); }
void MH3_ScnMdlSimpleDtor(CpuContext* cpu) { Destroy<ScnMdlSimple>(cpu); }
void MH3_ScnMdlDtor(CpuContext* cpu) { Destroy<ScnMdl>(cpu); }
void MH3_ScnGroupDtor(CpuContext* cpu) { Destroy<ScnGroup>(cpu); }
void MH3_ScnRootDtor(CpuContext* cpu) { Destroy<ScnRoot>(cpu); }
void MH3_AnmObjDtor(CpuContext* cpu) { Destroy<AnmObj>(cpu); }
void MH3_AnmObj5E5F0Dtor(CpuContext* cpu) { Destroy<AnmObj5E5F0>(cpu); }
void MH3_AnmObj5E64CDtor(CpuContext* cpu) { Destroy<AnmObj5E64C>(cpu); }
void MH3_AnmObj63C64Dtor(CpuContext* cpu) { Destroy<AnmObj63C64>(cpu); }
void MH3_AnmObj63DCCDtor(CpuContext* cpu) { Destroy<AnmObj63DCC>(cpu); }
void MH3_AnmObj657CCDtor(CpuContext* cpu) { Destroy<AnmObj657CC>(cpu); }
void MH3_AnmObj660E4Dtor(CpuContext* cpu) { Destroy<AnmObj660E4>(cpu); }
void MH3_AnmObj6AAB0Dtor(CpuContext* cpu) { Destroy<AnmObj6AAB0>(cpu); }
void MH3_AnmObj6AB0CDtor(CpuContext* cpu) { Destroy<AnmObj6AB0C>(cpu); }
void MH3_AnmObj6E8CCDtor(CpuContext* cpu) { Destroy<AnmObj6E8CC>(cpu); }
void MH3_AnmObj6EA2CDtor(CpuContext* cpu) { Destroy<AnmObj6EA2C>(cpu); }
}

REGISTER_NATIVE_FUNCTION_AS(0x8007B338, MH3_G3dObjDtor, "G3dObjDtor");
REGISTER_NATIVE_FUNCTION_AS(0x8008141C, MH3_ScnObjDtor, "ScnObjDtor");
REGISTER_NATIVE_FUNCTION_AS(0x8007BAA8, MH3_ScnLeafDtor, "ScnLeafDtor");
REGISTER_NATIVE_FUNCTION_AS(0x80080FE0, MH3_ScnMdlSimpleDtor, "ScnMdlSimpleDtor");
REGISTER_NATIVE_FUNCTION_AS(0x8007EECC, MH3_ScnMdlDtor, "ScnMdlDtor");
REGISTER_NATIVE_FUNCTION_AS(0x80082630, MH3_ScnGroupDtor, "ScnGroupDtor");
REGISTER_NATIVE_FUNCTION_AS(0x80083558, MH3_ScnRootDtor, "ScnRootDtor");
REGISTER_NATIVE_FUNCTION_AS(0x8005D3E8, MH3_AnmObjDtor, "AnmObjDtor");
REGISTER_NATIVE_FUNCTION_AS(0x8005E5F0, MH3_AnmObj5E5F0Dtor, "AnmObj5E5F0Dtor");
REGISTER_NATIVE_FUNCTION_AS(0x8005E64C, MH3_AnmObj5E64CDtor, "AnmObj5E64CDtor");
REGISTER_NATIVE_FUNCTION_AS(0x80063C64, MH3_AnmObj63C64Dtor, "AnmObj63C64Dtor");
REGISTER_NATIVE_FUNCTION_AS(0x80063DCC, MH3_AnmObj63DCCDtor, "AnmObj63DCCDtor");
REGISTER_NATIVE_FUNCTION_AS(0x800657CC, MH3_AnmObj657CCDtor, "AnmObj657CCDtor");
REGISTER_NATIVE_FUNCTION_AS(0x800660E4, MH3_AnmObj660E4Dtor, "AnmObj660E4Dtor");
REGISTER_NATIVE_FUNCTION_AS(0x8006AAB0, MH3_AnmObj6AAB0Dtor, "AnmObj6AAB0Dtor");
REGISTER_NATIVE_FUNCTION_AS(0x8006AB0C, MH3_AnmObj6AB0CDtor, "AnmObj6AB0CDtor");
REGISTER_NATIVE_FUNCTION_AS(0x8006E8CC, MH3_AnmObj6E8CCDtor, "AnmObj6E8CCDtor");
REGISTER_NATIVE_FUNCTION_AS(0x8006EA2C, MH3_AnmObj6EA2CDtor, "AnmObj6EA2CDtor");
