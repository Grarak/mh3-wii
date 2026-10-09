#pragma once
#include "runtime/native/mh3_model_state.h"
/* Platform memory/ownership boundary, owning typed copies only. No commands,
 * byte-list decoder or texture content inspection. */
mh3_native_texture_source ReadNativeResTexture(uint32_t reference);
mh3_native_palette_source ReadNativeResPalette(uint32_t reference);
mh3_native_texture_binding ReadNativeTextureObject(uint32_t object);
void StoreNativeTextureObject(uint32_t object,const mh3_native_texture_binding& binding);
mh3_native_palette_source ReadNativePaletteObject(uint32_t object);
void StoreNativePaletteObject(uint32_t object,const mh3_native_palette_source& palette);
void MarkNativeTextureProduced(uint32_t payload,uint64_t generation);
uint64_t ReadNativeTextureGeneration(uint32_t payload);
/* True only for the explicit resource-initialization revision. Unknown and
 * subsequently produced images/palettes remain on the mutable producer path.
 * Unreported raw writes are not covered by this provenance query. */
bool NativeTextureResourceIsOriginal(uint32_t payload,uint64_t generation);
/* Load/create hooks, never bind hooks. ResFile::Init calls ResTex::Init and
 * ResPltt::Init once per resource initialization; each event advances revision. */
void InitializeNativeTextureSource(uint32_t reference,bool palette);
void CopyNativeResourceObjects(uint32_t destination,uint32_t source,bool palette);
void RestoreNativeModelCamera();
/* Explicit view-array bind producer. Addresses do not establish allocation
 * counts; the eventual pose owner must supply those for indexed mesh loads. */
void StoreNativeModelMatrixSources(uint32_t state,const uint32_t addresses[3]);
mh3_matrix_binding ReadNativeModelMatrixSources(uint32_t state);
