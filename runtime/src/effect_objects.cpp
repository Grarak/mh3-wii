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
//   0x800FF938 EffectSpawnerUpdate         a spawner's state 1: its effects
//                                          spawned (0x800FFD10) on a random
//                                          wait, retired without managers,
//                                          calculated and drawn
//   0x8033065C EffectObjectUpdate          an object's state 1: its effects
//                                          follow the owner's joints
//                                          (0x800E0AF8), coloured and scaled
//                                          by its state
//   0x800F943C EffectDrawRequest           the effects' or models' primitive
//                                          requests (0x800E3BF0) and scene
//                                          root PushBack (0x8007F130)
//   0x802BDD44 GetCameraPos                the view camera's position, with
//                                          0x800473FC, 0x80082C30, 0x80082BD4
//                                          and 0x80074A2C
//   0x802FB9E0 StarQuestFirstStages        the star quest groups' progress,
//                                          with 0x802FB558, 0x802FB520,
//                                          0x802FB414 and 0x802FB498
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
#include "g3d_native.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

// CopiedMatAccess (0x8007BF10) in scn_mdl_tasks.cpp: whether material i's
// access is valid, and its copy of the part with a dirty bit.
bool CopiedMatAccessValid(uint32_t mdl, uint32_t i);
uint32_t CopiedMatAccessPart(uint32_t mdl, uint32_t i, uint32_t bit);

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
constexpr uint32_t kMapNo = 0x806BB805, kAreaNo = 0x806BB806; // u8: get_now_mapno, get_now_areano
constexpr uint32_t kVillageDeparture = 0x806ACF40;            // VillageDepartureSetting
constexpr uint32_t kRoot = 0x80796E34;                        // nw4r::g3d::ScnRoot* pRoot
constexpr uint32_t kQuestFlags = 0x80796D40;                  // the save's quest flags

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

// ran_suu (0x800CEF20, inlined by the translator): random stream `i` (u16s
// at 0x8065AA78), x * 176 mod 65363 with 0 taken as 1.
uint32_t RanSuu(uint32_t i) {
    const uint32_t seed = 0x8065AA78 + 2 * i;
    const uint32_t x = FlatRead16(seed) ? FlatRead16(seed) : 1;
    const uint32_t next = x * 176 % 65363;
    FlatWrite16(seed, uint16_t(next));
    return next;
}

// Whether every star quest of the list (u16s ending 0xFFFF) is unlocked
// (StarQuestUnlocked 0x8004D2E0, which the star quest cheat hooks).
bool StarQuestListDone(GuestCall& game, uint32_t list) {
    if (!list)
        return false;

    for (uint32_t quest; (quest = FlatRead16(list)) != 0xFFFF; list += 2)
        if (!game.Call<0x8004D2E0>({quest}))
            return false;

    return true;
}

// 0x802FB414: a star quest group below 0x1C done (its list at 0x805D9360).
bool StarQuestGroupDoneLow(GuestCall& game, uint32_t group) {
    if (Routed<0x802FB414>())
        return game.Call<0x802FB414>({group}) != 0;

    group &= 0xFFFF;
    return group < 0x1C && StarQuestListDone(game, FlatRead32(0x805D9360 + 4 * group));
}

// 0x802FB498: a group from 0x1C on (lists at 0x805D93D0).
bool StarQuestGroupDoneHigh(GuestCall& game, uint32_t group) {
    if (Routed<0x802FB498>())
        return game.Call<0x802FB498>({group}) != 0;

    group &= 0xFFFF;
    return group >= 0x1C && StarQuestListDone(game, FlatRead32(0x805D93D0 + 4 * (group - 0x1C)));
}

// 0x802FB520: whether the group is done; 0xFFFF never, group 0 always.
bool StarQuestGroupDone(GuestCall& game, uint32_t group) {
    if (Routed<0x802FB520>())
        return game.Call<0x802FB520>({group}) != 0;

    group &= 0xFFFF;

    if (group == 0xFFFF)
        return false;

    if (group == 0)
        return true;

    return group > 0x1B ? StarQuestGroupDoneHigh(game, group) : StarQuestGroupDoneLow(game, group);
}

// 0x802FB558: the group is the current one: done, the next not.
bool StarQuestGroupCurrent(GuestCall& game, uint32_t group) {
    if (Routed<0x802FB558>())
        return game.Call<0x802FB558>({group}) != 0;

    return StarQuestGroupDone(game, group) && !StarQuestGroupDone(game, group + 1);
}

// 0x802FB9E0: group 0 current, or group 0x14 with the quest flags' +0x3E01
// at 4.
bool StarQuestFirstStages(GuestCall& game) {
    if (Routed<0x802FB9E0>())
        return game.Call<0x802FB9E0>() != 0;

    if (StarQuestGroupCurrent(game, 0))
        return true;

    return StarQuestGroupCurrent(game, 0x14) && FlatRead8(FlatRead32(kQuestFlags) + 0x3E01) == 4;
}

// 0x80082C30: the ScnRoot's current camera (+0xF4) as a handle of its
// CameraData (0x10C bytes each from +0xF8).
uint32_t ScnRootCurrentCamera(GuestCall& game, uint32_t root) {
    if (Routed<0x80082C30>())
        return game.Call<0x80082C30>({root});

    return root + FlatRead8(root + 0xF4) * 0x10C + 0xF8;
}

// ScnRoot::GetCamera (0x80082BD4): camera `index`'s, 0 outside 0-31.
uint32_t ScnRootGetCamera(GuestCall& game, uint32_t root, uint32_t index) {
    if (Routed<0x80082BD4>())
        return game.Call<0x80082BD4>({root, index});

    return int32_t(index) < 0 || int32_t(index) > 31 ? 0 : root + index * 0x10C + 0xF8;
}

// 0x800473FC: the camera this machine views through: with split screen
// (0x8065B4D6) its player's (MyPlayerIndex 0x8065AA87, signed, into the
// camera bytes at 0x8065B50C), else the root's current one.
uint32_t ViewCamera(GuestCall& game) {
    if (Routed<0x800473FC>())
        return game.Call<0x800473FC>();

    const uint32_t root = FlatRead32(kRoot);

    if (!FlatRead8(0x8065B4D6))
        return ScnRootCurrentCamera(game, root);

    const int32_t player = int8_t(FlatRead8(0x8065AA87));
    return ScnRootGetCamera(game, root, FlatRead8(0x8065B50C + uint32_t(player)));
}

// Camera::GetPosition (0x80074A2C): the position (+0x74) of the CameraData
// the handle at `camera` holds.
void CameraGetPosition(GuestCall& game, uint32_t camera, uint32_t out) {
    if (Routed<0x80074A2C>())
        return void(game.Call<0x80074A2C>({camera, out}));

    const uint32_t data = FlatRead32(camera);

    if (!data)
        Panic("g3d_camera.cpp", 0xC3, "IsValid()");

    if (out)
        for (uint32_t k = 0; k < 12; k += 4)
            FlatWrite32(out + k, FlatRead32(data + 0x74 + k));
}

// get_camera_pos (0x802BDD44): the view camera's position; the handle in
// the 4 guest bytes at `scratch`.
void GetCameraPos(GuestCall& game, uint32_t out, uint32_t scratch) {
    if (Routed<0x802BDD44>())
        return void(game.Call<0x802BDD44>({out}));

    FlatWrite32(scratch, ViewCamera(game));
    CameraGetPosition(game, scratch, out);
}

