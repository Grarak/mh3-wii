#pragma once

#include <aurora/event.h>

union SDL_Event;

namespace wgpu {
class RenderPassEncoder;
} // namespace wgpu

namespace aurora::imgui {
void create_context() noexcept;
void initialize() noexcept;
void shutdown() noexcept;

void process_event(const SDL_Event& event) noexcept;
bool wants_capture_event(const SDL_Event& event) noexcept;
void new_frame(const AuroraWindowSize& size) noexcept;
// Build this frame's ImGui draw data. Idempotent for the rest of the frame: the lists are built
// once and every presentation slot replays them. Reset by new_frame.
void render_frame_data() noexcept;
void render(const wgpu::RenderPassEncoder& pass) noexcept;
} // namespace aurora::imgui
