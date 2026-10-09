// Thin RMHP08 ABI and owned producer registry. C11 owns resource/selector
// semantics; no guest function, GX, FIFO or vertex expansion is performed.
#include "abi_bridge.h"
#include "fatal.h"
#include <aurora/fast_map.h>
#include "guest_span.h"
#include "memory.h"
#include "native_frame_packet.h"
#include "native_graphics.h"
#include "runtime/native/adapters/model_pose_bridge.h"
#include "runtime/native/adapters/model_state_bridge.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#ifdef MH3_DEVELOPMENT
#include "dev_options.h"
#endif

const mh3_res_memory& MH3NativeResourceMemory();
namespace {
// Pose owners are game-thread state.
FastMap<uint32_t, mh3_model_pose_owner> owners;
struct Region {
    uint32_t bytes;
    std::vector<uint32_t> scenes;
};

std::map<uint32_t, Region> regions;
FastMap<uint32_t, std::vector<uint32_t>> positions;
uint64_t pose_revision = 0;
constinit mh3_model_matrix_selectors selectors{};

// Advances on every selector change (deferred preparation copies them).
constinit uint64_t selectorRevision = 0;

// Loaded matrix slots per owner: UI/effect producers load the Game copy; model
// draws rebuild the Render copy while the render thread replays them.
struct MatrixPalette {
    mh3_model_palette palette{};
    uint64_t revision = 0;
};

constinit MatrixPalette matrixPalettes[NativeOwnerCount]{};
MatrixPalette& Palette(NativeOwner owner) {
    return matrixPalettes[NativeOwnerIndex(owner)];
}

[[noreturn, gnu::cold, gnu::noinline]] void CheckedFailed(mh3_res_status status) {
    MH3_THROW(std::runtime_error("Native model pose input failed: " + std::to_string(status)));
}

inline void Checked(mh3_res_status status) {
    if (status != MH3_RES_OK) [[unlikely]]
        CheckedFailed(status);
}

uint32_t Canonical(uint32_t address) {
    return address ? (address & 0x3fffffffu) | 0x80000000u : 0;
}

// An owner's allocations: its scene, its MDL0 header and its view arrays (each
// over all views).
struct Allocation {
    uint32_t start, bytes;
};

unsigned Allocations(const mh3_model_pose_owner& owner, Allocation (&out)[5]) {
    unsigned n = 0;
    out[n++] = {owner.scene_address, 0x104};
    out[n++] = {owner.model_address, 0x70};

    for (unsigned i = 0; i < 3; i++)
        if (owner.base[i] && owner.views * owner.slice_bytes[i])
            out[n++] = {owner.base[i], owner.views * owner.slice_bytes[i]};

    return n;
}

void Reindex(const FastMap<uint32_t, mh3_model_pose_owner>& next) {
    std::map<uint32_t, Region> ranges;
    FastMap<uint32_t, std::vector<uint32_t>> arrays;
    auto add = [&](uint32_t start, uint32_t bytes, uint32_t scene) {
        if (!bytes)
            return;

        auto found = ranges.lower_bound(start);

        if (found != ranges.end() && found->first == start && found->second.bytes == bytes) {
            found->second.scenes.push_back(scene);
            return;
        }

        if ((found != ranges.end() && uint64_t(start) + bytes > found->first) || (found != ranges.begin() && uint64_t(std::prev(found)->first) + std::prev(found)->second.bytes > start))
            MH3_THROW(std::runtime_error("Native pose allocation has overlapping or inconsistent owners"));

        ranges.emplace(start, Region{bytes, {scene}});
    };

    for (const auto& entry : next) {
        const auto& owner = entry.second;
        Allocation allocations[5];

        for (unsigned k = 0, n = Allocations(owner, allocations); k < n; k++)
            add(allocations[k].start, allocations[k].bytes, entry.first);

        for (unsigned v = 0; v < owner.views; v++)
            if (owner.base[0] && owner.count)
                arrays[owner.base[0] + v * owner.slice_bytes[0]].push_back(entry.first);
    }

    regions = std::move(ranges);
    positions = std::move(arrays);
}

// A new owner into the index: all of its allocations are checked against the
// others (as Reindex checks the whole set) before anything changes.
void Index(uint32_t scene, const mh3_model_pose_owner& owner) {
    Allocation allocations[5];
    const unsigned n = Allocations(owner, allocations);

    for (unsigned k = 0; k < n; k++) {
        const auto [start, bytes] = allocations[k];
        const uint64_t end = uint64_t(start) + bytes;
        const auto found = regions.lower_bound(start);
        const bool exact = found != regions.end() && found->first == start && found->second.bytes == bytes;
        bool overlaps = !exact && ((found != regions.end() && end > found->first) || (found != regions.begin() && uint64_t(std::prev(found)->first) + std::prev(found)->second.bytes > start));

        for (unsigned j = 0; j < k && !overlaps; j++) {
            const Allocation& other = allocations[j];
            overlaps = !(other.start == start && other.bytes == bytes) && uint64_t(other.start) + other.bytes > start && end > other.start;
        }

        if (overlaps)
            MH3_THROW(std::runtime_error("Native pose allocation has overlapping or inconsistent owners"));
    }

    for (unsigned k = 0; k < n; k++) {
        const auto found = regions.find(allocations[k].start);

        if (found != regions.end())
            found->second.scenes.push_back(scene);
        else
            regions.emplace(allocations[k].start, Region{allocations[k].bytes, {scene}});
    }

    if (owner.base[0] && owner.count)
        for (unsigned v = 0; v < owner.views; v++)
            positions[owner.base[0] + v * owner.slice_bytes[0]].push_back(scene);
}

// An erased owner out of the index (its fields from before the retirement).
void Unindex(uint32_t scene, const mh3_model_pose_owner& owner) {
    const auto drop = [scene](std::vector<uint32_t>& scenes) {
        if (const auto it = std::find(scenes.begin(), scenes.end(), scene); it != scenes.end())
            scenes.erase(it);

        return scenes.empty();
    };

    Allocation allocations[5];

    for (unsigned k = 0, n = Allocations(owner, allocations); k < n; k++) {
        const auto found = regions.find(allocations[k].start);

        if (found != regions.end() && found->second.bytes == allocations[k].bytes && drop(found->second.scenes))
            regions.erase(found);
    }

    if (owner.base[0] && owner.count)
        for (unsigned v = 0; v < owner.views; v++) {
            const auto found = positions.find(owner.base[0] + v * owner.slice_bytes[0]);

            if (found != positions.end() && drop(found->second))
                positions.erase(found);
        }
}

mh3_model_pose_owner& Declare(uint32_t scene) {
    // Steady state (about 400 declarations a frame in Deserted Island area
    // 10): the scene still names the admitted owner's model, view arrays and
    // counts, which the full read checked then; only the current view moves.
    if (const auto found = owners.find(Canonical(scene)); found != owners.end()) {
        auto& known = found->second;
        const uint32_t views = MemoryInline::FlatRead8(scene + 0x100), current = MemoryInline::FlatRead8(scene + 0x101);

        if (views == known.views && current < views && MemoryInline::FlatRead16(scene + 0x102) == known.count &&
            Canonical(MemoryInline::FlatRead32(scene + 0xe8)) == known.model_address && Canonical(MemoryInline::FlatRead32(scene + 0xf4)) == known.base[0] &&
            Canonical(MemoryInline::FlatRead32(scene + 0xf8)) == known.base[1] && Canonical(MemoryInline::FlatRead32(scene + 0xfc)) == known.base[2]) {
            known.current_view = uint8_t(current);
            return known;
        }
    }

    mh3_model_pose_owner next{};
    Checked(mh3_model_pose_owner_read(&MH3NativeResourceMemory(), scene, &next));

    if (!owners.contains(next.scene_address)) {
        Index(next.scene_address, next);
        return owners.insert_or_assign(next.scene_address, next).first->second;
    }

    // A replaced owner (rare) rebuilds the index.
    auto prospective = owners;
    prospective.insert_or_assign(next.scene_address, next);
    Reindex(prospective);
    owners = std::move(prospective);
    return owners.at(next.scene_address);
}

void PublishSelectors() {
    ++selectorRevision;
    SetNativeModelMatrixSelectors(selectors);
}

void Select(const uint32_t rows[8]) {
    mh3_model_matrix_select(&selectors, rows);
    PublishSelectors();
}
} // namespace

