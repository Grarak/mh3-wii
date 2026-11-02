// Language import (language_import.h): makes language packs from the Japanese
// disc or from a translation file.
//
// A catalogue lists every text of the English game, each under a key:
// archive.entry.word for a string reference of data00, 10, 11, 20, the
// demo_data menus (word of the overlay) and the system messages,
// quest00.entry.field for a quest's text fields. A translation gives keys new
// text; the rest stays English. Packs need glyphs for what the European font
// lacks (228 characters): the Japanese disc's font, or a TrueType or OpenType
// font named by the translation.
#include "language_import.h"
#include "disc_image.h"
#include "fs_status.h"
#include "language_pack.h"
#include "rom_image.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <unordered_map>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "../../stb_truetype.h"
#pragma clang diagnostic pop

namespace LanguageImport {
namespace {
using namespace LanguagePack;

constexpr uint32_t kCell = 32;
constexpr uint32_t kAtlasWidth = 2048;
constexpr uint32_t kEuropeanGlyphTable = 0x80580818, kEuropeanGlyphs = 228;

// main.dol's message arrays, English: the disc and controller messages, the
// save check's at boot.
struct MessageTable {
    uint32_t slots, count;
};

constexpr MessageTable kMessages[]{{0x80608928, 24}, {0x8060AEB0, 9}};

// A Japanese glyph (20 by 20) keeps its share of the 24-pixel quad a 32-pixel
// cell fills.
constexpr uint32_t kGlyphSize = 27;

uint32_t Word(std::span<const uint8_t> data, size_t at) {
    if (at + 4 > data.size())
        return 0;

    return uint32_t(data[at]) << 24 | uint32_t(data[at + 1]) << 16 | uint32_t(data[at + 2]) << 8 | data[at + 3];
}

std::string TextAt(std::span<const uint8_t> data, size_t at) {
    size_t end = at;

    while (end < data.size() && data[end])
        ++end;

    return std::string(data.begin() + std::min(at, data.size()), data.begin() + end);
}

using Bytes = std::vector<uint8_t>;

// A text of the game.
struct Item {
    uint32_t file, entry, word; // the word, or a quest field's number
    std::string english;
    uint32_t offset = 0, bytes = 0; // a quest field
};

std::string Key(uint32_t file, uint32_t entry, uint32_t word) {
    static constexpr const char* kNames[kFileCount]{"data00", "data10", "data11", "data20", "demo_data", "main", "quest00", "quest01", "quest03"};
    return std::string(kNames[file]) + "." + std::to_string(entry) + "." + std::to_string(word);
}

// The words of an archive's entry that hold string offsets.
using Words = std::vector<uint32_t>;

Words TableWords(std::span<const uint8_t> entry) {
    Words words;
    const uint32_t first = Word(entry, 0);

    for (uint32_t i = 0; i < first / 8; ++i) {
        const uint32_t table = Word(entry, i * 8), count = Word(entry, i * 8 + 4);

        for (uint32_t k = 0; k < count; ++k)
            words.push_back(table / 4 + k);
    }

    return words;
}

Words RowWords(std::span<const uint8_t> entry, uint32_t width) {
    Words words;

    for (uint32_t i = 0; i < Word(entry, 0) * width; ++i)
        words.push_back(1 + i);

    return words;
}

// data10 and 11's talk: 64 groups of five (table, count) pairs.
Words TalkWords(std::span<const uint8_t> entry) {
    Words words;

    for (uint32_t i = 0; i < 64; ++i) {
        const uint32_t group = Word(entry, i * 4);

        if (!group)
            continue;

        for (uint32_t pair = 0; pair < 5; ++pair) {
            const uint32_t table = Word(entry, group + pair * 8), count = Word(entry, group + pair * 8 + 4);

            for (uint32_t k = 0; table && k < count; ++k)
                words.push_back(table / 4 + k);
        }
    }

    return words;
}

// data20: {u32 id, u32 text}.
Words PairWords(std::span<const uint8_t> entry) {
    Words words;
    const uint32_t count = Word(entry, 4) / 8;

    for (uint32_t k = 0; k < count; ++k)
        words.push_back(1 + k * 2);

    return words;
}

// Quest records: the text fields' offsets on the European disc, the Japanese
// disc's, and the widest the European field may be.
struct QuestField {
    uint32_t europe, japan, width;
};

constexpr QuestField kQuestFields[]{
    {0x000, 0x000, 0x2C}, {0x02E, 0x020, 0x5C}, {0x08C, 0x060, 0x29}, {0x0B5, 0x07C, 0x29}, {0x0DE, 0x098, 0x2F}, {0x13C, 0x0D8, 0x5E}, {0x19A, 0x118, 0x2F}, {0x1C9, 0x138, 0x180},
};

std::vector<std::pair<uint32_t, uint32_t>> Entries(const Bytes& file) {
    std::vector<std::pair<uint32_t, uint32_t>> list;

    if (file.size() < 8 || std::memcmp(file.data(), "OMOM", 4))
        return list;

    for (uint32_t i = 0; i < Word(file, 4); ++i)
        list.push_back({Word(file, 8 + i * 8), Word(file, 12 + i * 8)});

    return list;
}

// An overlay: its pointer slots, from the ADDR32 relocations, and its exports.
struct Overlay {
    Bytes data;
    std::unordered_map<uint32_t, uint32_t> slots;
    std::vector<std::pair<uint32_t, std::string>> symbols; // file offset, name, in order
};

Overlay ParseOverlay(Bytes data) {
    Overlay overlay;
    overlay.data = std::move(data);
    const std::span<const uint8_t> d = overlay.data;
    const uint32_t sectionCount = Word(d, 8), sections = Word(d, 12);
    const auto section = [&](uint32_t index) { return index < sectionCount ? Word(d, sections + index * 8) : 0; };
    const uint32_t internal = Word(d, 0x30), internalBytes = Word(d, 0x34);

    for (uint32_t i = 0; i + 12 <= internalBytes; i += 12) {
        const uint32_t info = Word(d, internal + i + 4), start = section(info >> 8);

        if (start && (info & 255) == 1)
            overlay.slots[Word(d, internal + i)] = start + Word(d, internal + i + 8);
    }

    const uint32_t exports = Word(d, 0x40), exportBytes = Word(d, 0x44), names = Word(d, 0x48);

    for (uint32_t i = 0; i + 16 <= exportBytes; i += 16) {
        const uint32_t start = section(Word(d, exports + i + 8));

        if (start)
            overlay.symbols.push_back({start + Word(d, exports + i + 4), TextAt(d, names + Word(d, exports + i))});
    }

    std::sort(overlay.symbols.begin(), overlay.symbols.end());
    return overlay;
}

// main.dol: guest address to file offset.
struct Executable {
    Bytes data;

