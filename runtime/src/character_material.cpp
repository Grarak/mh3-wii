// Native character material preparation, RMHP08 (0x80047DD0).
// Source: Ghidra decompilation plus frame150 producer / frame690 consumer trace.
// The game previously rendered into two EFB rectangles then copied each to
// RGBA8. Publish direct GPU render targets in FIFO order instead. No EFB copy,
// GXDrawDone, content hash, CPU composition, or readback is involved.
#include "abi_bridge.h"
#include "guest_call.h"
#include "gx_host.h"
#include "memory.h"
#include <aurora/generated_texture.hpp>
#include <aurora/native_assets.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace {
aurora::MaterialSource Snapshot(uint32_t object) {
    const uint32_t image = Memory::Read32(object + 8);
    const uint32_t address = (Memory::Read32(object + 12) & 0xffffffu) << 5;
    aurora::MaterialSource source;
    source.width = (image & 1023) + 1;
    source.height = ((image >> 10) & 1023) + 1;
    source.format = (image >> 20) & 15;
    if (source.width != 128 || source.height != 128 ||
        (source.format >= 8 && source.format <= 10))
        throw std::runtime_error("Unexpected character material source: " + std::to_string(source.width) + "x" +
            std::to_string(source.height) + " format " + std::to_string(source.format));
#ifdef __vita__
    if (address >= 64 && Memory::Contains(address - 64, 64)) {
        const uint32_t header = address - 64, tag = Memory::Read32(header + 60);
        if ((tag & 0xf0000000u) == AURORA_NATIVE_TEXTURE_TAG && Memory::Read32(header) == 0x54455830 &&
            Memory::Read32(header + 16) == 64 && Memory::Read16(header + 28) == 128 &&
            Memory::Read16(header + 30) == 128 && Memory::Read32(header + 32) == source.format)
            source.nativeAsset = tag & 0x0fffffffu;
    }
#endif
    if (!source.nativeAsset) {
        uint32_t bytes = 128 * 128;
        switch (source.format) {
        case 0: case 14: bytes /= 2; break;
        case 1: case 2: break;
        case 3: case 4: case 5: bytes *= 2; break;
        case 6: bytes *= 4; break;
        default: throw std::runtime_error("Unsupported character material source format");
        }
        const auto* data = Memory::GetPointer(address, bytes);
        source.bytes.assign(data, data + bytes);
    }
    return source;
}
void PrepareSlot(uint32_t slot) {
    const uint32_t base = Memory::Read8(0x8066BDB0 + slot);
    const uint32_t variant = Memory::Read8(0x8066B985 + slot);
    const int detail = int8_t(Memory::Read8(0x8066BE22 + slot));
    const uint32_t patch = Memory::Read8(0x8066BE2C + slot);
    // Preserve the game's float computation and truncation to GXColor alpha.
    const float opacity = float(Memory::Read8(0x8066BDBE + slot)) / Memory::ReadFloat32(0x8079803C);
    const uint8_t alpha = uint32_t(Memory::ReadFloat32(0x80798038) * opacity) & 255;
    for (uint32_t output = 0; output < 2; ++output) {
        aurora::MaterialComposition material;
        const uint32_t texture = 0x8066B990 + (base + output * 4) * 32;
        material.sources[0] = Snapshot(texture);
        material.sources[1] = Snapshot(texture + 32);
        material.opacity = float(alpha) / 255.f;
        material.optionalOverlay = output == 1 && detail > 0;
        if (material.optionalOverlay)
            material.sources[2] = Snapshot(0x8066BA90 + (uint32_t(detail) + variant * 12) * 32);
        material.atlas = output == 1;
        if (material.atlas) {
            material.sources[3] = Snapshot(0x8066BA90);
            material.atlasX = (patch & 3) * 32;
            material.atlasY = (patch >> 2) * 16 + variant * 64;
            if (material.atlasY > 112) throw std::runtime_error("Character material atlas region exceeds source");
        }
        const uint32_t destination = Memory::Read32((output ? 0x8066BDF0 : 0x8066BDC8) + 4 * slot);
        const void* data = Memory::GetPointer(destination, 128 * 128 * 4);
        SubmitHostGraphicsTask([owner = slot * 2 + output, data, material = std::move(material)]() mutable {
            aurora::publish_generated_material(owner, data, std::move(material));
        });
    }
#ifdef MH3_DEVELOPMENT
    std::fprintf(stderr, "MH3 material: slot %u, two direct GPU targets, alpha %u, detail %d, atlas %u/%u\n",
                 slot, alpha, detail, patch, variant);
#endif
}
}
extern "C" void MH3_CharacterMaterials(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x60);
    GuestCall game{ctx};
    if (Memory::Read8(0x8066B968) == 1) {
        for (uint32_t slot = 0; slot < 10; ++slot) {
            if (!Memory::Read8(0x8066B97B + slot) || !Memory::Read8(0x8066BE18 + slot)) continue;
            // Keep the original non-composition state setup: it disables fog,
            // alpha testing and alpha writes before the later screen clear.
            game(0x8005AF80); // dbg_drawgraph_init
            PrepareSlot(slot);
            Memory::Write8(0x8066BE18 + slot, 0);
        }
        game(0x804B8FA0); // GXInvalidateTexAll (state, no GPU fence)
    }
    // Original final untextured screen quad and inherited state.
    for (uint32_t i = 0; i < 3; ++i) Memory::Write32(frame[0x10 + i * 4], Memory::Read32(0x80798030));
    Memory::Write32(frame[0x20], Memory::Read32(0x8079804C));
    Memory::Write32(frame[0x24], Memory::Read32(0x80798050));
    Memory::Write32(frame[0x28], Memory::Read32(0x8065B21C));
    game(0x80041470, {frame[0x10], frame[0x20], frame[0x28]}); // disp_beta
    game(0x800885F4, {0x7ff});
    game(0x804B9900, {0}); // GXSetNumIndStages
}
REGISTER_NATIVE_FUNCTION_AS(0x80047DD0, MH3_CharacterMaterials, "CharacterMaterial::prepare");
