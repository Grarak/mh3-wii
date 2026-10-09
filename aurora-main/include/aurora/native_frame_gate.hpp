#pragma once

namespace aurora {
// Single-thread frame admission. Poll once before any native draw preparation
// or mapped buffer writes, and retain that decision until the frame ends.
class NativeFrameGate {
public:
  template <class PendingGpu>
  bool begin(bool frameskip, bool presentable, PendingGpu pending) noexcept {
    m_active = true;
    m_render = presentable && (!frameskip || !pending());
    return m_render;
  }
  bool active() const noexcept { return m_active; }
  bool should_render() const noexcept { return m_active && m_render; }
  bool drawing_allowed() const noexcept { return !m_active || m_render; }
  void end() noexcept { m_active = false; m_render = false; }

private:
  bool m_active = false;
  bool m_render = false;
};
// Backend draw/clear/present entry points also reject a skipped native frame,
// while allowing standalone resource preparation outside a game frame.
bool native_frame_drawing_allowed() noexcept;
} // namespace aurora