    size_t Offset(uint32_t address) const {
        const std::span<const uint8_t> d = data;

        for (uint32_t i = 0; i < 18; ++i) {
            const uint32_t at = Word(d, 0x48 + i * 4), size = Word(d, 0x90 + i * 4);

            if (size && address >= at && address - at < size)
                return Word(d, i * 4) + (address - at);
        }

        return data.size();
    }

    uint32_t Pointer(uint32_t address) const {
        return Word(data, Offset(address));
    }
};

Status ReadExecutable(const std::filesystem::path& gameData, Executable& out) {
    std::ifstream file(gameData / "sys/main.dol", std::ios::binary);

    if (!file)
        return Status::Error("Cannot read the game's executable");

    out.data.assign(std::istreambuf_iterator<char>(file), {});
    return {};
}

// Every text of the English game.
Status Catalogue(DiscImage& disc, const Executable& executable, std::vector<Item>& items) {
    const auto archive = [&](uint32_t file, const std::function<Words(uint32_t, std::span<const uint8_t>)>& words) {
        const Bytes bytes = disc.ReadFile(kFileNames[file]);
        const auto entries = Entries(bytes);

        for (uint32_t e = 0; e < entries.size(); ++e) {
            const std::span<const uint8_t> entry(bytes.data() + entries[e].first, entries[e].second);

            for (const uint32_t word : words(e, entry))
                if (const uint32_t reference = Word(entry, word * 4); reference)
                    items.push_back({file, e, word, TextAt(entry, reference)});
        }
    };

    archive(kData00, [](uint32_t, std::span<const uint8_t> entry) { return TableWords(entry); });

    for (const uint32_t file : {kData10, kData11})
        archive(file, [](uint32_t e, std::span<const uint8_t> entry) { return e == 0 ? RowWords(entry, 3) : e == 2 ? RowWords(entry, 7) : e == 3 ? TalkWords(entry) : TableWords(entry); });

    archive(kData20, [](uint32_t, std::span<const uint8_t> entry) { return PairWords(entry); });
    const Overlay demo = ParseOverlay(disc.ReadFile(kFileNames[kDemoData]));

    for (size_t s = 0; s < demo.symbols.size(); ++s) {
        const auto& [at, name] = demo.symbols[s];

        if (!name.ends_with("_eng") || !demo.slots.contains(at))
            continue;

        const uint32_t end = s + 1 < demo.symbols.size() ? demo.symbols[s + 1].first : at + 4;

        for (uint32_t slot = at; slot < end && demo.slots.contains(slot); slot += 4)
            items.push_back({kDemoData, 0, slot / 4, TextAt(demo.data, demo.slots.at(slot))});
    }

    for (uint32_t table = 0; table < std::size(kMessages); ++table)
        for (uint32_t i = 0; i < kMessages[table].count; ++i)
            items.push_back({kExecutable, table, i, TextAt(executable.data, executable.Offset(executable.Pointer(kMessages[table].slots + i * 4)))});

    for (const uint32_t file : {kQuest00, kQuest01, kQuest03}) {
        const Bytes bytes = disc.ReadFile(kFileNames[file]);
        const auto entries = Entries(bytes);

        for (uint32_t e = 0; e < entries.size(); ++e) {
            const std::span<const uint8_t> record(bytes.data() + entries[e].first, entries[e].second);

            for (uint32_t f = 0; f < std::size(kQuestFields); ++f) {
                const auto& field = kQuestFields[f];
                const std::string text = TextAt(record, field.europe);

                // Up to whatever follows the field's zeros.
                uint32_t room = uint32_t(text.size());

                while (room < field.width && field.europe + room < record.size() && !record[field.europe + room])
                    ++room;

                Item item{file, e, f, text};
                item.offset = field.europe;
                item.bytes = room;
                items.push_back(std::move(item));
            }
        }
    }

    return {};
}

void PutUtf8(std::string& out, uint32_t code) {
    if (code < 0x80)
        out += char(code);
    else if (code < 0x800) {
        out += char(0xC0 | code >> 6);
        out += char(0x80 | (code & 0x3F));
    } else {
        out += char(0xE0 | code >> 12);
        out += char(0x80 | (code >> 6 & 0x3F));
        out += char(0x80 | (code & 0x3F));
    }
}

// The characters of a UTF-8 text, as the game's reader takes them (up to 3 bytes).
bool Characters(const std::string& text, std::set<uint32_t>& codes) {
    for (size_t i = 0; i < text.size();) {
        const uint8_t lead = uint8_t(text[i]);
        const uint32_t length = lead < 0x80 ? 1 : lead - 0xC0u < 0x20 ? 2 : lead - 0xE0u < 0x10 ? 3 : 0;

        if (!length || i + length > text.size())
            return false;

        uint32_t code = length == 1 ? lead : lead & (length == 2 ? 0x1F : 0x0F);

        for (uint32_t k = 1; k < length; ++k) {
            const uint8_t next = uint8_t(text[i + k]);

            if ((next & 0xC0) != 0x80)
                return false;

            code = code << 6 | (next & 0x3F);
        }

        if (code >= 0x20 || code == '\n' || code == '\t')
            codes.insert(code);

        i += length;
    }

    return true;
}

constexpr uint16_t kJis[]{
#include "language_jis.inc"
};

// Shift-JIS to UTF-8, control codes kept.
std::string FromShiftJis(std::span<const uint8_t> text) {
    std::string out;

    for (size_t i = 0; i < text.size(); ++i) {
        const uint32_t lead = text[i];

        if (lead < 0x80) {
            out += char(lead);
            continue;
        }

        if (lead >= 0xA1 && lead <= 0xDF) {
            // The text's only half-width katakana is the dot, as an ellipsis.
            PutUtf8(out, lead == 0xA5 ? 0x30FB : 0xFF61 + lead - 0xA1);
            continue;
        }

        if (i + 1 >= text.size())
            break;

        const uint32_t trail = text[++i];
        const uint32_t row = ((lead < 0xE0 ? lead - 0x81 : lead - 0xC1) << 1) + (trail >= 0x9F ? 1 : 0);
        const uint32_t cell = trail >= 0x9F ? trail - 0x9F : trail - 0x40 - (trail > 0x7F ? 1 : 0);
        const uint32_t code = row < 94 && cell < 94 ? kJis[row * 94 + cell] : 0;
        PutUtf8(out, code ? code : '?');
    }

    return out;
}

class Atlas {
  public:
    explicit Atlas(uint32_t glyphs) {
        // The European font's 16 by 16 cells; new glyphs beside them, then below.
        const uint32_t columns = kAtlasWidth / kCell, beside = (columns - 16) * 16;
        const uint32_t rows = 16 + (glyphs > beside ? (glyphs - beside + columns - 1) / columns : 0);
        height = 512;

        while (height < rows * kCell)
            height *= 2;

        pixels.assign(size_t(kAtlasWidth) * height / 2, 0);
    }

