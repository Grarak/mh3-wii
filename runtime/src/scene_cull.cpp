// Game-side visibility of scene-queue models (scene_cull.h).
#include "scene_cull.h"
#ifdef MH3_DEVELOPMENT
#include "dev_options.h"
#endif
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

const mh3_res_memory& MH3NativeResourceMemory();
namespace {
// MH3_VERIFY=cull (development): draw every model and check each the cull
// would reject.
enum class Mode : uint8_t { Cull, Check };
Mode ReadMode() {
#ifdef MH3_DEVELOPMENT
    static const Mode mode = VerifyOptions().Has("cull") ? Mode::Check : Mode::Cull;
    return mode;
#else
    return Mode::Cull;
#endif
}

// The render path's margin (model_cull.cpp): 1% of the rectangle's NDC extent
// on every side covers rounding and the rasterizer's sub-pixel offset.
constexpr float Margin = 0.02f;

// ScnMdl's G3dProc (ModelSceneTask) and its draw-callback fields.
constexpr uint32_t ScnMdlProc = 0x8007DE60;
constexpr uint32_t CallbackObject = 0xd4, CallbackMask = 0xd8, CallbackFlags = 0xda, ProcDisabled = 0xcc, ResMdl = 0xe8;
constexpr uint32_t ViewMatrixCount = 0x68; // ResMdlInfo numViewMtx, from the MDL0

struct Mesh {
    const mh3_prepared_mesh* prepared;
    int32_t matrix_id;
    bool selectors;
};

// One model's bounds: valid while its certified draw table is the current one
// (a table is dropped with its owner's certificate and pack).
struct Record {
    std::shared_ptr<NativeModelDrawTable> table;
    NativeModelOwner owner{};
    bool bounded = false;

    // One box per view matrix ID.
    std::vector<uint32_t> ids;
    std::vector<mh3_cull_box> boxes;
    std::vector<Mesh> meshes;
};

FastMap<uint32_t, Record> records;
uint64_t recordsRevision = UINT64_MAX;
NativeSceneCullStatistics stats;

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

        if (!mh3_scene_cull_add_mesh(&record, &prepared->immutable.mesh, matrix_id, selectors, prepared->palette_batches, prepared->palette_batch_count))
            return;

        r.meshes.push_back({prepared, matrix_id, selectors});
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
#ifdef MH3_DEVELOPMENT
// MH3_MODEL_CULL_GAME=2: rebuild the render path's palettes from the same
// view arrays and test every mesh batch the way the render thread does.
bool CheckCulled(const Record& r, uint32_t object, uint32_t model, const mh3_cull_frustum& f) {
    const uint32_t addresses[3] = {NativeModelViewArray(object, 0), NativeModelViewArray(object, 1), NativeModelViewArray(object, 2)};
    mh3_matrix_sources sources{};

    if (const Status pose = TryReadNativeModelPose(addresses, model, sources); !pose.ok()) {
        std::fprintf(stderr, "MH3 scene cull check: model %08x pose: %s\n", model, pose.message().c_str());
        return false;
    }

    for (const auto& mesh : r.meshes) {
        const auto& prepared = *mesh.prepared;
        mh3_model_palette palette{};

        if (mesh.matrix_id >= 0) {
            mh3_shape_matrix_input input{};
            input.valid = 1;
            input.matrix_id = mesh.matrix_id;
            input.view_array_is_valid = 1;
            mh3_model_palette_rigid(&input, &sources, &palette);
        }

        const auto test = [&](const mh3_pack_palette_batch* batch) {
            const uint16_t used = mesh.selectors && batch ? batch->position_required : 1;
            const float* matrices[10];
            mh3_cull_box box[10];
            unsigned n = 0;

            for (unsigned slot = 0; slot < 10; ++slot) {
                if (!(used & (1u << slot)))
                    continue;

                if (!(palette.position_valid & (1u << slot)))
                    return false;

                matrices[n] = &palette.position[slot].v[0][0];
                std::memcpy(box[n].min, prepared.immutable.mesh.bounds_min, sizeof box[n].min);
                std::memcpy(box[n].max, prepared.immutable.mesh.bounds_max, sizeof box[n].max);
                ++n;
            }

            return mh3_cull_boxes_outside(&f, matrices, box, n) != 0;
        };

        if (!prepared.palette_batch_count) {
            if (!test(nullptr))
                return false;

            continue;
        }

        for (uint32_t i = 0; i < prepared.palette_batch_count; ++i) {
            mh3_model_palette_apply_batch(&prepared.palette_batches[i], &sources, &palette, &palette);

            if (!test(&prepared.palette_batches[i]))
                return false;
        }
    }

    return true;
}
#endif
} // namespace

bool SkipNativeSceneModel(uint32_t object) {
    const Mode mode = ReadMode();

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
#ifdef MH3_DEVELOPMENT
    if (mode == Mode::Check) {
        ++stats.checked;

        if (!CheckCulled(*r, object, model, *f)) {
            static uint32_t reported = 0;
            ++stats.mismatches;

            if (reported++ < 40)
                std::fprintf(stderr, "MH3 scene cull check: model %08x (object %08x) has a visible mesh batch\n", model, object);
        }

        return false;
    }
#endif
    ++stats.culled;
    Demand(r->owner);
    return true;
}

NativeSceneCullStatistics TakeNativeSceneCullStatistics() {
    return std::exchange(stats, {});
}
