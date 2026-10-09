#pragma once
#include "launcher_screen.h"
#include "status.h"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace VitaLauncher {
// The installed Latin PVF font on the Vita (a system TrueType font on a
// development host), drawn as solid rectangles.
class Font {
  public:
    // The font, or why the system has none.
    static Status Open(std::unique_ptr<Font>& font);
    ~Font();
    void Text(std::vector<Rectangle>& rects, float x, float y, std::string_view text, uint32_t color, float maxWidth = 880);
    float Width(std::string_view text);

    // Splits text into lines of at most width pixels at spaces and '\n'.
    std::vector<std::string> Wrap(std::string_view text, float width);

  private:
    struct Glyph {
        std::vector<Rectangle> runs;
        float advance;
    };

    struct Source;
    explicit Font(std::unique_ptr<Source> source);
    Glyph& Get(unsigned character);
    std::unique_ptr<Source> source;
    std::map<unsigned, Glyph> glyphs;
};
} // namespace VitaLauncher