// MHchar::get_joint_wpos (0x800E0AF8): the joint's world position, the
// translation of its world matrix (the ScnMdl's (+0x118) array at +0xEC by
// the ResNode's id, ResNode::GetID 0x8006FE30); without a model the default
// word (0x807988F8) thrice.
void GetJointWpos(GuestCall& game, uint32_t chara, uint32_t joint, uint32_t out) {
    if (Routed<0x800E0AF8>())
        return void(game.Call<0x800E0AF8>({chara, joint, out}));

    const uint32_t model = FlatRead32(chara + 0x118);

    if (!model) {
        const uint32_t word = FlatRead32(0x807988F8);

        for (uint32_t k = 0; k < 12; k += 4)
            FlatWrite32(out + k, word);

        return;
    }

    const uint32_t node = G3dNative::ResDicAtOffset14(chara + 0x114, joint);

    if (!node)
        Panic("g3d_resnode_ac.h", 0x53, "IsValid()");

    const uint32_t m = FlatRead32(model + 0xEC) + FlatRead32(node + 0x10) * 0x30;

    for (uint32_t k = 0; k < 3; ++k)
        FlatWrite32(out + 4 * k, FlatRead32(m + 0xC + 0x10 * k));
}

// 0x800A9778: the emitter's particle manager `index`.
uint32_t EmitterGetManager(GuestCall& game, uint32_t emitter, uint32_t index) {
    if (Routed<0x800A9778>())
        return game.Call<0x800A9778>({emitter, index});

    index &= 0xFFFF;

    if (index >= FlatRead16(emitter + 0xC0 + 8))
        Panic("ef_emitter.cpp", 0x505, "idx < GetNumParticleManager()");

    uint32_t manager = ListNext(emitter + 0xC0, 0);

    for (uint32_t i = 0; manager && i < index; ++i)
        manager = ListNext(emitter + 0xC0, manager);

    return manager;
}

// get_stg_eft_col (0x802B035C): the stage's effect colour (RGBA) of `area`
// in the map's StageData (0x802AE5C8; its areas at +4): +0x2C for `which`
// 0, +0x30 for 1, else (and for area 0xFF or none) white.
uint32_t StageEffectColor(GuestCall& game, uint32_t area, uint32_t which) {
    if (Routed<0x802B035C>())
        return game.Call<0x802B035C>({area, which});

    area &= 0xFF;

    if (area == 0xFF)
        return 0xFFFFFFFF;

    const uint32_t stage = game.Call<0x802AE5C8>({FlatRead8(kMapNo)});
    const uint32_t entry = FlatRead32(FlatRead32(stage + 4) + area * 4);

    if (!entry)
        return 0xFFFFFFFF;

    switch (which & 0xFF) {
    case 0:
        return FlatRead32(entry + 0x2C);
    case 1:
        return FlatRead32(entry + 0x30);
    default:
        return 0xFFFFFFFF;
    }
}

void ReadVec3(uint32_t address, float v[3]) {
    for (uint32_t k = 0; k < 3; ++k)
        v[k] = FlatReadFloat32(address + 4 * k);
}

// VECSquareMag (0x80050F40) and VECDotProduct (0x80052278) as their paired
// singles compute them: the x lane fused onto the y product, then z (the
// square: z fused onto x, then y) added.
float SquareMag3(const float v[3]) {
    const float xx = v[0] * v[0];
    const float yy = v[1] * v[1];
    const float zx = PpcFmaFloatInline(v[2], v[2], xx);
    return zx + yy;
}

float Dot3(const float a[3], const float b[3]) {
    const float yy = a[1] * b[1];
    const float zz = a[2] * b[2];
    const float xy = PpcFmaFloatInline(a[0], b[0], yy);
    return xy + zz;
}

// hit_point_sphr (0x8028F680): whether `point` is in the sphere of `radius`
// at `center`, the squares summed as the original (x + y, then z).
bool HitPointSphere(uint32_t point, uint32_t center, float radius) {
    const float dx = FlatReadFloat32(center) - FlatReadFloat32(point);
    const float dy = FlatReadFloat32(center + 4) - FlatReadFloat32(point + 4);
    const float dz = FlatReadFloat32(center + 8) - FlatReadFloat32(point + 8);
    const float zz = dz * dz;
    const float xx = dx * dx;
    const float yy = dy * dy;
    const float xy = xx + yy;
    const float sum = zz + xy;
    const float limit = radius * radius;
    return sum <= limit;
}

// 0x8028F8D0: the squared distance from `point` to the segment from
// `start` along `dir`: its parameter `t` the projection clamped to
// 0x8079C7D8..0x8079C7DC, or 0x8079C7D8 (the start) when the segment's
// squared length is below 0x8079C7D4.
float SegmentPointDistanceSq(const float start[3], const float dir[3], const float point[3], float& t) {
    float v[3];

    for (uint32_t k = 0; k < 3; ++k)
        v[k] = point[k] - start[k];

    const float length2 = SquareMag3(dir);
    t = FlatReadFloat32(0x8079C7D8);

    if (length2 >= FlatReadFloat32(0x8079C7D4)) {
        const float s = Dot3(v, dir) / length2, low = t, high = FlatReadFloat32(0x8079C7DC);
        t = s < low ? low : s > high ? high : s;

        for (uint32_t k = 0; k < 3; ++k) {
            const float w = dir[k] * t;
            v[k] = v[k] - w;
        }
    }

    return SquareMag3(v);
}

// 0x80290820: whether `point` is in the capsule (CapsuleSetup's 0x8028F5BC
// output: start +0, radius +0x18, the segment +0x1C).
bool CapsuleHit(uint32_t point, uint32_t capsule) {
    float start[3], dir[3], p[3], t;
    ReadVec3(capsule, start);
    ReadVec3(capsule + 0x1C, dir);
    ReadVec3(point, p);
    const float distance2 = SegmentPointDistanceSq(start, dir, p, t);
    const float radius = FlatReadFloat32(capsule + 0x18);
    const float limit = radius * radius;
    return distance2 < limit;
}

// 0x800FA26C: whether `point` is in the zone (s8 type +0: 1 a sphere at +4,
// radius +0x10; 2 a capsule from +0x14 to +0x20, radius +0x2C; 3 below the
// height 0x80798AB4).
bool ZoneContains(GuestCall& game, uint32_t point, uint32_t zone) {
    if (Routed<0x800FA26C>())
        return game.Call<0x800FA26C>({point, zone}) == 1;

    switch (int8_t(FlatRead8(zone))) {
    case 1:
        return HitPointSphere(point, zone + 4, FlatReadFloat32(zone + 0x10));
    case 2: {
        // The capsule CapsuleSetup (0x8028F5BC) would make of it.
        float start[3], end[3], dir[3], p[3], t;
        ReadVec3(zone + 0x14, start);
        ReadVec3(zone + 0x20, end);
        ReadVec3(point, p);

        for (uint32_t k = 0; k < 3; ++k)
            dir[k] = end[k] - start[k];

        const float distance2 = SegmentPointDistanceSq(start, dir, p, t);
        const float radius = FlatReadFloat32(zone + 0x2C);
        const float limit = radius * radius;
        return distance2 < limit;
    }
    case 3:
        return FlatReadFloat32(point + 4) < FlatReadFloat32(0x80798AB4);
    default:
        return false;
    }
}

// The ResMatChan or TEV colour copy (dirty bits 8 and 0x100) of the model's
// material: CopiedMatAccess (0x8007BF10) and its 0x8007BC90 / 0x8007BE90,
// 0 without one. `dirty` marks the copy in the ScnMdl's flags (+0x140).
uint32_t CopiedMatPart(uint32_t mdl, uint32_t mat, uint32_t bit, bool dirty) {
    const uint32_t part = CopiedMatAccessPart(mdl, mat, bit);

    if (part && dirty)
        FlatWrite32(FlatRead32(mdl + 0x140) + mat * 4, FlatRead32(FlatRead32(mdl + 0x140) + mat * 4) | bit);

    return part;
}

