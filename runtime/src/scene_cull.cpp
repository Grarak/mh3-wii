// Game-side visibility of scene-queue models (scene_cull.h).
#include "scene_cull.h"
#include <aurora/fast_map.h>
#include "guest_span.h"
#include "memory.h"
#include "model_assets.h"
#include "native_frame_packet.h"
#include "native_graphics.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include "runtime/native/adapters/model_pose_bridge.h"
#include "runtime/native/mh3_model_runtime.h"
#include "runtime/native/mh3_scene_cull.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

const mh3_res_memory& MH3NativeResourceMemory();
namespace {
// The render path's margin (model_cull.cpp): 1% of the rectangle's NDC extent
// on every side covers rounding and the rasterizer's sub-pixel offset.
constexpr float Margin = 0.02f;

// ScnMdl's G3dProc (ModelSceneTask) and its draw-callback fields.
constexpr uint32_t ScnMdlProc = 0x8007DE60;
constexpr uint32_t CallbackObject = 0xd4, CallbackMask = 0xd8, CallbackFlags = 0xda, ProcDisabled = 0xcc, ResMdl = 0xe8;
constexpr uint32_t ViewMatrixCount = 0x68; // ResMdlInfo numViewMtx, from the MDL0

// One model's bounds: valid while its certified draw table is the current one
// (a table is dropped with its owner's certificate and pack).
struct Record {
    std::shared_ptr<NativeModelDrawTable> table;
    NativeModelOwner owner{};
    bool bounded = false;

    // One box per view matrix ID.
    std::vector<uint32_t> ids;
    std::vector<mh3_cull_box> boxes;
};

FastMap<uint32_t, Record> records;
uint64_t recordsRevision = UINT64_MAX;
NativeSceneCullStatistics stats;

// The bounds of a box transformed by an affine 3x4 matrix.
void TransformBox(const float* m, const float* min, const float* max, float* outMin, float* outMax) {
    for (unsigned r = 0; r < 3; r++) {
        outMin[r] = outMax[r] = m[r * 4 + 3];

        for (unsigned c = 0; c < 3; c++) {
            const float a = m[r * 4 + c] * min[c], b = m[r * 4 + c] * max[c];
            outMin[r] += std::min(a, b);
            outMax[r] += std::max(a, b);
        }
    }
}

// A skinned mesh (docs/optimizations/rendering/gpu-skinning.md): every vertex blends its rows, so
// the mesh lies within its bounds under each row matrix. A rigid row is a view
// matrix; a skin bone row is its view matrix times the inverse bind matrix,
// whose transform of the bounds goes into the view matrix's box.
bool AddSkinRows(mh3_scene_cull_record& record, const mh3_prepared_mesh& prepared) {
    const auto& mesh = prepared.immutable.mesh;

    for (uint32_t i = 0; i < prepared.skin_row_count; i++) {
        const auto& row = prepared.skin_rows[i];
        float min[3], max[3];

        if (row.kind)
            TransformBox(row.inverse_bind, mesh.bounds_min, mesh.bounds_max, min, max);
        else {
            std::memcpy(min, mesh.bounds_min, sizeof min);
            std::memcpy(max, mesh.bounds_max, sizeof max);
        }

        if (!mh3_scene_cull_add(&record, row.matrix, min, max))
            return false;
    }

    return true;
}

void Build(Record& r, uint32_t model, std::shared_ptr<NativeModelDrawTable> table) {
    r = {};
    r.table = std::move(table);
    r.owner = ResolveNativeModelOwner(model);
    mh3_res_memory_view view{};

    if (mh3_res_runtime_map(&MH3NativeResourceMemory(), model, 16, &view) != MH3_RES_OK)
        return;

    mh3_res_model resource{};
    size_t dictionary = 0;
    uint32_t count = 0;

    if (mh3_res_model_open(view.span, view.offset, &resource) != MH3_RES_OK || mh3_res_model_dictionary(&resource, MH3_RES_SHAPES, &dictionary, &count) != MH3_RES_OK || !count)
        return;

    if (resource.size < ViewMatrixCount + 4)
        return;

    const uint32_t views = Memory::Read32(model + ViewMatrixCount);

    if (!views || views > 4096)
        return;
    // Every shape of the model is bounded or the model is never culled.
    // IDs index the model's view matrices: at most `views` distinct pairs.
    std::vector<mh3_scene_cull_pair> pairs(views ? views : 1);
    mh3_scene_cull_record record{pairs.data(), 0, uint32_t(pairs.size())};

    for (uint32_t id = 0; id < count; ++id) {
        mh3_res_entry entry{};

        if (mh3_res_model_entry(&resource, MH3_RES_SHAPES, id, &entry) != MH3_RES_OK)
            return;

        const uint32_t shape = view.guest_base + uint32_t(entry.offset);
        const mh3_prepared_mesh* prepared = nullptr;

        if (!ReadNativePreparedMesh(r.owner, shape, prepared).ok())
            return; // an unconverted shape

        const int32_t matrix_id = int32_t(Memory::Read32(shape + 8));
        const bool selectors = (prepared->immutable.layout.flags >> MH3_LAYOUT_MATRIX_MASK_SHIFT) & 1;

        if (prepared->skin_row_count) {
            if (!AddSkinRows(record, *prepared))
                return;
        } else if (!mh3_scene_cull_add_mesh(&record, &prepared->immutable.mesh, matrix_id, selectors, prepared->palette_batches, prepared->palette_batch_count))
            return;
    }

    for (uint32_t i = 0; i < record.count; ++i) {
        if (pairs[i].id >= views)
            return;

        r.ids.push_back(pairs[i].id);
        r.boxes.push_back(pairs[i].box);
    }

    r.bounded = record.count != 0;
}

const Record* Lookup(uint32_t model) {
    if (const uint64_t revision = ReadNativeModelStructureRevision(); revision != recordsRevision) {
        records.clear();
        recordsRevision = revision;
    }

    auto table = FindNativeModelDrawTable(model);

    if (!table)
        return nullptr;

    auto& r = records[model];

    if (r.table != table)
        Build(r, model, std::move(table));

    return &r;
}

// The drawable rectangle's planes for the current draw state.
struct Frustum {
    float key[25]{};
    bool valid = false, known = false;
    mh3_cull_frustum value{};
} frustum;

const mh3_cull_frustum* CurrentFrustum() {
    const auto& state = GetNativeDrawState();
    float key[25];
    std::memcpy(key, state.projection, 16 * sizeof(float));
    std::memcpy(key + 16, state.viewport, 4 * sizeof(float));
    int32_t scissor[4];
    const bool scissored = GetNativeScissor(scissor);

    if (!scissored) {
        scissor[0] = int32_t(state.viewport[0]);
        scissor[1] = int32_t(state.viewport[1]);
        scissor[2] = int32_t(state.viewport[2] + 1);
        scissor[3] = int32_t(state.viewport[3] + 1);
    }

    std::memcpy(key + 20, scissor, sizeof scissor);
    key[24] = scissored ? 1.f : 0.f;

    if (!frustum.known || std::memcmp(key, frustum.key, sizeof key)) {
        std::memcpy(frustum.key, key, sizeof key);
        frustum.known = true;
        float lo[2], hi[2];
        frustum.valid = mh3_cull_scissor_rect(state.viewport, scissor, Margin, lo, hi) && mh3_cull_frustum_make(state.projection, lo, hi, &frustum.value);
    }

    return frustum.valid ? &frustum.value : nullptr;
}

std::vector<const float*> matrixPointers;
std::vector<float> matrixValues;

// A culled model's pack stays demanded as its mesh draws would keep it
// (SubmitNativeModelMesh observes demand on the render thread): once per pack
// and packet, observed before that packet's draws.
std::vector<std::pair<uintptr_t, uint64_t>> demanded;
uint64_t demandedSequence = UINT64_MAX;
void Demand(const NativeModelOwner& owner) {
    if (!NativeGraphicsFrameShouldRender())
        return;

    if (demandedSequence != NativeFramePacketSequence()) {
        demandedSequence = NativeFramePacketSequence();
        demanded.clear();
    }

    const std::pair<uintptr_t, uint64_t> pack{owner.resident_handle, owner.resident_generation};

    for (const auto& entry : demanded)
        if (entry == pack)
            return;
    demanded.push_back(pack);
    QueueNativeGraphicsTask(NativeTaskOrder::BeforeDraws, [pack] { ObserveNativeModelAssetDemand(pack.first, pack.second); });
}
} // namespace