    uint32_t Height() const { return height; }
    Bytes&& Take() { return std::move(pixels); }

    // I4: tiles of 8 by 8, 4 bytes a row, the high nibble first.
    void Put(uint32_t x, uint32_t y, uint8_t value) {
        const size_t tile = size_t(y / 8) * (kAtlasWidth / 8) + x / 8;
        uint8_t& byte = pixels[tile * 32 + (y % 8) * 4 + (x % 8) / 2];
        byte = x & 1 ? (byte & 0xF0) | (value >> 4) : (byte & 0x0F) | (value & 0xF0);
    }

  private:
    uint32_t height = 0;
    Bytes pixels;
};

// A cell's coverage, 0..255, for a character.
using Rasterise = std::function<bool(uint32_t code, uint8_t* cell)>;

// The Japanese font: 20 by 20 pixels, 2 bits each, 100 bytes a glyph, by JIS cell.
Rasterise JapaneseFont(const Bytes& font) {
    auto cells = std::make_shared<std::unordered_map<uint32_t, uint32_t>>();

    for (uint32_t i = 94 * 84; i-- > 0;)
        if (kJis[i])
            (*cells)[kJis[i]] = i;

    return [&font, cells](uint32_t code, uint8_t* cell) {
        const auto found = cells->find(code);

        if (found == cells->end() || (found->second + 1) * 100 > font.size())
            return false;

        const uint8_t* glyph = font.data() + found->second * 100;
        const auto source = [&](int x, int y) -> float {
            x = std::clamp(x, 0, 19);
            y = std::clamp(y, 0, 19);
            const uint8_t byte = glyph[y * 5 + x / 4];
            return float((byte >> (6 - (x % 4) * 2)) & 3) / 3.f;
        };

        const uint32_t margin = (kCell - kGlyphSize) / 2;

        for (uint32_t y = 0; y < kGlyphSize; ++y)
            for (uint32_t x = 0; x < kGlyphSize; ++x) {
                const float sx = (x + 0.5f) * 20.f / kGlyphSize - 0.5f, sy = (y + 0.5f) * 20.f / kGlyphSize - 0.5f;
                const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                const float fx = sx - ix, fy = sy - iy;
                const float value = (source(ix, iy) * (1 - fx) + source(ix + 1, iy) * fx) * (1 - fy) + (source(ix, iy + 1) * (1 - fx) + source(ix + 1, iy + 1) * fx) * fy;
                cell[(y + margin) * kCell + x + margin] = uint8_t(std::clamp(value, 0.f, 1.f) * 255.f + 0.5f);
            }

        return true;
    };
}

// A TrueType or OpenType font, at the size of the Japanese font's glyphs.
Status TrueTypeFont(const Bytes& file, Rasterise& out) {
    auto info = std::make_shared<stbtt_fontinfo>();

    if (!stbtt_InitFont(info.get(), file.data(), stbtt_GetFontOffsetForIndex(file.data(), 0)))
        return Status::Error("The translation's font cannot be read");

    out = [&file, info](uint32_t code, uint8_t* cell) {
        const int glyph = stbtt_FindGlyphIndex(info.get(), int(code));

        if (!glyph)
            return false;

        // An em of the glyph size, the baseline where full-width glyphs fill it.
        const float scale = stbtt_ScaleForMappingEmToPixels(info.get(), float(kGlyphSize));
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBox(info.get(), glyph, scale, scale, &x0, &y0, &x1, &y1);
        const int baseline = int(kCell - kGlyphSize) / 2 + kGlyphSize * 7 / 8;
        const int left = int(kCell - (x1 - x0)) / 2, top = baseline + y0;

        if (left < 0 || top < 0 || left + (x1 - x0) > int(kCell) || top + (y1 - y0) > int(kCell))
            return x1 == x0;

        stbtt_MakeGlyphBitmap(info.get(), cell + top * kCell + left, x1 - x0, y1 - y0, kCell, scale, scale, glyph);
        return true;
    };

    return {};
}

// Makes the pack of a translation.
Status Build(const std::filesystem::path& gameData, const std::string& code, const std::string& name, const std::map<std::string, std::string>& strings, Rasterise rasterise, std::vector<Replacement> replacements,
             std::string& summary, bool strict) {
    DiscImage disc;
    MH3_RETURN_IF_ERROR(disc.Load(gameData, false));
    Executable executable;
    MH3_RETURN_IF_ERROR(ReadExecutable(gameData, executable));
    std::vector<Item> items;
    MH3_RETURN_IF_ERROR(Catalogue(disc, executable, items));
    Contents pack;
    pack.code = code;
    pack.name = name;
    pack.replacements = std::move(replacements);
    std::map<std::string, uint32_t> stored;
    std::set<uint32_t> known, used;
    std::set<std::string> keys;
    uint32_t translated = 0, shortened = 0;
    const auto store = [&](const std::string& text) {
        const auto [found, fresh] = stored.try_emplace(text, uint32_t(pack.text.size()));

        if (fresh) {
            pack.text.insert(pack.text.end(), text.begin(), text.end());
            pack.text.push_back(0);
        }

        return found->second;
    };

    for (const auto& item : items) {
        const std::string key = Key(item.file, item.entry, item.word);
        keys.insert(key);
        const auto found = strings.find(key);

        if (found == strings.end() || found->second.empty() || found->second == item.english)
            continue;

        ++translated;
        std::string text = found->second;

        if (!Characters(text, used))
            return Status::Error("The text of " + key + " is not valid UTF-8 up to 3 bytes a character");

        if (item.bytes) {
            // A fixed field.
            if (text.size() >= item.bytes) {
                ++shortened;
                size_t end = item.bytes - 1;

                while (end && (uint8_t(text[end]) & 0xC0) == 0x80)
                    --end;

                text.resize(end);
            }

            pack.fields.push_back({item.file, item.entry, item.offset, item.bytes, TextHash(item.english.c_str()), store(text)});
        } else {
            const uint32_t slot = item.file == kExecutable ? kMessages[item.entry].slots + item.word * 4 : item.word;
            pack.references.push_back({item.file, item.entry, slot, store(text)});
        }
    }

    if (strict)
        for (const auto& [key, text] : strings)
            if (!keys.contains(key))
                return Status::Error("The game has no text " + key);

    if (!translated && pack.replacements.empty())
        return Status::Error("The translation changes no text");

    // The glyphs the European font does not have.
    const std::span<const uint8_t> dol = executable.data;
    const size_t table = executable.Offset(kEuropeanGlyphTable);

    for (uint32_t i = 0; i < kEuropeanGlyphs; ++i)
        known.insert(Word(dol, table + i * 4) >> 16);

    std::vector<uint32_t> missing;

    for (const uint32_t c : used)
        if (!known.contains(c) && c != '\n' && c != '\t')
            missing.push_back(c);

    if (!missing.empty()) {
        if (!rasterise)
            return Status::Error(std::to_string(missing.size()) + " characters need a font: name one in the translation (\"font\")");

        // The European table holds 228 glyphs of the 256 cells; new ones start after.
        for (uint32_t i = kEuropeanGlyphs; i < 256; ++i)
            pack.glyphs.push_back({0xFFFE, 1});

        Atlas atlas(uint32_t(missing.size()));
        std::vector<uint8_t> cell(kCell * kCell);
        uint32_t index = 256, lacking = 0;

        for (const uint32_t c : missing) {
            std::fill(cell.begin(), cell.end(), 0);

            if (!rasterise(c, cell.data())) {
                ++lacking;
                continue;
            }

            uint32_t x = 0, y = 0;
            GlyphCell(index, kAtlasWidth, x, y);

            for (uint32_t py = 0; py < kCell; ++py)
                for (uint32_t px = 0; px < kCell; ++px)
                    atlas.Put(x * kCell + px, y * kCell + py, cell[py * kCell + px]);

            pack.glyphs.push_back({uint16_t(c), 0});
            ++index;
        }

        pack.atlasWidth = kAtlasWidth;
        pack.atlasHeight = atlas.Height();
        pack.atlas = atlas.Take();

        if (lacking)
            summary += std::to_string(lacking) + " characters are not in the font. ";
    }

    summary += std::to_string(translated) + " of " + std::to_string(items.size()) + " texts, " + std::to_string(missing.size()) + " new characters.";

    if (shortened)
        summary += " " + std::to_string(shortened) + " texts were shortened to fit their fields.";

    return Write(gameData, pack);
}

// The Japanese disc, read in place.
class JapaneseDisc {
  public:
    Status Open(const std::filesystem::path& image) {
        MH3_RETURN_IF_ERROR(RomImage::Open(image, rom, "RMHJ08"));
        Bytes boot;
        MH3_RETURN_IF_ERROR(rom->ReadPartition(0, 0x440, boot));
        const uint64_t fstAt = uint64_t(Word(boot, 0x424)) * 4, fstBytes = uint64_t(Word(boot, 0x428)) * 4;
        dolAt = uint64_t(Word(boot, 0x420)) * 4;

        if (fstBytes < 12 || fstBytes > 1024 * 1024)
            return Status::Error("Invalid disc file table");

        Bytes fst;
        MH3_RETURN_IF_ERROR(rom->ReadPartition(fstAt, size_t(fstBytes), fst));
        const uint32_t count = Word(fst, 8);
        struct Directory {
            uint32_t end;
            std::string path;
        };

        std::vector<Directory> stack{{count, ""}};

        for (uint32_t index = 1; index < count && !stack.empty(); ++index) {
            while (index >= stack.back().end)
                stack.pop_back();

            const uint32_t name = Word(fst, index * 12);
            const std::string path = stack.back().path + TextAt(fst, size_t(count) * 12 + (name & 0xFFFFFF));

            if (name >> 24)
                stack.push_back({Word(fst, index * 12 + 8), path + "/"});
            else
                files[path] = {uint64_t(Word(fst, index * 12 + 4)) * 4, Word(fst, index * 12 + 8)};
        }

        return {};
    }

