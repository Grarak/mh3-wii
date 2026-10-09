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
// sun had above water. MH3_TRACE=sun (development) reports each switch
// between updates that run and updates that return early, with the camera's
// height (the point ahead of it the fade gets). Registered in every build:
// the generated dispatch profile names these natives, so a release build
// without them stopped at its start ("omits translated winner 0x80102660").
#include "abi_bridge.h"
#ifdef MH3_DEVELOPMENT
#include "dev_options.h"
#include "memory.h"
#include <chrono>
#include <cstdio>
#endif

namespace {
#ifdef MH3_DEVELOPMENT
constexpr uint32_t kSeaFlag = 0x806BB854; // u8, bit 0: the camera's height decides (set in the Deserted Island, on land too)

bool Traced() {
    static const bool traced = TraceOptions().Has("sun");
    return traced;
}

double Seconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// The fade's call within the update being traced.
bool faded = false;
uint32_t fadeVisible = 0;
float fadeY = 0;
#else
constexpr bool Traced() {
    return false;
}
#endif
} // namespace

extern "C" void func_80102008(CpuContext* ctx);
extern "C" void MH3_LensFlareUpdate(CpuContext* ctx) {
    if (!Traced()) [[likely]] {
        func_80102008(ctx);
        return;
    }
#ifdef MH3_DEVELOPMENT

    static uint32_t calls = 0, ran = 0, switches = 0;
    static int lastRan = -1;
    const uint32_t task = ctx->gpr[3], flare = Memory::Read32(task + 0x38);
    const int sea = Memory::Read8(kSeaFlag) & 1;
    const float before = flare ? Memory::ReadFloat32(flare + 0xC) : 0;
    const uint32_t state = Memory::Read8(task + 6), counter = Memory::Read32(task + 0xC);
    faded = false;
    func_80102008(ctx);
    ++calls;
    ran += faded;

    if (int(faded) != lastRan) {
        ++switches;

        if (faded)
            std::fprintf(stderr, "MH3 sun: %.3f s update %u runs (sea flag %d): sun visible %u, ahead-of-camera y %.1f, part visibility %.3f -> %.3f, glare state %u counter %u alpha %u\n", Seconds(), calls, sea,
                         fadeVisible, fadeY, before, flare ? Memory::ReadFloat32(flare + 0xC) : 0.f, state, counter, flare ? Memory::Read8(flare + 0x47) : 0);
        else
            std::fprintf(stderr, "MH3 sun: %.3f s update %u returns early (sea flag %d), part visibility %.3f\n", Seconds(), calls, sea, before);
    }

    lastRan = faded;

    if (calls % 1800 == 0)
        std::fprintf(stderr, "MH3 sun: %.3f s %u updates, %u ran, %u switches; part visibility %.3f\n", Seconds(), calls, ran, switches, flare ? Memory::ReadFloat32(flare + 0xC) : 0.f);
#endif
}
REGISTER_NATIVE_FUNCTION_AS(0x80102008, MH3_LensFlareUpdate, "LensFlareUpdate");

extern "C" void func_80102660(CpuContext* ctx);
extern "C" void MH3_LensFlareFade(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (Traced()) [[unlikely]] {
        faded = true;
        fadeVisible = ctx->gpr[4] & 0xFF;
        fadeY = Memory::ReadFloat32(ctx->gpr[5] + 4);
    }
#endif
    func_80102660(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80102660, MH3_LensFlareFade, "LensFlareFade");
