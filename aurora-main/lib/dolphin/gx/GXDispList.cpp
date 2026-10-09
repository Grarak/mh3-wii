#include <aurora/legacy_gx.h>
#include "gx.hpp"
#include "__gx.h"

#include "../../gx/command_processor.hpp"
#include "../../gx/fifo.hpp"

#include <cstring>

static __GXData_struct sSavedGXData;

extern "C" {
void GXBeginDisplayList(void* list, u32 size) {
  aurora_legacy_gx_panic("GXBeginDisplayList");
}

u32 GXEndDisplayList() {
  aurora_legacy_gx_panic("GXEndDisplayList");
}

void GXCallDisplayList(const void* data, u32 nbytes) {
  aurora_legacy_gx_panic("GXCallDisplayList");
}

void GXCallDisplayListLE(const void* data, u32 nbytes) {
  aurora_legacy_gx_panic("GXCallDisplayListLE");
}
}