bool SkipNativeSceneModel(uint32_t object) {
    if (!object)
        return false;

    ++stats.models;

    if (Memory::Read32(Memory::Read32(object) + 0x0c) != ScnMdlProc) {
        ++stats.other_class;
        return false;
    }

    // A draw callback may draw or move more than the model's own meshes.
    if (Memory::Read32(object + CallbackObject) && (Memory::Read16(object + CallbackFlags) & 0x30) && (Memory::Read8(object + CallbackMask) & 5)) {
        ++stats.callback;
        return false;
    }

    // Both draw tasks disabled: the draw does nothing anyway.
    if ((Memory::Read32(object + ProcDisabled) & 0x60) == 0x60)
        return false;

    const uint32_t model = Memory::Read32(object + ResMdl);
    const Record* r = Lookup(model);

    if (!r) {
        ++stats.no_table;
        return false;
    }

    if (!r->bounded) {
        ++stats.unbounded;
        return false;
    }

    const mh3_cull_frustum* f = CurrentFrustum();

    if (!f)
        return false;

    ++stats.tested;
    const uint32_t base = NativeModelViewArray(object, 0);

    if (!base)
        return false;

    const size_t count = r->ids.size();

    // Grow only: a resize back up to a larger model's count would zero the
    // elements again before they are overwritten.
    if (matrixValues.size() < count * 12) {
        matrixValues.resize(count * 12);
        matrixPointers.resize(count);
    }

    for (size_t i = 0; i < count; ++i) {
        if (!mh3_scene_cull_read_matrix(GuestRange(base + r->ids[i] * 48, 48), &matrixValues[i * 12]))
            return false;

        matrixPointers[i] = &matrixValues[i * 12];
    }

    if (!mh3_cull_boxes_outside(f, matrixPointers.data(), r->boxes.data(), unsigned(count)))
        return false;

    ++stats.culled;
    Demand(r->owner);
    return true;
}

NativeSceneCullStatistics TakeNativeSceneCullStatistics() {
    return std::exchange(stats, {});
}