    Status Read(const std::string& name, Bytes& bytes) const {
        const auto found = files.find(name);

        if (found == files.end())
            return Status::Error("The Japanese disc has no " + name);

        return rom->ReadPartition(found->second.first, found->second.second, bytes);
    }

    Status ReadExecutable(Executable& out) const {
        Bytes head;
        MH3_RETURN_IF_ERROR(rom->ReadPartition(dolAt, 0x100, head));
        uint64_t size = 0x100;

        for (uint32_t i = 0; i < 18; ++i)
            size = std::max(size, uint64_t(Word(head, i * 4)) + Word(head, 0x90 + i * 4));

        return rom->ReadPartition(dolAt, size_t(size), out.data);
    }

  private:
    std::unique_ptr<RomImage> rom;
    std::map<std::string, std::pair<uint64_t, uint32_t>> files;
    uint64_t dolAt = 0;
};

// {archive, entry, word, word step, overlay, slot, slot step, count}
struct Run {
    uint32_t file, entry, word, wordStep, overlay, slot, slotStep, count;
};

constexpr Run kRuns[]{
#include "language_japanese_map.inc"
};

// Files of the Japanese disc that replace files of the European one.
constexpr std::pair<const char*, const char*> kReplacedFiles[]{
    {"id/arena.brres", "04/arena_eng.brres"}, {"id/charmake.tpl", "04/charmake_eng.tpl"}, {"id/co_clear.brres", "04/co_clear_eng.brres"}, {"id/co_retire.brres", "04/co_retire_eng.brres"},
    {"id/cock.brres", "04/cock_eng.brres"},   {"id/cock_d.brres", "04/cock_d_eng.brres"}, {"id/gallery.tpl", "04/gallery_eng.tpl"},     {"id/menu.brres", "04/menu_eng.brres"},
    {"id/result.brres", "04/result_eng.brres"}, {"id/title_bg.tpl", "04/title_bg_eng.tpl"}, {"id/title_menu.tpl", "04/title_menu_eng.tpl"}, {"id/village.brres", "04/village_eng.brres"},
    {"id/waku.brres", "04/waku_eng.brres"},   {"system/loading.tpl", "00/loading_eng.tpl"},
};

constexpr uint32_t kOverlayCount = 5, kScrambled = 8, kPlayer1 = 16, kPlayer2 = 32;
constexpr const char* kOverlayNames[kOverlayCount]{"rel/com_data.rso", "rel/lobby_data.rso", "rel/quest_data.rso", "rel/demo_data.rso", "rel/net_data.rso"};

// The Japanese game's text for the European keys.
Status JapaneseStrings(const JapaneseDisc& disc, std::map<std::string, std::string>& strings) {
    std::vector<Overlay> overlays;

    for (const char* name : kOverlayNames) {
        Bytes bytes;
        MH3_RETURN_IF_ERROR(disc.Read(name, bytes));
        overlays.push_back(ParseOverlay(std::move(bytes)));
    }

    Executable executable;
    MH3_RETURN_IF_ERROR(disc.ReadExecutable(executable));

    for (const auto& run : kRuns)
        for (uint32_t k = 0; k < run.count; ++k) {
            const uint32_t slot = run.slot + k * run.slotStep, which = run.overlay & 7;
            std::span<const uint8_t> data;
            size_t at = 0;

            if (which == kOverlayCount) {
                data = executable.data;
                at = executable.Offset(executable.Pointer(slot));

                if (at >= data.size())
                    continue;
            } else {
                const auto found = overlays[which].slots.find(slot);

                if (found == overlays[which].slots.end())
                    continue;

                data = overlays[which].data;
                at = found->second;
            }

            size_t end = at;

            while (end < data.size() && data[end])
                ++end;

            Bytes raw(data.begin() + at, data.begin() + end);

            if (run.overlay & kScrambled)
                for (auto& byte : raw)
                    byte ^= 0xFF;

            const std::string player = run.overlay & kPlayer1 ? "\xEF\xBC\x91\xEF\xBC\xB0" : run.overlay & kPlayer2 ? "\xEF\xBC\x92\xEF\xBC\xB0" : ""; // full-width 1P, 2P
            strings[Key(run.file, run.entry, run.word + k * run.wordStep)] = player + FromShiftJis(raw);
        }

    // Quests: the same entries, the text scrambled in fixed fields.
    constexpr std::pair<uint32_t, const char*> kQuests[]{{kQuest00, "quest/quest00.bin"}, {kQuest01, "quest/quest01.bin"}, {kQuest03, "quest/quest03.bin"}};

    for (const auto& [file, name] : kQuests) {
        Bytes bytes;
        MH3_RETURN_IF_ERROR(disc.Read(name, bytes));
        const auto entries = Entries(bytes);

        for (uint32_t e = 0; e < entries.size(); ++e)
            for (uint32_t f = 0; f < std::size(kQuestFields); ++f) {
                Bytes raw;

                for (size_t at = entries[e].first + kQuestFields[f].japan; at < bytes.size() && bytes[at]; ++at)
                    raw.push_back(bytes[at] ^ 0xFF);

                strings[Key(file, e, f)] = FromShiftJis(raw);
            }
    }

    return {};
}

// A JSON document: objects, arrays, strings, and the rest skipped.
class Json {
  public:
    explicit Json(const std::string& text) : s(text) {}