bool PublishNativeModelPose(uint32_t position, uint32_t normal, uint32_t environment, uint32_t count, uint32_t reference) {
    if (!count)
        return false;

    uint32_t model = 0;
    Checked(mh3_res_runtime_reference(&MH3NativeResourceMemory(), reference, 1, &model));
    return PublishNativeModelPoseFor(position, normal, environment, count, model);
}

bool PublishNativeModelPoseFor(uint32_t position, uint32_t normal, uint32_t environment, uint32_t count, uint32_t model) {
    if (!count)
        return false;

    if (pose_revision == UINT64_MAX)
        MH3_THROW(std::runtime_error("Native model pose revision overflow"));

    const uint64_t revision = pose_revision + 1;
    std::vector<mh3_model_pose_owner> updates;
    const auto found = positions.find(Canonical(position));

    if (found == positions.end())
        return false;

    for (const uint32_t scene : found->second) {
        auto next = owners.at(scene);
        const auto status = mh3_model_pose_publish(&next, position, normal, environment, count, model, revision);

        if (status == MH3_RES_ABSENT)
            continue;

        Checked(status);
        updates.push_back(next);
    }

    for (const auto& next : updates)
        owners.at(next.scene_address) = next;

    if (!updates.empty())
        pose_revision = revision;

    return !updates.empty();
}

