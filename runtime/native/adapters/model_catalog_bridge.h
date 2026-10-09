#pragma once
#include <memory>
#include <vector>
#include "runtime/native/mh3_model_submission.h"
#include "runtime/native/mh3_vertex_sources.h"
#include "runtime/native/mh3_texture_matrices.h"
#include "runtime/native/adapters/model_prepare_diagnostics.h"
/* Explicit loader events. Tracking disc reads alone does not certify copied,
 * recycled or decompressed owners. No bind-time revision bumps or hashing. */
void RecordNativeDiscWrite(uint32_t destination, uint64_t partition_offset, uint32_t bytes, uint32_t diagnostic_frame = UINT32_MAX);
void RecordNativeResourceCopy(uint32_t destination, uint32_t source, uint32_t bytes,
    NativeSourceEpochProducer producer = NativeSourceEpochProducer::ResourceCopy, uint32_t diagnostic_frame = UINT32_MAX);
void RetireNativeResourceOwner(uint32_t address, uint32_t bytes,
    NativeSourceEpochProducer producer = NativeSourceEpochProducer::ResourceRetire, uint32_t diagnostic_frame = UINT32_MAX);
/* The structural producer token. Changes on source/pack lifetime
 * events, never a draw or a typed material-field update. */
uint64_t ReadNativeModelStructureRevision();
/* Advances on every typed material group write or copy. With the structure
 * revision unchanged, every live material block keeps its node and version. */
uint64_t ReadNativeMaterialBlockRevision();
/* Advances whenever a live material block node is erased or fully
 * (re)published. While it is unchanged every live block keeps its node; an
 * in-place producer update (WritableNativeLiveMaterialBlock) changes only the
 * node's fields and fields.generation. */
uint64_t ReadNativeLiveMaterialRevision();
/* A vertex producer installs this before retaining any sources/revisions.
 * Generic overwrites notify it even when no disc interval is tracked. */
using NativeModelVertexRetireHook = void (*)(uint32_t address, uint32_t bytes);
void SetNativeModelVertexRetireHook(NativeModelVertexRetireHook hook);
/* Only a verified native material producer may use these. Exact block bounds
 * preserve the certified allocation/model identity while content revisions
 * change. Generic memcpy/DI/free/unknown overwrite always retires overlapping
 * certificates. Typed field records must be published by the producer too. */
void RecordNativeTypedMaterialWrite(uint32_t address, uint32_t bytes, mh3_model_material_block kind);
void RecordNativeTypedMaterialCopy(uint32_t destination, uint32_t source, uint32_t bytes, mh3_model_material_block kind);
/* Caller owns validated metadata/catalog and resident allocation until explicit
 * unregistration. Backend's asynchronous fence lifetime is a separate owner. */
void RegisterNativeModelPack(const mh3_mesh_pack* pack, const mh3_model_catalog* catalog, uintptr_t resident_handle, uint64_t resident_generation);
void UnregisterNativeModelPack(uintptr_t resident_handle, uint32_t diagnostic_frame = UINT32_MAX);
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
/* Per-model draw tables (built by model_scope.cpp): the resolved material
 * scopes and shape sources of one certified model, by draw-stream material
 * and shape ID. A table belongs to its model's owner certificate: whatever
 * retires or replaces that certificate (an overwrite, a copy over it, the
 * allocation's retirement, the pack's unregistration) drops the table with
 * it, and a copied certificate starts without one. Game thread. */
struct NativeModelDrawTable;
/* The table of the certified model at model_address, or null. */
std::shared_ptr<NativeModelDrawTable> FindNativeModelDrawTable(uint32_t model_address);
/* Attaches a table to owner's certificate, which must be the current one. */
void AttachNativeModelDrawTable(const NativeModelOwner& owner, std::shared_ptr<NativeModelDrawTable> table);
/* Certified owners that have no table yet. */
void ListNativeModelOwnersWithoutDrawTable(std::vector<NativeModelOwner>& out);
/* Certifies the models of every complete model file the provenance tracker
 * holds (admitting their packs), as a first draw would; for loading screens.
 * Returns the number of models certified. */
size_t CertifyLoadedNativeModels();
size_t CountNativeModelOwners();
/* Prepared at RegisterNativeModelPack, not lazily decoded during a draw.
 * Returns immutable metadata only; valid until the resident handle is removed.
 * Resource-address/header joins are cached until a provenance event invalidates
 * them. Never stores a pose/material/camera pointer. */
const mh3_prepared_mesh& ReadNativePreparedMesh(const NativeModelOwner& owner, uint32_t shape_address);
/* The prepared mesh at a pack index of the owner's resident pack, or null. */
const mh3_prepared_mesh* ReadNativePreparedMeshAt(const NativeModelOwner& owner, uint32_t pack_index);
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
/* An immutable copy of one live block version, shared with the render thread
 * (deferred model preparation). Pipeline blocks carry their coordinate map. */
struct NativeFrozenMaterialBlock {
    mh3_native_material_state fields;
    uint8_t texcoord_map[8];
    uint8_t texcoord_known;
};
/* Deleter of frozen copies: deletes after the render thread finished the
 * packet being recorded (immediately while no render thread runs). */
void ReleaseNativeFrozenMaterialBlock(const NativeFrozenMaterialBlock* block);
/* An immutable version of a pixel or colour block's per-draw fields (the
 * pixel group, or the TEV and konst colours), made when they change and
 * referenced by every draw until they change again. */
