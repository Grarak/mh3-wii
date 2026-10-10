// The Vita's button symbols on the game's UI atlases. The prompts and the
// controls pages draw the Classic Controller's buttons from texture atlases
// as round discs with a letter (a, b, y; + and − shared with the Wii
// Remote). The texture converter (launcher_textures.cpp) hands each decoded
// atlas here before packing it: the letter is erased from its disc, which
// keeps the game's own disc, rim and shadow, and the Vita symbol is drawn in
// the letters' ink, following docs/vita-controls.md:
//
//   a -> circle, b -> cross, y -> square (x -> triangle has no disc in
//   these atlases), + -> START, − -> SELECT (the words in 3x6 pixel
//   letters).
//
// The symbols are coverage masks drawn by scripts/vita_button_glyphs.py
// (button_glyphs.inc). The Wii Remote's and Nunchuk's buttons (A, B, 1, 2,
// C, Z) and L, R and the gold z (ZL/ZR) stay as they are.
#include "button_glyphs.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace {

#include "button_glyphs.inc"

enum class Symbol : uint8_t { Circle, Cross, Square, Triangle, Start, Select };

struct Mask {
    const uint8_t* small;
    const uint8_t* large;
};

constexpr Mask kMasks[] = {
    {k_circle_small, k_circle_large},
    {k_cross_small, k_cross_large},
    {k_square_small, k_square_large},
    {k_triangle_small, k_triangle_large},
    {k_start_small, k_start_large},
    {k_select_small, k_select_large},
};

constexpr int kSmallMask = 16, kLargeMask = 20;

// A disc: a box around its face and rim, 16 to 20 px.
struct Disc {
    uint16_t x, y, size;
    Symbol symbol;
};

constexpr Disc kMenu01[] = {
    {228, 29, 20, Symbol::Square},  // y
    {192, 54, 20, Symbol::Circle},  // a
    {54, 191, 20, Symbol::Select},  // −
    {209, 209, 16, Symbol::Circle}, // a
    {230, 209, 16, Symbol::Cross},  // b
    {230, 231, 16, Symbol::Square}, // y
    {11, 25, 16, Symbol::Select},   // −
    {33, 25, 16, Symbol::Start},    // +
};

constexpr Disc kReward02[] = {
    {222, 183, 16, Symbol::Circle}, // a ("a Done" on the result screen)
};

// title_menu_*.tpl image 1: the controls page's buttons.
constexpr Disc kTitleMenu[] = {
    {682, 139, 16, Symbol::Circle}, // a
    {706, 139, 16, Symbol::Cross},  // b
    {634, 163, 16, Symbol::Square}, // y
};

// charmake_*.tpl image 0: "y Zoom", a disc on an opaque bar.
constexpr Disc kCharmake[] = {
    {161, 262, 18, Symbol::Square},
};

struct Atlas {
    std::string_view name;
    uint32_t width, height;
    std::span<const Disc> discs;
};

// Every language's variant has its discs at the same places.
constexpr Atlas kAtlases[] = {
    {"menu01", 256, 288, kMenu01},
    {"a_menu01", 256, 288, kMenu01},
    {"reward02", 512, 256, kReward02},
};

// The images the renderer decodes from guest memory: GX format, size and
// level 0 hash (SourceHash).
struct Source {
    uint64_t hash;
    uint32_t format, width, height;
    std::span<const Disc> discs;
};

constexpr uint32_t kRGB5A3 = 5, kRGBA8 = 6;

constexpr Source kSources[] = {
    {0xa233b620aa08920e, kRGBA8, 256, 288, kMenu01},     // menu01, eng
    {0x127babf529a69599, kRGBA8, 256, 288, kMenu01},     // fre
    {0x7158f3c28c529062, kRGBA8, 256, 288, kMenu01},     // ger
    {0x036498912db0d7c4, kRGBA8, 256, 288, kMenu01},     // ita
    {0xe1de26d428cef80d, kRGBA8, 256, 288, kMenu01},     // spa
    {0x7d0224435340dd77, kRGB5A3, 512, 256, kReward02},  // reward02, eng
    {0x124e7f02838c6b91, kRGB5A3, 512, 256, kReward02},  // fre
    {0xfe560a70bdec0eca, kRGB5A3, 512, 256, kReward02},  // ger
    {0xd421a69587ee1bdc, kRGB5A3, 512, 256, kReward02},  // ita
    {0x79e3c370d96da4f5, kRGB5A3, 512, 256, kReward02},  // spa
    {0x211cb0901d1766a4, kRGB5A3, 768, 256, kTitleMenu}, // every language's
    {0x008debac7dac382d, kRGB5A3, 288, 288, kCharmake},  // eng
    {0xfdb60a610840ce3c, kRGB5A3, 288, 288, kCharmake},  // fre
    {0x3cb15245ef6da7af, kRGB5A3, 288, 288, kCharmake},  // ger
    {0x19d11d879f3a7c94, kRGB5A3, 288, 288, kCharmake},  // ita
    {0x0940f4f12bed1bdf, kRGB5A3, 288, 288, kCharmake},  // spa
};

constexpr uint8_t kInk[3] = {78, 78, 87}; // the letters' colour

