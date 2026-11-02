#pragma once
#include "status.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class DiscImage;

// An imported language: a pack file in <game data>/languages/ with the
// language's strings, its font and any replacement files. The game runs as
// the English game; the strings replace the English text as it loads, and
// live in guest memory the arena gives up.
//
//   Text archives (data00, 10, 11, 20): the strings' offsets are patched
//   after a load (load_file), so they point out of the buffer, whose size the
//   game fixes. Everything else in a file stays the English disc's.
//   Quest files: their fixed text fields are written in place.
//   Menus in demo_data and the system messages in main.dol: pointer slots.
//   Replacement files appear at offsets past the disc's end.
//
// Language import (language_import.cpp) makes packs from the Japanese disc or
// a translation file.
namespace LanguagePack {
// An installed pack.
struct Listing {
    std::string code, name;
};

std::filesystem::path Directory(const std::filesystem::path& gameData);
std::vector<Listing> Installed(const std::filesystem::path& gameData);

// The pack the game starts with: its code, or empty for none. MH3_LANGUAGE
// names one too.
void Select(std::string code);
const std::string& Selected();

// Whether a pack is in use; loads it at the first call.
bool Active();

// The pack's strings and atlas take the end of the MEM2 arena: the new end.
uint32_t Reserve(uint32_t arenaEnd);

// The replacement files of every installed pack, readable from `disc`: the
// launcher chooses the language after the asset cache starts. The boot FST
// sends the game to the chosen pack's.
struct DiscFile {
    std::string name;
    uint64_t offset;
    uint32_t bytes;
};

std::vector<DiscFile> RegisterFiles(DiscImage& disc, const std::filesystem::path& gameData);
void PatchFst(uint32_t fst, uint32_t bytes);

// load_file has put a file at `destination`.
void FileLoaded(const std::string& path, uint32_t destination);

// Pointer patches of a module, once it is linked, and of main.dol.
void PatchDemoData(uint32_t base);
void PatchExecutable();

// The font's glyphs after the European table's, in order; the European atlas
// goes into the pack's (at the same cells) when the game decodes it.
struct Glyph {
    uint16_t code, narrow;
};

struct Font {
    const std::vector<Glyph>* glyphs;
    uint32_t atlas, width, height; // guest address, pixels
};

// Null without a pack or when it needs no glyphs of its own.
const Font* ActiveFont();
void EuropeanAtlasDecoded(uint32_t pixels, uint32_t width, uint32_t height, uint32_t format);

// The glyph's cell in a font's atlas, in cells of 32 pixels; the European
// font keeps the first 16 by 16.
inline void GlyphCell(uint32_t index, uint32_t atlasWidth, uint32_t& x, uint32_t& y) {
    constexpr uint32_t kCell = 32, kEuropean = 16;
    const uint32_t columns = atlasWidth / kCell;

    if (index < kEuropean * kEuropean) {
        x = index % kEuropean;
        y = index / kEuropean;
        return;
    }

    index -= kEuropean * kEuropean;
    const uint32_t beside = (columns - kEuropean) * kEuropean;

    if (index < beside) {
        x = kEuropean + index % (columns - kEuropean);
        y = index / (columns - kEuropean);
        return;
    }

    index -= beside;
    x = index % columns;
    y = kEuropean + index / columns;
}

// What language_import.cpp writes.
struct Reference {
    uint32_t file, entry, word, text;
};

struct Field {
    uint32_t file, entry, offset, bytes, english, text;
};

struct Replacement {
    std::string name;
    std::vector<uint8_t> bytes;
};

struct Contents {
    std::string code, name;
    std::vector<uint8_t> text;
    std::vector<Reference> references;
    std::vector<Field> fields;
    std::vector<Glyph> glyphs;
    uint32_t atlasWidth = 0, atlasHeight = 0;
    std::vector<uint8_t> atlas;
    std::vector<Replacement> replacements;
};

Status Write(const std::filesystem::path& gameData, const Contents& pack);

// A pack as read: its replacement files stay in the file.
struct Stored {
    std::string name;
    uint64_t at; // in the pack
    uint32_t bytes;
};

struct Pack {
    std::filesystem::path path;
    Contents contents;
    std::vector<Stored> files;
};

std::filesystem::path PackPath(const std::filesystem::path& gameData, const std::string& code);
Status Read(const std::filesystem::path& path, Pack& out, bool whole);
uint64_t VirtualOffset(const std::string& code, size_t index);

// Disc files with text, by the numbers of Reference::file and Field::file.
enum FileId : uint32_t { kData00, kData10, kData11, kData20, kDemoData, kExecutable, kQuest00, kQuest01, kQuest03, kFileCount };
inline constexpr const char* kFileNames[kFileCount]{"00/data00_eng.bin", "00/data10_eng.bin", "00/data11_eng.bin", "00/data20_eng.bin", "01/demo_data.rso", "", "05/quest00_eng.bin", "05/quest01_eng.bin",
                                                    "05/quest03_eng.bin"};

// A quest field's check: FNV-1a of its English text.
uint32_t TextHash(const char* text);
} // namespace LanguagePack