void RetireNativeModelPose(uint32_t address, uint32_t bytes) {
    if (!bytes)
        return;

    address = Canonical(address);
    const uint64_t end = uint64_t(address) + bytes;
    auto it = regions.upper_bound(address);

    if (it != regions.begin())
        --it;

    std::vector<std::pair<uint32_t, mh3_model_pose_owner>> erased;

    // Straight over the regions, without collecting their scenes first (the
    // view pass retires every model's output each frame): retiring changes
    // only the owners, and the erased ones leave the index afterwards.
    for (; it != regions.end() && it->first < end; ++it) {
        if (uint64_t(it->first) + it->second.bytes <= address)
            continue;

        for (const uint32_t scene : it->second.scenes) {
            const auto found = owners.find(scene);

            if (found == owners.end())
                continue;

            const mh3_model_pose_owner before = found->second;
            mh3_model_pose_retire(&found->second, address, bytes);

            if (!found->second.scene_address) {
                erased.emplace_back(scene, before);
                owners.erase(found);
            }
        }
    }

    for (const auto& [scene, owner] : erased)
        Unindex(scene, owner);
}

void ClearNativeModelPose() {
    owners.clear();
    regions.clear();
    positions.clear();
    selectors = {};
    ++selectorRevision;
}

namespace {
// The pose's failure, or null; `status` is set for a failed source read.
const char* PoseSources(const uint32_t addresses[3], uint32_t model, mh3_matrix_sources& result, mh3_res_status& status) {
    if (!addresses)
        return "Native model pose requires explicit source addresses";

    bool found = false;
    const auto matching = positions.find(Canonical(addresses[0]));

    if (matching == positions.end())
        return "Native model matrices have no allocation owner";

    for (const uint32_t scene : matching->second) {
        const auto& owner = owners.at(scene);
        mh3_matrix_sources candidate{};
        status = mh3_model_pose_sources(&MH3NativeResourceMemory(), &owner, addresses, model ? model : owner.model_address, &candidate);

        if (status == MH3_RES_ABSENT)
            continue;

        if (status != MH3_RES_OK)
            return "Native model pose input failed: ";

        if (found)
            return "Native model pose has ambiguous live allocation owners";

        result = candidate;
        found = true;
    }

    status = MH3_RES_OK;
    return found ? nullptr : "Native model matrices have no completed typed pose producer";
}

std::string PoseFailure(const char* failure, mh3_res_status status) {
    return status == MH3_RES_OK ? std::string(failure) : failure + std::to_string(status);
}
} // namespace

mh3_matrix_sources ReadNativeModelPose(const uint32_t addresses[3], uint32_t model) {
    mh3_matrix_sources result{};
    mh3_res_status status = MH3_RES_OK;

    if (const char* failure = PoseSources(addresses, model, result, status)) [[unlikely]]
        MH3_THROW(std::runtime_error(PoseFailure(failure, status)));

    return result;
}

