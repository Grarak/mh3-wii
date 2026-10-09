#pragma once
#include "runtime/native/mh3_model_state.h"
/* Platform memory/ownership boundary, owning typed copies only. No commands,
 * byte-list decoder or texture content inspection. */
mh3_native_texture_source ReadNativeResTexture(uint32_t reference);
mh3_native_palette_source ReadNativeResPalette(uint32_t reference);
mh3_native_texture_binding ReadNativeTextureObject(uint32_t object);
void StoreNativeTextureObject(uint32_t object, const mh3_native_texture_binding& binding);
mh3_native_palette_source ReadNativePaletteObject(uint32_t object);
/* The stored bindings in place, for comparing without a copy (game thread;
 * valid until the next store). */
mh3_native_texture_binding NativeTextureObject(uint32_t object);
mh3_native_palette_source NativePaletteObject(uint32_t object);
/* Advances whenever a typed texture or palette object, or the generation of
 * an image one refers to, changes (game thread). */
uint64_t ReadNativeTextureObjectRevision();
/* Field-wise equality (no padding bytes). */
bool EqualNativeTextureBinding(const mh3_native_texture_binding& a, const mh3_native_texture_binding& b);
bool EqualNativePaletteSource(const mh3_native_palette_source& a, const mh3_native_palette_source& b);
void StoreNativePaletteObject(uint32_t object, const mh3_native_palette_source& palette);
void SelectNativeModelTextureObjects(uint32_t state, uint32_t objects);
void SelectNativeModelPaletteObjects(uint32_t state, uint32_t objects);
void LoadNativeModelTextureSrt(uint32_t resource);
void MarkNativeTextureProduced(uint32_t payload, uint64_t generation);
uint64_t ReadNativeTextureGeneration(uint32_t payload);
/* The same without the lock: the game thread writes every generation, so its
 * own reads need none (game thread only). */
uint64_t NativeTextureGeneration(uint32_t payload);
/* True only for the explicit resource-initialization revision. Unknown and
 * subsequently produced images/palettes remain on the mutable producer path.
 * Unreported raw writes are not covered by this provenance query. */
bool NativeTextureResourceIsOriginal(uint32_t payload, uint64_t generation);
/* Load/create hooks, never bind hooks. ResFile::Init calls ResTex::Init and
 * ResPltt::Init once per resource initialization; each event advances revision. */
void InitializeNativeTextureSource(uint32_t reference, bool palette);
/* A load of new bytes at an image or palette address without the original
 * certificate (the effect texture projects): advances its generation and
 * returns it. */
uint64_t ProduceNativeTextureSource(uint32_t payload);
void CopyNativeResourceObjects(uint32_t destination, uint32_t source, bool palette);
void RestoreNativeModelCamera();
/* 800885F4 Invalidate: forgets the selected CPU cache fields (0x7ff: all). */
void InvalidateNativeModelCaches(uint32_t fields);
/* Selection from a previously imported typed LightState. This publishes the
 * chosen slots and returns owned masks/ambient, never borrowed guest storage. */
const mh3_native_light_slots& SelectNativeModelLightSet(uint32_t state, uint32_t set_id);
/* Explicit view-array bind producer. Addresses do not establish allocation
 * counts; the eventual pose owner must supply those for indexed mesh loads. */
void StoreNativeModelMatrixSources(uint32_t state, const uint32_t addresses[3]);
mh3_matrix_binding ReadNativeModelMatrixSources(uint32_t state);
