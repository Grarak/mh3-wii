// The Vita's button symbols on the game's UI atlases (button_glyphs.cpp).
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace ButtonGlyphs {

// Redraws the Classic Controller's letter buttons of the atlas `name` in
// its decoded RGBA8 level 0 (width x height, rows of width * 4 bytes) as
// the Vita's buttons. Returns whether the atlas has any.
bool Patch(std::string_view name, uint32_t width, uint32_t height, uint8_t* rgba);

// The same for an image the renderer decodes from guest memory (a TPL's, or
// an atlas without its converted pack), told apart by its GX format, size
// and the hash of its level 0 (SourceHash).
bool PatchSource(uint32_t format, uint32_t width, uint32_t height, std::span<const uint8_t> source, uint8_t* rgba);

// FNV-1a 64 of an image's level 0.
uint64_t SourceHash(std::span<const uint8_t> level0);

} // namespace ButtonGlyphs
