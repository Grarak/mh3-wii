// MH3's effect objects and the nw4r ef calls their tasks make, RMHP08: C
// reconstructions from the Ghidra decompilation. In the task pass these are
// the game thread's first uses of the effect pools (emitters, managers and
// particles), before its first Effect::Calc.
//
//   0x80330444 EffectObjectStep            an effect object's step by its
//   0x800FE9A0 EffectSpawnerStep           state (+5): 0 start, 1 update,
//                                          2 the next state, 3 release
//   0x800F88D0 EffectObjectRelease         the release callback (+0x40) and
//                                          retirement (0x800F8CDC) of its
//                                          effects (+0x38, +0x3C), the effect
//                                          system's virtual +0xC, the object
//                                          (0x48 bytes) cleared, the live
//                                          object count (0x806A4584) down
//   0x800F8B1C RetireEffects               push_eft_effect_heap_num: each
//                                          effect of an array retired with
//                                          its particles and emitters
//   0x800FE94C EffectSpawnerRetireEffects  the spawner's two effect arrays
//   0x800A5420 EffectSetRootMtx            the root matrix (+0x58), then the
//                                          root emitters' SetMtxDirty
//   0x800A963C EmitterSetMtxDirty          the emitter's, its managers' and
//                                          its descendants' matrices dirty
//   0x800A9724 ParticleManagerSetMtxDirty  (inline) +0x8A set; the calc's
//                                          cached manager id (r13 - 0x4500)
//                                          dropped when it is this one
//   0x800A4A90 EffectRetireEmitterAll      the effect's active emitters
//   0x800A6484 EmitterDestroyFunc          its particles, managers and (with
//                                          flag 1) its descendants retired
//   0x800A63B4 EmitterRetireParticleAll    each manager's RetireParticleAll
//   0x800A699C EmitterRetireParticleManagerAll  its active managers
//   0x800A44C8 EffectSendClosing           the effect's system (+0x20) told
//                                          (EffectSystem::Closing, 0x800A59AC)
//   0x800D6000 EffMemoryGarbageCollection  MemoryManagerTmp: closing objects
//                                          no one references back to their
//                                          pool's free list; particles of
//                                          closing managers, then the
//   0x800D6308 EffMemoryCollectManagers    managers,
//   0x800D6244 EffMemoryCollectEmitters    emitters
//   0x800D6168 EffMemoryCollectEffects     and effects
//
// Virtual functions (vtable at +0x1C): +8 SendClosing, +0xC DestroyFunc;
// Effect (0x80592D48) +0x18 Calc (0x800A4C20); Emitter (0x80593368).
//
// Offsets: Effect 0x0C life status (1 active), 0x20 EffectSystem*, 0x24
// ut::List of Emitter, 0x58 root Mtx34; Emitter 0x0C life status, 0x20 flags,
// 0xBC Effect*, 0xC0 ut::List of ParticleManager, 0xF0 its wait counter,
// 0xF4 parent Emitter*, 0x120 matrix dirty; ParticleManager 0x08 id (low 16
// bits), 0x0C life status, 0x20 Emitter*, 0x38 ActivityList of Particle (its
// closing list at 0x44), 0x8A matrix dirty; Particle 0x10 references, 0xC8
// ParticleManager*. ut::List: head 0x0, tail 0x4, count u16 0x8, link offset
// u16 0xA (link: prev, next). ActivityList: active list 0x0, closing 0xC.
// MemoryManagerTmp: 0x08 effect pool, 0x10 emitter, 0x18 manager, 0x20
// particle; a pool: free list 0x04, used list 0x10.
#include "abi_bridge.h"
#include "fatal.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace {
using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatRead8;
using MemoryInline::FlatReadFloat32;
using MemoryInline::FlatWrite16;
using MemoryInline::FlatWrite32;
using MemoryInline::FlatWrite8;
using MemoryInline::FlatWriteFloat32;

constexpr uint32_t kEffectSystem = 0x806A4574;    // nw4r::ef::EffectSystem*
constexpr uint32_t kLiveEffectObjects = 0x806A4584; // s32

// The vtables of the classes whose virtual functions the ports call natively.
constexpr uint32_t kEffectVtable = 0x80592D48, kEmitterVtable = 0x80593368, kManagerVtable = 0x80593CA0,
                   kParticleVtable = 0x805935F0, kMemoryManagerVtable = 0x80595B50;

void SendClosing(GuestCall& game, uint32_t object);
void DestroyFunc(GuestCall& game, uint32_t object);
void GarbageCollection(GuestCall& game, uint32_t memory);

// While MH3_PORT_CHECK is set, the ports' native calls of other ports go
// through the guest dispatch, so each port is also checked from these
// callers; the port whose body runs (g_portBody) calls its own body directly.
// Port checks keep everything on the game thread.
uint32_t g_portBody = 0;

template <uint32_t Address>
bool Routed() {
    return PortCheck::Set() && g_portBody != Address;
}

[[noreturn]] void Panic(const char* file, uint32_t line, const char* what) {
    char text[160];
    std::snprintf(text, sizeof text, "NW4R panic (%s:%u): Failed assertion %s", file, line, what);
    MH3_THROW(std::runtime_error(text));
}

// nw4r::ut::List_GetNext: the head for 0, else the link's next.
uint32_t ListNext(uint32_t list, uint32_t object) {
    return object ? FlatRead32(object + FlatRead16(list + 0xA) + 4) : FlatRead32(list);
}

// The callers' size assertion, then EffUtilListToArray (0x8009B3D8): the
// list's objects in order, so retiring them can unlink them.
template <size_t N>
uint32_t ListArray(uint32_t list, std::array<uint32_t, N>& out, const char* file, uint32_t line) {
    const uint32_t count = FlatRead16(list + 8);

    if (count >= N)
        Panic(file, line, "UtlistSize(list) <= MAX");

    uint32_t n = 0;

    // At least one: the original stores before it tests the limit.
    for (uint32_t object = ListNext(list, 0); object && n < N; object = ListNext(list, object)) {
        out[n++] = object;

        if (!(int32_t(n) < int32_t(count)))
            break;
    }

    return n;
}

// nw4r::ut::List_Append (0x805020E4).
void ListAppend(uint32_t list, uint32_t object) {
    const uint32_t link = object + FlatRead16(list + 0xA), tail = FlatRead32(list + 4);
    FlatWrite32(link, tail);
    FlatWrite32(link + 4, 0);

    if (tail)
        FlatWrite32(tail + FlatRead16(list + 0xA) + 4, object);
    else
        FlatWrite32(list, object);

    FlatWrite32(list + 4, object);
    FlatWrite16(list + 8, uint16_t(FlatRead16(list + 8) + 1));
}

// nw4r::ut::List_Remove (0x80502274).
void ListRemove(uint32_t list, uint32_t object) {
    const uint32_t offset = FlatRead16(list + 0xA), link = object + offset;
    const uint32_t prev = FlatRead32(link), next = FlatRead32(link + 4);

    if (prev)
        FlatWrite32(prev + offset + 4, next);
    else
        FlatWrite32(list, next);

    if (next)
        FlatWrite32(next + offset, prev);
    else
        FlatWrite32(list + 4, prev);

    FlatWrite32(link, 0);
    FlatWrite32(link + 4, 0);
    FlatWrite16(list + 8, uint16_t(FlatRead16(list + 8) - 1));
}

uint32_t LifeStatus(uint32_t object) {
    return FlatRead32(object + 0xC);
}

// NW4R_POINTER_CHECK: MEM1, MEM2 or their uncached mirrors, or the locked
// cache.
bool PointerValid(uint32_t p) {
    return (p & 0xFF000000) == 0x80000000 || (p & 0xFF800000) == 0x81000000 || (p & 0xF8000000) == 0x90000000 ||
           (p & 0xFF000000) == 0xC0000000 || (p & 0xFF800000) == 0xC1000000 || (p & 0xF8000000) == 0xD0000000 ||
           (p & 0xFFFFC000) == 0xE0000000;
}

void CheckPointer(uint32_t p, const char* file, uint32_t line) {
    if (!PointerValid(p))
        Panic(file, line, "pointer error");
}

