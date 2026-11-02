// Imported languages in the game (language_pack.h).
#include "language_pack.h"
#include "abi_bridge.h"
#include "fatal.h"
#include "memory.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace LanguagePack {
namespace {
constexpr uint32_t kEuropeanSize = 512;

// Above the 0x93620000 where a Wii's game memory ends.
constexpr uint32_t kGuestArea = 0x93800000, kGuestBytes = 7u << 20;

struct Game {
    Pack read;
    uint32_t base = 0, atlas = 0;
    Font font{};
};

std::string selected;
bool loaded = false;
std::unique_ptr<Game> pack;

uint32_t Align(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

uint32_t GuestSize(const Pack& p) {
    return Align(uint32_t(p.contents.text.size()), 32) + uint32_t(p.contents.atlas.size());
}

const char* EnvironmentLanguage() {
    const char* value = std::getenv("MH3_LANGUAGE");
    return value ? value : "";
}
} // namespace

void Select(std::string code) {
    selected = std::move(code);
}

const std::string& Selected() {
    return selected;
}

bool Active() {
    if (loaded)
        return pack != nullptr;

    loaded = true;
    std::string code = selected;
    const char* root = std::getenv("MH3_DISC_DIR");

    if (code.empty() && root) {
        // MH3_LANGUAGE names a built-in language or a pack.
        const std::string named = EnvironmentLanguage();

        if (!named.empty() && std::filesystem::exists(PackPath(root, named)))
            code = named;
    }

    if (code.empty())
        return false;

    auto next = std::make_unique<Game>();
    CheckStatus(Read(PackPath(root ? root : "Assets/DATA", code), next->read, true));
    pack = std::move(next);
    return true;
}

void Place() {
    if (!Active())
        return;

    auto& p = *pack;
    auto& contents = p.read.contents;
    const uint32_t size = GuestSize(p.read);

    if (size > kGuestBytes)
        MH3_THROW(std::runtime_error("The language pack takes more guest memory than the game can spare"));

    p.base = kGuestArea;
    std::memcpy(Memory::GetPointer(p.base, uint32_t(contents.text.size())), contents.text.data(), contents.text.size());

    if (!contents.atlas.empty()) {
        p.atlas = p.base + Align(uint32_t(contents.text.size()), 32);
        std::memcpy(Memory::GetPointer(p.atlas, uint32_t(contents.atlas.size())), contents.atlas.data(), contents.atlas.size());
        p.font = {&contents.glyphs, p.atlas, contents.atlasWidth, contents.atlasHeight};
    }

    std::fprintf(stderr, "MH3 language: %s, %u KiB of guest memory\n", contents.name.c_str(), size / 1024);
}

void PatchFst(uint32_t fst, uint32_t bytes) {
    if (!Active() || pack->read.files.empty())
        return;

    // The FST's records, then their names: rebuild each file's path.
    const uint32_t count = Memory::Read32(fst + 8);
    struct Directory {
        uint32_t end;
        std::string path;
    };

    std::vector<Directory> stack{{count, ""}};

    for (uint32_t index = 1; index < count; ++index) {
        while (index >= stack.back().end)
            stack.pop_back();

        const uint32_t nameWord = Memory::Read32(fst + index * 12);
        const uint32_t name = fst + count * 12 + (nameWord & 0xFFFFFF);

        if (name >= fst + bytes)
            MH3_THROW(std::runtime_error("Invalid FST name offset"));

        const std::string path = stack.back().path + reinterpret_cast<const char*>(Memory::GetPointer(name, 1));

        if (nameWord >> 24) {
            stack.push_back({Memory::Read32(fst + index * 12 + 8), path + "/"});
            continue;
        }

        for (size_t i = 0; i < pack->read.files.size(); ++i)
            if (pack->read.files[i].name == path) {
                Memory::Write32(fst + index * 12 + 4, uint32_t(VirtualOffset(pack->read.contents.code, i) >> 2));
                Memory::Write32(fst + index * 12 + 8, pack->read.files[i].bytes);
            }
    }
}

void FileLoaded(const std::string& path, uint32_t destination) {
    if (!Active())
        return;

    const auto* end = std::end(kFileNames);
    const auto* found = std::find(std::begin(kFileNames), end, path);

    if (found == end)
        return;

    const uint32_t file = uint32_t(found - std::begin(kFileNames));
    const auto entryBase = [&](uint32_t entry) { return destination + Memory::Read32(destination + 8 + entry * 8); };

    for (const auto& r : pack->read.contents.references)
        if (r.file == file) {
            const uint32_t base = entryBase(r.entry);
            Memory::Write32(base + r.word * 4, pack->base + r.text - base);
        }

    for (const auto& f : pack->read.contents.fields)
        if (f.file == file) {
            const uint32_t at = entryBase(f.entry) + f.offset;
            char* field = reinterpret_cast<char*>(Memory::GetPointer(at, f.bytes));

            // A custom quest keeps the text it was given.
            if (TextHash(std::string(field, strnlen(field, f.bytes)).c_str()) != f.english)
                continue;

            const char* text = reinterpret_cast<const char*>(pack->read.contents.text.data()) + f.text;
            std::memset(field, 0, f.bytes);
            std::memcpy(field, text, std::min<size_t>(std::strlen(text), f.bytes - 1));
        }
}

void PatchDemoData(uint32_t base) {
    if (!Active())
        return;

    for (const auto& r : pack->read.contents.references)
        if (r.file == kDemoData)
            Memory::Write32(base + r.word * 4, pack->base + r.text);
}

// load_file (0x800CECC8): every text archive and quest file comes through it.
extern "C" void func_800CECC8(CpuContext* cpu);
extern "C" void MH3_LoadFile(CpuContext* cpu) {
    const uint32_t path = cpu->gpr[3], destination = cpu->gpr[4];
    const std::string name = Active() ? std::string(reinterpret_cast<const char*>(Memory::GetPointer(path, 1))) : std::string();
    func_800CECC8(cpu);

    if (!name.empty() && cpu->gpr[3])
        FileLoaded(name, destination);
}
REGISTER_NATIVE_FUNCTION_AS(0x800CECC8, MH3_LoadFile, "load_file");

void PatchExecutable() {
    if (!Active())
        return;

    for (const auto& r : pack->read.contents.references)
        if (r.file == kExecutable)
            Memory::Write32(r.word, pack->base + r.text);
}

const Font* ActiveFont() {
    return Active() && pack->font.atlas ? &pack->font : nullptr;
}

void EuropeanAtlasDecoded(uint32_t pixels, uint32_t width, uint32_t height, uint32_t format) {
    const auto* font = ActiveFont();

    if (!font)
        return;

    if (width != kEuropeanSize || height != kEuropeanSize || format != 0)
        MH3_THROW(std::runtime_error("Unexpected European font atlas"));

    // I4: tiles of 8 by 8 pixels, 32 bytes each, in rows.
    constexpr uint32_t kTileBytes = 32, kTile = 8;
    const uint32_t tiles = font->width / kTile;

    for (uint32_t row = 0; row < kEuropeanSize / kTile; ++row)
        std::memcpy(Memory::GetPointer(font->atlas + row * tiles * kTileBytes, kEuropeanSize / kTile * kTileBytes), Memory::GetPointer(pixels + row * (kEuropeanSize / kTile) * kTileBytes, kEuropeanSize / kTile * kTileBytes),
                    kEuropeanSize / kTile * kTileBytes);
}
} // namespace LanguagePack
