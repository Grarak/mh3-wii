#pragma once
// Host depth encoding, shared by gx.hpp and the fixed shader sources (which
// the GXM shader bank generator compiles without the GX headers).
namespace aurora::gx {
inline constexpr bool UseReversedZ = true;
} // namespace aurora::gx
