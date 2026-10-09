#include <aurora/legacy_gx.h>
#include "gx.hpp"
#include "__gx.h"
#include "dolphin/gx/GXAurora.h"

extern "C" {
void GXDestroyTexObj(GXTexObj* obj_) {
  aurora_legacy_gx_panic("GXDestroyTexObj");
}

void GXDestroyTlutObj(GXTlutObj* obj_) {
  aurora_legacy_gx_panic("GXDestroyTlutObj");
}

void GXDestroyCopyTex(void* dest) {
  aurora_legacy_gx_panic("GXDestroyCopyTex");
}
}