Status TryReadNativeModelPose(const uint32_t addresses[3], uint32_t model, mh3_matrix_sources& result) {
    mh3_res_status status = MH3_RES_OK;

    if (const char* failure = PoseSources(addresses, model, result, status))
        return Status::Error(PoseFailure(failure, status));

    return {};
}

// ---- Pose versions (model_pose_bridge.h) ----
namespace {
// The version a binding (keyed by its position array) was last captured as.
struct PoseBinding {
    uint32_t address[3]{};
    uint64_t generation = 0, used = 0;
    NativePoseVersion* version = nullptr;
};

FastMap<uint32_t, PoseBinding> poseBindings;

// Version storage by size class (256 << class bytes), recycled. Each class
// keeps at most PoseFreeLimit free versions: a render thread that falls
// behind leaves many versions retired at once, and they are deleted after.
constexpr unsigned PoseClasses = 16;
constexpr size_t PoseFreeLimit = 64;
std::vector<NativePoseVersion*> poseFree[PoseClasses];

// Replaced versions with the sequence of the packet recorded when they were
// replaced: it and every older packet may still reference them.
struct RetiredPose {
    uint64_t sequence;
    NativePoseVersion* version;
};

std::deque<RetiredPose> poseRetired;
uint64_t poseSwept = 0;
uint32_t poseVersions = 0;
constinit const NativePoseVersion emptyPose{};
#ifdef MH3_DEVELOPMENT
NativePoseStatistics poseStats{};
uint64_t poseVerified = 0, poseMismatches = 0;
std::atomic<uint64_t> poseLifetimeErrors{0};
#define MH3_POSE_STAT(...) __VA_ARGS__
#else
#define MH3_POSE_STAT(...)
#endif

// A version no packet references any more.
void FreePose(NativePoseVersion* version) {
    auto& free = poseFree[version->size_class];

    if (free.size() < PoseFreeLimit) {
        free.push_back(version);
        return;
    }

    delete[] version->bytes;
    delete version;
    --poseVersions;
}

NativePoseVersion* AcquirePose(size_t bytes) {
    for (const uint64_t done = NativeFramePacketsDone(); !poseRetired.empty() && poseRetired.front().sequence <= done; poseRetired.pop_front())
        FreePose(poseRetired.front().version);

    unsigned size_class = 0;

    while (size_class < PoseClasses && (size_t(256) << size_class) < bytes)
        ++size_class;

    if (size_class == PoseClasses)
        MH3_THROW(std::runtime_error("Native pose arrays exceed the largest version size"));

    if (auto& free = poseFree[size_class]; !free.empty()) {
        auto* version = free.back();
        free.pop_back();
#ifdef MH3_DEVELOPMENT
        version->epoch.fetch_add(1, std::memory_order_relaxed);
#endif
        return version;
    }

    auto* version = new NativePoseVersion{};
    version->bytes = new uint8_t[size_t(256) << size_class];
    version->size_class = uint8_t(size_class);
    ++poseVersions;
    return version;
}

void RetirePose(NativePoseVersion* version) {
    if (!version)
        return;

    if (NativeRenderThreadRunning())
        poseRetired.push_back({NativeFramePacketSequence(), version});
    else
        FreePose(version);
}

// Bindings not captured for a while (models gone or out of view) give their
// versions back.
void SweepPoses(uint64_t sequence) {
    constexpr uint64_t Idle = 120;

    if (sequence - poseSwept < Idle)
        return;

    poseSwept = sequence;

    for (auto it = poseBindings.begin(); it != poseBindings.end();) {
        if (sequence - it->second.used >= Idle) {
            RetirePose(it->second.version);
            it = poseBindings.erase(it);
        } else
            ++it;
    }
}

const mh3_matrix_table& PoseTable(const mh3_matrix_sources& sources, unsigned i) {
    return i == 0 ? sources.positions : i == 1 ? sources.normals : sources.environment;
}

// A version's tables are the live ones in host byte order (CopyPose).
bool SamePoseLayout(const mh3_matrix_sources& version, const mh3_matrix_sources& live) {
    for (unsigned i = 0; i < 3; ++i) {
        const auto &x = PoseTable(version, i), &y = PoseTable(live, i);

        if (!x.bytes.data != !y.bytes.data || x.bytes.size != y.bytes.size || x.count != y.count || x.stride != y.stride || x.big_endian)
            return false;
    }

    return true;
}

// Word compare of a version's host-order words with the live words (swapped
// when big-endian), 16 bytes a step (memcmp is a byte loop on the Vita); stops
// at the first difference, which a moved camera puts in the first matrix.
// Matrix tables are whole words.
bool SamePoseBytes(const uint8_t* have, const uint8_t* now, size_t bytes, bool swap) {
    const auto word = [swap](uint32_t value) { return swap ? __builtin_bswap32(value) : value; };
    size_t i = 0;
#if defined(__ARM_NEON)
    for (; i + 16 <= bytes; i += 16) {
        uint8x16_t y = vld1q_u8(now + i);

        if (swap)
            y = vrev32q_u8(y);

        const uint32x4_t differ = vreinterpretq_u32_u8(veorq_u8(vld1q_u8(have + i), y));
        const uint32x2_t folded = vorr_u32(vget_low_u32(differ), vget_high_u32(differ));

        if (vget_lane_u32(vpmax_u32(folded, folded), 0))
            return false;
    }
#endif

    for (; i + 16 <= bytes; i += 16) {
        uint32_t x[4], y[4];
        std::memcpy(x, have + i, 16);
        std::memcpy(y, now + i, 16);

        if ((x[0] ^ word(y[0])) | (x[1] ^ word(y[1])) | (x[2] ^ word(y[2])) | (x[3] ^ word(y[3])))
            return false;
    }

    for (; i + 4 <= bytes; i += 4) {
        uint32_t x, y;
        std::memcpy(&x, have + i, 4);
        std::memcpy(&y, now + i, 4);

        if (x != word(y))
            return false;
    }

    return true;
}

// The version holds what the live arrays hold now (layouts already equal).
bool SamePoseContents(const NativePoseVersion& version, const mh3_matrix_sources& live) {
    for (unsigned i = 0; i < 3; ++i) {
        const auto &have = PoseTable(version.sources, i), &now = PoseTable(live, i);

        if (now.bytes.data && !SamePoseBytes(have.bytes.data, now.bytes.data, now.bytes.size, now.big_endian))
            return false;
    }

    return true;
}

// The live (guest, big-endian) words in host order, so the render thread
// copies a version's matrices without swapping each one for every batch that
// loads it (mh3_model_palette_apply_batch).
void CopyHostOrder(uint8_t* to, const uint8_t* from, size_t bytes) {
    size_t i = 0;
#if defined(__ARM_NEON)
    // The game thread wrote the arrays on another core: fetch ahead of the
    // swap, which otherwise waits on every line.
    for (; i + 16 <= bytes; i += 16) {
        __builtin_prefetch(from + i + 192);
        vst1q_u8(to + i, vrev32q_u8(vld1q_u8(from + i)));
    }
#endif
    for (; i + 4 <= bytes; i += 4) {
        uint32_t word;
        std::memcpy(&word, from + i, 4);
        word = __builtin_bswap32(word);
        std::memcpy(to + i, &word, 4);
    }
}

// The skin view the view pass recorded for a position array (GPU skinning):
// the bones' world rows as the view pass saw them, in host order. Copied
// there, not at the draw: in the overlapped draw the next logic step rewrites
// the world arrays while the draw thread captures poses.
struct SkinView {
    float camera[12];
    uint32_t count, total;
    std::vector<uint8_t> bones;
};

FastMap<uint32_t, SkinView> skinViews;

// The version's skin bones are those of `view` (or both are absent).
bool SameSkinBones(const NativePoseVersion& version, const SkinView* view) {
    if (!view)
        return !version.bone_count;

    return version.bone_first == view->count && version.bone_count == view->total - view->count && !std::memcmp(version.camera, view->camera, sizeof view->camera) &&
           !std::memcmp(version.bones, view->bones.data(), view->bones.size());
}

NativePoseVersion* CopyPose(const mh3_matrix_sources& live, const SkinView* view) {
    size_t total = 0;

    for (unsigned i = 0; i < 3; ++i)
        total += PoseTable(live, i).bytes.data ? PoseTable(live, i).bytes.size : 0;

    const size_t boneBytes = view ? size_t(view->total - view->count) * 48 : 0;
    auto* version = AcquirePose(total + boneBytes);
    version->sources = live;
    version->bone_first = version->bone_count = 0;
    version->bones = nullptr;
    uint8_t* at = version->bytes;

    if (view) {
        std::memcpy(at + total, view->bones.data(), boneBytes);
        version->bone_first = view->count;
        version->bone_count = view->total - view->count;
        version->bones = reinterpret_cast<const float(*)[12]>(at + total);
        std::memcpy(version->camera, view->camera, sizeof view->camera);
    }

    for (auto* table : {&version->sources.positions, &version->sources.normals, &version->sources.environment}) {
        if (!table->bytes.data)
            continue;

        if (table->big_endian)
            CopyHostOrder(at, table->bytes.data, table->bytes.size);
        else
            std::memcpy(at, table->bytes.data, table->bytes.size);

        table->bytes.data = at;
        table->big_endian = 0;
        at += table->bytes.size;
    }

    MH3_POSE_STAT(++poseStats.copies; poseStats.copied_bytes += total;)
    return version;
}
} // namespace

