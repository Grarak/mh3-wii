// Pure array binding ABI: retain typed source provenance instead of GXSetArray.
#include "abi_bridge.h"
#include "fast_map.h"
#include "guest_span.h"
#include "memory.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <stdexcept>

namespace {
// Bound arrays (game thread draw state).
struct VertexBindings {
    std::array<mh3_native_vertex_source, 12> bindings{};
    std::array<bool, 12> known{};
    uint64_t generation = 0;
};
constinit VertexBindings vertexBindings{};
// Sources bound in place (BindNativeVertexSourcesInPlace): for the semantics
// in `known` the bindings are these, not vertexBindings', until a reader, a
// store, a retirement or the owner's release copies them over.
struct InPlace {
    const mh3_native_vertex_source* sources = nullptr;
    uint16_t known = 0;
};
constinit InPlace inPlace{};
void StoreBinding(uint32_t semantic, const mh3_native_vertex_source& source);
void Materialize() {
    if (!inPlace.known)
        return;
    const InPlace pending = inPlace;
    inPlace = {};
    for (uint32_t i = 0; i < 12; i++)
        if (pending.known & (1u << i))
            StoreBinding(i, pending.sources[i]);
}
struct Revision {
    uint64_t value;
    uint32_t bytes;
};
// Producer revisions.
FastMap<uint32_t, Revision> generations;
FastMap<uint32_t, Revision>& Generations() {
    return generations;
}
uint32_t Canonical(uint32_t address, uint32_t bytes = 1) {
    uint32_t canonical = 0;
    if (mh3_source_canonical_address(address, bytes, &canonical) != MH3_RES_OK)
        throw std::runtime_error("Invalid native vertex source range");
    return canonical;
}
bool Intersects(uint32_t a, uint32_t n, uint32_t b, uint32_t m) {
    return n && m && uint64_t(a) + n > b && uint64_t(b) + m > a;
}
void Retire(uint32_t address, uint32_t bytes) {
    Materialize();
    auto& state = vertexBindings;
    for (uint32_t i = 0; i < 12; i++)
        if (state.known[i]) {
            const auto& source = state.bindings[i];
            if (Intersects(Canonical(source.resource_address), source.resource_bytes, address, bytes) || Intersects(Canonical(source.payload_address), source.payload_bytes, address, bytes))
                state.known[i] = false;
        }
    auto& generations = Generations();
    for (auto it = generations.begin(); it != generations.end();) {
        if (Intersects(it->first, it->second.bytes ? it->second.bytes : 1, address, bytes))
            it = generations.erase(it);
        else
            ++it;
    }
}
void Observe() {
    SetNativeModelVertexRetireHook(Retire);
}
void Bind(CpuContext* cpu, mh3_vertex_kind kind, uint32_t semantic) {
    if (!Memory::Read32(cpu->gpr[3]))
        return; // Original invalid optional resource.
    StoreNativeVertexSource(semantic, ReadNativeVertexResource(cpu->gpr[3], kind));
}
} // namespace
void MarkNativeVertexProduced(uint32_t payload, uint64_t generation) {
    if (!payload || !generation)
        throw std::runtime_error("Invalid native vertex producer generation");
    Observe();
    Materialize();
    payload = Canonical(payload);
    auto& generations = Generations();
    if (generations.contains(payload) && generation < generations.at(payload).value)
        throw std::runtime_error("Invalid native vertex producer generation");
    const uint32_t bytes = generations.contains(payload) ? generations.at(payload).bytes : 0;
    generations.insert_or_assign(payload, Revision{generation, bytes});
    auto& state = vertexBindings;
    for (uint32_t i = 0; i < 12; i++)
        if (state.known[i] && Canonical(state.bindings[i].payload_address) == payload) {
            state.bindings[i].source_generation = generation;
            generations.at(payload).bytes = state.bindings[i].payload_bytes;
        }
}
mh3_native_vertex_source ReadNativeVertexResource(uint32_t reference, mh3_vertex_kind kind) {
    return ReadNativeVertexResourceData(Memory::Read32(reference), kind);
}
mh3_native_vertex_source ReadNativeVertexResourceData(uint32_t resource, mh3_vertex_kind kind) {
    Observe();
    const uint32_t size = Memory::Read32(resource);
    mh3_native_vertex_source source{};
    if (mh3_vertex_source_read({GuestRange(resource, size), size}, resource, kind, &source) != MH3_RES_OK)
        throw std::runtime_error("Invalid native vertex array resource");
    auto& generations = Generations();
    const auto generation = generations.find(Canonical(source.payload_address, source.payload_bytes));
    if (generation != generations.end()) {
        source.source_generation = generation->second.value;
        generation->second.bytes = source.payload_bytes;
    }
    return source;
}
namespace {
void StoreBinding(uint32_t semantic, const mh3_native_vertex_source& source) {
    auto& state = vertexBindings;
    if (semantic >= 12 || !source.payload_address || state.generation == UINT64_MAX)
        throw std::runtime_error("Invalid native vertex source binding");
    // Rebinding the bound array (every draw of a shape does) changes nothing:
    // its source generation is kept current while bound (MarkNativeVertexProduced)
    // and a retirement unbinds it.
    if (state.known[semantic] && std::memcmp(&state.bindings[semantic], &source, offsetof(mh3_native_vertex_source, source_generation)) == 0)
        return;
    Observe();
    state.bindings[semantic] = source;
    state.bindings[semantic].binding_generation = ++state.generation;
    state.known[semantic] = true;
    auto& generations = Generations();
    const auto generation = generations.find(Canonical(source.payload_address, source.payload_bytes));
    if (generation != generations.end()) {
        state.bindings[semantic].source_generation = generation->second.value;
        generation->second.bytes = source.payload_bytes;
    }
}
} // namespace
void StoreNativeVertexSource(uint32_t semantic, const mh3_native_vertex_source& source) {
    Materialize();
    StoreBinding(semantic, source);
}
void BindNativeVertexSourcesInPlace(const mh3_native_vertex_source* sources, uint16_t known) {
    if (!sources || !known || known >> 12)
        throw std::runtime_error("Invalid native vertex source binding");
    // Semantics the previous in-place set binds and this one does not keep
    // their binding: copy those over.
    if (const uint16_t kept = inPlace.known & ~known) {
        const auto* previous = inPlace.sources;
        inPlace = {};
        for (uint32_t i = 0; i < 12; i++)
            if (kept & (1u << i))
                StoreBinding(i, previous[i]);
    }
    inPlace = {sources, known};
}
bool NativeVertexSourcesBoundInPlace(const mh3_native_vertex_source* sources) {
    return inPlace.known && inPlace.sources == sources;
}
void ReleaseNativeVertexSources(const void* begin, const void* end) {
    const auto* at = static_cast<const void*>(inPlace.sources);
    if (inPlace.known && std::less_equal<const void*>()(begin, at) && std::less<const void*>()(at, end))
        Materialize();
}
bool NativeVertexSourcesUnproduced() {
    return Generations().empty();
}
const mh3_native_vertex_source* PeekNativeVertexSource(uint32_t semantic) {
    Materialize();
    if (semantic >= 12 || !vertexBindings.known[semantic])
        return nullptr;
    return &vertexBindings.bindings[semantic];
}
bool ReadNativeVertexSource(uint32_t semantic, mh3_native_vertex_source* out) {
    if (semantic >= 12 || !out)
        return false;
    Materialize();
    const auto& state = vertexBindings;
    if (!state.known[semantic])
        return false;
    *out = state.bindings[semantic];
    return true;
}
extern "C" void MH3_ModelPositionArray(CpuContext* cpu) {
    Bind(cpu, MH3_VERTEX_POSITION, 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x80088E88, MH3_ModelPositionArray, "NativeModelPositionArray");
extern "C" void MH3_ModelNormalArray(CpuContext* cpu) {
    Bind(cpu, MH3_VERTEX_NORMAL, 1);
}
REGISTER_NATIVE_FUNCTION_AS(0x80089000, MH3_ModelNormalArray, "NativeModelNormalArray");
extern "C" void MH3_ModelColorArray(CpuContext* cpu) {
    const uint32_t slot = cpu->gpr[4];
    if (slot == 11 || slot == 12)
        Bind(cpu, MH3_VERTEX_COLOR, slot - 9);
}
REGISTER_NATIVE_FUNCTION_AS(0x80089178, MH3_ModelColorArray, "NativeModelColorArray");
extern "C" void MH3_ModelFurPositionArray(CpuContext* cpu) {
    const uint32_t resource = Memory::Read32(cpu->gpr[3]);
    if (!resource)
        return;
    const uint32_t size = Memory::Read32(resource);
    mh3_native_vertex_source source{};
    if (mh3_vertex_fur_source_read({GuestRange(resource, size), size}, resource, uint16_t(cpu->gpr[4]), &source) != MH3_RES_OK)
        throw std::runtime_error("Invalid native fur position sample");
    auto& generations = Generations();
    const auto generation = generations.find(Canonical(source.payload_address, source.payload_bytes));
    if (generation != generations.end()) {
        source.source_generation = generation->second.value;
        generation->second.bytes = source.payload_bytes;
    }
    StoreNativeVertexSource(0, source);
}
REGISTER_NATIVE_FUNCTION_AS(0x8008961C, MH3_ModelFurPositionArray, "NativeModelFurPositionArray");