// ActivityList::ToClosing (0x800A4A80): one active fewer (+0x18); the object
// stays on the active list until its system's Closing.
void ActivityListToClosing(uint32_t activity) {
    FlatWrite16(activity + 0x18, uint16_t(FlatRead16(activity + 0x18) - 1));
}

// ActivityList::ToWait (0x800A4640): from the active list to the closing one.
void ActivityListClose(GuestCall& game, uint32_t activity, uint32_t object) {
    if (Routed<0x800A4640>())
        return void(game.Call<0x800A4640>({activity, object}));

    if (FlatRead16(activity + 8) < FlatRead16(activity + 0x18))
        Panic("activitylist.h", 0x86, "mActiveList.GetSize() >= mNumActive");

    ListRemove(activity, object);
    ListAppend(activity + 0xC, object);
}

// ReferencedObject::Destroy (0x800A4A1C): its DestroyFunc (vtable +0xC, the
// vtable at +0x1C), status 2 (waiting), and with no references its
// SendClosing (+8).
void ReferencedObjectDestroy(GuestCall& game, uint32_t object) {
    if (Routed<0x800A4A1C>())
        return void(game.Call<0x800A4A1C>({object}));

    DestroyFunc(game, object);
    FlatWrite32(object + 0xC, 2);

    if (!FlatRead32(object + 0x10))
        SendClosing(game, object);
}

// ReferencedObject::UnRef (0x800A3864): one reference fewer; a waiting
// object's last one sends it closing. The references left.
uint32_t UnRef(GuestCall& game, uint32_t object) {
    if (Routed<0x800A3864>())
        return game.Call<0x800A3864>({object});

    const uint32_t references = FlatRead32(object + 0x10);

    if (!references)
        Panic("referencedobject.h", 0x6C, "mRefCount > 0");

    FlatWrite32(object + 0x10, references - 1);

    if (references == 1 && LifeStatus(object) == 2)
        SendClosing(game, object);

    return FlatRead32(object + 0x10);
}

// The RetireX of an active child: its owner's ActivityList one active fewer,
// then Destroy. Whether it was active.
bool RetireChild(GuestCall& game, uint32_t activity, uint32_t object) {
    if (LifeStatus(object) != 1)
        return false;

    ActivityListToClosing(activity);
    ReferencedObjectDestroy(game, object);
    return true;
}

// ParticleManager::RetireParticle (0x800AB8E4).
bool ParticleManagerRetireParticle(GuestCall& game, uint32_t manager, uint32_t particle) {
    if (Routed<0x800AB8E4>())
        return game.Call<0x800AB8E4>({manager, particle}) != 0;

    CheckPointer(particle, "ef_particlemanager.cpp", 0x48);

    if (FlatRead32(particle + 0xC8) != manager)
        Panic("ef_particlemanager.cpp", 0x49, "target->mParticleManager == this");

    return RetireChild(game, manager + 0x38, particle);
}

// ParticleManager::RetireParticleAll (0x800ABA58): its active particles.
uint32_t ParticleManagerRetireParticleAll(GuestCall& game, uint32_t manager) {
    if (Routed<0x800ABA58>())
        return game.Call<0x800ABA58>({manager});

    uint32_t retired = 0;

    for (uint32_t particle = FlatRead32(manager + 0x38), next; particle; particle = next) {
        next = FlatRead32(particle + FlatRead16(manager + 0x42) + 4);

        if (LifeStatus(particle) == 1)
            retired += ParticleManagerRetireParticle(game, manager, particle);
    }

    return retired;
}

// Emitter::RetireParticleManager (0x800A684C).
bool EmitterRetireParticleManager(GuestCall& game, uint32_t emitter, uint32_t manager) {
    if (Routed<0x800A684C>())
        return game.Call<0x800A684C>({emitter, manager}) != 0;

    CheckPointer(manager, "ef_emitter.cpp", 0x82);
    return RetireChild(game, emitter + 0xC0, manager);
}

// Effect::RetireEmitter (0x800A48D0).
bool EffectRetireEmitter(GuestCall& game, uint32_t effect, uint32_t emitter) {
    if (Routed<0x800A48D0>())
        return game.Call<0x800A48D0>({effect, emitter}) != 0;

    CheckPointer(emitter, "ef_effect.cpp", 0x90);
    return RetireChild(game, effect + 0x24, emitter);
}

// EffectSystem::Closing (0x800A59AC): the effect onto its group's closing
// list, status 3.
void EffectSystemClosing(GuestCall& game, uint32_t system, uint32_t effect) {
    if (Routed<0x800A59AC>())
        return void(game.Call<0x800A59AC>({system, effect}));

    CheckPointer(effect, "ef_effectsystem.cpp", 0x67);
    ActivityListClose(game, FlatRead32(system + 0xC018) + FlatRead32(effect + 0x40) * 0x1C, effect);
    FlatWrite32(effect + 0xC, 3);
}

// Effect::Closing (0x800A44D8): the emitter's references to its parent
// (+0xF4), its link (+0xF8) and its effect dropped, onto the effect's closing
// list, status 3.
void EffectClosing(GuestCall& game, uint32_t effect, uint32_t emitter) {
    if (Routed<0x800A44D8>())
        return void(game.Call<0x800A44D8>({effect, emitter}));

    CheckPointer(emitter, "ef_effect.cpp", 0x5D);

    if (const uint32_t parent = FlatRead32(emitter + 0xF4))
        UnRef(game, parent);

    if (const uint32_t link = FlatRead32(emitter + 0xF8)) {
        UnRef(game, link);
        FlatWrite32(emitter + 0xF8, 0);
    }

    UnRef(game, FlatRead32(emitter + 0xBC));
    ActivityListClose(game, effect + 0x24, emitter);
    FlatWrite32(emitter + 0xC, 3);
}

// Effect::RemoveDrawOrder (0x800A6710): the draw order's (+0xA0) virtual
// Remove (+0xC, vtable at +0).
void EffectRemoveDrawOrder(GuestCall& game, uint32_t effect, uint32_t manager) {
    if (Routed<0x800A6710>())
        return void(game.Call<0x800A6710>({effect, manager}));

    CheckPointer(manager, "effect.h", 0xF4);
    const uint32_t order = FlatRead32(effect + 0xA0);
    game(FlatRead32(FlatRead32(order) + 0xC), {order, effect, manager});
}

// Emitter::Closing (0x800A65B8).
void EmitterClosing(GuestCall& game, uint32_t emitter, uint32_t manager) {
    if (Routed<0x800A65B8>())
        return void(game.Call<0x800A65B8>({emitter, manager}));

    CheckPointer(manager, "ef_emitter.cpp", 0x76);
    EffectRemoveDrawOrder(game, FlatRead32(emitter + 0xBC), manager);
    UnRef(game, FlatRead32(manager + 0x20));
    ActivityListClose(game, emitter + 0xC0, manager);
    FlatWrite32(manager + 0xC, 3);
}

// ParticleManager::Closing (0x800AB7A4).
void ParticleManagerClosing(GuestCall& game, uint32_t manager, uint32_t particle) {
    if (Routed<0x800AB7A4>())
        return void(game.Call<0x800AB7A4>({manager, particle}));

    CheckPointer(particle, "ef_particlemanager.cpp", 0x3D);
    UnRef(game, FlatRead32(particle + 0xC8));
    ActivityListClose(game, manager + 0x38, particle);
    FlatWrite32(particle + 0xC, 3);
}

// SendClosing (+8): each class tells its owner's Closing (Effect 0x800A44C8,
// Emitter 0x800A6478, ParticleManager 0x800AB794, Particle 0x800AA324).
void SendClosing(GuestCall& game, uint32_t object) {
    const uint32_t vtable = FlatRead32(object + 0x1C);

    if (PortCheck::Set())
        return void(game(FlatRead32(vtable + 8), {object}));

    switch (vtable) {
    case kEffectVtable:
        EffectSystemClosing(game, FlatRead32(object + 0x20), object);
        break;
    case kEmitterVtable:
        EffectClosing(game, FlatRead32(object + 0xBC), object);
        break;
    case kManagerVtable:
        EmitterClosing(game, FlatRead32(object + 0x20), object);
        break;
    case kParticleVtable:
        ParticleManagerClosing(game, FlatRead32(object + 0xC8), object);
        break;
    default:
        game(FlatRead32(vtable + 8), {object});
        break;
    }
}

