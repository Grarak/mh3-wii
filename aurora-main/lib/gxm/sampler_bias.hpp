#pragma once

namespace aurora::gxm {
// DADJUST encodes (value - 31) / 8. Never silently round a material's bias.
// LINEAR_STRIDED uses these bits for stride and cannot use sampler bias.
constexpr int sampler_bias_encoding(float bias) noexcept {
  if (!(bias >= -3.875f && bias <= 4.f))
    return -1;
  const float eighths = bias * 8.f;
  const int integer = static_cast<int>(eighths);
  return float(integer) == eighths ? integer + 31 : -1;
}
} // namespace aurora::gxm
