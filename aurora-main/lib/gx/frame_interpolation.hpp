#pragma once

#include "gx.hpp"

#include "aurora/gfx.h"

#include <atomic>

// Frame interpolation: generates intermediate presentation frames between two consecutive 60 Hz
// guest frames by re-staging each perspective draw's uniform block with interpolated transforms.
namespace aurora::gx {
constexpr uint32_t MaxInterpolatedFrames = 3;

// Identifies a draw across frames. `combined` includes the geometry signature and is exact, while
// `pipeline`/`texture` is the material-only fallback for meshes whose vertex data changes.
struct FrameInterpolationDrawIdentity {
  HashType combined = 0;
  HashType pipeline = 0;
  HashType texture = 0;
  // Hash of the per-vertex PNMTXIDX stream. Matrix values animate, but this topology decides which
  // absolute palette slot each vertex reads, so it must match across a pair.
  HashType matrixTopology = 0;
};

// Where the transforms live inside a draw's staged uniform block. Offsets are
// relative to the start of the mapped range.
struct InterpolatedUniformLayout {
  const uint8_t* sourceUniformData = nullptr;
  size_t uniformSize = 0;
  size_t projectionOffset = 0;
  size_t positionOffset = 0;
  size_t normalOffset = 0;
  // Slot the live matrix occupies; a compacted position region holds it at 0.
  size_t currentMatrix = 0;
  bool indexedMatrices = false;
};

namespace detail {
// Defined in frame_interpolation.cpp, exposed so the early-outs below stay inline: the shipped
// build compiles shards without LTO, so a cross-TU call would land on every draw in the frame.
extern std::atomic_uint32_t g_frameInterpolationFps;
} // namespace detail

// 0 when interpolation is disabled; otherwise the configured target (120/180/240).
inline uint32_t frame_interpolation_fps() noexcept {
  return detail::g_frameInterpolationFps.load(std::memory_order_acquire);
}
inline bool frame_interpolation_active() noexcept { return frame_interpolation_fps() != 0; }

void set_frame_interpolation_fps(uint32_t targetFps) noexcept;

// Feedback for the adaptive slot controller: whether the producer met its
// retrace boundary for the frame that just presented.
void report_producer_paced(bool paced) noexcept;

void begin_frame_interpolation() noexcept;
void finalize_frame_interpolation() noexcept;
// Fills the observability snapshot behind aurora_get_frame_interpolation_diagnostics.
void get_frame_interpolation_diagnostics(AuroraFrameInterpolationDiagnostics& diagnostics) noexcept;
bool has_interpolated_frame() noexcept;
uint32_t interpolated_frame_count() noexcept;
void mark_frame_interpolation_replay_unsafe() noexcept;
// Drops the interpolation tasks staged into the currently mapped uniform range. Required of
// anything that unmaps or rotates that range before the frame is finalized.
void drop_pending_frame_interpolation_uniforms() noexcept;
bool frame_interpolation_replay_safe() noexcept;

// Records one perspective draw and maps its intermediate uniform copies, returning the mapped
// range per slot (empty when the draw has no counterpart). Called by build_uniform.
std::array<gfx::Range, MaxInterpolatedFrames> record_interpolation_draw(
    const FrameInterpolationDrawIdentity& identity, const Mat4x4<float>& projection,
    uint16_t usedPnMtxMask, const InterpolatedUniformLayout& uniformLayout) noexcept;

// Folds a merged draw back into the snapshot of the draw it joined. aurora renders merged
// primitives through the first one's uniform block, so without this the merged-in bones tear.
void extend_interpolation_draw(uint16_t usedPnMtxMask) noexcept;

bool interpolate_transform(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                           float weight, Mat3x4<float>& output) noexcept;
bool interpolate_transform_midpoint(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                                    Mat3x4<float>& output) noexcept;
// Matrix palettes are already-composed skinning transforms, so interpolating their coefficients
// keeps shared boundaries intact. Ordinary one-matrix draws keep the rigid TRS path above.
bool interpolate_indexed_transform(const Mat3x4<float>& previous,
                                   const Mat3x4<float>& current, float weight,
                                   Mat3x4<float>& output) noexcept;
float transform_match_distance_squared(const Mat3x4<float>& previous,
                                       const Mat3x4<float>& current) noexcept;
} // namespace aurora::gx