// A pixel of a disc's face or letter: opaque and lighter than the rim, the
// shadow and the bar some discs sit on.
bool Face(const uint8_t* p) {
    return p[3] >= 0xE8 && p[0] >= 0x40;
}

void Redraw(uint8_t* rgba, uint32_t width, const Disc& disc) {
    const auto at = [&](int x, int y) { return rgba + (size_t(y) * width + size_t(x)) * 4; };

    // The face's centre and radius from its pixels.
    double sx = 0, sy = 0;
    int count = 0;

    for (int y = disc.y; y < disc.y + disc.size; ++y)
        for (int x = disc.x; x < disc.x + disc.size; ++x)
            if (Face(at(x, y))) {
                sx += x;
                sy += y;
                ++count;
            }

    if (count == 0)
        return;

    const double cx = sx / count + 0.5, cy = sy / count + 0.5, r = std::sqrt(count / 3.14159265358979);

    // The face's colour at each whole radius: the brightest pixel there, or
    // the next ring's where the letter covers the whole ring.
    constexpr int kRings = 16;
    const uint8_t* face[kRings] = {};
    const auto brightness = [](const uint8_t* p) { return p ? p[0] + p[1] + p[2] : 0; };
    const auto ring = [&](int x, int y) { return int(std::lround(std::hypot(x + 0.5 - cx, y + 0.5 - cy))); };

    for (int y = disc.y; y < disc.y + disc.size; ++y)
        for (int x = disc.x; x < disc.x + disc.size; ++x) {
            const uint8_t* p = at(x, y);
            const int k = ring(x, y);

            if (Face(p) && k < kRings && brightness(p) > brightness(face[k]))
                face[k] = p;
        }

    uint8_t colour[kRings][3] = {};
    bool known[kRings] = {};

    for (int k = 0; k < kRings; ++k)
        if (face[k]) {
            std::copy_n(face[k], 3, colour[k]);
            known[k] = true;
        }

    const auto sum = [](const uint8_t* c) { return c[0] + c[1] + c[2]; };

    // The face is flat inside its rim: from the first ring outward that has
    // its outer neighbour's colour, every ring inside takes that colour.
    bool flat = false;

    for (int k = kRings - 2; k >= 0; --k) {
        flat = flat || (known[k] && known[k + 1] && std::equal(colour[k], colour[k] + 3, colour[k + 1]));

        if (known[k + 1] && (flat || !known[k] || sum(colour[k]) < 0.9 * sum(colour[k + 1]))) {
            std::copy_n(colour[k + 1], 3, colour[k]);
            known[k] = true;
        }
    }

    // Erase the letter, then draw the symbol. Next to the rim only the
    // letter's dark pixels go (a y's tail), the rim's shading stays.
    for (int y = disc.y; y < disc.y + disc.size; ++y)
        for (int x = disc.x; x < disc.x + disc.size; ++x) {
            uint8_t* p = at(x, y);
            const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            const uint8_t* c = colour[std::min(ring(x, y), kRings - 1)];

            if (Face(p) && (d < r - 1.5 || (d < r - 0.5 && brightness(p) < 0.85 * sum(c))))
                std::copy_n(c, 3, p);
        }

    const Mask& masks = kMasks[int(disc.symbol)];
    const int size = r < 9 ? kSmallMask : kLargeMask;
    const uint8_t* mask = r < 9 ? masks.small : masks.large;
    const int mx = int(std::lround(cx - size / 2.0)), my = int(std::lround(cy - size / 2.0));

    for (int j = 0; j < size; ++j)
        for (int i = 0; i < size; ++i) {
            const int a = mask[j * size + i];

            if (a == 0)
                continue;

            uint8_t* p = at(mx + i, my + j);

            for (int c = 0; c < 3; ++c)
                p[c] = uint8_t((p[c] * (255 - a) + kInk[c] * a + 127) / 255);
        }
}

} // namespace

namespace ButtonGlyphs {

bool Patch(std::string_view name, uint32_t width, uint32_t height, uint8_t* rgba) {
    for (const Atlas& atlas : kAtlases) {
        if (atlas.name != name || atlas.width != width || atlas.height != height)
            continue;

        for (const Disc& disc : atlas.discs)
            Redraw(rgba, width, disc);

        return true;
    }

    return false;
}

uint64_t SourceHash(std::span<const uint8_t> level0) {
    uint64_t hash = 0xcbf29ce484222325;

    for (const uint8_t b : level0)
        hash = (hash ^ b) * 0x100000001b3;

    return hash;
}

bool PatchSource(uint32_t format, uint32_t width, uint32_t height, std::span<const uint8_t> source, uint8_t* rgba) {
    const size_t bytes = size_t(width) * height * (format == kRGBA8 ? 4 : 2);
    uint64_t hash = 0;

    for (const Source& s : kSources) {
        if (s.format != format || s.width != width || s.height != height || source.size() < bytes)
            continue;

        // Hashed once a decode, only for an image of a listed size.
        if (!hash)
            hash = SourceHash(source.first(bytes));

        if (s.hash != hash)
            continue;

        for (const Disc& disc : s.discs)
            Redraw(rgba, width, disc);

        return true;
    }

    return false;
}

} // namespace ButtonGlyphs
