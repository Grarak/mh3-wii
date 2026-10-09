#pragma once
#include "runtime/native/mh3_model_pose.h"
#include "runtime/include/native_owner.h"
#ifdef MH3_DEVELOPMENT
#include <atomic>
#endif
/* Typed producer/publication boundary. Getters declare allocation extents;
 * completed CPU writes establish pose revisions. A bind alone does neither. */
bool PublishNativeModelPose(uint32_t position, uint32_t normal, uint32_t environment, uint32_t count, uint32_t model_reference);
void RetireNativeModelPose(uint32_t address, uint32_t bytes);
void ClearNativeModelPose();
mh3_matrix_sources ReadNativeModelPose(const uint32_t addresses[3], uint32_t model_address = 0);
/* An immutable copy of one binding's view arrays (positions, normals,
 * environment) as their producer left them; the tables of `sources` point
 * into it. Model draws reference it instead of copying the arrays: a binding
 * captured again with the same producer revision, or with a new revision but
 * equal contents (an unmoved camera over an unmoved model), keeps its
 * version. A replaced version is reused once every packet recorded while it
 * was current has rendered or been replaced unrendered. */
struct NativePoseVersion {
    mh3_matrix_sources sources;
    uint8_t* bytes;
    uint8_t size_class;
#ifdef MH3_DEVELOPMENT
    // Advances whenever the storage is reused for other arrays: an item that
    // renders after its version's reuse is a lifetime error (MH3_VERIFY=pose).
    std::atomic<uint32_t> epoch{0};
#endif
};
/* Game thread: the version of the arrays at `addresses` that `live`
 * (ReadNativeModelPose of them) describes; without arrays (addresses[0] 0)
 * an empty one. Valid until the packets recorded meanwhile have rendered. */
const NativePoseVersion* VersionNativeModelPose(const uint32_t addresses[3], const mh3_matrix_sources& live);
#ifdef MH3_DEVELOPMENT
/* MH3_VERIFY=pose: compare every reused version with the live arrays. */
bool VerifyNativeModelPoses();
void VerifyNativeModelPose(const NativePoseVersion& version, const mh3_matrix_sources& live);
/* Render thread, after reading a version an item captured at `epoch`. */
void CheckNativeModelPoseLifetime(const NativePoseVersion& version, uint32_t epoch);
struct NativePoseStatistics {
    // Captures, those copied (and their bytes), those reusing the version of
    // an unchanged producer revision or of equal contents; checked reuses,
    // and versions reused for other arrays before their items rendered.
    uint64_t captures, copies, copied_bytes, same_revision, same_contents, verified, mismatches, lifetime_errors;
    uint32_t versions, bindings;
};
/* Since the last call; verified/mismatches are totals. */
NativePoseStatistics TakeNativePoseStatistics();
#endif
/* 80080BC0/80080C0C/80080C68: the scene's current view array (0 position,
 * 1 normal, 2 environment), declaring its allocation owner. */
uint32_t NativeModelViewArray(uint32_t scene, unsigned kind);
void PublishNativeModelDualTexture(bool enabled);
/* Reapply independent live selector state after an immutable material overlay.
 * Unknown remains unknown; this is never an identity/dual fallback. */
void ApplyNativeModelSelectors();
/* The game thread's selector state. */
mh3_model_matrix_selectors ReadNativeModelSelectors();
uint64_t ReadNativeModelSelectorsRevision();
void SetNativeModelSelectors(const mh3_model_matrix_selectors& state);
void PublishNativeModelMatrixRows(const uint32_t rows[8]);
/* Actual native matrix setter publications, shared with UI/effects. Selectors
 * are PN rows0..27 and main texture rows30..57, step3; post rows64+ do not
 * change these palette slots. Values are copied and retained across frames,
 * source retirement and model changes until another actual load replaces them. */
void PublishNativeModelPositionMatrix(uint32_t selector, const float matrix[12], NativeOwner owner = NativeOwner::Game);
void PublishNativeModelNormalMatrix(uint32_t selector, const float matrix[9], NativeOwner owner = NativeOwner::Game);
/* 2x4 loads replace only the first two rows. They preserve an actually known
 * third row; a new partial slot remains incomplete for 3x4 model consumption. */
void PublishNativeModelTextureMatrix(uint32_t selector, const float matrix[12], bool matrix3x4 = true, NativeOwner owner = NativeOwner::Game);
const mh3_model_palette& ReadNativeModelPalette(NativeOwner owner = NativeOwner::Game);
uint64_t ReadNativeModelPaletteRevision(NativeOwner owner = NativeOwner::Game);
void PublishNativeModelPalette(const mh3_model_palette& palette, NativeOwner owner = NativeOwner::Game);
/* The owner's palette to rebuild in place (a publication without the copy):
 * advances its revision; the reference stays valid. Matrix setters given one
 * of its own slots leave that slot as it is. */
mh3_model_palette& EditNativeModelPalette(NativeOwner owner);