void ParticleManagerSetMtxDirty(CpuContext* cpu, uint32_t manager) {
    FlatWrite8(manager + 0x8A, 1);
    const uint32_t cached = cpu->gpr[13] - 0x4500;

    if (FlatRead32(cached) == (FlatRead32(manager + 8) & 0xFFFF))
        FlatWrite32(cached, ~0u);
}

void EmitterManagersDirty(CpuContext* cpu, uint32_t emitter) {
    for (uint32_t manager = ListNext(emitter + 0xC0, 0); manager; manager = ListNext(emitter + 0xC0, manager))
        ParticleManagerSetMtxDirty(cpu, manager);
}

void EmitterSetMtxDirty(CpuContext* cpu, uint32_t emitter) {
    if (Routed<0x800A963C>())
        return void(GuestCall{cpu}.Call<0x800A963C>({emitter}));

    FlatWrite8(emitter + 0x120, 1);
    EmitterManagersDirty(cpu, emitter);
    const uint32_t effect = FlatRead32(emitter + 0xBC);

    if (!effect)
        return;

    // Every emitter descending from this one (its parent chain, +0xF4).
    for (uint32_t other = ListNext(effect + 0x24, 0); other; other = ListNext(effect + 0x24, other)) {
        if (FlatRead8(other + 0x120))
            continue;

        for (uint32_t parent = FlatRead32(other + 0xF4); parent; parent = FlatRead32(parent + 0xF4)) {
            if (parent == emitter) {
                FlatWrite8(other + 0x120, 1);
                EmitterManagersDirty(cpu, other);
            }
        }
    }
}

void RootEmittersDirty(CpuContext* cpu, uint32_t effect) {
    for (uint32_t emitter = ListNext(effect + 0x24, 0); emitter; emitter = ListNext(effect + 0x24, emitter))
        if (!FlatRead32(emitter + 0xF4))
            EmitterSetMtxDirty(cpu, emitter);
}

void EffectSetRootMtx(CpuContext* cpu, uint32_t effect, uint32_t matrix) {
    for (uint32_t k = 0; k < 0x30; k += 4) // MTX34 operator= (PSMTXCopy)
        FlatWrite32(effect + 0x58 + k, FlatRead32(matrix + k));

    RootEmittersDirty(cpu, effect);
}

// SetRootMtxTrans (0x800F9744): the root matrix with a new translation.
void EffectSetRootTrans(CpuContext* cpu, uint32_t effect, uint32_t trans) {
    if (Routed<0x800F9744>())
        return void(GuestCall{cpu}.Call<0x800F9744>({effect, trans}));

    if (!effect)
        return;

    for (uint32_t k = 0; k < 3; ++k)
        FlatWrite32(effect + 0x58 + 0xC + 0x10 * k, FlatRead32(trans + 4 * k));

    RootEmittersDirty(cpu, effect);
}

// 0x800F9BCC: the emitter's scale (+0x9C), its matrices dirty.
void EmitterSetScale(CpuContext* cpu, uint32_t emitter, uint32_t scale) {
    if (Routed<0x800F9BCC>())
        return void(GuestCall{cpu}.Call<0x800F9BCC>({emitter, scale}));

    for (uint32_t k = 0; k < 3; ++k)
        FlatWrite32(emitter + 0x9C + 4 * k, FlatRead32(scale + 4 * k));

    EmitterSetMtxDirty(cpu, emitter);
}

uint32_t EmitterRetireParticleAll(GuestCall& game, uint32_t emitter) {
    if (Routed<0x800A63B4>())
        return game.Call<0x800A63B4>({emitter});

    std::array<uint32_t, 0x400> managers;
    const uint32_t n = ListArray(emitter + 0xC0, managers, "ef_emitter.cpp", 0x35);
    uint32_t retired = 0;

    for (uint32_t i = 0; i < n; ++i)
        retired += ParticleManagerRetireParticleAll(game, managers[i]);

    return retired;
}

uint32_t EmitterRetireParticleManagerAll(GuestCall& game, uint32_t emitter) {
    if (Routed<0x800A699C>())
        return game.Call<0x800A699C>({emitter});

    std::array<uint32_t, 0x400> managers;
    const uint32_t n = ListArray(emitter + 0xC0, managers, "ef_emitter.cpp", 0x93);
    uint32_t retired = 0;

    for (uint32_t i = 0; i < n; ++i)
        if (LifeStatus(managers[i]) == 1)
            retired += EmitterRetireParticleManager(game, emitter, managers[i]);

    return retired;
}

uint32_t EffectRetireEmitterAll(GuestCall& game, uint32_t effect) {
    if (Routed<0x800A4A90>())
        return game.Call<0x800A4A90>({effect});

    std::array<uint32_t, 0x200> emitters;
    const uint32_t n = ListArray(effect + 0x24, emitters, "ef_effect.cpp", 0xA0);
    uint32_t retired = 0;

    for (uint32_t i = 0; i < n; ++i)
        if (LifeStatus(emitters[i]) == 1)
            retired += EffectRetireEmitter(game, effect, emitters[i]);

    return retired;
}

// Effect::RetireParticleAll (0x800A4B5C): each emitter's.
uint32_t EffectRetireParticleAll(GuestCall& game, uint32_t effect) {
    if (Routed<0x800A4B5C>())
        return game.Call<0x800A4B5C>({effect});

    std::array<uint32_t, 0x200> emitters;
    const uint32_t n = ListArray(effect + 0x24, emitters, "ef_effect.cpp", 0xB4);
    uint32_t retired = 0;

    for (uint32_t i = 0; i < n; ++i)
        retired += EmitterRetireParticleAll(game, emitters[i]);

    return retired;
}

void EmitterDestroyFunc(GuestCall& game, uint32_t emitter) {
    if (Routed<0x800A6484>())
        return void(game.Call<0x800A6484>({emitter}));

    if (FlatRead32(emitter + 0xF0))
        FlatWrite32(emitter + 0xF0, 0);

    if (FlatRead32(emitter + 0x20) & 1)
        EmitterRetireParticleAll(game, emitter);

    EmitterRetireParticleManagerAll(game, emitter);

    if (!(FlatRead32(emitter + 0x20) & 1))
        return;

    const uint32_t effect = FlatRead32(emitter + 0xBC);
    std::array<uint32_t, 0x200> emitters;
    const uint32_t n = ListArray(effect + 0x24, emitters, "ef_emitter.cpp", 0x58);

    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t other = emitters[i];

        for (uint32_t parent = FlatRead32(other + 0xF4); parent; parent = FlatRead32(parent + 0xF4)) {
            if (parent == emitter) {
                EmitterRetireParticleAll(game, other);

                if (LifeStatus(other) == 1)
                    EffectRetireEmitter(game, effect, other);

                break;
            }
        }
    }
}

// DestroyFunc (+0xC): Effect 0x800A44D4 (RetireEmitterAll), Emitter
// 0x800A6484; ParticleManager's and Particle's are empty.
void DestroyFunc(GuestCall& game, uint32_t object) {
    const uint32_t vtable = FlatRead32(object + 0x1C);

    if (PortCheck::Set())
        return void(game(FlatRead32(vtable + 0xC), {object}));

    switch (vtable) {
    case kEffectVtable:
        EffectRetireEmitterAll(game, object);
        break;
    case kEmitterVtable:
        EmitterDestroyFunc(game, object);
        break;
    case kManagerVtable:
    case kParticleVtable:
        break;
    default:
        game(FlatRead32(vtable + 0xC), {object});
        break;
    }
}

// The effect system's MemoryManager (+0)'s GarbageCollection (+0xC).
void SystemGarbageCollection(GuestCall& game, uint32_t system) {
    const uint32_t memory = FlatRead32(system);

    if (FlatRead32(memory) == kMemoryManagerVtable)
        GarbageCollection(game, memory);
    else
        game(FlatRead32(FlatRead32(memory) + 0xC), {memory});
}

bool DescendsFrom(uint32_t emitter, uint32_t ancestor) {
    for (uint32_t parent = FlatRead32(emitter + 0xF4); parent; parent = FlatRead32(parent + 0xF4))
        if (parent == ancestor)
            return true;

    return false;
}

