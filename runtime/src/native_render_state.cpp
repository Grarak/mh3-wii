// Direct application render state. No GX calls, packets, register banks or FIFO.
#include "native_graphics.h"
#include "fatal.h"
#include "native_shade_snapshot.h"
#include "runtime/native/adapters/model_pose_bridge.h"
#include "gl/gl.hpp"
#include "gl/device.hpp"
#include "dolphin/vi/vi_internal.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace {
struct NormalMatrix {
    std::array<float, 9> values;
    bool known = false;
};

// Logical state per owner: the game thread records against Game; the render
// thread restores snapshots into Render and applies them.
struct DrawOwner {
    std::array<NormalMatrix, 10> normalMatrices{};
    std::array<int32_t, 4> scissor{};
    bool viewportKnown = false, scissorKnown = false, rasterKnown = false;
    mh3_draw_state drawState = {
        {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, {0, 0, 640, 448, 0, 1}, 0, {0, 1, 0, 3, 1, 3, 1, 0, 1, 1}};
};

constinit DrawOwner owners[NativeOwnerCount]{};
DrawOwner& D(NativeOwner owner = NativeOwner::Game) {
    return owners[NativeOwnerIndex(owner)];
}

// Game: the material imports raster publications.
constinit NativeRasterObserver rasterObserver = nullptr;
void Changed(NativeOwner owner = NativeOwner::Game) {
    ++nativeStateRevision[NativeOwnerIndex(owner)][NATIVE_STATE_DRAW];
}

std::array<float, 2> TargetScale() {
    const auto size = aurora::vi::configured_fb_size();

    if (!size.x || !size.y)
        MH3_THROW(std::runtime_error("Native target has no base dimensions"));

    return {float(aurora::gpu::g_frameBuffer.width) / size.x, float(aurora::gpu::g_frameBuffer.height) / size.y};
}
} // namespace

const mh3_draw_state& GetNativeDrawState(NativeOwner owner) {
    return D(owner).drawState;
}

void GetNativeRenderViewportSize(float out[2]) {
    const auto scale = TargetScale();
    const auto& viewport = D(NativeOwner::Render).drawState.viewport;
    out[0] = float(std::lround(viewport[2] * scale[0]));
    out[1] = float(std::lround(viewport[3] * scale[1]));
}

void SetNativeRasterObserver(NativeRasterObserver observer) {
    rasterObserver = observer;
}
#ifdef MH3_DEVELOPMENT
namespace {
// MH3_RENDER_WIDEN=f (development): draw every perspective projection with an
// f times wider and taller view than the game's camera, which its own
// visibility tests use. Objects the game leaves out show as gaps in the outer
// strip of the picture.
float RenderWiden() {
    static const float widen = [] {
        const char* value = std::getenv("MH3_RENDER_WIDEN");
        const float f = value ? std::strtof(value, nullptr) : 1.f;
        return std::isfinite(f) && f > 0 ? f : 1.f;
    }();
    return widen;
}
} // namespace
#endif
void SetNativeProjection(const float matrix[16], bool perspective) {
    auto& state = D().drawState;
    std::memcpy(state.projection, matrix, sizeof(state.projection));
#ifdef MH3_DEVELOPMENT
    if (perspective && RenderWiden() != 1.f)
        for (unsigned i = 0; i < 8; ++i)
            state.projection[i] /= RenderWiden();
#endif
    state.perspective = perspective;
    Changed();
}

void SetNativePositionMatrix(const float matrix[12], NativeOwner owner) {
    PublishNativeModelPositionMatrix(0, matrix, owner);
    auto& state = D(owner).drawState;
    std::memcpy(state.position, matrix, sizeof(state.position));
    Changed(owner);
}

void SetNativeNormalMatrix(uint32_t selector, const float matrix[9], NativeOwner owner) {
    PublishNativeModelNormalMatrix(selector, matrix, owner);
    auto& destination = D(owner).normalMatrices[selector / 3];
    std::memcpy(destination.values.data(), matrix, sizeof(float) * 9);
    destination.known = true;
}

bool GetNativeNormalMatrix(uint32_t selector, float matrix[9]) {
    const auto& source = D().normalMatrices[selector / 3];

    if (!source.known)
        return false;

    std::memcpy(matrix, source.values.data(), sizeof(float) * 9);
    return true;
}

