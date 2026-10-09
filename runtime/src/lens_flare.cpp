// The sun's lens flare, RMHP08 (task 0x80101E1C, decompiled with Ghidra):
//   0x80101E58  setup: the glare model (task +0x38 -> flare +0x40) and up to
//               12 flare part models (flare +0x10...), the area's sun position
//               (flare +0x48)
//   0x80102008  LensFlareUpdate (task): returns at once while the hunter is
//               underwater (*(u8*)0x806BB854 & 1) with the camera below y 0;
//               else projects the sun, ramps the part visibility (flare +0xC,
//               0x80102834's raycast, per step) and submits each part at its
//               alpha times that visibility, then runs the glare fade
//   0x80102660  the glare fade (task, visible, position): a flash keyed over
//               0x34 steps when the sun comes into view, 7 steps out; it
//               submits the glare model every call
//
// The early return keeps the visibility and the fade where they were, so an
// update that gets past it underwater draws the parts at the visibility the
// sun had above water. Both are called through: the generated dispatch
// profile names these natives, so a release build without them stopped at
// its start ("omits translated winner 0x80102660").
#include "abi_bridge.h"

extern "C" void func_80102008(CpuContext* ctx);
extern "C" void MH3_LensFlareUpdate(CpuContext* ctx) {
    func_80102008(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80102008, MH3_LensFlareUpdate, "LensFlareUpdate");

extern "C" void func_80102660(CpuContext* ctx);
extern "C" void MH3_LensFlareFade(CpuContext* ctx) {
    func_80102660(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80102660, MH3_LensFlareFade, "LensFlareFade");
