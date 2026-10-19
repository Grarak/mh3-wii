#pragma once
// The mouse's share of the field camera (mouse look, controller_host.h).
// FieldCameraUpdate takes the motion once a step; the yaw and pitch input
// functions (camera_input.cpp) apply it: across, it turns the camera's yaw
// one to one, without the stick's ramp; up and down, where the stick steps
// the five distance rows on land, it moves between them continuously (a row
// blended from the two nearest, which the camera's row pointer then names)
// and, swimming, tilts the pitch.
#include <cstdint>

namespace CameraMouse {
// FieldCameraUpdate, at its start: this step's motion.
void Take();

// The step's motion in pixels times the sensitivity (y down, MH3_MOUSE_INVERT_Y
// applied).
float Dx();
float Dy();

// Land: the distance level, 0..4, as the mouse left it; the stick and the
// game's own changes (a reset, a target snap) move it to their whole level.
float Level(uint32_t gameLevel);
void SetLevel(float level);

// The row of a distance level: `rows` + level * 28 for a whole level, else the
// blend of the two nearest in guest memory reserved at boot (main.cpp).
inline constexpr uint32_t kRowBytes = 32;
void ReserveRow(uint32_t base);
uint32_t Row(uint32_t rows, float level);
} // namespace CameraMouse