    // The top object: each member goes to `member`, which reads its value.
    Status Object(const std::function<Status(const std::string&)>& member) {
        Space();

        if (!Take('{'))
            return Failed("an object");

        Space();

        if (Take('}'))
            return {};

        while (true) {
            std::string key;
            MH3_RETURN_IF_ERROR(String(key));
            Space();

            if (!Take(':'))
                return Failed("':'");

            Space();
            MH3_RETURN_IF_ERROR(member(key));
            Space();

            if (Take('}'))
                return {};

            if (!Take(','))
                return Failed("',' or '}'");

            Space();
        }
    }

    Status String(std::string& out) {
        if (!Take('"'))
            return Failed("a string");

        while (at < s.size()) {
            const char c = s[at++];

            if (c == '"')
                return {};

            if (c != '\\') {
                out += c;
                continue;
            }

            if (at >= s.size())
                break;

            const char e = s[at++];

            switch (e) {
            case 'n':
                out += '\n';
                break;
            case 't':
                out += '\t';
                break;
            case 'r':
            case 'b':
            case 'f':
                break;
            case 'u': {
                uint32_t code = 0;
                MH3_RETURN_IF_ERROR(Hex(code));

                if (code >= 0xD800 && code < 0xE000)
                    return Failed("a character below U+10000");

                PutUtf8(out, code);
                break;
            }
            default:
                out += e;
            }
        }

        return Failed("the end of a string");
    }

