#pragma once
#include <cstdint>
#include <optional>
#include <span>

// The console's system language (SYSCONF IPL.LNG) as the game reads it
// through SCGetLanguage: the European disc's text, menus and keyboard in
// English, French, German, Italian or Spanish. MH3_LANGUAGE (en, fr, de, it,
// es) forces one, otherwise the console's setting stands. The game reads it
// at startup, so a launcher sets it before the game starts:
// SetLanguageSetting overrides the variable.
struct GameLanguage {
    const char* name; // "English"
    const char* code; // "en"
    uint8_t system;   // SCGetLanguage's value
};

std::span<const GameLanguage> GameLanguages();
std::optional<uint8_t> LanguageSetting();
void SetLanguageSetting(uint8_t system);