// ResMatChan::SetColor (0x80096DE0) on the channel data `data`: channel
// `chan`'s (bit 0 the second's, +0x14) rgb, except for the alpha channels
// (bit 1); alpha for those and for GX_COLOR0A0 / GX_COLOR1A1 (4, 5).
void MatChanSetColor(uint32_t data, uint32_t chan, uint32_t color) {
    if (!data)
        Panic("g3d_resmat_ac.h", 0x1D1, "ResMatChan: Object not valid");

    const uint32_t at = data + (chan & 1) * 0x14;

    if (!(chan & 2)) {
        for (uint32_t c = 0; c < 3; ++c)
            FlatWrite8(at + 4 + c, FlatRead8(color + c));

        if (chan - 4 < 2)
            FlatWrite8(at + 7, FlatRead8(color + 3));
    } else {
        FlatWrite8(at + 7, FlatRead8(color + 3));
    }
}

// ResMatChan::GetColor (0x80096EF0): the channel's colour into `out`.
uint32_t MatChanGetColor(uint32_t data, uint32_t chan, uint32_t out) {
    if (!out)
        Panic("g3d_resmat.cpp", 0x903, "Pointer must not be NULL (color)");

    if (!data)
        Panic("g3d_resmat_ac.h", 0x1D1, "ResMatChan: Object not valid");

    FlatWrite32(out, FlatRead32(data + (chan & 1) * 0x14 + 4));
    return 1;
}

// ResMatTevColor::GXGetTevKColor (0x80096424): konst colour `id` (0-3) from
// its two BP commands (10 bytes each from +0x40: the command byte, then the
// big-endian register word: red | alpha << 12, then blue | green << 12)
// into `out`; whether it is set.
uint32_t TevKColorGet(uint32_t data, uint32_t id, uint32_t out) {
    if (!out)
        Panic("g3d_resmat.cpp", 0x654, "Pointer must not be NULL (color)");

    if (id > 3)
        Panic("g3d_resmat.cpp", 0x655, "id is out of bounds");

    if (!data)
        Panic("g3d_resmat_ac.h", 0x17A, "ResMatTevColor: Object not valid");

    const uint32_t base = data + id * 10;

    if (!FlatRead8(base + 0x40))
        return 0;

    const uint32_t ra = FlatRead32(base + 0x41), bg = FlatRead32(base + 0x46);
    FlatWrite8(out, uint8_t(ra));
    FlatWrite8(out + 1, uint8_t(bg >> 12));
    FlatWrite8(out + 2, uint8_t(bg));
    FlatWrite8(out + 3, uint8_t(ra >> 12));
    return 1;
}

// MHchar::setMatColor (0x800E2950): channel `chan`'s colour of the model's
// (+0x118) material `mat`, its copy marked dirty with `dirty`.
void SetMatColor(GuestCall& game, uint32_t chara, uint32_t mat, uint32_t chan, uint32_t color, uint32_t dirty) {
    if (Routed<0x800E2950>())
        return void(game.Call<0x800E2950>({chara, mat, chan, color, dirty}));

    const uint32_t mdl = FlatRead32(chara + 0x118);

    if (mdl && CopiedMatAccessValid(mdl, mat))
        MatChanSetColor(CopiedMatPart(mdl, mat, 8, dirty != 0), chan, color);
}

// MHchar::getMatColor (0x800E2B58): the channel's colour into `out`;
// whether the model has the material.
uint32_t GetMatColor(GuestCall& game, uint32_t chara, uint32_t mat, uint32_t chan, uint32_t out) {
    if (Routed<0x800E2B58>())
        return game.Call<0x800E2B58>({chara, mat, chan, out});

    const uint32_t mdl = FlatRead32(chara + 0x118);

    if (!mdl || !CopiedMatAccessValid(mdl, mat))
        return 0;

    return MatChanGetColor(CopiedMatPart(mdl, mat, 8, false), chan, out);
}

// MHchar::getTevKColor (0x800E2D38).
void GetTevKColor(GuestCall& game, uint32_t chara, uint32_t mat, uint32_t id, uint32_t out) {
    if (Routed<0x800E2D38>())
        return void(game.Call<0x800E2D38>({chara, mat, id, out}));

    const uint32_t mdl = FlatRead32(chara + 0x118);

    if (mdl && CopiedMatAccessValid(mdl, mat))
        TevKColorGet(CopiedMatPart(mdl, mat, 0x100, false), id, out);
}

// MHchar::setTevKColor (0x800E2DC4): the native MatKonstColor (0x80096548)
// on the copy, its handle in the 4 guest bytes at `scratch`. The cache
// store of the copy after it (0x8006F140) has nothing to emulate.
void SetTevKColor(GuestCall& game, uint32_t chara, uint32_t mat, uint32_t id, uint32_t color, uint32_t scratch) {
    if (Routed<0x800E2DC4>())
        return void(game.Call<0x800E2DC4>({chara, mat, id, color}));

    const uint32_t mdl = FlatRead32(chara + 0x118);

    if (!mdl || !CopiedMatAccessValid(mdl, mat))
        return;

    FlatWrite32(scratch, CopiedMatPart(mdl, mat, 0x100, false));
    game.Call<0x80096548>({scratch, id, color});
}

// A zone colour channel: __cvt_fp2unsigned (0x80457360), at most 255.
uint8_t ZoneChannel(float value) {
    const uint32_t u = uint32_t(value);
    return uint8_t(u > 0xFF ? 0xFF : u);
}

// 0x800F9E68: the stage's zone colour (get_stg_eft_col of the area, set
// `which`) times the zone's (+0x30, by 0x80798AB8 as bytes) on an effect
// (kind 0: rgba, a light on its managers at its root) or a model (kind 1:
// rgb also by the object's brightness (+0x15); its materials +0x16 up to
// +0x17 keep their alpha: TEV konst colour 3 with object flag 1 (+0x14),
// else channel GX_COLOR0A0 with flag 2). `scratch`: 0x20 guest bytes.
void ZoneApplyColor(GuestCall& game, uint32_t object, uint32_t target, uint32_t kind, uint32_t zone, uint32_t which, uint32_t scratch) {
    if (Routed<0x800F9E68>())
        return void(game.Call<0x800F9E68>({object, target, kind, zone, which}));

    const uint32_t stage = StageEffectColor(game, FlatRead8(kAreaNo), which & 0xFF);
    const uint32_t color = scratch, read = scratch + 4, position = scratch + 8, copy = scratch + 0x14;
    const float k = FlatReadFloat32(0x80798AB8);

    // The products in the original's order.
    if ((kind & 0xFF) == 1) {
        const float brightness = float(FlatRead8(object + 0x15));

        for (uint32_t c = 0; c < 3; ++c) {
            const float kz = k * float(FlatRead8(zone + 0x30 + c));
            const float scaled = float(stage >> (24 - 8 * c) & 0xFF) * kz;
            const float bright = brightness * scaled;
            FlatWrite8(color + c, ZoneChannel(k * bright));
        }

        FlatWrite8(color + 3, 0xFF);
        const uint32_t flags = FlatRead8(object + 0x14);

        if (!(flags & 3))
            return;

        for (uint32_t m = FlatRead8(object + 0x16); m < FlatRead8(object + 0x17); ++m) {
            if (flags & 1) {
                GetTevKColor(game, target, m, 3, read);
                FlatWrite8(color + 3, FlatRead8(read + 3));
                SetTevKColor(game, target, m, 3, color, copy);
            } else {
                GetMatColor(game, target, m, 4, read);
                FlatWrite8(color + 3, FlatRead8(read + 3));
                FlatWrite32(copy, FlatRead32(color));
                SetMatColor(game, target, m, 4, copy, 0);
            }
        }
    } else if ((kind & 0xFF) == 0) {
        for (uint32_t j = 0; j < 3; ++j)
            FlatWrite32(position + 4 * j, FlatRead32(target + 0x58 + 0xC + 0x10 * j));

        for (uint32_t c = 0; c < 4; ++c) {
            const float kz = k * float(FlatRead8(zone + 0x30 + c));
            FlatWrite8(color + c, ZoneChannel(float(stage >> (24 - 8 * c) & 0xFF) * kz));
        }

        EffectChangeColor(game, target, position, color);
    }
}

