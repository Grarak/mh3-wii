// nw4r::ef::Resource (ef_resource.cpp), RMHP08: the effect library's lookups
// of emitters and textures by name, native with a name index per project.
//
// The game creates every effect by name (res_eft_create, 0x800F9228, looks up
// the emitter name of its effect table entry), and the originals answer each
// lookup with a strcmp walk over the project's name table; the relink after
// each effect package load looks up three textures per emitter the same way.
// The ports hash the name once and probe an index of each project's table,
// built on the project's first lookup. Every change of the project lists goes
// through the ported Initialize/Add/Remove functions below, which drop the
// indexes, so a lookup never sees a project's table from before a change.
//
// The Resource singleton (0x80696A18, returned by 0x800B28DC):
//   +0x00 ut::List of EffectProject (BREFF),  +0x0C emitter count
//   +0x10 ut::List of TextureProject (BREFT), +0x1C texture count
// ut::List: +0 head, +4 tail, +8 u16 count, +0xA u16 link offset (a link is
// {prev, next}). A project starts with its header size; the name table at
// project + header size is {u32 size; u16 count; u16 pad; entries}, an entry
// {u16 length; char name[length]; u32 offset; u32 size}, the offset relative
// to the table.
//
//   0x800B2918 Initialize               both lists empty, counts 0
//   0x800B2E5C _FindEmitter(project, name)  table + offset of the first entry
//   0x800B3440 _FindTexture(project, name)  named name, or 0
//   0x800B3A78 FindEmitter(res, name, hint) the hint project first, then the
//                                           projects from the last added
//   0x800B3C08 FindTexture(res, name, hint) the same, 0 for an empty name
//   0x800B36D4 AddEffectProject(res, data)  checks the REFF header, appends
//   0x800B3EE4 AddTextureProject(res, data) and relocates the project
//   0x800B4724 RemoveEffectProject(res, project)  1 when it was listed
//   0x800B48FC RemoveTextureProject(res, project)
//   0x800B48A0 RemoveAllEffectProjects(res), 0x800B4A78 RemoveAllTextureProjects(res)
//
// MH3_PORT_CHECK=EfFindEmitter,EfFindTexture,EfFindEmitterInProject,
// EfFindTextureInProject checks the lookups on every call (the returned r3);
// EfResourceLists checks the list changes against their originals.
#include "abi_bridge.h"
#include "fatal.h"
#include "game_step.h"
#include "guest_call.h"
#include "guest_flat_memory.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"
#include "runtime/native/adapters/model_state_bridge.h"

#include <aurora/fast_map.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#ifdef MH3_DEVELOPMENT
#include "dev_options.h"
#endif

extern "C" void func_800B2918(CpuContext* ctx);
extern "C" void func_800B2E5C(CpuContext* ctx);
extern "C" void func_800B3440(CpuContext* ctx);
extern "C" void func_800B3A78(CpuContext* ctx);
extern "C" void func_800B3C08(CpuContext* ctx);
extern "C" void func_800B36D4(CpuContext* ctx);
extern "C" void func_800B3EE4(CpuContext* ctx);
extern "C" void func_800B4724(CpuContext* ctx);
extern "C" void func_800B48FC(CpuContext* ctx);
extern "C" void func_800B48A0(CpuContext* ctx);
extern "C" void func_800B4A78(CpuContext* ctx);

namespace {

using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatRead8;
using MemoryInline::FlatWrite32;

// Guest functions the list changes still call.
enum : uint32_t {
    kListAppend = 0x805020E4,        // ut::List_Append(list, object)
    kListRemove = 0x80502274,        // ut::List_Remove(list, object)
    kRelocateEffects = 0x800B2960,   // (EffectProject*): entry names, track tables
    kRelocateTextures = 0x800B32A4,  // (TextureProject*): entry names, data pointers
};

[[noreturn]] void Panic(const char* what, uint32_t value) {
    char text[128];
    std::snprintf(text, sizeof text, "NW4R panic: ef_resource.cpp: %s (%08x)", what, value);
    MH3_THROW(std::runtime_error(text));
}

// NW4R_ASSERT_VALID_PTR always passes in the port; the pointer is folded to
// its cached alias instead.
void FoldPointer(uint32_t& p) {
    p = GuestFlat::CachedAlias(p);
}

const char* HostString(uint32_t address) {
    return reinterpret_cast<const char*>(GuestFlat::Host(address));
}

// FNV-1a over the name up to its terminator.
uint32_t NameHash(const char* name) {
    uint32_t h = 2166136261u;

    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p)
        h = (h ^ *p) * 16777619u;