void PublishNativeModelSkinView(uint32_t position, const float camera[12], uint32_t world, uint32_t count, uint32_t total) {
    auto& view = skinViews[position];
    std::memcpy(view.camera, camera, sizeof view.camera);
    view.count = count;
    view.total = total;
    view.bones.resize(size_t(total - count) * 48);
    CopyHostOrder(view.bones.data(), GuestRange(world + count * 48, view.bones.size()), view.bones.size());
}

const NativePoseVersion* VersionNativeModelPose(const uint32_t addresses[3], const mh3_matrix_sources& live) {
    MH3_POSE_STAT(++poseStats.captures;)
    if (!addresses[0])
        return &emptyPose;

    const auto skin = skinViews.find(addresses[0]);
    const SkinView* view = skin != skinViews.end() ? &skin->second : nullptr;

    const uint64_t sequence = NativeFramePacketSequence();
    SweepPoses(sequence);
    auto& binding = poseBindings[addresses[0]];
    binding.used = sequence;

    if (binding.version && binding.address[1] == addresses[1] && binding.address[2] == addresses[2]) {
        // No producer completed on these arrays since the version was taken.
        if (binding.generation == live.generation) {
            MH3_POSE_STAT(++poseStats.same_revision;)
#ifdef MH3_DEVELOPMENT
            if (VerifyNativeModelPoses())
                VerifyNativeModelPose(*binding.version, live);
#endif
            return binding.version;
        }

        // CalcView rewrites every view each frame; an unmoved camera over an
        // unmoved model writes the same matrices again.
        if (SamePoseLayout(binding.version->sources, live) && SamePoseContents(*binding.version, live) && SameSkinBones(*binding.version, view)) {
            MH3_POSE_STAT(++poseStats.same_contents;)
            binding.generation = live.generation;
            return binding.version;
        }
    }

    auto* version = CopyPose(live, view);
    RetirePose(binding.version);
    std::memcpy(binding.address, addresses, sizeof binding.address);
    binding.generation = live.generation;
    binding.version = version;
    return version;
}
#ifdef MH3_DEVELOPMENT
bool VerifyNativeModelPoses() {
    static const bool enabled = VerifyOptions().Has("pose");
    return enabled;
}