// Emitter::ForeachParticleManager (0x800A9938): `f` on each manager (active
// ones only unless `anyStatus`), with `descendants` also those of every
// emitter descending from this one (Effect::ForeachEmitterFrom 0x800A5348
// over any status, with 0x800A97F4). The count.
template <typename F>
uint32_t EmitterForEachManager(uint32_t emitter, bool anyStatus, bool descendants, F&& f) {
    uint32_t n = 0;

    for (uint32_t manager = ListNext(emitter + 0xC0, 0), next; manager; manager = next) {
        next = ListNext(emitter + 0xC0, manager);

        if (anyStatus || LifeStatus(manager) == 1) {
            f(manager);
            ++n;
        }
    }

    if (descendants) {
        const uint32_t list = FlatRead32(emitter + 0xBC) + 0x24;

        for (uint32_t other = ListNext(list, 0), next; other; other = next) {
            next = ListNext(list, other);

            if (DescendsFrom(other, emitter)) {
                CheckPointer(other, "ef_emitter.cpp", 0x519);
                n += EmitterForEachManager(other, anyStatus, false, f);
            }
        }
    }

    return n;
}

// Effect::ForeachParticleManager (0x800A52BC): each emitter's.
template <typename F>
uint32_t EffectForEachManager(uint32_t effect, bool anyStatus, F&& f) {
    uint32_t n = 0;

    for (uint32_t emitter = ListNext(effect + 0x24, 0), next; emitter; emitter = next) {
        next = ListNext(effect + 0x24, emitter);
        n += EmitterForEachManager(emitter, anyStatus, false, f);
    }

    return n;
}

// The ParticleManager's SimpleLight modifier (0x800AC164): type (+0x6C),
// ambient (+0x6D) and diffuse (+0x71) GXColors, radius (+0x78), position
// (+0x7C).
struct SimpleLight {
    uint8_t type;
    uint32_t ambient, diffuse; // the GXColors' bytes as one big-endian word
    float radius;
    uint32_t position[3];
};

void ParticleManagerSetSimpleLight(uint32_t manager, const SimpleLight& light) {
    FlatWrite8(manager + 0x6C, light.type);

    for (uint32_t k = 0; k < 4; ++k) {
        FlatWrite8(manager + 0x6D + k, uint8_t(light.ambient >> (24 - 8 * k)));
        FlatWrite8(manager + 0x71 + k, uint8_t(light.diffuse >> (24 - 8 * k)));
    }

    FlatWriteFloat32(manager + 0x78, light.radius);

    for (uint32_t k = 0; k < 3; ++k)
        FlatWrite32(manager + 0x7C + 4 * k, light.position[k]);
}

uint32_t ReadColor(uint32_t color) {
    return uint32_t(FlatRead8(color)) << 24 | uint32_t(FlatRead8(color + 1)) << 16 | uint32_t(FlatRead8(color + 2)) << 8 | FlatRead8(color + 3);
}

// Effect::GetEmitter (0x800A523C): ut::List_GetNth (0x8050231C).
uint32_t EffectGetEmitter(GuestCall& game, uint32_t effect, uint32_t index) {
    if (Routed<0x800A523C>())
        return game.Call<0x800A523C>({effect, index});

    if (index >= FlatRead16(effect + 0x24 + 8))
        Panic("ef_effect.cpp", 0x1A7, "idx < GetNumEmitter()");

    uint32_t emitter = ListNext(effect + 0x24, 0);

    for (uint32_t i = 0; emitter && i < index; ++i)
        emitter = ListNext(effect + 0x24, emitter);

    return emitter;
}

// change_paramscale_eff (0x800F9B24): the first emitter's scale, from
// 0.00001 (0x80798A9C) up.
void EffectChangeScale(CpuContext* cpu, uint32_t effect, double scale) {
    if (Routed<0x800F9B24>())
        return void(GuestCall{cpu}.Call<0x800F9B24>({effect}, {scale}));

    GuestCall game{cpu};

    if (!(double(FlatReadFloat32(0x80798A9C)) <= scale) || !FlatRead16(effect + 0x24 + 8))
        return;

    if (const uint32_t emitter = EffectGetEmitter(game, effect, 0)) {
        FlatWriteFloat32(emitter + 0x9C, scale);
        FlatWriteFloat32(emitter + 0xA0, scale);
        FlatWriteFloat32(emitter + 0xA4, scale);
        EmitterSetMtxDirty(cpu, emitter);
    }
}

// 0x800F98E8: whether any of the effect's emitters has managers.
bool EffectHasManagers(GuestCall& game, uint32_t effect) {
    if (Routed<0x800F98E8>())
        return game.Call<0x800F98E8>({effect}) != 0;

    const uint32_t count = FlatRead16(effect + 0x24 + 8);
    uint32_t managers = 0, emitter = 0;

    for (uint32_t i = 0; i < count; ++i) {
        emitter = ListNext(effect + 0x24, emitter);

        if (!emitter)
            Panic("effect.cpp", 0xB26, "0");

        managers += FlatRead16(emitter + 0xC0 + 8);
    }

    return managers != 0;
}

// 0x800F99D0: the effect's virtual Calc (+0x18).
void EffectCalcVirtual(GuestCall& game, uint32_t effect, uint32_t onlyBillboard) {
    if (Routed<0x800F99D0>())
        return void(game.Call<0x800F99D0>({effect, onlyBillboard}));

    game(FlatRead32(FlatRead32(effect + 0x1C) + 0x18), {effect, onlyBillboard != 0});
}

// effect_move (0x800F9984): Calc while it has managers.
bool EffectMove(GuestCall& game, uint32_t effect) {
    if (Routed<0x800F9984>())
        return game.Call<0x800F9984>({effect}) != 0;

    const bool live = EffectHasManagers(game, effect);

    if (live)
        EffectCalcVirtual(game, effect, 0);

    return live;
}

// 0x800FFFFC: the first emitter's translation (+0x90, emitter.h's
// GetTranslate, 0x80100088) plus the root matrix's; whether there is one.
bool EffectGetPosition(GuestCall& game, uint32_t effect, uint32_t out) {
    if (Routed<0x800FFFFC>())
        return game.Call<0x800FFFFC>({effect, out}) != 0;

    const uint32_t first = FlatRead32(effect + 0x24);

    if (!first)
        return false;

    if (!out)
        Panic("emitter.h", 0x187, "trans != NULL");

    for (uint32_t k = 0; k < 3; ++k)
        FlatWrite32(out + 4 * k, FlatRead32(first + 0x90 + 4 * k));

    for (uint32_t k = 0; k < 3; ++k)
        FlatWriteFloat32(out + 4 * k, FlatReadFloat32(out + 4 * k) + FlatReadFloat32(effect + 0x58 + 0xC + 0x10 * k));

    return true;
}

// push_eft_effect_heap_num (0x800F8B1C): each effect of the array retired
// with its particles and emitters, its slot cleared.
void RetireEffects(GuestCall& game, uint32_t effects, int32_t count) {
    if (Routed<0x800F8B1C>())
        return void(game.Call<0x800F8B1C>({effects, uint32_t(count)}));

    const uint32_t system = FlatRead32(kEffectSystem);

    for (int32_t i = 0; i < count; ++i, effects += 4) {
        if (const uint32_t effect = FlatRead32(effects)) {
            EffectRetireParticleAll(game, effect);
            EffectRetireEmitterAll(game, effect);

            if (system)
                game.Call<0x800A5C6C>({system, effect}); // EffectSystem::RetireEffect
        }

        FlatWrite32(effects, 0);
    }
}

// 0x800F8CDC: `count` 0x40-byte effect slots from `first` back to the free
// pool (0x806A4598), their in-use bytes (0x806A459C) cleared.
void EffectSlotsFree(GuestCall& game, uint32_t first, int32_t count) {
    if (Routed<0x800F8CDC>())
        return void(game.Call<0x800F8CDC>({first, uint32_t(count)}));

    if (count <= 0)
        return;

    const uint32_t slots = FlatRead32(0x806A4598), index = (first - slots) >> 6;

    for (uint32_t k = 0; k < uint32_t(count) << 6; k += 4)
        FlatWrite32(slots + (index << 6) + k, 0);

    for (int32_t i = 0; i < count; ++i)
        FlatWrite8(FlatRead32(0x806A459C) + index + i, 0);
}