struct NativeMaterialGroupVersion {
    mh3_pack_pixel_inputs pixel;
    mh3_pack_tev_color color[4], konst[4];
};
/* The same deferred deleter for group versions; batched per packet. */
void ReleaseNativeMaterialGroupVersion(const NativeMaterialGroupVersion* version);
/* The immutable copy a live block owns for its current version (game thread).
 * A copy of the live block does not take it; replacing or destroying the
 * owning block retires it, so items already recorded keep reading it until
 * their packet has rendered. Holders of the plain pointer keep it while no
 * live block changed (ReadNativeLiveMaterialRevision) and ask again after. */
template <class T, void (*Release)(const T*)>
class NativeSharedCopy {
  public:
    NativeSharedCopy() = default;
    NativeSharedCopy(const NativeSharedCopy&) noexcept {}
    NativeSharedCopy& operator=(const NativeSharedCopy&) noexcept {
        Reset(nullptr);
        return *this;
    }
    ~NativeSharedCopy() { Reset(nullptr); }
    const T* get() const { return block_; }
    void Reset(const T* block) {
        if (block_ && block_ != block)
            Release(block_);
        block_ = block;
    }
    // The block generation the copy was last confirmed against.
    uint64_t generation = 0;

  private:
    const T* block_ = nullptr;
};
using NativeFrozenCopy = NativeSharedCopy<NativeFrozenMaterialBlock, ReleaseNativeFrozenMaterialBlock>;
using NativeGroupCopy = NativeSharedCopy<NativeMaterialGroupVersion, ReleaseNativeMaterialGroupVersion>;
struct NativeLiveMaterialBlock {
    mh3_native_material_state fields;
    uint32_t address, bytes;
    mh3_model_material_block kind;
    uintptr_t resident_handle;
    uint64_t resident_generation, origin_offset, source_generation;
    // The frozen copy of fields.generation, made on demand.
    mutable NativeFrozenCopy frozen;
    // Pixel and colour blocks: the group version draws reference.
    mutable NativeGroupCopy group;
    // Typed writes may update this node in place (any publication, and a new
    // block or owner over its bytes, clears it).
    bool writable = false;
};
NativeLiveMaterialBlock ReadNativeLiveMaterialBlock(uint32_t address, mh3_model_material_block kind);
/* Game thread. The live block a typed producer may update in place: its
 * fields, fields.generation (NextNativeMaterialRevision) and guest bytes, with
 * no provenance event and no publication. Null when the write must take
 * RecordNativeTypedMaterialWrite and PublishNativeLiveMaterialBlock: before the
 * block's first such write, while its bytes have tracked provenance, or after
 * another block or a model owner over its bytes was admitted. */
NativeLiveMaterialBlock* WritableNativeLiveMaterialBlock(uint32_t address, mh3_model_material_block kind);
/* After a full typed write or copy of exactly this block was recorded and
 * published: its next writes may take the in-place path. */
void MarkNativeLiveMaterialBlockWritable(uint32_t address, mh3_model_material_block kind);
/* Owner-only immediate read view. The cache owns these bytes; consume them
 * before any producer publication, source write/retirement or guest callback.
 * Mutable setters must use ReadNativeLiveMaterialBlock's private snapshot. */
const NativeLiveMaterialBlock& ReadNativeLiveMaterialBlockView(uint32_t address, mh3_model_material_block kind);
/* Mapping belongs to a certified pipeline source extent. Mutable/copy blocks
 * keep the admitted origin; unknown writes retire them. No live command read. */
void ReadNativeModelCoordinateMap(uint32_t pipeline, uint8_t mapping[8], uint8_t* known);
void PublishNativeLiveMaterialBlock(const NativeLiveMaterialBlock& block);
/* Game thread: the immutable copy of the block's current version, made once
 * per published version and owned by the block (NativeFrozenCopy). */
const NativeFrozenMaterialBlock* FreezeNativeLiveMaterialBlock(const NativeLiveMaterialBlock& block);
/* Game thread: the version of a pixel or colour block's current group fields.
 * A new generation with equal fields keeps the previous version, so a draw
 * costs one generation compare while its material is not animating. */
const NativeMaterialGroupVersion* VersionNativeLiveMaterialGroup(const NativeLiveMaterialBlock& block);
uint64_t NextNativeMaterialRevision();
void CopyNativeLiveMaterialBlock(uint32_t destination, uint32_t source, mh3_model_material_block kind);
/* Explicit mutable vertex producers must advance source revisions when arrays
 * change in place. A bind event only advances binding_generation. */
void MarkNativeVertexProduced(uint32_t payload, uint64_t generation);
mh3_native_vertex_source ReadNativeVertexResource(uint32_t reference, mh3_vertex_kind kind);
mh3_native_vertex_source ReadNativeVertexResourceData(uint32_t resource, mh3_vertex_kind kind);
void StoreNativeVertexSource(uint32_t semantic, const mh3_native_vertex_source& source);
/* Binds sources[i] for every semantic i in `known` without copying them: they
 * read as bound (every reader, store, retirement and producer revision sees
 * them) while the caller keeps them unchanged at that address. An owner about
 * to free or change such sources calls ReleaseNativeVertexSources first. */
void BindNativeVertexSourcesInPlace(const mh3_native_vertex_source* sources, uint16_t known);
/* Nothing was stored since `sources` were bound in place. */
bool NativeVertexSourcesBoundInPlace(const mh3_native_vertex_source* sources);
/* Copies sources bound in place from [begin, end) into the binding state. */
void ReleaseNativeVertexSources(const void* begin, const void* end);
/* No producer revision exists: every bound source has source generation 0. */
bool NativeVertexSourcesUnproduced();
bool ReadNativeVertexSource(uint32_t semantic, mh3_native_vertex_source* out);
/* The bound source of `semantic` without a copy, or null when none is bound;
 * valid until the next bind or retirement. */
const mh3_native_vertex_source* PeekNativeVertexSource(uint32_t semantic);