// The zones (0x806BDB00, four of 0x34 bytes) holding `point` colour the
// target.
void ApplyZones(GuestCall& game, uint32_t object, uint32_t target, uint32_t kind, uint32_t which, uint32_t point, uint32_t scratch) {
    for (uint32_t zone = 0x806BDB00; zone < 0x806BDB00 + 4 * 0x34; zone += 0x34)
        if (ZoneContains(game, point, zone))
            ZoneApplyColor(game, object, target, kind, zone, which, scratch);
}

// 0x800FA4B4: an effect's zone colour. Its first emitter with particle
// managers decides by its first manager's ResEmitter emit flags (data +8
// +0x8C, byte +3: 0x80 the stage colour set 0, 0x40 set 1, else none); the
// zones holding the effect's root position colour it. `scratch`: 0x30 guest
// bytes.
void EffectZoneColor(GuestCall& game, uint32_t effect, uint32_t scratch) {
    if (Routed<0x800FA4B4>())
        return void(game.Call<0x800FA4B4>({effect}));

    const uint32_t count = FlatRead16(effect + 0x24 + 8);
    uint32_t emitter = 0;

    for (uint32_t i = 0; i < count && !emitter; ++i)
        if (const uint32_t e = EffectGetEmitter(game, effect, i); e && FlatRead16(e + 0xC0 + 8))
            emitter = e;

    if (!emitter)
        return;

    const uint32_t res = FlatRead32(EmitterGetManager(game, emitter, 0) + 0x24);
    CheckPointer(res, "res_emitter_ac.h", 0x5F);
    const uint32_t flags = FlatRead8(res + 8 + 0x8C + 3);

    if (!(flags & 0xC0))
        return;

    for (uint32_t k = 0; k < 3; ++k)
        FlatWrite32(scratch + 4 * k, FlatRead32(effect + 0x58 + 0xC + 0x10 * k));

    ApplyZones(game, 0, effect, 0, flags & 0x80 ? 0 : 1, scratch, scratch + 0x10);
}

// 0x800FA638: a model's zone colour, when each of its materials has the
// copy the object's flags (+0x14) colour (bit 0 TEV colours, bit 1 the
// channels): the zones holding its joint 0 colour it, flag 0x80 picking
// the stage colour set 1. `scratch`: 0x30 guest bytes.
void ModelZoneColor(GuestCall& game, uint32_t object, uint32_t chara, uint32_t scratch) {
    if (Routed<0x800FA638>())
        return void(game.Call<0x800FA638>({object, chara}));

    const int32_t count = int32_t(game.Call<0x800988CC>({chara + 0x114})); // MdlMaterialCount
    const uint32_t flags = FlatRead8(object + 0x14), mdl = FlatRead32(chara + 0x118);

    if (!(flags & 3))
        return;

    for (int32_t i = 0; i < count; ++i)
        if (!CopiedMatAccessPart(mdl, uint32_t(i), flags & 1 ? 0x100 : 8))
            return;

    GetJointWpos(game, chara, 0, scratch);
    ApplyZones(game, object, chara, 1, flags & 0x80 ? 1 : 0, scratch, scratch + 0x10);
}

// 0x800E3BF0: a primitive request in `group` (PrimAlloc 0x800E3C80): kind
// (+0x1D), object (+0x18), type (+0x1C), pass flags (+0x1E, bit 0 with a
// position at +0xC), +0x20 `extra`. The entry, 0 with the group full.
uint32_t PrimRequest(GuestCall& game, uint32_t group, uint32_t kind, uint32_t object, uint32_t type, uint32_t pass, uint32_t position, uint32_t extra) {
    if (Routed<0x800E3BF0>())
        return game.Call<0x800E3BF0>({group, kind, object, type, pass, position, extra});

    const uint32_t entry = game.Call<0x800E3C80>({group & 0xFF});

    if (!entry)
        return 0;

    FlatWrite8(entry + 0x1D, uint8_t(kind));
    FlatWrite32(entry + 0x18, object);
    FlatWrite8(entry + 0x1C, uint8_t(type));
    FlatWrite16(entry + 0x1E, uint16_t(pass));

    if (position) {
        for (uint32_t k = 0; k < 12; k += 4)
            FlatWrite32(entry + 0xC + k, FlatRead32(position + k));

        FlatWrite16(entry + 0x1E, FlatRead16(entry + 0x1E) | 1);
    }

    FlatWrite32(entry + 0x20, extra);
    return entry;
}

// 0x8007F130 (ScnGroup::PushBack): the root's virtual Insert (+0x34) at its
// size (+0xE4).
uint32_t ScnRootPushBack(GuestCall& game, uint32_t root, uint32_t object) {
    if (Routed<0x8007F130>())
        return game.Call<0x8007F130>({root, object});

    return game(FlatRead32(FlatRead32(root) + 0x34), {root, FlatRead32(root + 0xE4), object});
}

// 0x800F943C: the draw requests of `count` effects (flags bit 0) or models
// (bit 1) from `list` while the object is shown (+1) in the current area
// (+0x44): an effect's zone colour (0x800FA4B4) and a type 2 request; a
// model whose ScnMdl (+0x118) is its resource's current one (+0x10C's +4),
// its zone colour (0x800FA638), a type 0 request and the scene root's
// PushBack. Flags 4, 8 and 0x10 pick the passes 8, 0x400 and 0x800. Out of
// the area the models only join the scene.
void EffectDrawRequest(GuestCall& game, uint32_t object, uint32_t list, uint32_t flags, int32_t count, uint32_t position) {
    if (Routed<0x800F943C>())
        return void(game.Call<0x800F943C>({object, list, flags, uint32_t(count), position}));

    const auto drawable = [](uint32_t model) { return model && FlatRead32(model + 0x118) == FlatRead32(FlatRead32(model + 0x10C) + 4); };

    if (FlatRead8(object + 0x44) != FlatRead8(kAreaNo) || !FlatRead8(object + 1)) {
        if (flags & 2)
            for (int32_t i = 0; i < count; ++i)
                if (const uint32_t model = FlatRead32(list + 4 * i); drawable(model))
                    ScnRootPushBack(game, FlatRead32(kRoot), FlatRead32(model + 0x118));

        return;
    }

    uint32_t pass = flags & 4 ? 8 : 0;

    if (flags & 8)
        pass |= 0x400;

    if (flags & 0x10)
        pass |= 0x800;

    if (flags & 1) {
        for (int32_t i = 0; i < count; ++i) {
            if (const uint32_t effect = FlatRead32(list + 4 * i)) {
                game.Call<0x800FA4B4>({effect});
                PrimRequest(game, 1, 1, FlatRead32(list + 4 * i), 2, pass, position, 0);
            }
        }
    }

    if (flags & 2) {
        for (int32_t i = 0; i < count; ++i) {
            if (const uint32_t model = FlatRead32(list + 4 * i); drawable(model)) {
                game.Call<0x800FA638>({object, model});
                const uint32_t current = FlatRead32(list + 4 * i);
                PrimRequest(game, 1, FlatRead8(current + 0x35), FlatRead32(current + 0x118), 0, pass, position, 0);
                ScnRootPushBack(game, FlatRead32(kRoot), FlatRead32(FlatRead32(list + 4 * i) + 0x118));
            }
        }
    }
}

