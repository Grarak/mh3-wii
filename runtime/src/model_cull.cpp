// Native visibility culling of model draws (model_cull.h).
#include "model_cull.h"
#include "native_graphics.h"
#include "runtime/native/mh3_model_cull.h"
#include <atomic>
#include <cstdlib>

namespace {
bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("MH3_MODEL_CULL");
        return !value || value[0] != '0';
    }();
    return enabled;
}
// Widens the drawable rectangle by 1% of its NDC extent: covers rounding and
// the GXM rasterizer's sub-pixel offset.
constexpr float Margin = 0.02f;
std::atomic<uint64_t> draws{0}, culled{0};
} // namespace

bool CullNativeModelMesh(const mh3_resolved_mesh_submit& mesh) {
    if (!Enabled() || !mesh.palette || !mesh.projection)
        return false;
    draws.fetch_add(1, std::memory_order_relaxed);
    const auto& state = GetNativeDrawState(NativeOwner::Render);
    int32_t scissor[4];
    const float viewport[4]{state.viewport[0], state.viewport[1], state.viewport[2], state.viewport[3]};
    if (!GetNativeScissor(scissor, NativeOwner::Render)) {
        scissor[0] = int32_t(viewport[0]);
        scissor[1] = int32_t(viewport[1]);
        scissor[2] = int32_t(viewport[2] + 1);
        scissor[3] = int32_t(viewport[3] + 1);
    }
    float lo[2], hi[2];
    mh3_cull_frustum frustum;
    if (!mh3_cull_scissor_rect(viewport, scissor, Margin, lo, hi) || !mh3_cull_frustum_make(mesh.projection, lo, hi, &frustum))
        return false;
    // The position matrices the vertices select: the batch's required slots
    // when the layout carries PN selectors, else the current matrix (slot 0).
    const bool selectors = (mesh.layout.flags >> MH3_LAYOUT_MATRIX_MASK_SHIFT) & 1;
    uint32_t slots = selectors && mesh.palette_batch ? mesh.palette_batch->position_required : 1;
    if (!slots)
        return false;
    const float* matrices[10];
    mh3_cull_box boxes[10];
    unsigned count = 0;
    for (unsigned slot = 0; slot < 10; slot++) {
        if (!(slots & (1u << slot)))
            continue;
        if (!(mesh.palette->position_valid & (1u << slot)))
            return false;
        matrices[count] = &mesh.palette->position[slot].v[0][0];
        for (unsigned c = 0; c < 3; c++) {
            boxes[count].min[c] = mesh.mesh.bounds_min[c];
            boxes[count].max[c] = mesh.mesh.bounds_max[c];
        }
        ++count;
    }
    if (!mh3_cull_boxes_outside(&frustum, matrices, boxes, count))
        return false;
    culled.fetch_add(1, std::memory_order_relaxed);
    return true;
}

NativeModelCullStatistics TakeNativeModelCullStatistics() {
    return {draws.exchange(0, std::memory_order_relaxed), culled.exchange(0, std::memory_order_relaxed)};
}