    return h;
}

// One project's name table: open addressing over the first entry of each
// name, in table order, as the originals' walk finds it.
struct ProjectIndex {
    struct Slot {
        uint32_t hash;
        uint32_t name; // guest address, 0 for an empty slot
        uint32_t result;
    };

    std::vector<Slot> slots;
    uint32_t mask = 0;

    void Build(uint32_t project) {
        const uint32_t table = project + FlatRead32(project);
        const uint32_t count = FlatRead16(table + 4);
        uint32_t size = 8;

        while (size < count * 2)
            size *= 2;

        slots.assign(size, Slot{0, 0, 0});
        mask = size - 1;
        uint32_t entry = table;

        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t name = entry + 10;
            entry = name + FlatRead16(entry + 8);
            const uint32_t result = table + FlatRead32(entry);
            const char* text = HostString(name);
            const uint32_t hash = NameHash(text);

            for (uint32_t k = hash & mask;; k = (k + 1) & mask) {
                Slot& slot = slots[k];

                if (!slot.name) {
                    slot = Slot{hash, name, result};
                    break;
                }

                if (slot.hash == hash && !std::strcmp(HostString(slot.name), text))
                    break; // a later duplicate: the walk stops at the first
            }
        }
    }

    uint32_t Find(const char* name, uint32_t hash) const {
        for (uint32_t k = hash & mask;; k = (k + 1) & mask) {
            const Slot& slot = slots[k];

            if (!slot.name)
                return 0;

            if (slot.hash == hash && !std::strcmp(HostString(slot.name), name))
                return slot.result;
        }
    }
};

// The indexes by project address. Dropped whenever a project list changes;
// the game thread is the only caller.
FastMap<uint32_t, ProjectIndex> g_indexes;
uint32_t g_lastProject = 0;
const ProjectIndex* g_lastIndex = nullptr;

void DropIndexes() {
    g_indexes.clear();
    g_lastProject = 0;
    g_lastIndex = nullptr;
}

const ProjectIndex& IndexOf(uint32_t project) {
    if (project == g_lastProject) [[likely]]
        return *g_lastIndex;
    auto [it, inserted] = g_indexes.try_emplace(project);

    if (inserted)
        it->second.Build(project);

    g_lastProject = project;
    g_lastIndex = &it->second;
    return it->second;
}

// _FindEmitter / _FindTexture: the pointer checks, then the index.
uint32_t FindInProject(uint32_t project, uint32_t name) {
    FoldPointer(project);
    FoldPointer(name);
    const char* text = HostString(name);
    return IndexOf(project).Find(text, NameHash(text));
}

// FindEmitter / FindTexture over the list at `list`: the hint project, then
// every project from the tail (the hint again among them, as the original).
uint32_t FindInList(uint32_t list, uint32_t name, uint32_t hint) {
    const char* text = HostString(name);
    const uint32_t hash = NameHash(text);

    if (hint) {
        FoldPointer(hint);

        if (const uint32_t result = IndexOf(hint).Find(text, hash))
            return result;
    }

    const uint32_t linkOffset = FlatRead16(list + 10);

    for (uint32_t project = FlatRead32(list + 4); project; project = FlatRead32(project + linkOffset)) {
        FoldPointer(project);

        if (const uint32_t result = IndexOf(project).Find(text, hash))
            return result;
    }

    return 0;
}

uint32_t FindEmitter(uint32_t resource, uint32_t name, uint32_t hint) {
    FoldPointer(name);
    return FindInList(resource, name, hint);
}