void VerifyNativeModelPose(const NativePoseVersion& version, const mh3_matrix_sources& live) {
    ++poseVerified;

    if (SamePoseLayout(version.sources, live) && SamePoseContents(version, live))
        return;

    if (poseMismatches++ < 20)
        std::fprintf(stderr, "MH3 verify pose: a reused version differs from the live arrays (%u matrices, revision %llu)\n", unsigned(live.positions.count),
                     static_cast<unsigned long long>(live.generation));
}

void CheckNativeModelPoseLifetime(const NativePoseVersion& version, uint32_t epoch) {
    if (version.epoch.load(std::memory_order_relaxed) == epoch)
        return;

    if (poseLifetimeErrors.fetch_add(1, std::memory_order_relaxed) < 20)
        std::fprintf(stderr, "MH3 verify pose: a version was reused before an item that references it rendered\n");
}

NativePoseStatistics TakeNativePoseStatistics() {
    auto result = std::exchange(poseStats, {});
    result.verified = poseVerified;
    result.mismatches = poseMismatches;
    result.lifetime_errors = poseLifetimeErrors.load(std::memory_order_relaxed);
    result.versions = poseVersions;
    result.bindings = uint32_t(poseBindings.size());
    return result;
}
#endif
void PublishNativeModelDualTexture(bool enabled) {
    mh3_model_matrix_dual(&selectors, enabled ? 1 : 0);
    PublishSelectors();
}

