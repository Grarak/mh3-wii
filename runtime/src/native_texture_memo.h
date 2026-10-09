// A material record's texture slot as the render thread last bound it
// (BindNativeTexture with a memo, native_texture_state.cpp).
#pragma once
#include "runtime/native/mh3_model_state.h"
#include <aurora/native_texture.hpp>
#include <cstdint>

// The backend's resolution and what resolving the binding's source read
// besides the binding: the texture producers' revision and an indexed
// image's bound palette. While both are unchanged, the same binding rebinds
// the resolved texture without resolving its source again.
struct NativeTextureBindMemo {
    aurora::NativeTextureMemo backend;
    uint64_t revision = 0;
    mh3_native_palette_source palette{};
};