uint32_t FindTexture(uint32_t resource, uint32_t name, uint32_t hint) {
    FoldPointer(name);

    if (!FlatRead8(name))
        return 0;

    return FindInList(resource + 0x10, name, hint);
}

// The list changes. `list` is the Resource's effect (+0) or texture (+0x10)
// list, its count at list + 0xC.
struct ProjectKind {
    uint32_t magic;    // "REFF" / "REFT"
    uint32_t relocate; // the guest relocation of a new project
};

constexpr ProjectKind kEffects{0x52454646u, kRelocateEffects};
constexpr ProjectKind kTextures{0x52454654u, kRelocateTextures};

uint32_t ProjectCount(uint32_t project) {
    FoldPointer(project);
    return FlatRead16(project + FlatRead32(project) + 4);
}

void Initialize(uint32_t resource) {
    for (uint32_t list : {resource, resource + 0x10}) {
        FlatWrite32(list + 0, 0);
        FlatWrite32(list + 4, 0);
        MemoryInline::FlatWrite16(list + 8, 0);
        MemoryInline::FlatWrite16(list + 10, 4);
        FlatWrite32(list + 0xC, 0);
    }

    DropIndexes();
}

// The file header (magic, byte-order mark 0xFEFF, version 11, header size),
// then the first block (magic) holding the project at +8.
uint32_t AddProject(CpuContext* ctx, uint32_t list, uint32_t data, const ProjectKind& kind) {
    FoldPointer(data);

    if (FlatRead32(data) != kind.magic)
        Panic("strncmp(fhd->signature) failed", data);

    if (FlatRead8(data + 4) != 0xFE || FlatRead8(data + 5) != 0xFF)
        Panic("fhd->byteOrder", data);

    if (FlatRead16(data + 6) != 11)
        Panic("fhd->version", data);

    const uint32_t block = data + FlatRead16(data + 12);

    if (FlatRead32(block) != kind.magic)
        Panic("strncmp(bhd->kind) failed", block);

    const uint32_t project = block + 8;
    FlatWrite32(project + 4, 0);
    FlatWrite32(project + 8, 0);
    GuestCall call{ctx};
    call(kListAppend, {list, project});
    FlatWrite32(list + 0xC, FlatRead32(list + 0xC) + ProjectCount(project));
    DropIndexes();
    call(kind.relocate, {project});
    return project;
}

uint32_t RemoveProject(CpuContext* ctx, uint32_t list, uint32_t project) {
    FoldPointer(project);
    const uint32_t linkOffset = FlatRead16(list + 10);

    for (uint32_t p = FlatRead32(list); p; p = FlatRead32(p + linkOffset + 4)) {
        if (p != project)
            continue;

        GuestCall{ctx}(kListRemove, {list, project});
        FlatWrite32(list + 0xC, FlatRead32(list + 0xC) - ProjectCount(project));
        DropIndexes();
        return 1;
    }

    return 0;
}

uint32_t RemoveAllProjects(CpuContext* ctx, uint32_t list) {
    GuestCall call{ctx};

    while (const uint32_t head = FlatRead32(list))
        call(kListRemove, {list, head});

    FlatWrite32(list + 0xC, 0);
    DropIndexes();
    return 1;
}

void InitializePort(CpuContext* ctx) {
    Initialize(ctx->gpr[3]);
}

void AddEffectProjectPort(CpuContext* ctx) {
    ctx->gpr[3] = AddProject(ctx, ctx->gpr[3], ctx->gpr[4], kEffects);
}

void AddTextureProjectPort(CpuContext* ctx) {
    ctx->gpr[3] = AddProject(ctx, ctx->gpr[3] + 0x10, ctx->gpr[4], kTextures);
}

void RemoveEffectProjectPort(CpuContext* ctx) {
    ctx->gpr[3] = RemoveProject(ctx, ctx->gpr[3], ctx->gpr[4]);
}

void RemoveTextureProjectPort(CpuContext* ctx) {
    ctx->gpr[3] = RemoveProject(ctx, ctx->gpr[3] + 0x10, ctx->gpr[4]);
}