// The spawner's per-map tables of effect ids by area (0x800FFD10's switch).
uint32_t SpawnTables(uint32_t map) {
    switch (map) {
    case 0x01: return 0x8059C750;
    case 0x02: return 0x8059C784;
    case 0x04: return 0x8059C800;
    case 0x06: return 0x8059C834;
    case 0x08: return 0x8059C840;
    case 0x09: return 0x8059C84C;
    case 0x0C: return 0x8059C864;
    case 0x0D: return 0x8059C898;
    case 0x0F: return 0x8059C910;
    case 0x11: return 0x8059C944;
    case 0x13: return 0x8059C950;
    default: return 0;
    }
}

// 0x800FFD10: the spawner's effect `index` created (res_eft_create
// 0x800F9218, group +0xD4) at its matrix (+0x330, 0x30 each): its id from
// the map's table for the spawner's area (+0x44) by the index's byte
// (+0x8D5), else index + base (+0xD0). In area 12 of map 1 the stage flags
// (0x806BD45C + 0xC) bit 5 hold back effects 0xD-0xF and bit 4 0x10-0x11.
void EffectSpawnerSpawn(CpuContext* cpu, GuestCall& game, uint32_t spawner, uint32_t index) {
    if (Routed<0x800FFD10>())
        return void(game.Call<0x800FFD10>({spawner, index}));

    const uint32_t data = FlatRead32(spawner + 0x38), map = FlatRead8(kMapNo), tables = SpawnTables(map);
    const uint32_t table = tables ? FlatRead32(tables + 4 * FlatRead8(spawner + 0x44)) : 0;

    if (map == 1 && FlatRead8(kAreaNo) == 0xC) {
        const uint32_t stage = FlatRead8(0x806BD45C + 0xC);

        if ((index - 0xD <= 2 && (stage & 0x20)) || (index - 0x10 <= 1 && (stage & 0x10)))
            return;
    }

    const uint32_t slot = data + 4 + 4 * index;
    const uint32_t id = table ? FlatRead16(table + 2 * FlatRead8(data + index + 0x8D5)) : (index + FlatRead32(data + 0xD0)) & 0xFFFF;
    FlatWrite32(slot, game.Call<0x800F9218>({id, FlatRead32(data + 0xD4) & 0xFFFF, 1}));

    if (const uint32_t effect = FlatRead32(slot))
        EffectSetRootMtx(cpu, effect, data + index * 0x30 + 0x330);
}

// The spawner's `count` effects from `effects`: each one's Calc and draw
// request, at its position (0x800FFFFC, into `position`) when it has one.
void SpawnerDrawEffects(GuestCall& game, uint32_t spawner, uint32_t effects, int32_t count, uint32_t position) {
    for (int32_t i = 0; i < count; ++i) {
        const uint32_t slot = effects + 4 * i;

        if (!FlatRead32(slot))
            continue;

        EffectCalcVirtual(game, FlatRead32(slot), 0);
        const bool placed = EffectGetPosition(game, FlatRead32(slot), position);
        EffectDrawRequest(game, spawner, slot, 1, 1, placed ? position : 0);
    }
}

// 0x800FF938 EffectSpawnerUpdate, the effect spawner's state 1 (data +0x38:
// effects from +4, count +0; the second set's count +0x7C, effects from
// +0x80; spawn slots of 0x14 bytes from +0xD8, count +0x8D0). Out of its
// area (+0x44) it moves on to state 2. Kind 1 (+7) only draws, its second
// set with VillageDepartureSetting, three effects fewer on map 0x16 in the
// first star stages. Otherwise each slot (u8 index, 0xFF none; +4 its wait,
// +8 its random modulus, +0xC the random extra, +0x10 the frames waited)
// spawns its effect once the wait and extra have passed and retires it
// when it has no particle managers left; then the effects draw, with the
// camera flag (+0x8D4) only while the camera is at or above 0x80798B68.
// `frame` gives the guest scratch.
void EffectSpawnerUpdate(CpuContext* cpu, GuestCall& game, uint32_t spawner, const GuestFrame& frame) {
    const uint32_t data = FlatRead32(spawner + 0x38), position = frame[0x10];

    if (!FlatRead32(kEffectSystem))
        return;

    if (FlatRead8(spawner + 0x44) != FlatRead8(kAreaNo)) {
        FlatWrite8(spawner + 5, FlatRead8(spawner + 5) + 1);
        return;
    }

    if (FlatRead8(spawner + 7) == 1) {
        int32_t first = int32_t(FlatRead32(data)), second = int32_t(FlatRead32(data + 0x7C));

        if (FlatRead8(kMapNo) == 0x16 && StarQuestFirstStages(game)) {
            first -= 3;
            second -= 3;
        }

        if (!FlatRead32(kVillageDeparture))
            SpawnerDrawEffects(game, spawner, data + 4, first, position);
        else
            SpawnerDrawEffects(game, spawner, data + 0x80, second, position);

        return;
    }

    for (int32_t i = 0; i < int32_t(FlatRead32(data + 0x8D0)); ++i) {
        const uint32_t slot = data + 0xD8 + 0x14 * i, index = FlatRead8(slot);

        if (index == 0xFF)
            continue;

        if (const uint32_t effect = FlatRead32(data + 4 + 4 * index)) {
            if (!EffectHasManagers(game, effect))
                EffectSpawnerRetireEffect(game, spawner, index);

            continue;
        }

        const uint32_t waited = FlatRead32(slot + 0x10) + 1;
        FlatWrite32(slot + 0x10, waited);

        if (int32_t(FlatRead32(slot + 4) + FlatRead32(slot + 0xC)) < int32_t(waited)) {
            EffectSpawnerSpawn(cpu, game, spawner, index);
            FlatWrite32(slot + 0x10, 0);
            const int32_t r = int32_t(RanSuu(0) & 0xFFFF), modulus = int32_t(FlatRead32(slot + 8));
            FlatWrite32(slot + 0xC, uint32_t(modulus ? r % modulus : r)); // divw by 0 gives 0
        }
    }

    if (FlatRead8(data + 0x8D4) == 1) {
        GetCameraPos(game, frame[0x20], frame[0x2C]);

        if (!(FlatReadFloat32(0x80798B68) <= FlatReadFloat32(frame[0x24])))
            return;
    }

    SpawnerDrawEffects(game, spawner, data + 4, int32_t(FlatRead32(data)), position);
}

void CopyVec3(uint32_t to, uint32_t from) {
    for (uint32_t k = 0; k < 12; k += 4)
        FlatWrite32(to + k, FlatRead32(from + k));
}

// The y the object's effects sit at: `foot` the joint whose position they
// follow (its y in `position`); the owner's pose (3 flat, 2 its foot's y,
// else by its height from `upper` and `lower`) picks between the object's
// own y (+0x1C) less 0x8079D6DC (or `poseLow`) and the foot's y less
// `drop`.
float EffectObjectHeight(GuestCall& game, uint32_t object, uint32_t part, uint32_t pose, uint32_t foot, uint32_t upper, uint32_t lower, uint32_t position, uint32_t a, uint32_t b,
                         float drop) {
    switch (pose) {
    case 3:
        return FlatReadFloat32(object + 0x1C) - drop;
    case 2:
        GetJointWpos(game, part, foot, a);
        return FlatReadFloat32(a + 4) - drop;
    default:
        break;
    }

    GetJointWpos(game, part, upper, a);
    GetJointWpos(game, part, lower, b);

    // Single steps as the original's fadds, fmuls and fadds (no fused
    // multiply-add).
    const float sum = FlatReadFloat32(a + 4) + FlatReadFloat32(b + 4);
    const float scaled = FlatReadFloat32(0x8079D6D4) * sum;
    const float limit = FlatReadFloat32(0x8079D6D8) + scaled;

    if (FlatReadFloat32(position + 4) <= limit)
        return FlatReadFloat32(object + 0x1C) - FlatReadFloat32(0x8079D6DC);

    GetJointWpos(game, part, foot, a);
    return FlatReadFloat32(a + 4) - drop;
}