void EffectObjectRelease(GuestCall& game, uint32_t object) {
    if (Routed<0x800F88D0>())
        return void(game.Call<0x800F88D0>({object}));

    if (FlatRead32(object + 0x38)) {
        if (const uint32_t callback = FlatRead32(object + 0x40))
            game(callback, {object, 0x806A0000u});

        EffectSlotsFree(game, FlatRead32(object + 0x38), int32_t(FlatRead32(object + 0x3C)));
    }

    if (const uint32_t system = FlatRead32(kEffectSystem))
        SystemGarbageCollection(game, system);

    for (uint32_t k = 0; k < 0x48; k += 4)
        FlatWrite32(object + k, 0);

    if (const int32_t live = int32_t(FlatRead32(kLiveEffectObjects)); live > 0)
        FlatWrite32(kLiveEffectObjects, uint32_t(live - 1));
}

// 0x800FFF6C: the spawner's effect `index` retired with its particles and
// emitters, the memory collected.
void EffectSpawnerRetireEffect(GuestCall& game, uint32_t spawner, uint32_t index) {
    if (Routed<0x800FFF6C>())
        return void(game.Call<0x800FFF6C>({spawner, index}));

    const uint32_t system = FlatRead32(kEffectSystem), slot = FlatRead32(spawner + 0x38) + index * 4 + 4;

    if (const uint32_t effect = FlatRead32(slot)) {
        EffectRetireParticleAll(game, effect);
        EffectRetireEmitterAll(game, effect);
        game.Call<0x800A5C6C>({system, effect}); // EffectSystem::RetireEffect
        SystemGarbageCollection(game, system);
        FlatWrite32(slot, 0);
    }
}

// 0x800F9358: whether the object's owner (+0x30) is still there (its first
// byte set) for owner kinds 0-3.
bool EffectObjectAlive(GuestCall& game, uint32_t object, uint32_t kind) {
    if (Routed<0x800F9358>())
        return game.Call<0x800F9358>({object, kind}) != 0;

    return kind > 3 || FlatRead8(FlatRead32(object + 0x30)) != 0;
}

// 0x800E3BA0: the character's draw pass, 8 without split screen
// (0x8065B4D6), else 0x400 for the first player (+8 0), 0x800 for the other.
uint32_t MHcharDrawPass(GuestCall& game, uint32_t chara) {
    if (Routed<0x800E3BA0>())
        return game.Call<0x800E3BA0>({chara});

    if (!FlatRead8(0x8065B4D6))
        return 8;

    return FlatRead8(chara + 8) ? 0x800 : 0x400;
}

// 0x800F93E4: the effect draw flag of the character's pass.
uint32_t EffectDrawFlags(GuestCall& game, uint32_t chara) {
    if (Routed<0x800F93E4>())
        return game.Call<0x800F93E4>({chara});

    switch (MHcharDrawPass(game, chara)) {
    case 0x400:
        return 8;
    case 0x800:
        return 0x10;
    case 8:
        return 4;
    default:
        return 0;
    }
}

// Pl_master_ck (0x8026FE38): whether this machine runs the player: play
// mode 2 (0x8065AA85), online mode 3 (0x8065AA84), or its index (+8) is
// MyPlayerIndex (0x8065AA87, signed).
bool PlMasterCheck(GuestCall& game, uint32_t player) {
    if (Routed<0x8026FE38>())
        return game.Call<0x8026FE38>({player}) != 0;

    if (FlatRead8(0x8065AA85) == 2 || FlatRead8(0x8065AA84) == 3)
        return true;

    return int32_t(int8_t(FlatRead8(0x8065AA87))) == int32_t(FlatRead8(player + 8));
}

// eftGetKeyRGB (0x800FA804): the color at `t` of keys {t, r, g, b} (four
// bytes each, t 0xFF ends with its color), linear between keys.
void EftKeyRGB(uint32_t keys, int32_t t, uint8_t rgb[3]) {
    for (;; keys += 4) {
        const int32_t t0 = FlatRead8(keys);

        if (t0 == 0xFF) {
            for (uint32_t c = 0; c < 3; ++c)
                rgb[c] = FlatRead8(keys + 1 + c);

            return;
        }

        if (t0 <= t && t <= int32_t(FlatRead8(keys + 4)))
            break;
    }

    const int32_t t0 = FlatRead8(keys);
    const float f = float(t - t0) / float(int32_t(FlatRead8(keys + 4)) - t0);

    for (uint32_t c = 0; c < 3; ++c) {
        const uint32_t a = FlatRead8(keys + 1 + c), b = FlatRead8(keys + 5 + c);
        rgb[c] = uint8_t(int32_t(float(a) + f * float(int32_t(b - a))));
    }
}

// eftGetKeyAlpha (0x800FA95C): the same for keys {t, alpha}.
uint32_t EftKeyAlpha(GuestCall& game, uint32_t keys, int32_t t) {
    if (Routed<0x800FA95C>())
        return game.Call<0x800FA95C>({keys, uint32_t(t)});

    for (;; keys += 2) {
        const int32_t t0 = FlatRead8(keys);

        if (t0 == 0xFF)
            return FlatRead8(keys + 1);

        if (t0 <= t && t <= int32_t(FlatRead8(keys + 2)))
            break;
    }

    const int32_t t0 = FlatRead8(keys);
    const uint32_t a = FlatRead8(keys + 1), b = FlatRead8(keys + 3);
    return uint32_t(int32_t(float(a) + float(t - t0) / float(int32_t(FlatRead8(keys + 2)) - t0) * float(int32_t(b - a))));
}

// 0x800F9A38: the SimpleLight of each of the effect's managers (with the
// callback 0x800F9AD4).
void EffectSetSimpleLight(uint32_t effect, const SimpleLight& light, bool anyStatus) {
    EffectForEachManager(effect, anyStatus, [&](uint32_t manager) { ParticleManagerSetSimpleLight(manager, light); });
}

SimpleLight ReadSimpleLight(uint32_t type, uint32_t ambient, uint32_t diffuse, float radius, uint32_t position) {
    SimpleLight light{uint8_t(type), ReadColor(ambient), ReadColor(diffuse), radius, {}};

    for (uint32_t k = 0; k < 3; ++k)
        light.position[k] = FlatRead32(position + 4 * k);

    return light;
}

// change_color_eff (0x800F99EC): light type 1, ambient `color`, black
// diffuse, radius 5000 (0x80798A98) at `position`, on every manager.
void EffectChangeColor(GuestCall& game, uint32_t effect, uint32_t position, uint32_t color) {
    if (Routed<0x800F99EC>())
        return void(game.Call<0x800F99EC>({effect, position, color}));

    SimpleLight light{1, ReadColor(color), 0x000000FF, FlatReadFloat32(0x80798A98), {}};

    for (uint32_t k = 0; k < 3; ++k)
        light.position[k] = FlatRead32(position + 4 * k);

    EffectSetSimpleLight(effect, light, true);
}

// The pool's closing objects (life status 3) no one references (+0x10) leave
// their owner's closing list (`owner(object)` an ActivityList) and the pool's
// used list for its free list.
template <typename Owner>
void CollectPool(uint32_t pool, Owner owner) {
    const uint32_t used = pool + 0x10;
    uint32_t object = ListNext(used, 0);

    while (object) {
        const uint32_t next = ListNext(used, object);

        if (LifeStatus(object) == 3 && !FlatRead32(object + 0x10)) {
            ListRemove(owner(object) + 0xC, object);
            ListRemove(used, object);
            ListAppend(pool + 4, object);
        }

        object = next;
    }
}

void CollectManagers(GuestCall& game, uint32_t pool) {
    if (Routed<0x800D6308>())
        return void(game.Call<0x800D6308>({pool}));

    CollectPool(pool, [](uint32_t manager) { return FlatRead32(manager + 0x20) + 0xC0; });
}

void CollectEmitters(GuestCall& game, uint32_t pool) {
    if (Routed<0x800D6244>())
        return void(game.Call<0x800D6244>({pool}));

    CollectPool(pool, [](uint32_t emitter) { return FlatRead32(emitter + 0xBC) + 0x24; });
}