    // Any value.
    Status Skip() {
        Space();

        if (at < s.size() && s[at] == '"') {
            std::string ignored;
            return String(ignored);
        }

        if (at < s.size() && (s[at] == '{' || s[at] == '[')) {
            const char open = s[at], close = open == '{' ? '}' : ']';
            int depth = 0;

            while (at < s.size()) {
                if (s[at] == '"') {
                    MH3_RETURN_IF_ERROR(Skip());
                    continue;
                }

                depth += s[at] == open;
                depth -= s[at] == close;
                ++at;

                if (!depth)
                    return {};
            }

            return Failed("a closing bracket");
        }

        while (at < s.size() && !std::strchr(",}] \t\r\n", s[at]))
            ++at;

        return {};
    }

  private:
    void Space() {
        while (at < s.size() && std::strchr(" \t\r\n", s[at]))
            ++at;
    }

    bool Take(char c) {
        if (at >= s.size() || s[at] != c)
            return false;

        ++at;
        return true;
    }

    Status Hex(uint32_t& code) {
        for (unsigned i = 0; i < 4; ++i, ++at) {
            const char c = at < s.size() ? s[at] : 0;
            const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;

            if (digit < 0)
                return Failed("a hex digit");

            code = code << 4 | uint32_t(digit);
        }

        return {};
    }