// The colour mode (+6) from the owner's state bits: 0x10 1, 0x40 2, both 3.
uint32_t EffectColorMode(uint32_t state) {
    switch (state & 0x50) {
    case 0x10: return 1;
    case 0x40: return 2;
    case 0x50: return 3;
    default: return 0;
    }
}

// 0x8033065C EffectObjectUpdate, an effect object's state 1: the effects
// (data +0x38: count u8 +0, entries of 0x34 bytes from +4: +0 a model, +4
// the effect, +8 its position, +0x14 the colour key time, +0x18 the alpha
// key time, +0x1C/+0x20 the model's rotation, +0x28 the model's GXColor,
// +0x2C scale, +0x30 fading) follow the owner (+0x30; kind +7: 0 a player,
// its part at +0x13C's +4, 2 a monster at +8), coloured by the owner's
// state (+6). A gone owner moves the object on (+5); online mode 2 skips it.
void EffectObjectUpdate(CpuContext* cpu, GuestCall& game, uint32_t object, const GuestFrame& frame) {
    const uint32_t data = FlatRead32(object + 0x38);

    // The owner's positions, the second the draw requests' (for other kinds
    // they are left as the stack had them, as in the original).
    const uint32_t position = frame[0x10], low = frame[0x20], a = frame[0x30], b = frame[0x40], color = frame[0x50], alpha = frame[0x54];

    if (!EffectObjectAlive(game, object, FlatRead8(object + 7))) {
        FlatWrite8(object + 1, 0);
        FlatWrite8(object + 5, FlatRead8(object + 5) + 1);
        return;
    }

    if (FlatRead8(0x8065AA84) == 2) // GetOnlineMode
        return;

    uint32_t flags = 1;
    const uint32_t kind = FlatRead8(object + 7);

    if (kind == 0) {
        const uint32_t player = FlatRead32(object + 0x30);
        FlatWrite8(object + 1, FlatRead8(player + 1));
        FlatWrite8(object + 0x44, FlatRead8(player + 0x16));

        if (FlatRead8(object + 8) == 0xFF || FlatRead8(object + 0x44) != FlatRead8(kAreaNo) || game.Call<0x803C4878>() == 1) // IsEventDemoState4
            return;

        if (PlMasterCheck(game, player))
            flags = EffectDrawFlags(game, player) | 1;

        const uint32_t part = FlatRead32(player + 0x13C) + 4;
        GetJointWpos(game, part, 0, position);
        CopyVec3(object + 0x18, position);
        GetJointWpos(game, part, 0xE, position);
        CopyVec3(low, position);
        const float y = EffectObjectHeight(game, object, part, FlatRead8(player + 9), 0xE, 0x14, 0x11, position, a, b, FlatReadFloat32(0x8079D6D0));
        FlatWriteFloat32(position + 4, y);
        FlatWrite8(object + 6, uint8_t(EffectColorMode(FlatRead32(player + 0x3D8))));

        if (FlatRead32(player + 0x3DC) & 0xF8000)
            FlatWrite8(object + 6, FlatRead8(object + 6) | 0x10);
    } else if (kind == 2) {
        const uint32_t monster = FlatRead32(object + 0x30);
        FlatWrite8(object + 1, FlatRead8(monster + 1));
        FlatWrite8(object + 0x44, FlatRead8(monster + 0x1A4));

        if (FlatRead8(object + 8) == 0xFF || FlatRead8(object + 0x44) != FlatRead8(kAreaNo) || game.Call<0x803C4878>() == 1)
            return;

        if (FlatRead8(monster + 0x171) == 1 && (FlatRead16(monster + 0x172) == 0x53 || FlatRead16(monster + 0x172) == 0x55))
            return;

        const uint32_t part = monster + 8;
        GetJointWpos(game, part, 0, position);
        CopyVec3(object + 0x18, position);
        GetJointWpos(game, part, 3, position);
        CopyVec3(low, position);
        const float y = EffectObjectHeight(game, object, part, FlatRead8(monster + 0x170), 3, 0x14, 0x12, position, a, b, FlatReadFloat32(0x8079D6E0));
        FlatWriteFloat32(position + 4, y);
        FlatWrite8(object + 6, uint8_t(EffectColorMode(FlatRead32(monster + 0x20C))));
    }

    for (int32_t i = 0; i < int32_t(FlatRead8(data)); ++i) {
        const uint32_t entry = data + 4 + 0x34 * i, mode = FlatRead8(object + 6);
        const bool fading = FlatRead8(entry + 0x30) == 1;

        if ((mode >= 0x11 && mode <= 0x13) || mode == 3) {
            FlatWrite32(entry + 0x14, FlatRead32(entry + 0x14) + 4);
            FlatWrite32(entry + 0x18, FlatRead32(entry + 0x18) + (fading ? 6 : 3));
        } else if (mode == 1 || mode == 2 || mode == 0x10) {
            FlatWrite32(entry + 0x14, FlatRead32(entry + 0x14) + 2);
            FlatWrite32(entry + 0x18, FlatRead32(entry + 0x18) + (fading ? 4 : 2));
        }

        if (int32_t(FlatRead32(entry + 0x14)) > 0xF0)
            FlatWrite32(entry + 0x14, 0);

        CopyVec3(entry + 8, position);

        if (int32_t(FlatRead32(entry + 0x18)) > 0x3B) {
            const int32_t shown = int32_t(FlatRead32(object + 0x10)) + 1;
            FlatWrite32(object + 0x10, uint32_t(shown));

            if (shown < 2) {
                FlatWrite8(entry + 0x30, 0);
            } else {
                FlatWrite32(object + 0x10, 0);
                FlatWrite8(entry + 0x30, 1);
            }

            FlatWrite32(entry + 0x20, RanSuu(0) & 0xFFFF);
            FlatWrite32(entry + 0x18, 0);
        }

        EffectSetRootTrans(cpu, FlatRead32(entry + 4), entry + 8);
        uint8_t rgb[3];
        const auto fixed = [&](uint32_t address) {
            for (uint32_t c = 0; c < 3; ++c)
                rgb[c] = FlatRead8(address + c);
        };

        switch (FlatRead8(object + 6)) {
        case 0:
            continue;
        case 1:
            fixed(0x807951E8);
            break;
        case 2:
            fixed(0x807951EC);
            break;
        case 3:
            EftKeyRGB(0x805E0CD0, int32_t(FlatRead32(entry + 0x14)), rgb);
            break;
        case 0x10:
            fixed(0x807951F0);
            break;
        case 0x11:
            EftKeyRGB(0x805E0D00, int32_t(FlatRead32(entry + 0x14)), rgb);
            break;
        case 0x12:
            EftKeyRGB(0x805E0D30, int32_t(FlatRead32(entry + 0x14)), rgb);
            break;
        default: // 0x13
            EftKeyRGB(0x805E0D60, int32_t(FlatRead32(entry + 0x14)), rgb);
            break;
        }

        EffectChangeScale(cpu, FlatRead32(entry + 4), FlatReadFloat32(entry + 0x2C));
        EffectMove(game, FlatRead32(entry + 4));
        FlatWrite32(color, uint32_t(rgb[0]) << 24 | uint32_t(rgb[1]) << 16 | uint32_t(rgb[2]) << 8 | 0xFF);
        EffectChangeColor(game, FlatRead32(entry + 4), entry + 8, color);
        EffectDrawRequest(game, object, entry + 4, flags, 1, low);

        if (FlatRead8(entry + 0x30) != 1)
            continue;

        // The fading model: its alpha by key (0x805E0D98), setMatColor
        // (0x800E2950), its scale (y by the alpha time over 0x8079D6E0),
        // position and rotation, MHchar::move, drawn as a model from here on.
        FlatWrite8(entry + 0x2B, uint8_t(EftKeyAlpha(game, 0x805E0D98, int32_t(FlatRead32(entry + 0x18)))));
        FlatWrite32(alpha, FlatRead32(entry + 0x28));
        SetMatColor(game, FlatRead32(entry), 0, 4, alpha, 0);
        FlatWrite32(FlatRead32(entry) + 0x1C, FlatRead32(entry + 0x2C));
        const float scaled = FlatReadFloat32(entry + 0x2C) * float(int32_t(FlatRead32(entry + 0x18)));
        FlatWriteFloat32(FlatRead32(entry) + 0x20, scaled / FlatReadFloat32(0x8079D6E0));
        const uint32_t model = FlatRead32(entry);
        FlatWrite32(model + 0x24, FlatRead32(entry + 0x2C));
        CopyVec3(model + 4, entry + 8);
        FlatWrite32(model + 0x28, FlatRead32(entry + 0x1C));
        FlatWrite32(model + 0x2C, FlatRead32(entry + 0x20));
        FlatWrite32(model + 0x30, 0);
        game.Call<0x800E078C>({model, 0}); // MHchar::move
        flags = (flags & ~1u) | 2;
        EffectDrawRequest(game, object, entry, flags, 1, 0);
    }
}
} // namespace

