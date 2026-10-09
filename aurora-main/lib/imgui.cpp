#include "imgui.hpp"

#include <imgui.h>

namespace aurora::imgui {
void create_context() noexcept {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
}

void initialize() noexcept {}

void shutdown() noexcept {
  if (ImGui::GetCurrentContext() != nullptr) {
    ImGui::DestroyContext();
  }
}

void process_event(const SDL_Event&) noexcept {}

bool wants_capture_event(const SDL_Event&) noexcept { return false; }

void new_frame(const AuroraWindowSize&) noexcept {}
} // namespace aurora::imgui
