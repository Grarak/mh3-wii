// Native visibility culling of model draws (model_cull.h).
#include "model_cull.h"
#include "bytes_equal.h"
#include "native_graphics.h"
#include "runtime/native/mh3_model_cull.h"
#include <atomic>
#include <cstdlib>
#include <cstring>

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
// Render thread: the last draw's frustum and what it was made from; draws in
// a row mostly share the projection, viewport and scissor.
struct FrustumInputs {
    float projection[16], viewport[4];
    int32_t scissor[4];
};
struct {
    FrustumInputs inputs;
    mh3_cull_frustum value;
    bool known = false, valid = false;
} lastFrustum;
} // namespace

bool CullNativeModelMesh(const mh3_resolved_mesh_submit& mesh) {
    if (!Enabled() || !mesh.palette || !mesh.projection)
        return false;
    draws.fetch_add(1, std::memory_order_relaxed);
    const auto& state = GetNativeDrawState(NativeOwner::Render);
    const float* viewport = state.viewport;
    int32_t scissor[4];
    if (!GetNativeScissor(scissor, NativeOwner::Render)) {
        scissor[0] = int32_t(viewport[0]);
        scissor[1] = int32_t(viewport[1]);
        scissor[2] = int32_t(viewport[2] + 1);
        scissor[3] = int32_t(viewport[3] + 1);
    }
    auto& last = lastFrustum;
    auto& inputs = last.inputs;
    if (!last.known || !BytesEqual(mesh.projection, inputs.projection, sizeof inputs.projection) || !BytesEqual(viewport, inputs.viewport, sizeof inputs.viewport) ||
        !BytesEqual(scissor, inputs.scissor, sizeof inputs.scissor)) {
        std::memcpy(inputs.projection, mesh.projection, sizeof inputs.projection);
        std::memcpy(inputs.viewport, viewport, sizeof inputs.viewport);
        std::memcpy(inputs.scissor, scissor, sizeof inputs.scissor);
        last.known = true;
        float lo[2], hi[2];
        last.valid = mh3_cull_scissor_rect(inputs.viewport, inputs.scissor, Margin, lo, hi) && mh3_cull_frustum_make(inputs.projection, lo, hi, &last.value);
    }
    if (!last.valid)
        return false;
    const auto& frustum = last.value;
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