void ApplyNativeModelSelectors() {
    PublishSelectors();
}

mh3_model_matrix_selectors ReadNativeModelSelectors() {
    return selectors;
}

uint64_t ReadNativeModelSelectorsRevision() {
    return selectorRevision;
}

void SetNativeModelSelectors(const mh3_model_matrix_selectors& state) {
    selectors = state;
    PublishSelectors();
}

void PublishNativeModelMatrixRows(const uint32_t rows[8]) {
    Select(rows);
}

// A matrix given from the palette's own slot (EditNativeModelPalette) is
// already in place.
void PublishNativeModelPositionMatrix(uint32_t selector, const float matrix[12], NativeOwner owner) {
    auto& p = Palette(owner);
    ++p.revision;

    if (auto* slot = &p.palette.position[selector / 3]; static_cast<const void*>(slot) != matrix)
        std::memcpy(slot, matrix, 48);

    p.palette.position_valid |= uint16_t(1u << (selector / 3));
}

void PublishNativeModelNormalMatrix(uint32_t selector, const float matrix[9], NativeOwner owner) {
    auto& p = Palette(owner);
    ++p.revision;

    if (auto* slot = &p.palette.normal[selector / 3]; static_cast<const void*>(slot) != matrix)
        std::memcpy(slot, matrix, 36);

    p.palette.normal_valid |= uint16_t(1u << (selector / 3));
}

void PublishNativeModelTextureMatrix(uint32_t selector, const float matrix[12], bool matrix3x4, NativeOwner owner) {
    if (selector >= 64)
        return; // post/SRT matrices have an independent owner
    auto& p = Palette(owner);
    ++p.revision;

    if (auto* slot = &p.palette.texture[(selector - 30) / 3]; static_cast<const void*>(slot) != matrix)
        std::memcpy(slot, matrix, matrix3x4 ? 48 : 32);

    if (matrix3x4)
        p.palette.texture_valid |= uint16_t(1u << ((selector - 30) / 3));
}

const mh3_model_palette& ReadNativeModelPalette(NativeOwner owner) {
    return Palette(owner).palette;
}

uint64_t ReadNativeModelPaletteRevision(NativeOwner owner) {
    return Palette(owner).revision;
}

void PublishNativeModelPalette(const mh3_model_palette& palette, NativeOwner owner) {
    auto& p = Palette(owner);
    ++p.revision;
    p.palette = palette;
}

mh3_model_palette& EditNativeModelPalette(NativeOwner owner) {
    auto& p = Palette(owner);
    ++p.revision;
    return p.palette;
}

namespace {
uint32_t ViewArray(const mh3_model_pose_owner& owner, uint32_t scene, unsigned kind) {
    uint32_t result = 0;
    Checked(mh3_model_pose_array(&owner, kind, &result));

    // The native records use cached aliases; the guest getter retains its input
    // base alias exactly, including uncached resources.
    const uint32_t base = Memory::Read32(scene + 0xf4 + kind * 4);

    if (base)
        result = base + (result - owner.base[kind]);

    return result;
}
} // namespace

uint32_t NativeModelViewArray(uint32_t scene, unsigned kind) {
    return ViewArray(Declare(scene), scene, kind);
}

void NativeModelViewArrays(uint32_t scene, uint32_t arrays[3]) {
    const auto& owner = Declare(scene);

    for (unsigned kind = 0; kind < 3; kind++)
        arrays[kind] = ViewArray(owner, scene, kind);
}

