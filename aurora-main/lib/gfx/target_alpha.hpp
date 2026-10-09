#pragma once
#include <array>

namespace aurora::gfx {
// Ordered commands certify physical target bytes, not material/source alpha.
// The object belongs to one live target; a replacement starts uncertified.
struct TargetAlpha {
  bool one = false;

  void invalidate() noexcept { one = false; }
  void draw(bool writesAlpha, bool preservesOne = false) noexcept {
    if (writesAlpha && !preservesOne)
      invalidate();
  }

  void clear(bool writesAlpha, float alpha, bool fullTarget) noexcept {
    if (!writesAlpha)
      return;

    one = alpha == 1.f && (fullTarget || one);
  }

  void copy(bool sourceOne, bool fullTarget) noexcept {
    one = sourceOne && (fullTarget || one);
  }
};

// An explicit fixed-program promise and a live certificate are both required.
// Discard is allowed: source alpha must still be compared before output A=1.
inline bool target_alpha_one_eligible(bool rgba8, bool display, const TargetAlpha& alpha,
                                      bool blend, const std::array<bool, 4>& mask) noexcept {
  return rgba8 && !display && alpha.one && !blend && mask[0] && mask[1] && mask[2] && !mask[3];
}

inline bool target_alpha_one_expand(bool eligible, bool fixedProgram, bool preservesOne, bool replacesDepth) noexcept {
  return eligible && fixedProgram && preservesOne && !replacesDepth;
}
} // namespace aurora::gfx