// An effect's ActivityList is its group's (+0x40) in the effect system's
// array (+0xC018, 0x1C each).
void CollectEffects(GuestCall& game, uint32_t pool) {
    if (Routed<0x800D6168>())
        return void(game.Call<0x800D6168>({pool}));

    CollectPool(pool, [](uint32_t effect) { return FlatRead32(FlatRead32(effect + 0x20) + 0xC018) + FlatRead32(effect + 0x40) * 0x1C; });
}

void GarbageCollection(GuestCall& game, uint32_t memory) {
    if (Routed<0x800D6000>())
        return void(game.Call<0x800D6000>({memory}));

    const uint32_t particles = FlatRead32(memory + 0x20);

    // Every particle of each manager's closing list, whatever its references.
    for (uint32_t manager = ListNext(FlatRead32(memory + 0x18) + 0x10, 0), next; manager; manager = next) {
        next = ListNext(FlatRead32(memory + 0x18) + 0x10, manager);
        uint32_t particle = FlatRead32(manager + 0x44);

        while (particle) {
            const uint32_t following = FlatRead32(particle + FlatRead16(manager + 0x4E) + 4);

            if (FlatRead32(particle + 0x10))
                Panic("memorymanagertmp.h", 0x17A, "pp->GetRef() == 0");

            const uint32_t status = LifeStatus(FlatRead32(particle + 0xC8));

            if (status < 1 || status > 3)
                Panic("memorymanagertmp.h", 0x17D, "pp->mParticleManager life status");

            ListRemove(FlatRead32(particle + 0xC8) + 0x44, particle);
            ListRemove(particles + 0x10, particle);
            ListAppend(particles + 4, particle);
            particle = following;
        }
    }

    if (const uint32_t pool = FlatRead32(memory + 0x18))
        CollectManagers(game, pool);

    if (const uint32_t pool = FlatRead32(memory + 0x10))
        CollectEmitters(game, pool);

    if (const uint32_t pool = FlatRead32(memory + 8))
        CollectEffects(game, pool);
}
} // namespace

// The ports run their bodies in a guest frame (restoring the caller's GPRs)
// and return what the body returns in r3; while port checks run, the body's
// address is g_portBody.
template <uint32_t Address, uint32_t (*Body)(CpuContext*, GuestCall&, const GuestFrame&)>
void FramedPort(CpuContext* cpu) {
    const uint32_t outer = g_portBody;
    const bool checking = PortCheck::Set();

    if (checking)
        g_portBody = Address;

    uint32_t result;
    {
        GuestFrame frame(cpu, 0x40);
        GuestCall game{cpu};
        result = Body(cpu, game, frame);
    }

    cpu->gpr[3] = result;

    if (checking)
        g_portBody = outer;
}

#define MH3_EFFECT_PORT(ADDRESS, NAME, ...)                                                                                                          \
    static uint32_t NAME##Body(CpuContext* cpu, GuestCall& game, const GuestFrame& frame) {                                                          \
        (void)cpu;                                                                                                                                    \
        (void)game;                                                                                                                                   \
        __VA_ARGS__                                                                                                                                   \
    }                                                                                                                                                 \
    extern "C" void func_##ADDRESS(CpuContext* ctx);                                                                                                  \
    extern "C" void MH3_##NAME(CpuContext* ctx) {                                                                                                     \
        PortCheck::Call(ctx, #NAME, func_##ADDRESS, FramedPort<0x##ADDRESS, NAME##Body>);                                                             \
    }

extern "C" void MH3_EffectObjectRelease(CpuContext* ctx);

// The step functions branch to their state's function with the registers as
// they came; state 2 only moves to 3 (r4 the old state, r0 the new).
template <uint32_t Start, uint32_t Update>
void ObjectStep(CpuContext* cpu) {
    const uint32_t object = cpu->gpr[3];

    switch (const uint8_t state = FlatRead8(object + 5)) {
    case 0:
        InvokeDirectCpu<Start>(cpu);
        break;
    case 1:
        InvokeDirectCpu<Update>(cpu);
        break;
    case 2:
        FlatWrite8(object + 5, 3);
        cpu->gpr[4] = state;
        cpu->gpr[0] = 3;
        break;
    case 3:
        MH3_EffectObjectRelease(cpu);
        break;
    default:
        break;
    }
}

static void EffectObjectStepPort(CpuContext* cpu) {
    ObjectStep<0x80330480, 0x8033065C>(cpu);
}

