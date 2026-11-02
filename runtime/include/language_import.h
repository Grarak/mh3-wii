#pragma once
#include "status.h"
#include <filesystem>
#include <functional>
#include <map>
#include <string>

// Makes language packs (language_pack.h) in the game data directory.
namespace LanguageImport {
using Progress = std::function<void(const std::string&)>;

// Japanese text, font and textures from the Japanese disc (RMHJ08). The pack
// holds what it needs: the disc can go afterwards.
Status ImportJapanese(const std::filesystem::path& image, const std::filesystem::path& gameData, const Progress& progress, std::string& summary);

// A translation file: JSON with "format": 1, "code", "name" (plain Latin: the
// launcher shows it), "strings" (key to text, whatever is missing stays
// English) and, for characters the game's font lacks, "font": a TrueType or
// OpenType font beside the file. Gives the pack's code.
Status ImportTranslation(const std::filesystem::path& file, const std::filesystem::path& gameData, std::string& code, std::string& summary);

struct Translation {
    std::string code, name, font;
    std::map<std::string, std::string> strings;
};

// A translation file's text, checked.
Status ParseTranslation(const std::string& text, Translation& out);

// Writes a translation file with every English text, to start from.
Status ExportTemplate(const std::filesystem::path& file, const std::filesystem::path& gameData);
} // namespace LanguageImport