void RemoveAllEffectProjectsPort(CpuContext* ctx) {
    ctx->gpr[3] = RemoveAllProjects(ctx, ctx->gpr[3]);
}

void RemoveAllTextureProjectsPort(CpuContext* ctx) {
    ctx->gpr[3] = RemoveAllProjects(ctx, ctx->gpr[3] + 0x10);
}

void ListChange(CpuContext* ctx, void (*original)(CpuContext*), void (*port)(CpuContext*)) {
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("EfResourceLists");

    if (check) [[unlikely]] {
        PortCheck::Run(ctx, "EfResourceLists", original, port);
        DropIndexes();
        return;
    }
#else
    (void)original;
#endif
    port(ctx);
}

// A texture project's images and palettes are new bytes, often where another
// project's were: give each a new generation, which the effect draw records
// with its binding, so the renderer converts them instead of reusing what it
// converted from that address before. The relocation (0x800B32A4) pointed
// each texture's image (+0x1C) and palette (+0x20) into its data.
void ProduceTextures(uint32_t project) {
    const uint32_t table = project + FlatRead32(project);
    uint32_t entry = table + 8;

    for (uint32_t i = 0, count = FlatRead16(table + 4); i < count; ++i) {
        const uint32_t offset = entry + 2 + FlatRead16(entry);
        const uint32_t texture = table + FlatRead32(offset);
        const uint8_t format = FlatRead8(texture + 0xC);
        ProduceNativeTextureSource(FlatRead32(texture + 0x1C));

        if (format >= 8 && format <= 10)
            ProduceNativeTextureSource(FlatRead32(texture + 0x20));

        entry = offset + 8;
    }
}

void FindEmitterInProjectPort(CpuContext* p) {
    p->gpr[3] = FindInProject(p->gpr[3], p->gpr[4]);
}

void FindEmitterPort(CpuContext* p) {
    p->gpr[3] = FindEmitter(p->gpr[3], p->gpr[4], p->gpr[5]);
}

void FindTexturePort(CpuContext* p) {
    p->gpr[3] = FindTexture(p->gpr[3], p->gpr[4], p->gpr[5]);
}