    Status Failed(const char* wanted) const {
        const size_t line = size_t(std::count(s.begin(), s.begin() + std::min(at, s.size()), '\n')) + 1;
        return Status::Error("Translation file, line " + std::to_string(line) + ": expected " + wanted);
    }

    const std::string& s;
    size_t at = 0;
};

void PutJson(std::string& out, const std::string& text) {
    out += '"';

    for (const char c : text)
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (uint8_t(c) >= 0x20)
                out += c;
        }

    out += '"';
}

bool ValidCode(const std::string& code) {
    if (code.size() < 2 || code.size() > 16)
        return false;

    for (const char* builtin : {"en", "fr", "de", "it", "es"})
        if (code == builtin)
            return false;

    return std::all_of(code.begin(), code.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}
} // namespace

Status ImportJapanese(const std::filesystem::path& image, const std::filesystem::path& gameData, const Progress& progress, std::string& summary) {
    progress("Reading the Japanese disc");
    JapaneseDisc disc;
    MH3_RETURN_IF_ERROR(disc.Open(image));
    std::map<std::string, std::string> strings;
    progress("Reading the Japanese text");
    MH3_RETURN_IF_ERROR(JapaneseStrings(disc, strings));
    Bytes font;
    MH3_RETURN_IF_ERROR(disc.Read("system/s20rm04.bin", font));
    progress("Reading the Japanese textures");
    std::vector<Replacement> replacements;

    for (const auto& [japanese, european] : kReplacedFiles) {
        Replacement file;
        file.name = european;
        MH3_RETURN_IF_ERROR(disc.Read(japanese, file.bytes));
        replacements.push_back(std::move(file));
    }

    progress("Building the Japanese language");
    return Build(gameData, "ja", "Japanese", strings, JapaneseFont(font), std::move(replacements), summary, false);
}

Status ParseTranslation(const std::string& text, Translation& out) {
    Json json(text);
    bool format = false;
    MH3_RETURN_IF_ERROR(json.Object([&](const std::string& key) -> Status {
        if (key == "code")
            return json.String(out.code);

        if (key == "name")
            return json.String(out.name);

        if (key == "font")
            return json.String(out.font);

        if (key == "format") {
            format = true;
            return json.Skip();
        }

        if (key != "strings")
            return json.Skip();

        return json.Object([&](const std::string& id) { return json.String(out.strings[id]); });
    }));

    if (!format)
        return Status::Error("The translation has no \"format\": is it a Monster Hunter Tri translation file?");

    if (!ValidCode(out.code))
        return Status::Error("The translation's \"code\" must be 2 to 16 lowercase letters, digits or -, and not a language the game has");

    if (out.name.empty() || out.name.size() > 40 || !std::all_of(out.name.begin(), out.name.end(), [](char c) { return c >= 0x20 && c < 0x7F; }))
        return Status::Error("The translation's \"name\" must be up to 40 plain Latin characters: the launcher shows it");

    return {};
}

Status ImportTranslation(const std::filesystem::path& file, const std::filesystem::path& gameData, std::string& code, std::string& summary) {
    std::ifstream in(file, std::ios::binary);

    if (!in)
        return Status::Error("Cannot read " + file.string());

    const std::string text((std::istreambuf_iterator<char>(in)), {});
    Translation translation;
    MH3_RETURN_IF_ERROR(ParseTranslation(text, translation));
    code = translation.code;
    Bytes fontFile;
    Rasterise rasterise;

    if (!translation.font.empty()) {
        std::ifstream font(file.parent_path() / std::filesystem::path(translation.font).filename(), std::ios::binary);

        if (!font)
            return Status::Error("Cannot read the translation's font: " + translation.font);

        fontFile.assign(std::istreambuf_iterator<char>(font), {});
        MH3_RETURN_IF_ERROR(TrueTypeFont(fontFile, rasterise));
    }

    return Build(gameData, translation.code, translation.name, translation.strings, rasterise, {}, summary, true);
}

Status ExportTemplate(const std::filesystem::path& file, const std::filesystem::path& gameData) {
    DiscImage disc;
    MH3_RETURN_IF_ERROR(disc.Load(gameData, false));
    Executable executable;
    MH3_RETURN_IF_ERROR(ReadExecutable(gameData, executable));
    std::vector<Item> items;
    MH3_RETURN_IF_ERROR(Catalogue(disc, executable, items));
    std::string out = "{\n  \"format\": 1,\n  \"code\": \"xx\",\n  \"name\": \"My language\",\n  \"font\": \"\",\n  \"strings\": {\n";
    bool first = true;

    for (const auto& item : items) {
        if (item.english.empty())
            continue;

        out += first ? "    " : ",\n    ";
        first = false;
        PutJson(out, Key(item.file, item.entry, item.word));
        out += ": ";
        PutJson(out, item.english);
    }

    out += "\n  }\n}\n";
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output.write(out.data(), std::streamsize(out.size()));
    output.close();
    return output ? Status{} : Status::Error("Cannot write " + file.string());
}
} // namespace LanguageImport
