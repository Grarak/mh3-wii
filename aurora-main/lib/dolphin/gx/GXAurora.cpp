#include <aurora/legacy_gx.h>
#include "dolphin/gx/GXAurora.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "__gx.h"
#include "gx.hpp"
#include "../../window.hpp"

#include "../../gfx/common.hpp"
#include "../../gx/fifo.hpp"

// Single definition for the `Log` that gx.hpp declares for this directory.
aurora::Module Log("aurora::gx");

static void GXWriteString(const char* label) {
  aurora_legacy_gx_panic("GXWriteString");
}

void GXPushDebugGroup(const char* label) {
  aurora_legacy_gx_panic("GXPushDebugGroup");
}

void GXPopDebugGroup() {
  aurora_legacy_gx_panic("GXPopDebugGroup");
}

void GXInsertDebugMarker(const char* label) {
  aurora_legacy_gx_panic("GXInsertDebugMarker");
}

void AuroraSetViewportPolicy(AuroraViewportPolicy policy) {
  const bool changed = g_gxState.viewportPolicy != policy;

  if (changed) {
    // Finish commands using the old framebuffer mapping before changing it.
    aurora::gx::fifo::drain();
  }

  g_gxState.viewportPolicy = policy;
  aurora::window::set_frame_buffer_aspect_fit(policy == AURORA_VIEWPORT_FIT);
  aurora::window::set_force_aspect_16_9(policy == AURORA_VIEWPORT_16_9);
  aurora::window::set_present_surface_fill(policy == AURORA_VIEWPORT_STRETCH);

  if (changed) {
    // Reapply the guest viewport and scissor after a resize.
    aurora::gx::set_logical_viewport(g_gxState.logicalViewport);
    aurora::gx::set_logical_scissor(g_gxState.logicalScissor);
  }
}

void AuroraGetRenderSize(u32* width, u32* height) {
  // The guest CPU thread uses this safe render-size value outside render passes.
  const auto renderSize = aurora::gfx::get_frame_buffer_size();

  if (width != nullptr) {
    *width = renderSize.x;
  }

  if (height != nullptr) {
    *height = renderSize.y;
  }
}

void AuroraGetSurfaceSize(u32* width, u32* height) {
  const auto windowSize = aurora::window::get_window_size();

  if (width != nullptr) {
    *width = windowSize.native_fb_width;
  }

  if (height != nullptr) {
    *height = windowSize.native_fb_height;
  }
}

void GXSetViewportRender(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
  aurora_legacy_gx_panic("GXSetViewportRender");
}

void GXSetScissorRender(u32 left, u32 top, u32 wd, u32 ht) {
  aurora_legacy_gx_panic("GXSetScissorRender");
}

namespace {
void WriteMappedRenderState(const aurora::gx::MappedRenderState& mapped) {
  GXSetViewportRender(mapped.viewport.left, mapped.viewport.top, mapped.viewport.width, mapped.viewport.height,
                      mapped.viewport.znear, mapped.viewport.zfar);
  GXSetScissorRender(static_cast<u32>(std::max(mapped.scissor.x, 0)),
                     static_cast<u32>(std::max(mapped.scissor.y, 0)),
                     static_cast<u32>(std::max(mapped.scissor.width, 0)),
                     static_cast<u32>(std::max(mapped.scissor.height, 0)));
}
} // namespace

void GXSetViewportScissorRenderSafeArea(f32 aspect) {
  aurora_legacy_gx_panic("GXSetViewportScissorRenderSafeArea");
}

void GXRestoreViewportScissorRender() {
  aurora_legacy_gx_panic("GXRestoreViewportScissorRender");
}

void GXSetTexCopySrcRender(u16 left, u16 top, u16 wd, u16 ht) {
  aurora_legacy_gx_panic("GXSetTexCopySrcRender");
}

void GXCreateFrameBuffer(u32 width, u32 height) {
  aurora_legacy_gx_panic("GXCreateFrameBuffer");
}

void GXRestoreFrameBuffer() {
  aurora_legacy_gx_panic("GXRestoreFrameBuffer");
}