void SetNativeViewport(float x, float y, float width, float height, float near_z, float far_z) {
    const float values[]{x, y, width, height, near_z, far_z};

    for (float v : values)
        if (!std::isfinite(v))
            MH3_THROW(std::runtime_error("Non-finite native viewport"));
    if (width < 0 || height < 0)
        MH3_THROW(std::runtime_error("Negative native viewport"));

    auto& d = D();
    std::memcpy(d.drawState.viewport, values, sizeof(values));
    d.viewportKnown = true;
    Changed();
}

bool GetNativeScissor(int32_t out[4], NativeOwner owner) {
    if (!out)
        MH3_THROW(std::runtime_error("Null native scissor destination"));

    const auto& d = D(owner);

    if (!d.scissorKnown)
        return false;

    std::memcpy(out, d.scissor.data(), sizeof(int32_t) * 4);
    return true;
}

void SetNativeScissor(int32_t x, int32_t y, int32_t width, int32_t height) {
    if (width < 0 || height < 0)
        MH3_THROW(std::runtime_error("Negative native scissor"));

    D().scissor = {x, y, width, height};
    D().scissorKnown = true;
    Changed();
}

namespace {
void CheckRaster(const mh3_raster_state& raster) {
    if (raster.blend_mode > 3 || raster.src_factor > 7 || raster.dst_factor > 7 || raster.depth_compare > 7 || raster.cull_mode > 3 || raster.logic_op > 15)
        MH3_THROW(std::runtime_error("Native raster state is outside its typed domain"));

    if (raster.blend_mode == 2 && raster.logic_op != 3 && raster.logic_op != 5)
        MH3_THROW(std::runtime_error("Native bitwise logic operation requires a framebuffer material"));
}

aurora::gl::RasterState BackendRaster(const mh3_raster_state& raster) {
    aurora::gl::RasterState out;

    // Source color factors read destination color; destination factors read
    // source color. Alpha uses the corresponding alpha factor on each side.
    constexpr GLenum source[]{GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    constexpr GLenum destination[]{GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    constexpr GLenum sourceAlpha[]{GL_ZERO, GL_ONE, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    constexpr GLenum destinationAlpha[]{GL_ZERO, GL_ONE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    constexpr GLenum depth[]{GL_NEVER, GL_GREATER, GL_EQUAL, GL_GEQUAL, GL_LESS, GL_NOTEQUAL, GL_LEQUAL, GL_ALWAYS};
    out.blend = raster.blend_mode == 1 || raster.blend_mode == 3;
    out.srcColor = source[raster.src_factor];
    out.dstColor = destination[raster.dst_factor];
    out.srcAlpha = sourceAlpha[raster.src_factor];
    out.dstAlpha = destinationAlpha[raster.dst_factor];

    if (raster.blend_mode == 3) {
        out.srcColor = out.dstColor = out.srcAlpha = out.dstAlpha = GL_ONE;
        out.colorEquation = out.alphaEquation = GL_FUNC_REVERSE_SUBTRACT;
    }

    out.depthTest = raster.depth_test != 0;
    out.depthWrite = raster.depth_write != 0;
    out.depthFunc = depth[raster.depth_compare];
    out.cull = raster.cull_mode != 0;
    out.cullFace = raster.cull_mode == 1 ? GL_FRONT : GL_BACK;
    out.colorMask = {raster.color_write != 0, raster.color_write != 0, raster.color_write != 0, raster.alpha_write != 0};

    if (raster.cull_mode == 3) {
        out.cull = false;
        out.depthTest = true;
        out.depthFunc = GL_NEVER;
        out.depthWrite = false;
    }

    if (raster.blend_mode == 2 && raster.logic_op == 5)
        out.colorMask = {false, false, false, false};

    return out;
}

void SetRaster(const mh3_raster_state& raster, bool publish) {
    CheckRaster(raster);
    auto& d = D();
    const auto& previous = d.drawState.raster;

    if (!d.rasterKnown || previous.blend_mode != raster.blend_mode || previous.src_factor != raster.src_factor || previous.dst_factor != raster.dst_factor ||
        previous.logic_op != raster.logic_op || previous.depth_test != raster.depth_test || previous.depth_compare != raster.depth_compare ||
        previous.depth_write != raster.depth_write || previous.cull_mode != raster.cull_mode || previous.color_write != raster.color_write || previous.alpha_write != raster.alpha_write) {
        d.drawState.raster = raster;
        d.rasterKnown = true;
        Changed();
    }

    // Material restoration can leave its imported pixel fields stale even
    // when the draw raster is unchanged. Always retain that publication.
    if (publish && rasterObserver)
        rasterObserver(raster);
}

// Render thread: apply the logical viewport, scissor and raster to the backend.
void ApplyRenderState() {
    if (!NativeGraphicsFrameShouldRender())
        return;

    const auto& d = D(NativeOwner::Render);

    // Convert logical rectangles again because the target may have resized.
    const auto scale = TargetScale();

    if (d.viewportKnown) {
        const auto& v = d.drawState.viewport;
        aurora::gl::set_viewport(std::lround(v[0] * scale[0]), std::lround(v[1] * scale[1]), std::lround(v[2] * scale[0]), std::lround(v[3] * scale[1]),
                                 1.f - std::clamp(std::max(v[4], v[5]), 0.f, 1.f), 1.f - std::clamp(std::min(v[4], v[5]), 0.f, 1.f));
    }

    if (d.scissorKnown) {
        const auto [x, y, width, height] = d.scissor;
        const int32_t left = std::floor(x * scale[0]), top = std::floor(y * scale[1]);
        const int32_t right = std::ceil((double(x) + width) * scale[0]), bottom = std::ceil((double(y) + height) * scale[1]);
        aurora::gl::set_scissor_enabled(true);
        aurora::gl::set_scissor(left, top, right - left, bottom - top);
    } else
        aurora::gl::set_scissor_enabled(false);

    if (d.rasterKnown)
        aurora::gl::apply_raster_state(BackendRaster(d.drawState.raster));
}
} // namespace

void SetNativeRaster(const mh3_raster_state& raster) {
    SetRaster(raster, true);
}

void SetNativeRaster(const mh3_raster_state& raster, NativeOwner owner) {
    if (owner == NativeOwner::Game) {
        SetRaster(raster, true);
        return;
    }

    // Render and Shadow publish logically; the render thread applies the
    // result once per draw (ApplyNativeRenderRaster).
    CheckRaster(raster);
    auto& d = D(owner);

    if (!d.rasterKnown || std::memcmp(&d.drawState.raster, &raster, sizeof raster)) {
        d.drawState.raster = raster;
        d.rasterKnown = true;
        Changed(owner);
    }
}

void ApplyNativeRenderRaster() {
    const auto& d = D(NativeOwner::Render);

    if (d.rasterKnown && NativeGraphicsFrameShouldRender())
        aurora::gl::apply_raster_state(BackendRaster(d.drawState.raster));
}

void CopyNativeDrawRaster(NativeOwner from, NativeOwner to) {
    D(to).drawState.raster = D(from).drawState.raster;
    D(to).rasterKnown = D(from).rasterKnown;
}

void CaptureNativeDrawSnapshot(NativeDrawSnapshot& out) {
    const auto& d = D();
    out.draw = d.drawState;
    out.scissor = d.scissor;
    out.viewportKnown = d.viewportKnown;
    out.scissorKnown = d.scissorKnown;
    out.rasterKnown = d.rasterKnown;
}

void RestoreNativeDrawSnapshot(const NativeDrawSnapshot& snapshot) {
    // The material snapshot carries its own raster import; do not publish.
    auto& d = D(NativeOwner::Render);
    d.drawState = snapshot.draw;
    d.scissor = snapshot.scissor;
    d.viewportKnown = snapshot.viewportKnown;
    d.scissorKnown = snapshot.scissorKnown;
    d.rasterKnown = snapshot.rasterKnown;
    Changed(NativeOwner::Render);
    ApplyRenderState();
}

void CopyNativeDrawStoreForDraw() {
    owners[unsigned(NativeOwner::Draw)] = owners[unsigned(NativeOwner::Game)];
    nativeStateRevision[unsigned(NativeOwner::Draw)][NATIVE_STATE_DRAW] = nativeStateRevision[unsigned(NativeOwner::Game)][NATIVE_STATE_DRAW];
}