extern "C" void func_80330444(CpuContext* ctx);
extern "C" void MH3_EffectObjectStep(CpuContext* ctx) {
    PortCheck::Call(ctx, "EffectObjectStep", func_80330444, EffectObjectStepPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80330444, MH3_EffectObjectStep, "EffectObjectStep");

static void EffectSpawnerStepPort(CpuContext* cpu) {
    ObjectStep<0x800FE9DC, 0x800FF938>(cpu);
}

extern "C" void func_800FE9A0(CpuContext* ctx);
extern "C" void MH3_EffectSpawnerStep(CpuContext* ctx) {
    PortCheck::Call(ctx, "EffectSpawnerStep", func_800FE9A0, EffectSpawnerStepPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800FE9A0, MH3_EffectSpawnerStep, "EffectSpawnerStep");

MH3_EFFECT_PORT(800F88D0, EffectObjectRelease, {
    EffectObjectRelease(game, frame.Arg(3));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800F88D0, MH3_EffectObjectRelease, "EffectObjectRelease");

MH3_EFFECT_PORT(800F8B1C, RetireEffects, {
    RetireEffects(game, frame.Arg(3), int32_t(frame.Arg(4)));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800F8B1C, MH3_RetireEffects, "RetireEffects");

// 0x800FE94C: the spawner's two effect arrays (+0x38: count, effects at +4;
// count +0x7C, effects at +0x80).
MH3_EFFECT_PORT(800FE94C, EffectSpawnerRetireEffects, {
    const uint32_t arrays = FlatRead32(frame.Arg(3) + 0x38);
    RetireEffects(game, arrays + 4, int32_t(FlatRead32(arrays)));
    FlatWrite32(arrays, 0);
    RetireEffects(game, arrays + 0x80, int32_t(FlatRead32(arrays + 0x7C)));
    FlatWrite32(arrays + 0x7C, 0);
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800FE94C, MH3_EffectSpawnerRetireEffects, "EffectSpawnerRetireEffects");

// r3 0: the emitter list's end.
MH3_EFFECT_PORT(800A5420, EffectSetRootMtx, {
    EffectSetRootMtx(cpu, frame.Arg(3), frame.Arg(4));
    return 0;
});
REGISTER_NATIVE_FUNCTION_AS(0x800A5420, MH3_EffectSetRootMtx, "EffectSetRootMtx");

MH3_EFFECT_PORT(800A963C, EmitterSetMtxDirty, {
    EmitterSetMtxDirty(cpu, frame.Arg(3));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800A963C, MH3_EmitterSetMtxDirty, "EmitterSetMtxDirty");

MH3_EFFECT_PORT(800A4A90, EffectRetireEmitterAll, { return EffectRetireEmitterAll(game, frame.Arg(3)); });
REGISTER_NATIVE_FUNCTION_AS(0x800A4A90, MH3_EffectRetireEmitterAll, "EffectRetireEmitterAll");

MH3_EFFECT_PORT(800A6484, EmitterDestroyFunc, {
    EmitterDestroyFunc(game, frame.Arg(3));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800A6484, MH3_EmitterDestroyFunc, "EmitterDestroyFunc");

MH3_EFFECT_PORT(800A63B4, EmitterRetireParticleAll, { return EmitterRetireParticleAll(game, frame.Arg(3)); });
REGISTER_NATIVE_FUNCTION_AS(0x800A63B4, MH3_EmitterRetireParticleAll, "EmitterRetireParticleAll");
MH3_EFFECT_PORT(800A699C, EmitterRetireParticleManagerAll, { return EmitterRetireParticleManagerAll(game, frame.Arg(3)); });
REGISTER_NATIVE_FUNCTION_AS(0x800A699C, MH3_EmitterRetireParticleManagerAll, "EmitterRetireParticleManagerAll");

// Effect::SendClosing (0x800A44C8): its system's Closing.
MH3_EFFECT_PORT(800A44C8, EffectSendClosing, {
    EffectSystemClosing(game, FlatRead32(frame.Arg(3) + 0x20), frame.Arg(3));
    return 1;
});
REGISTER_NATIVE_FUNCTION_AS(0x800A44C8, MH3_EffectSendClosing, "EffectSendClosing");

MH3_EFFECT_PORT(800D6000, EffMemoryGarbageCollection, {
    GarbageCollection(game, frame.Arg(3));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800D6000, MH3_EffMemoryGarbageCollection, "EffMemoryGarbageCollection");

MH3_EFFECT_PORT(800D6308, EffMemoryCollectManagers, {
    CollectManagers(game, frame.Arg(3));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800D6308, MH3_EffMemoryCollectManagers, "EffMemoryCollectManagers");

MH3_EFFECT_PORT(800D6244, EffMemoryCollectEmitters, {
    CollectEmitters(game, frame.Arg(3));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800D6244, MH3_EffMemoryCollectEmitters, "EffMemoryCollectEmitters");

MH3_EFFECT_PORT(800D6168, EffMemoryCollectEffects, {
    CollectEffects(game, frame.Arg(3));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800D6168, MH3_EffMemoryCollectEffects, "EffMemoryCollectEffects");

MH3_EFFECT_PORT(800F8CDC, EffectSlotsFree, {
    EffectSlotsFree(game, frame.Arg(3), int32_t(frame.Arg(4)));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800F8CDC, MH3_EffectSlotsFree, "EffectSlotsFree");

MH3_EFFECT_PORT(800A4A1C, ReferencedObjectDestroy, {
    ReferencedObjectDestroy(game, frame.Arg(3));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800A4A1C, MH3_ReferencedObjectDestroy, "ReferencedObjectDestroy");

MH3_EFFECT_PORT(800A4640, ActivityListClose, {
    ActivityListClose(game, frame.Arg(3), frame.Arg(4));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800A4640, MH3_ActivityListClose, "ActivityListClose");

MH3_EFFECT_PORT(800A4B5C, EffectRetireParticleAll, { return EffectRetireParticleAll(game, frame.Arg(3)); });
REGISTER_NATIVE_FUNCTION_AS(0x800A4B5C, MH3_EffectRetireParticleAll, "EffectRetireParticleAll");
MH3_EFFECT_PORT(800ABA58, ParticleManagerRetireParticleAll, { return ParticleManagerRetireParticleAll(game, frame.Arg(3)); });
REGISTER_NATIVE_FUNCTION_AS(0x800ABA58, MH3_ParticleManagerRetireParticleAll, "ParticleManagerRetireParticleAll");
MH3_EFFECT_PORT(800AB8E4, ParticleManagerRetireParticle, { return ParticleManagerRetireParticle(game, frame.Arg(3), frame.Arg(4)); });
REGISTER_NATIVE_FUNCTION_AS(0x800AB8E4, MH3_ParticleManagerRetireParticle, "ParticleManagerRetireParticle");
MH3_EFFECT_PORT(800A684C, EmitterRetireParticleManager, { return EmitterRetireParticleManager(game, frame.Arg(3), frame.Arg(4)); });
REGISTER_NATIVE_FUNCTION_AS(0x800A684C, MH3_EmitterRetireParticleManager, "EmitterRetireParticleManager");
MH3_EFFECT_PORT(800A48D0, EffectRetireEmitter, { return EffectRetireEmitter(game, frame.Arg(3), frame.Arg(4)); });
REGISTER_NATIVE_FUNCTION_AS(0x800A48D0, MH3_EffectRetireEmitter, "EffectRetireEmitter");

MH3_EFFECT_PORT(800A59AC, EffectSystemClosing, {
    EffectSystemClosing(game, frame.Arg(3), frame.Arg(4));
    return 1;
});
REGISTER_NATIVE_FUNCTION_AS(0x800A59AC, MH3_EffectSystemClosing, "EffectSystemClosing");

MH3_EFFECT_PORT(800A3864, ReferencedObjectUnRef, { return UnRef(game, frame.Arg(3)); });
REGISTER_NATIVE_FUNCTION_AS(0x800A3864, MH3_ReferencedObjectUnRef, "ReferencedObjectUnRef");
MH3_EFFECT_PORT(800A44D4, EffectDestroyFunc, { return EffectRetireEmitterAll(game, frame.Arg(3)); });
REGISTER_NATIVE_FUNCTION_AS(0x800A44D4, MH3_EffectDestroyFunc, "EffectDestroyFunc");

MH3_EFFECT_PORT(800A44D8, EffectClosing, {
    EffectClosing(game, frame.Arg(3), frame.Arg(4));
    return 1;
});
REGISTER_NATIVE_FUNCTION_AS(0x800A44D8, MH3_EffectClosing, "EffectClosing");

MH3_EFFECT_PORT(800A6478, EmitterSendClosing, {
    EffectClosing(game, FlatRead32(frame.Arg(3) + 0xBC), frame.Arg(3));
    return 1;
});
REGISTER_NATIVE_FUNCTION_AS(0x800A6478, MH3_EmitterSendClosing, "EmitterSendClosing");

MH3_EFFECT_PORT(800A65B8, EmitterClosing, {
    EmitterClosing(game, frame.Arg(3), frame.Arg(4));
    return 1;
});
REGISTER_NATIVE_FUNCTION_AS(0x800A65B8, MH3_EmitterClosing, "EmitterClosing");

MH3_EFFECT_PORT(800A6710, EffectRemoveDrawOrder, {
    EffectRemoveDrawOrder(game, frame.Arg(3), frame.Arg(4));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800A6710, MH3_EffectRemoveDrawOrder, "EffectRemoveDrawOrder");

MH3_EFFECT_PORT(800AB794, ParticleManagerSendClosing, {
    EmitterClosing(game, FlatRead32(frame.Arg(3) + 0x20), frame.Arg(3));
    return 1;
});
REGISTER_NATIVE_FUNCTION_AS(0x800AB794, MH3_ParticleManagerSendClosing, "ParticleManagerSendClosing");

MH3_EFFECT_PORT(800AB7A4, ParticleManagerClosing, {
    ParticleManagerClosing(game, frame.Arg(3), frame.Arg(4));
    return 1;
});
REGISTER_NATIVE_FUNCTION_AS(0x800AB7A4, MH3_ParticleManagerClosing, "ParticleManagerClosing");

MH3_EFFECT_PORT(800AA324, ParticleSendClosing, {
    ParticleManagerClosing(game, FlatRead32(frame.Arg(3) + 0xC8), frame.Arg(3));
    return 1;
});
REGISTER_NATIVE_FUNCTION_AS(0x800AA324, MH3_ParticleSendClosing, "ParticleSendClosing");

// The foreach ports call guest callbacks (r3 the manager or emitter, r4 the
// argument).
MH3_EFFECT_PORT(800A9938, EmitterForeachParticleManager, {
    const uint32_t fn = frame.Arg(4), arg = frame.Arg(5);
    return EmitterForEachManager(frame.Arg(3), frame.Arg(6) != 0, frame.Arg(7) != 0, [&](uint32_t manager) { game(fn, {manager, arg}); });
});
REGISTER_NATIVE_FUNCTION_AS(0x800A9938, MH3_EmitterForeachParticleManager, "EmitterForeachParticleManager");

MH3_EFFECT_PORT(800A52BC, EffectForeachParticleManager, {
    const uint32_t fn = frame.Arg(4), arg = frame.Arg(5);
    return EffectForEachManager(frame.Arg(3), frame.Arg(6) != 0, [&](uint32_t manager) { game(fn, {manager, arg}); });
});
REGISTER_NATIVE_FUNCTION_AS(0x800A52BC, MH3_EffectForeachParticleManager, "EffectForeachParticleManager");

// Effect::ForeachEmitterFrom (0x800A5348): `fn` on each emitter (active
// ones unless r6) descending from r7.
MH3_EFFECT_PORT(800A5348, EffectForeachEmitterFrom, {
    const uint32_t list = frame.Arg(3) + 0x24, fn = frame.Arg(4), arg = frame.Arg(5), ancestor = frame.Arg(7);
    const bool anyStatus = frame.Arg(6) != 0;
    uint32_t n = 0;

    for (uint32_t emitter = ListNext(list, 0), next; emitter; emitter = next) {
        next = ListNext(list, emitter);

        if ((anyStatus || LifeStatus(emitter) == 1) && DescendsFrom(emitter, ancestor)) {
            game(fn, {emitter, arg});
            ++n;
        }
    }

    return n;
});
REGISTER_NATIVE_FUNCTION_AS(0x800A5348, MH3_EffectForeachEmitterFrom, "EffectForeachEmitterFrom");

// 0x800A97E8: ForeachEmitterFrom on the emitter's effect from the emitter.
MH3_EFFECT_PORT(800A97E8, EmitterForeachDescendant, { return game.Call<0x800A5348>({FlatRead32(frame.Arg(3) + 0xBC), frame.Arg(4), frame.Arg(5), frame.Arg(6), frame.Arg(3)}); });
REGISTER_NATIVE_FUNCTION_AS(0x800A97E8, MH3_EmitterForeachDescendant, "EmitterForeachDescendant");

// 0x800A97F4: the descendant callback of ForeachParticleManager; r4 its
// context {count, fn, arg, any status (u8)}.
MH3_EFFECT_PORT(800A97F4, EmitterForeachDescendantManagers, {
    const uint32_t emitter = frame.Arg(3), context = frame.Arg(4);
    CheckPointer(emitter, "ef_emitter.cpp", 0x519);
    const uint32_t n = game.Call<0x800A9938>({emitter, FlatRead32(context + 4), FlatRead32(context + 8), FlatRead8(context + 0xC), 0});
    FlatWrite32(context, FlatRead32(context) + n);
    return n;
});
REGISTER_NATIVE_FUNCTION_AS(0x800A97F4, MH3_EmitterForeachDescendantManagers, "EmitterForeachDescendantManagers");

MH3_EFFECT_PORT(800AC164, ParticleManagerSetSimpleLight, {
    ParticleManagerSetSimpleLight(frame.Arg(3), ReadSimpleLight(frame.Arg(4) & 0xFF, frame.Arg(5), frame.Arg(6), float(cpu->fpr[1].d), frame.Arg(7)));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800AC164, MH3_ParticleManagerSetSimpleLight, "ParticleManagerSetSimpleLight");

MH3_EFFECT_PORT(800A523C, EffectGetEmitter, { return EffectGetEmitter(game, frame.Arg(3), frame.Arg(4) & 0xFFFF); });
REGISTER_NATIVE_FUNCTION_AS(0x800A523C, MH3_EffectGetEmitter, "EffectGetEmitter");

// 0x800F9AD4: SetSimpleLight from a context {type, ambient, diffuse, pad,
// radius, position}.
MH3_EFFECT_PORT(800F9AD4, EffectSimpleLightCallback, {
    const uint32_t c = frame.Arg(4);
    ParticleManagerSetSimpleLight(frame.Arg(3), ReadSimpleLight(FlatRead8(c), c + 1, c + 5, FlatReadFloat32(c + 0xC), c + 0x10));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800F9AD4, MH3_EffectSimpleLightCallback, "EffectSimpleLightCallback");

MH3_EFFECT_PORT(800F9A38, EffectSetSimpleLight, {
    EffectSetSimpleLight(frame.Arg(3), ReadSimpleLight(frame.Arg(4) & 0xFF, frame.Arg(5), frame.Arg(6), float(cpu->fpr[1].d), frame.Arg(7)), frame.Arg(8) != 0);
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800F9A38, MH3_EffectSetSimpleLight, "EffectSetSimpleLight");

MH3_EFFECT_PORT(800F99EC, EffectChangeColor, {
    EffectChangeColor(game, frame.Arg(3), frame.Arg(4), frame.Arg(5));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800F99EC, MH3_EffectChangeColor, "EffectChangeColor");

MH3_EFFECT_PORT(800F9B24, EffectChangeScale, {
    EffectChangeScale(cpu, frame.Arg(3), cpu->fpr[1].d);
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800F9B24, MH3_EffectChangeScale, "EffectChangeScale");

MH3_EFFECT_PORT(800F9BCC, EmitterSetScale, {
    EmitterSetScale(cpu, frame.Arg(3), frame.Arg(4));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800F9BCC, MH3_EmitterSetScale, "EmitterSetScale");

MH3_EFFECT_PORT(800F9744, EffectSetRootTrans, {
    EffectSetRootTrans(cpu, frame.Arg(3), frame.Arg(4));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800F9744, MH3_EffectSetRootTrans, "EffectSetRootTrans");

MH3_EFFECT_PORT(800F98E8, EffectHasManagers, { return EffectHasManagers(game, frame.Arg(3)); });
REGISTER_NATIVE_FUNCTION_AS(0x800F98E8, MH3_EffectHasManagers, "EffectHasManagers");
MH3_EFFECT_PORT(800F9984, EffectMove, { return EffectMove(game, frame.Arg(3)); });
REGISTER_NATIVE_FUNCTION_AS(0x800F9984, MH3_EffectMove, "EffectMove");

MH3_EFFECT_PORT(800F99D0, EffectCalcVirtual, {
    EffectCalcVirtual(game, frame.Arg(3), frame.Arg(4));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800F99D0, MH3_EffectCalcVirtual, "EffectCalcVirtual");

MH3_EFFECT_PORT(800FFFFC, EffectGetPosition, { return EffectGetPosition(game, frame.Arg(3), frame.Arg(4)); });
REGISTER_NATIVE_FUNCTION_AS(0x800FFFFC, MH3_EffectGetPosition, "EffectGetPosition");

MH3_EFFECT_PORT(80100088, EmitterGetTranslate, {
    if (!frame.Arg(4))
        Panic("emitter.h", 0x187, "trans != NULL");

    for (uint32_t k = 0; k < 3; ++k)
        FlatWrite32(frame.Arg(4) + 4 * k, FlatRead32(frame.Arg(3) + 0x90 + 4 * k));

    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x80100088, MH3_EmitterGetTranslate, "EmitterGetTranslate");

MH3_EFFECT_PORT(800FFF6C, EffectSpawnerRetireEffect, {
    EffectSpawnerRetireEffect(game, frame.Arg(3), frame.Arg(4));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800FFF6C, MH3_EffectSpawnerRetireEffect, "EffectSpawnerRetireEffect");

MH3_EFFECT_PORT(800F9358, EffectObjectAlive, { return EffectObjectAlive(game, frame.Arg(3), frame.Arg(4)); });
REGISTER_NATIVE_FUNCTION_AS(0x800F9358, MH3_EffectObjectAlive, "EffectObjectAlive");
MH3_EFFECT_PORT(800E3BA0, MHcharDrawPass, { return MHcharDrawPass(game, frame.Arg(3)); });
REGISTER_NATIVE_FUNCTION_AS(0x800E3BA0, MH3_MHcharDrawPass, "MHcharDrawPass");
MH3_EFFECT_PORT(800F93E4, EffectDrawFlags, { return EffectDrawFlags(game, frame.Arg(3)); });
REGISTER_NATIVE_FUNCTION_AS(0x800F93E4, MH3_EffectDrawFlags, "EffectDrawFlags");
MH3_EFFECT_PORT(8026FE38, PlMasterCheck, { return PlMasterCheck(game, frame.Arg(3)); });
REGISTER_NATIVE_FUNCTION_AS(0x8026FE38, MH3_PlMasterCheck, "PlMasterCheck");

MH3_EFFECT_PORT(800FA804, EftKeyRGB, {
    uint8_t rgb[3];
    EftKeyRGB(frame.Arg(3), int32_t(frame.Arg(4)), rgb);

    for (uint32_t c = 0; c < 3; ++c)
        FlatWrite8(frame.Arg(5 + c), rgb[c]);

    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800FA804, MH3_EftKeyRGB, "EftKeyRGB");

MH3_EFFECT_PORT(800FA95C, EftKeyAlpha, { return EftKeyAlpha(game, frame.Arg(3), int32_t(frame.Arg(4))); });
REGISTER_NATIVE_FUNCTION_AS(0x800FA95C, MH3_EftKeyAlpha, "EftKeyAlpha");
