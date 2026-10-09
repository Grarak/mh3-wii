#pragma once

#include <aurora/event.h>

union SDL_Event;

// The OpenGL ES renderer has no ImGui backend; these keep the context alive
// for embedders that link ImGui, but nothing is drawn.
namespace aurora::imgui {
void create_context() noexcept;
void initialize() noexcept;
void shutdown() noexcept;

void process_event(const SDL_Event& event) noexcept;
bool wants_capture_event(const SDL_Event& event) noexcept;
void new_frame(const AuroraWindowSize& size) noexcept;
} // namespace aurora::imgui