// A lookup returning r3, checked with RunPure.
#ifdef MH3_DEVELOPMENT
#define MH3_LOOKUP_CHECK(Name, Addr, Port)                                                                                                                                                             \
    const bool check = PortCheck::Enabled(#Name);                                                                                                                                                      \
    if (check) [[unlikely]] {                                                                                                                                                                          \
        CpuContext* c = TryGetCpuContext();                                                                                                                                                            \
        c->gpr[1] = r1;                                                                                                                                                                                \
        c->gpr[3] = r3;                                                                                                                                                                                \
        c->gpr[4] = r4;                                                                                                                                                                                \
        c->gpr[5] = r5;                                                                                                                                                                                \
        PortCheck::RunPure(c, #Name, PortCheck::Returns::Gpr3, func_##Addr, Port);                                                                                                                     \
        return c->gpr[3];                                                                                                                                                                              \
    }
#else
#define MH3_LOOKUP_CHECK(Name, Addr, Port) (void)r1;
#endif

uint32_t FindEmitterInProjectChecked(uint32_t r1, uint32_t r3, uint32_t r4, [[maybe_unused]] uint32_t r5) {
    MH3_LOOKUP_CHECK(EfFindEmitterInProject, 800B2E5C, FindEmitterInProjectPort)
    return FindInProject(r3, r4);
}

uint32_t FindTextureInProjectChecked(uint32_t r1, uint32_t r3, uint32_t r4, [[maybe_unused]] uint32_t r5) {
    MH3_LOOKUP_CHECK(EfFindTextureInProject, 800B3440, FindEmitterInProjectPort)
    return FindInProject(r3, r4);
}

uint32_t FindEmitterChecked(uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5) {
    MH3_LOOKUP_CHECK(EfFindEmitter, 800B3A78, FindEmitterPort)
    return FindEmitter(r3, r4, r5);
}

uint32_t FindTextureChecked(uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5) {
    MH3_LOOKUP_CHECK(EfFindTexture, 800B3C08, FindTexturePort)
    return FindTexture(r3, r4, r5);
}

} // namespace

extern "C" void MH3_EfFindEmitterInProject(CpuContext* ctx) {
    ctx->gpr[3] = FindEmitterInProjectChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x800B2E5C, MH3_EfFindEmitterInProject, "EfFindEmitterInProject");

extern "C" void MH3_EfFindTextureInProject(CpuContext* ctx) {
    ctx->gpr[3] = FindTextureInProjectChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x800B3440, MH3_EfFindTextureInProject, "EfFindTextureInProject");

extern "C" void MH3_EfFindEmitter(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    const uint32_t name = ctx->gpr[4], sp = ctx->gpr[1];
#endif
    ctx->gpr[3] = FindEmitterChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
#ifdef MH3_DEVELOPMENT
    // MH3_TRACE=particles: every emitter looked up by name (each effect the
    // game creates), its data (the particle managers' resource) and the
    // return addresses of the five frames above, from the stack's back chain.
    static const TraceFlag trace("particles");

    if (trace) {
        char text[48] = {};

        for (uint32_t i = 0; i + 1 < sizeof text && Memory::Read8(name + i); ++i)
            text[i] = char(Memory::Read8(name + i));

        uint32_t callers[5] = {}, frame = Memory::Read32(sp);

        for (uint32_t& lr : callers) {
            if (!frame)
                break;

            lr = Memory::Read32(frame + 4);
            frame = Memory::Read32(frame);
        }

        std::fprintf(stderr, "MH3 effect emitter: tick %.0f name %s data %08x callers %08x %08x %08x %08x %08x\n", std::floor(GameStep::Elapsed() + 1e-9), text, ctx->gpr[3], callers[0],
                     callers[1], callers[2], callers[3], callers[4]);
    }
#endif
}
REGISTER_NATIVE_FUNCTION_AS(0x800B3A78, MH3_EfFindEmitter, "EfFindEmitter");

extern "C" void MH3_EfFindTexture(CpuContext* ctx) {
    ctx->gpr[3] = FindTextureChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800B3C08, MH3_EfFindTexture, "EfFindTexture");

extern "C" void MH3_EfResourceInitialize(CpuContext* ctx) {
    ListChange(ctx, func_800B2918, InitializePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800B2918, MH3_EfResourceInitialize, "EfResourceInitialize");

extern "C" void MH3_EfAddEffectProject(CpuContext* ctx) {
    ListChange(ctx, func_800B36D4, AddEffectProjectPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800B36D4, MH3_EfAddEffectProject, "EfAddEffectProject");

extern "C" void MH3_EfAddTextureProject(CpuContext* ctx) {
    ListChange(ctx, func_800B3EE4, AddTextureProjectPort);
    ProduceTextures(ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800B3EE4, MH3_EfAddTextureProject, "EfAddTextureProject");

extern "C" void MH3_EfRemoveEffectProject(CpuContext* ctx) {
    ListChange(ctx, func_800B4724, RemoveEffectProjectPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800B4724, MH3_EfRemoveEffectProject, "EfRemoveEffectProject");

extern "C" void MH3_EfRemoveTextureProject(CpuContext* ctx) {
    ListChange(ctx, func_800B48FC, RemoveTextureProjectPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800B48FC, MH3_EfRemoveTextureProject, "EfRemoveTextureProject");

extern "C" void MH3_EfRemoveAllEffectProjects(CpuContext* ctx) {
    ListChange(ctx, func_800B48A0, RemoveAllEffectProjectsPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800B48A0, MH3_EfRemoveAllEffectProjects, "EfRemoveAllEffectProjects");

extern "C" void MH3_EfRemoveAllTextureProjects(CpuContext* ctx) {
    ListChange(ctx, func_800B4A78, RemoveAllTextureProjectsPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800B4A78, MH3_EfRemoveAllTextureProjects, "EfRemoveAllTextureProjects");