// The ports run their bodies in a guest frame of 0x80 bytes (restoring the
// caller's GPRs; scratch from +0x10) and return what the body returns in r3;
// while port checks run, the body's address is g_portBody.
template <uint32_t Address, uint32_t (*Body)(CpuContext*, GuestCall&, const GuestFrame&)>
void FramedPort(CpuContext* cpu) {
    const uint32_t outer = g_portBody;
    const bool checking = PortCheck::Set();

    if (checking)
        g_portBody = Address;

    uint32_t result;
    {
        GuestFrame frame(cpu, 0x80);
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
extern "C" void MH3_EffectObjectUpdate(CpuContext* ctx);
extern "C" void MH3_EffectSpawnerUpdate(CpuContext* ctx);

// The step functions branch to their state's function with the registers as
// they came; state 2 only moves to 3 (r4 the old state, r0 the new).
template <uint32_t Start, void (*Update)(CpuContext*)>
void ObjectStep(CpuContext* cpu) {
    const uint32_t object = cpu->gpr[3];

    switch (const uint8_t state = FlatRead8(object + 5)) {
    case 0:
        InvokeDirectCpu<Start>(cpu);
        break;
    case 1:
        Update(cpu);
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
    ObjectStep<0x80330480, MH3_EffectObjectUpdate>(cpu);
}

extern "C" void func_80330444(CpuContext* ctx);
extern "C" void MH3_EffectObjectStep(CpuContext* ctx) {
    PortCheck::Call(ctx, "EffectObjectStep", func_80330444, EffectObjectStepPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80330444, MH3_EffectObjectStep, "EffectObjectStep");

static void EffectSpawnerStepPort(CpuContext* cpu) {
    ObjectStep<0x800FE9DC, MH3_EffectSpawnerUpdate>(cpu);
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

MH3_EFFECT_PORT(800FF938, EffectSpawnerUpdate, {
    EffectSpawnerUpdate(cpu, game, frame.Arg(3), frame);
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800FF938, MH3_EffectSpawnerUpdate, "EffectSpawnerUpdate");

MH3_EFFECT_PORT(8033065C, EffectObjectUpdate, {
    EffectObjectUpdate(cpu, game, frame.Arg(3), frame);
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x8033065C, MH3_EffectObjectUpdate, "EffectObjectUpdate");

MH3_EFFECT_PORT(800FFD10, EffectSpawnerSpawn, {
    EffectSpawnerSpawn(cpu, game, frame.Arg(3), frame.Arg(4));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800FFD10, MH3_EffectSpawnerSpawn, "EffectSpawnerSpawn");

MH3_EFFECT_PORT(800F943C, EffectDrawRequest, {
    EffectDrawRequest(game, frame.Arg(3), frame.Arg(4), frame.Arg(5), int32_t(frame.Arg(6)), frame.Arg(7));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800F943C, MH3_EffectDrawRequest, "EffectDrawRequest");

MH3_EFFECT_PORT(800E3BF0, PrimRequest, { return PrimRequest(game, frame.Arg(3), frame.Arg(4), frame.Arg(5), frame.Arg(6), frame.Arg(7), frame.Arg(8), frame.Arg(9)); });
REGISTER_NATIVE_FUNCTION_AS(0x800E3BF0, MH3_PrimRequest, "PrimRequest");
MH3_EFFECT_PORT(8007F130, ScnRootPushBack, { return ScnRootPushBack(game, frame.Arg(3), frame.Arg(4)); });
REGISTER_NATIVE_FUNCTION_AS(0x8007F130, MH3_ScnRootPushBack, "ScnRootPushBack");

MH3_EFFECT_PORT(800E0AF8, GetJointWpos, {
    GetJointWpos(game, frame.Arg(3), frame.Arg(4), frame.Arg(5));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800E0AF8, MH3_GetJointWpos, "GetJointWpos");

// ResNode::GetID (0x8006FE30): the id (+0x10) of the ResNode the handle at
// r3 holds.
MH3_EFFECT_PORT(8006FE30, ResNodeGetId, {
    const uint32_t node = FlatRead32(frame.Arg(3));

    if (!node)
        Panic("g3d_resnode_ac.h", 0x53, "IsValid()");

    return FlatRead32(node + 0x10);
});
REGISTER_NATIVE_FUNCTION_AS(0x8006FE30, MH3_ResNodeGetId, "ResNodeGetId");

MH3_EFFECT_PORT(802BDD44, GetCameraPos, {
    GetCameraPos(game, frame.Arg(3), frame[0x10]);
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x802BDD44, MH3_GetCameraPos, "GetCameraPos");

MH3_EFFECT_PORT(800473FC, ViewCamera, { return ViewCamera(game); });
REGISTER_NATIVE_FUNCTION_AS(0x800473FC, MH3_ViewCamera, "ViewCamera");
MH3_EFFECT_PORT(80082C30, ScnRootCurrentCamera, { return ScnRootCurrentCamera(game, frame.Arg(3)); });
REGISTER_NATIVE_FUNCTION_AS(0x80082C30, MH3_ScnRootCurrentCamera, "ScnRootCurrentCamera");
MH3_EFFECT_PORT(80082BD4, ScnRootGetCamera, { return ScnRootGetCamera(game, frame.Arg(3), frame.Arg(4)); });
REGISTER_NATIVE_FUNCTION_AS(0x80082BD4, MH3_ScnRootGetCamera, "ScnRootGetCamera");

MH3_EFFECT_PORT(80074A2C, CameraGetPosition, {
    CameraGetPosition(game, frame.Arg(3), frame.Arg(4));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x80074A2C, MH3_CameraGetPosition, "CameraGetPosition");

MH3_EFFECT_PORT(802FB9E0, StarQuestFirstStages, { return StarQuestFirstStages(game); });
REGISTER_NATIVE_FUNCTION_AS(0x802FB9E0, MH3_StarQuestFirstStages, "StarQuestFirstStages");
MH3_EFFECT_PORT(802FB558, StarQuestGroupCurrent, { return StarQuestGroupCurrent(game, frame.Arg(3)); });
REGISTER_NATIVE_FUNCTION_AS(0x802FB558, MH3_StarQuestGroupCurrent, "StarQuestGroupCurrent");
MH3_EFFECT_PORT(802FB520, StarQuestGroupDone, { return StarQuestGroupDone(game, frame.Arg(3)); });
REGISTER_NATIVE_FUNCTION_AS(0x802FB520, MH3_StarQuestGroupDone, "StarQuestGroupDone");
MH3_EFFECT_PORT(802FB414, StarQuestGroupDoneLow, { return StarQuestGroupDoneLow(game, frame.Arg(3)); });
REGISTER_NATIVE_FUNCTION_AS(0x802FB414, MH3_StarQuestGroupDoneLow, "StarQuestGroupDoneLow");
MH3_EFFECT_PORT(802FB498, StarQuestGroupDoneHigh, { return StarQuestGroupDoneHigh(game, frame.Arg(3)); });
REGISTER_NATIVE_FUNCTION_AS(0x802FB498, MH3_StarQuestGroupDoneHigh, "StarQuestGroupDoneHigh");

MH3_EFFECT_PORT(800A9778, EmitterGetManager, { return EmitterGetManager(game, frame.Arg(3), frame.Arg(4)); });
REGISTER_NATIVE_FUNCTION_AS(0x800A9778, MH3_EmitterGetManager, "EmitterGetManager");
MH3_EFFECT_PORT(802B035C, StageEffectColor, { return StageEffectColor(game, frame.Arg(3), frame.Arg(4)); });
REGISTER_NATIVE_FUNCTION_AS(0x802B035C, MH3_StageEffectColor, "StageEffectColor");
MH3_EFFECT_PORT(800FA26C, ZoneContains, { return ZoneContains(game, frame.Arg(3), frame.Arg(4)) ? 1 : 0; });
REGISTER_NATIVE_FUNCTION_AS(0x800FA26C, MH3_ZoneContains, "ZoneContains");

MH3_EFFECT_PORT(800F9E68, ZoneApplyColor, {
    ZoneApplyColor(game, frame.Arg(3), frame.Arg(4), frame.Arg(5), frame.Arg(6), frame.Arg(7), frame[0x10]);
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800F9E68, MH3_ZoneApplyColor, "ZoneApplyColor");

MH3_EFFECT_PORT(800FA4B4, EffectZoneColor, {
    EffectZoneColor(game, frame.Arg(3), frame[0x10]);
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800FA4B4, MH3_EffectZoneColor, "EffectZoneColor");

MH3_EFFECT_PORT(800FA638, ModelZoneColor, {
    ModelZoneColor(game, frame.Arg(3), frame.Arg(4), frame[0x10]);
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800FA638, MH3_ModelZoneColor, "ModelZoneColor");

MH3_EFFECT_PORT(800E2950, SetMatColor, {
    SetMatColor(game, frame.Arg(3), frame.Arg(4), frame.Arg(5), frame.Arg(6), frame.Arg(7));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800E2950, MH3_SetMatColor, "SetMatColor");

MH3_EFFECT_PORT(800E2B58, GetMatColor, { return GetMatColor(game, frame.Arg(3), frame.Arg(4), frame.Arg(5), frame.Arg(6)); });
REGISTER_NATIVE_FUNCTION_AS(0x800E2B58, MH3_GetMatColor, "GetMatColor");

MH3_EFFECT_PORT(800E2D38, GetTevKColor, {
    GetTevKColor(game, frame.Arg(3), frame.Arg(4), frame.Arg(5), frame.Arg(6));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800E2D38, MH3_GetTevKColor, "GetTevKColor");

MH3_EFFECT_PORT(800E2DC4, SetTevKColor, {
    SetTevKColor(game, frame.Arg(3), frame.Arg(4), frame.Arg(5), frame.Arg(6), frame[0x10]);
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x800E2DC4, MH3_SetTevKColor, "SetTevKColor");

// The ResMatChan and ResMatTevColor accessors take the handle's address.
MH3_EFFECT_PORT(80096DE0, ResMatChanSetColor, {
    MatChanSetColor(FlatRead32(frame.Arg(3)), frame.Arg(4), frame.Arg(5));
    return cpu->gpr[3];
});
REGISTER_NATIVE_FUNCTION_AS(0x80096DE0, MH3_ResMatChanSetColor, "ResMatChanSetColor");

MH3_EFFECT_PORT(80096EF0, ResMatChanGetColor, { return MatChanGetColor(FlatRead32(frame.Arg(3)), frame.Arg(4), frame.Arg(5)); });
REGISTER_NATIVE_FUNCTION_AS(0x80096EF0, MH3_ResMatChanGetColor, "ResMatChanGetColor");
MH3_EFFECT_PORT(80096424, ResTevColorGetKColor, { return TevKColorGet(FlatRead32(frame.Arg(3)), frame.Arg(4), frame.Arg(5)); });
REGISTER_NATIVE_FUNCTION_AS(0x80096424, MH3_ResTevColorGetKColor, "ResTevColorGetKColor");

// The geometry tests write no guest memory but their result (and the
// segment's parameter): checked on every call.
static void HitPointSpherePort(CpuContext* cpu) {
    cpu->gpr[3] = HitPointSphere(cpu->gpr[3], cpu->gpr[4], float(cpu->fpr[1].d)) ? 1 : 0;
}

extern "C" void func_8028F680(CpuContext* ctx);
extern "C" void MH3_HitPointSphere(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("HitPointSphere")) [[unlikely]]
        return PortCheck::RunPure(ctx, "HitPointSphere", PortCheck::Returns::Gpr3, func_8028F680, HitPointSpherePort);
#endif
    HitPointSpherePort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8028F680, MH3_HitPointSphere, "HitPointSphere");

// (segment r3: start +0, direction +0x18; point r4; t out r5) -> f1.
static void SegmentPointDistanceSqPort(CpuContext* cpu) {
    float start[3], dir[3], point[3], t;
    ReadVec3(cpu->gpr[3], start);
    ReadVec3(cpu->gpr[3] + 0x18, dir);
    ReadVec3(cpu->gpr[4], point);
    cpu->fpr[1].d = SegmentPointDistanceSq(start, dir, point, t);
    FlatWriteFloat32(cpu->gpr[5], t);
}

extern "C" void func_8028F8D0(CpuContext* ctx);
extern "C" void MH3_SegmentPointDistanceSq(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("SegmentPointDistanceSq")) [[unlikely]]
        return PortCheck::RunOutput(ctx, "SegmentPointDistanceSq", PortCheck::Returns::Fpr1, func_8028F8D0, SegmentPointDistanceSqPort, {{ctx->gpr[5], 4}});
#endif
    SegmentPointDistanceSqPort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8028F8D0, MH3_SegmentPointDistanceSq, "SegmentPointDistanceSq");

static void CapsuleHitPort(CpuContext* cpu) {
    cpu->gpr[3] = CapsuleHit(cpu->gpr[3], cpu->gpr[4]) ? 1 : 0;
}

extern "C" void func_80290820(CpuContext* ctx);
extern "C" void MH3_CapsuleHit(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("CapsuleHit")) [[unlikely]]
        return PortCheck::RunPure(ctx, "CapsuleHit", PortCheck::Returns::Gpr3, func_80290820, CapsuleHitPort);
#endif
    CapsuleHitPort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80290820, MH3_CapsuleHit, "CapsuleHit");
