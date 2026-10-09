#pragma once
#include <cstdint>
#include <string_view>
#include <vector>
#include <map>
#include <psp2/pvf.h>

namespace VitaLauncher {
struct Rectangle {
    float x, y, w, h;
    uint32_t color;
};
class Font {
  public:
    Font();
    ~Font();
    void Text(std::vector<Rectangle>& rects, float x, float y, std::string_view text, uint32_t color, float maxWidth = 880);

  private:
    struct Glyph {
        std::vector<Rectangle> runs;
        float advance;
    };
    Glyph& Get(unsigned character);
    ScePvfLibId library = nullptr;
    ScePvfFontId font = nullptr;
    std::map<unsigned, Glyph> glyphs;
};
} // namespace VitaLauncher
