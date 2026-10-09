#pragma once

#include "dolphin/gx/GXEnum.h"
#include "../internal.hpp"

namespace aurora::gx::fifo {

// Optional development capture. Callers describe the FIFO producer, rather
// than the later GX worker. Display-list offsets are set before each draw.
void set_capture_callers(const uint32_t* callers);
void set_capture_display_list(uint32_t address, uint32_t offset);

// Reset the CP write-deduplication state whenever the GX shadow state is reinitialized.
void reset_cp_register_cache();

// Process a buffer of GX FIFO commands
uint32_t process(const uint8_t* data, uint32_t size, bool bigEndian);

// Submit already-packed direct vertex bytes against the current GX state.
bool submit_raw_draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount,
                     uint32_t vertexBytes);

} // namespace aurora::gx::fifo