extern "C" void MH3_ModelViewPosition(CpuContext* cpu) {
    cpu->gpr[3] = NativeModelViewArray(cpu->gpr[3], 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x80080BC0, MH3_ModelViewPosition, "ModelViewPosition");
extern "C" void MH3_ModelViewNormal(CpuContext* cpu) {
    cpu->gpr[3] = NativeModelViewArray(cpu->gpr[3], 1);
}
REGISTER_NATIVE_FUNCTION_AS(0x80080C0C, MH3_ModelViewNormal, "ModelViewNormal");
extern "C" void MH3_ModelViewEnvironment(CpuContext* cpu) {
    cpu->gpr[3] = NativeModelViewArray(cpu->gpr[3], 2);
}
REGISTER_NATIVE_FUNCTION_AS(0x80080C68, MH3_ModelViewEnvironment, "ModelViewEnvironment");
extern "C" void MH3_ModelViewCount(CpuContext* cpu) {
    uint32_t count = 0;
    Checked(mh3_model_pose_count({GuestRange(cpu->gpr[3], 0x104), 0x104}, &count));
    cpu->gpr[3] = count;
}
REGISTER_NATIVE_FUNCTION_AS(0x8007FD0C, MH3_ModelViewCount, "ModelViewCount");

extern "C" void MH3_ModelMainMatrixReset(CpuContext*) {
    mh3_model_matrix_reset(&selectors);
    PublishSelectors();
}
REGISTER_NATIVE_FUNCTION_AS(0x8009B13C, MH3_ModelMainMatrixReset, "ModelMainMatrixReset");
extern "C" void MH3_ModelMainMatrixSelect(CpuContext* cpu) {
    uint32_t rows[8];

    for (uint32_t i = 0; i < 8; i++)
        rows[i] = Memory::Read32(cpu->gpr[3] + 4 * i);

    Select(rows);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009B1A4, MH3_ModelMainMatrixSelect, "ModelMainMatrixSelect");
extern "C" void MH3_ModelCurrentMatrixReset(CpuContext* cpu) {
    if (!Memory::Read8(cpu->gpr[3])) {
        mh3_model_matrix_reset(&selectors);
        PublishSelectors();
        Memory::Write8(cpu->gpr[3], 1);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80087CF0, MH3_ModelCurrentMatrixReset, "ModelCurrentMatrixReset");
extern "C" void MH3_ModelPreTextureMatrices(CpuContext* cpu) {
    if (int32_t(cpu->gpr[4]) < 0)
        return;

    uint32_t types[8], rows[8];
    int apply = 0;

    for (uint32_t i = 0; i < 8; i++)
        types[i] = Memory::Read32(cpu->gpr[3] + 4 * i);

    mh3_model_matrix_rows(types, int32_t(cpu->gpr[4]), rows, &apply);
    const bool identity = std::all_of(std::begin(rows), std::end(rows), [](uint32_t row) { return row == 60; });

    if (identity) {
        if (!Memory::Read8(0x80796db4)) {
            mh3_model_matrix_reset(&selectors);
            PublishSelectors();
            Memory::Write8(0x80796db4, 1);
        }

        return;
    }

    if (std::any_of(std::begin(types), std::end(types), [](uint32_t type) { return type == 1; })) {
        const auto binding = ReadNativeModelMatrixSources(0x80685530);
        const auto sources = ReadNativeModelPose(binding.address);
        mh3_shape_matrix_input input{};
        input.valid = 1;
        input.matrix_id = int32_t(cpu->gpr[4]);
        std::copy(std::begin(types), std::end(types), std::begin(input.texture_source));
        mh3_model_palette palette{};
        mh3_model_palette_rigid(&input, &sources, &palette);

        for (unsigned i = 0; i < 8; i++)
            if (types[i] == 1)
                SetNativeTextureMatrix(30 + i * 3, &palette.texture[i].v[0][0], true);
    }

    Select(rows);
    Memory::Write8(0x80796db4, 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x80087D30, MH3_ModelPreTextureMatrices, "ModelPreTextureMatrices");

void CopyNativePaletteStoreForDraw() {
    matrixPalettes[unsigned(NativeOwner::Draw)] = matrixPalettes[unsigned(NativeOwner::Game)];
}
