#pragma once

#include <string_view>

namespace RuntimeProduct {

enum class Kind {
    BaseGame,
    RetroRewind,
};

struct Descriptor {
    Kind kind;
    std::string_view displayName;
};

// Each public executable links exactly one small provider definition. Keeping
// this selection out of target-wide preprocessor definitions lets the native
// runtime be compiled once and shared by every product.
const Descriptor& Active() noexcept;

inline bool IsRetroRewind() noexcept {
    return Active().kind == Kind::RetroRewind;
}

} // namespace RuntimeProduct
