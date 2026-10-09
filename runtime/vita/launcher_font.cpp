#include "launcher_font.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <stdexcept>
#ifdef __vita__
#include <psp2/pvf.h>
#else
#include <fstream>
#include <iterator>
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include <imstb_truetype.h>
#pragma clang diagnostic pop
#endif

namespace VitaLauncher {
namespace {
// Glyphs render into a 64x64 coverage bitmap with the pen at (8, 40): the
// baseline is row 40. Both sources draw at an 18-pixel em.
constexpr unsigned Cell = 64, PenX = 8, Baseline = 40;
using Bitmap = std::array<uint8_t, Cell * Cell>;
} // namespace

#ifdef __vita__
struct Font::Source {
    ScePvfLibId library = nullptr;
    ScePvfFontId font = nullptr;
    Source() {
        ScePvfInitRec init{};
        init.maxNumFonts = 1;
        init.allocFunc = [](void*, unsigned size) -> void* {
            return std::malloc(size);
        };

        init.reallocFunc = [](void*, void* p, unsigned size) -> void* {
            return std::realloc(p, size);
        };

        init.freeFunc = [](void*, void* p) {
            std::free(p);
        };

        ScePvfError error = 0;
        library = scePvfNewLib(&init, &error);

        if (!library || error)
            throw std::runtime_error("Cannot open the system font library");

        font = scePvfOpenDefaultLatinFontOnSharedMemory(library, &error);

        if (!font || error) {
            scePvfDoneLib(library);
            throw std::runtime_error("Cannot open the installed Latin system font");
        }

        scePvfSetResolution(library, 72, 72);
        scePvfSetCharSize(font, 18, 18);
    }

    ~Source() {
        scePvfClose(font);
        scePvfDoneLib(library);
    }

    // False when the font has no such glyph.
    bool Render(unsigned character, Bitmap& bitmap, float& advance) {
        ScePvfCharInfo info{};

        if (scePvfGetCharInfo(font, character, &info) < 0)
            return false;

        ScePvfUserImageBufferRec image{};
        image.pixelFormat = SCE_PVF_USERIMAGE_DIRECT8;
        image.xPos64 = PenX * 64;
        image.yPos64 = Baseline * 64;
        image.rect = {Cell, Cell};
        image.bytesPerLine = Cell;
        image.buffer = bitmap.data();

        if (scePvfGetCharGlyphImage(font, character, &image) < 0)
            return false;

        advance = info.glyphMetrics.horizontalAdvance64 / 64.f;
        return true;
    }
};
#else
// Development hosts (MH3 --launcher): the first installed system sans font.
struct Font::Source {
    std::vector<unsigned char> file;
    stbtt_fontinfo info{};
    float scale = 1;
    Source() {
        for (const char* path : {"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                 "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/freefont/FreeSans.ttf"}) {
            std::ifstream input(path, std::ios::binary);
            file.assign(std::istreambuf_iterator<char>(input), {});

            if (!file.empty() && stbtt_InitFont(&info, file.data(), stbtt_GetFontOffsetForIndex(file.data(), 0))) {
                scale = stbtt_ScaleForMappingEmToPixels(&info, 18);
                return;
            }
        }

        throw std::runtime_error("No system TrueType font for the launcher (Liberation Sans or DejaVu Sans)");
    }

    bool Render(unsigned character, Bitmap& bitmap, float& advance) {
        if (!stbtt_FindGlyphIndex(&info, int(character)))
            return false;

        int width, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&info, int(character), &width, &lsb);
        stbtt_GetCodepointBitmapBox(&info, int(character), scale, scale, &x0, &y0, &x1, &y1);
        const int left = std::max(0, int(PenX) + x0), top = std::max(0, int(Baseline) + y0);
        const int w = std::min(int(Cell) - left, x1 - x0), h = std::min(int(Cell) - top, y1 - y0);

        if (w > 0 && h > 0)
            stbtt_MakeCodepointBitmap(&info, bitmap.data() + top * Cell + left, w, h, Cell, scale, scale, int(character));

        advance = width * scale;
        return true;
    }
};
#endif

Font::Font() : source(std::make_unique<Source>()) {}
Font::~Font() = default;
Font::Glyph& Font::Get(unsigned character) {
    if (character < 32 || character > 126)
        character = '?';

    auto [it, fresh] = glyphs.try_emplace(character);

    if (!fresh)
        return it->second;

    Bitmap bitmap{};

    if (!source->Render(character, bitmap, it->second.advance)) {
        it->second.advance = 10;
        return it->second;
    }

    if (it->second.advance < 1)
        it->second.advance = 6;
    // CPU-only glyph descriptions, recorded into each frame packet. A row's
    // run that continues one of the row above with the same span extends it,
    // so stems and bars are one rectangle each: a frame of text stays well
    // inside the renderer's per-frame vertex memory.
    auto& runs = it->second.runs;

    for (unsigned y = 0; y < Cell; ++y)
        for (unsigned x = 0; x < Cell;) {
            if (bitmap[y * Cell + x] < 96) {
                ++x;
                continue;
            }

            const unsigned start = x;

            while (x < Cell && bitmap[y * Cell + x] >= 96)
                ++x;

            const float left = float(start) - PenX, top = float(y) - Baseline, width = float(x - start);
            const auto above = std::find_if(runs.begin(), runs.end(), [&](const Rectangle& r) { return r.x == left && r.w == width && r.y + r.h == top; });

            if (above != runs.end())
                ++above->h;
            else
                runs.push_back({left, top, width, 1, 0});
        }
    return it->second;
}

void Font::Text(std::vector<Rectangle>& rects, float x, float y, std::string_view text, uint32_t color, float maxWidth) {
    const float end = x + maxWidth;

    for (unsigned char c : text) {
        auto& glyph = Get(c);

        if (x + glyph.advance > end)
            break;

        for (const auto& run : glyph.runs)
            rects.push_back({x + run.x, y + 18 + run.y, run.w, run.h, color});

        x += glyph.advance;
    }
}

float Font::Width(std::string_view text) {
    float width = 0;

    for (unsigned char c : text)
        width += Get(c).advance;

    return width;
}

std::vector<std::string> Font::Wrap(std::string_view text, float width) {
    std::vector<std::string> lines;

    for (size_t at = 0; at <= text.size();) {
        const size_t end = std::min(text.find('\n', at), text.size());
        std::string line;

        for (size_t word = at; word < end;) {
            const size_t space = std::min(text.find(' ', word), end);
            const auto piece = text.substr(word, space - word);

            if (!line.empty() && Width(line) + Width(" ") + Width(piece) > width) {
                lines.push_back(std::move(line));
                line.clear();
            }

            if (!line.empty())
                line += ' ';

            line += piece;
            word = space + 1;
        }

        lines.push_back(std::move(line));
        at = end + 1;
    }

    return lines;
}
} // namespace VitaLauncher
