#pragma once
#include "runtime/native/mh3_model_submission.h"
#include "runtime/native/mh3_vertex_sources.h"
#include "runtime/native/mh3_texture_matrices.h"
/* Explicit loader events. Tracking disc reads alone does not certify copied,
 * recycled or decompressed owners. No bind-time revision bumps or hashing. */
void RecordNativeDiscWrite(uint32_t destination, uint64_t partition_offset, uint32_t bytes);
void RecordNativeResourceCopy(uint32_t destination, uint32_t source, uint32_t bytes);
void RetireNativeResourceOwner(uint32_t address, uint32_t bytes);
/* A vertex producer installs this before retaining any sources/revisions.
 * Generic overwrites notify it even when no disc interval is tracked. */
using NativeModelVertexRetireHook = void (*)(uint32_t address, uint32_t bytes);
void SetNativeModelVertexRetireHook(NativeModelVertexRetireHook hook);
/* Copies the game replica of the host draw state into the render replica
 * (draw_replica.h), before game mutations start being mirrored. */
void SeedNativeCatalogReplica();
void SeedNativeTextureObjectReplica();
void SeedNativeVertexReplica();
/* Only a verified native material producer may use these. Exact block bounds
 * preserve the certified allocation/model identity while content revisions
 * change. Generic memcpy/DI/free/unknown overwrite always retires overlapping
 * certificates. Typed field records must be published by the producer too. */
void RecordNativeTypedMaterialWrite(uint32_t address, uint32_t bytes, mh3_model_material_block kind);
void RecordNativeTypedMaterialCopy(uint32_t destination, uint32_t source, uint32_t bytes, mh3_model_material_block kind);
/* Caller owns validated metadata/catalog and resident allocation until explicit
 * unregistration. Backend's asynchronous fence lifetime is a separate owner. */
void RegisterNativeModelPack(const mh3_mesh_pack* pack, const mh3_model_catalog* catalog, uintptr_t resident_handle, uint64_t resident_generation);
void UnregisterNativeModelPack(uintptr_t resident_handle);
/* Conservative demand-loader pin: certified owners, copied typed material
 * groups, or any tracked partial source receipt keep metadata alive. */
bool NativeModelPackHasLiveSources(uintptr_t resident_handle);
struct NativeModelOwner {
    const mh3_mesh_pack* pack;
    const mh3_model_catalog* catalog;
    uint32_t catalog_model_index, container_address, model_address;
    uintptr_t resident_handle;
    uint64_t resident_generation, source_generation;
};
NativeModelOwner ResolveNativeModelOwner(uint32_t model_address);
mh3_catalog_resource ResolveNativeModelResource(const NativeModelOwner& owner, uint32_t resource_address, bool material);
/* Prepared at RegisterNativeModelPack, not lazily decoded during a draw.
 * Returns immutable metadata only; valid until the resident handle is removed.
 * Resource-address/header joins are cached until a provenance event invalidates
 * them. Never stores a pose/material/camera pointer. */
const mh3_prepared_mesh& ReadNativePreparedMesh(const NativeModelOwner& owner, uint32_t shape_address);
using NativePreparedMaterial = mh3_prepared_material;
/* Immutable resource defaults/recipes only. Live replacements/inherited pass
 * state must be merged by typed producers; no live borrowed pointers cached. */
const NativePreparedMaterial& ReadNativePreparedMaterial(const NativeModelOwner& owner, uint32_t material_address);
struct NativeMaterialBlockDefaults {
    const NativePreparedMaterial* prepared;
    uint32_t source_address, source_bytes;
    mh3_model_material_block kind;
    uint64_t source_generation;
};
/* Origin/default scope only. The current producer must separately certify live
 * mutations and supply typed overrides. actual_block may be a proven copy;
 * no packed-list inspection, pointer/name guesses or bind-time hashing. */
NativeMaterialBlockDefaults ReadNativeMaterialBlockDefaults(const NativeModelOwner& owner, uint32_t material_address, mh3_model_material_block kind, uint32_t actual_block);
/* Owned CPU fields with a separate producer revision. No borrowed RAM/material
 * pointer survives an overwrite or resident retirement. First creation joins
 * complete source provenance to admitted typed defaults; later reads are O(1).
 * Writes must call RecordNativeTypedMaterialWrite before publishing. */
struct NativeLiveMaterialBlock {
    mh3_native_material_state fields;
    uint32_t address, bytes;
    mh3_model_material_block kind;
    uintptr_t resident_handle;
    uint64_t resident_generation, origin_offset, source_generation;
};
NativeLiveMaterialBlock ReadNativeLiveMaterialBlock(uint32_t address, mh3_model_material_block kind);
/* Owner-only immediate read view. The cache owns these bytes; consume them
 * before any producer publication, source write/retirement or guest callback.
 * Mutable setters must use ReadNativeLiveMaterialBlock's private snapshot. */
const NativeLiveMaterialBlock& ReadNativeLiveMaterialBlockView(uint32_t address, mh3_model_material_block kind);
/* Mapping belongs to a certified pipeline source extent. Mutable/copy blocks
 * keep the admitted origin; unknown writes retire them. No live command read. */
void ReadNativeModelCoordinateMap(uint32_t pipeline, uint8_t mapping[8], uint8_t* known);
void PublishNativeLiveMaterialBlock(const NativeLiveMaterialBlock& block);
uint64_t NextNativeMaterialRevision();
void CopyNativeLiveMaterialBlock(uint32_t destination, uint32_t source, mh3_model_material_block kind);
/* Explicit mutable vertex producers must advance source revisions when arrays
 * change in place. A bind event only advances binding_generation. */
void MarkNativeVertexProduced(uint32_t payload, uint64_t generation);
mh3_native_vertex_source ReadNativeVertexResource(uint32_t reference, mh3_vertex_kind kind);
mh3_native_vertex_source ReadNativeVertexResourceData(uint32_t resource, mh3_vertex_kind kind);
void StoreNativeVertexSource(uint32_t semantic, const mh3_native_vertex_source& source);
bool ReadNativeVertexSource(uint32_t semantic, mh3_native_vertex_source* out);
