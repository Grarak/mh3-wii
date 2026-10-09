// NW4R g3d AnmObjChrRes (runtime/src/anm_obj_chr_res.cpp).
#pragma once

#include "guest_call.h"

#include <cstdint>

inline constexpr uint32_t kAnmObjChrResVtable = 0x8058C468u;

// Model node `index`'s result of AnmObjChrRes `anim` at `frame`, as setting
// that frame and calling GetResult would give, without changing the object's
// frame or result cache: the node alone is evaluated into `result` (whose
// flags are 0 for an unbound node). Returns `result`.
uint32_t AnmObjChrResResultAt(GuestCall& game, uint32_t anim, uint32_t result, uint32_t index, float frame);
