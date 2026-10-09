#pragma once

#include <dolphin/gx/GXStruct.h>

#include <cstdint>

#include <aurora/math.hpp>

namespace aurora::vi {
void configure(const GXRenderModeObj* rm) noexcept;
Vec2<uint32_t> configured_fb_size() noexcept;
Vec2<uint32_t> visible_fb_size() noexcept;
float present_aspect_correction() noexcept;
} // namespace aurora::vi
