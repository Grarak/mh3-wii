#include "vita_launcher.h"
#include "fatal.h"
#include "disc_image.h"
#include "fs_status.h"
#include <cstdio>
#include <algorithm>
#include <array>
#include <fstream>
#include <numeric>
#include <stdexcept>

namespace VitaLauncher {
namespace {
uint32_t Word(std::span<const uint8_t> data, size_t at) {
    if (at > data.size() || data.size() - at < 4)
        MH3_THROW(std::runtime_error("Truncated quest data"));

    return uint32_t(data[at]) << 24 | uint32_t(data[at + 1]) << 16 | uint32_t(data[at + 2]) << 8 | data[at + 3];
}

void PutWord(std::vector<uint8_t>& data, size_t at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        data.at(at + i) = value >> (24 - 8 * i);
}

void PutHalf(std::vector<uint8_t>& data, size_t at, uint16_t value) {
    data.at(at) = value >> 8;
    data.at(at + 1) = value;
}

const char* QuestPath(QuestCatalog::Set set) {
    switch (set) {
    case QuestCatalog::Set::Village:
        return "05/quest00_eng.bin";
    case QuestCatalog::Set::Online:
        return "05/quest01_eng.bin";
    case QuestCatalog::Set::Arena:
        return "05/quest03_eng.bin";
    }

    MH3_THROW(std::runtime_error("Invalid quest category"));
}

std::filesystem::path Directory(const std::filesystem::path& user) {
    return user / "custom-quests";
}

std::string Key(const CustomQuest& q) {
    return std::to_string(unsigned(q.set)) + "-" + std::to_string(q.base);
}

Status Validate(const CustomQuest& q) {
    if (q.name.empty() || q.name.size() > 43 || q.name.find_first_of("\r\n\t") != q.name.npos || !q.base || !q.count || q.count > 99 || q.stars < 1 || q.stars > 7 || q.minutes > 50 ||
        unsigned(q.set) > 2)
        return Status::Error("Invalid custom quest settings");
    for (unsigned char c : q.name)
        if (c < 32 || c > 126)
            return Status::Error("Quest name must use printable Latin characters");
    for (unsigned char c : q.storageKey)
        if ((c < '0' || c > '9') && c != '-')
            return Status::Error("Invalid custom quest storage key");
    return {};
}

// The entries' offsets. Each entry holds at least 0x4b8 bytes: reads below
// that cannot fail.
Status Entries(std::span<const uint8_t> bytes, std::vector<size_t>& offsets) {
    if (bytes.size() < 8 || Word(bytes, 0) != 0x4f4d4f4d || Word(bytes, 4) > (bytes.size() - 8) / 8)
        return Status::Error("Invalid quest table");

    offsets.clear();

    for (uint32_t i = 0; i < Word(bytes, 4); ++i) {
        const size_t at = Word(bytes, 8 + i * 8), size = Word(bytes, 12 + i * 8);

        if (size < 0x4b8 || at > bytes.size() || size > bytes.size() - at)
            return Status::Error("Invalid quest extent");

        offsets.push_back(at);
    }

    return {};
}

Status Entry(std::span<const uint8_t> source, uint16_t id, size_t& entry) {
    std::vector<size_t> entries;
    MH3_RETURN_IF_ERROR(Entries(source, entries));

    for (size_t at : entries)
        if ((Word(source, at + 0x2c) >> 16) == id) {
            entry = at;
            return {};
        }
    return Status::Error("Quest template no longer exists on this disc");
}

bool Composable(std::span<const uint8_t> source, size_t at) {
    const uint32_t type = Word(source, at + 0x330), target = Word(source, at + 0x334) >> 16;

    // 803AE9F4 selects the monster's authored map placement using this complete
    // eight-byte slot. Exclude special quest modes, invaders and extra slots.
    return source[at + 0x8b] >= 1 && source[at + 0x8b] <= 5 && (Word(source, at + 0x310) & ~1u) == 0x300c4 && (type == 1 || type == 0x81) && target == source[at + 0x314] && source[at + 0x317] == 1 &&
           !source[at + 0x32c] && Word(source, at + 0x31c) == 0 && Word(source, at + 0x320) == 0 && Word(source, at + 0x324) == 0 && Word(source, at + 0x328) == 0;
}
} // namespace

Status QuestCompositionOptions(std::span<const uint8_t> source, uint16_t base, std::vector<uint16_t>& result) {
    size_t original = 0;
    MH3_RETURN_IF_ERROR(Entry(source, base, original));
    result.clear();

    if (!Composable(source, original))
        return {};

    std::vector<size_t> entries;
    MH3_RETURN_IF_ERROR(Entries(source, entries));

    for (size_t at : entries)
        if (Composable(source, at) && source[at + 0x8b] == source[original + 0x8b] && (Word(source, at + 0x310) & 0x30000) == (Word(source, original + 0x310) & 0x30000))
            result.push_back(Word(source, at + 0x2c) >> 16);
    return {};
}

Status CompositionOptions(const CustomQuest& q, std::vector<uint16_t>& options) {
    return QuestCompositionOptions(GameDiscImage().ReadFile(QuestPath(q.set)), q.base, options);
}

Status CompositionMaps(const CustomQuest& q, std::vector<uint16_t>& result) {
    const auto source = GameDiscImage().ReadFile(QuestPath(q.set));
    std::array<bool, 6> seen{};
    std::vector<size_t> entries;
    MH3_RETURN_IF_ERROR(Entries(source, entries));
    result.clear();

    for (size_t at : entries)
        if (Composable(source, at) && !seen[source[at + 0x8b]]) {
            seen[source[at + 0x8b]] = true;
            result.push_back(Word(source, at + 0x2c) >> 16);
        }
    return {};
}

bool ValidSave(std::span<const uint8_t> bytes, unsigned character) {
    constexpr size_t size = 0x14000, records = 0x48, recordSize = 0x6000;

    if (bytes.size() != size || character >= 3)
        return false;

    return (Word(bytes, 0) & 1) && Word(bytes, records + character * recordSize) != 0 && std::accumulate(bytes.begin() + 8, bytes.end(), uint32_t(0)) == Word(bytes, 4);
}

std::vector<SaveSlot> ProbeSaves(const std::filesystem::path& user) {
    std::vector<SaveSlot> slots;
    const auto directory = user / "nand/title/00010000/524d4850/data/nocopy";
    std::array<uint8_t, 0x14000> bytes{};

    for (unsigned file = 0; file < 6; ++file) {
        const auto path = directory / ("data0" + std::to_string(file));
        std::ifstream input(path, std::ios::binary | std::ios::ate);

        if (!input || input.tellg() != std::streampos(bytes.size()))
            continue;

        input.seekg(0);
        input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());

        if (!input)
            continue;

        for (unsigned character = 0; character < 3; ++character)
            if (ValidSave(bytes, character))
                slots.push_back({file, character});
    }

    return slots;
}

Status EditQuestFile(std::span<const uint8_t> source, const CustomQuest& q, std::vector<uint8_t>& result) {
    MH3_RETURN_IF_ERROR(Validate(q));

    if (source.size() < 8 || Word(source, 0) != 0x4f4d4f4d)
        return Status::Error("Invalid quest container");

    const uint32_t count = Word(source, 4);

    if (count > (source.size() - 8) / 8)
        return Status::Error("Invalid quest table");

    std::vector<uint8_t> edited(source.begin(), source.end());

    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = Word(source, 8 + i * 8), size = Word(source, 12 + i * 8);

        if (size < 0x4b8 || at > source.size() || size > source.size() - at)
            return Status::Error("Invalid quest extent");

        if ((Word(source, at + 0x2c) >> 16) != q.base)
            continue;
        // Retain the template's maps, spawn tables, supplies and rewards. Only
        // supported hunt/capture objectives may be changed by this editor.
        size_t encounter = at;

        if (q.encounter && q.encounter != q.base) {
            std::vector<uint16_t> options;
            MH3_RETURN_IF_ERROR(QuestCompositionOptions(source, q.base, options));

            if (std::find(options.begin(), options.end(), q.encounter) == options.end())
                return Status::Error("This monster has no compatible authored placement on the selected map");

            MH3_RETURN_IF_ERROR(Entry(source, q.encounter, encounter));
            std::copy_n(source.begin() + encounter + 0x314, 8, edited.begin() + at + 0x314);

            // Expanded records are the live objective/reward data copied by
            // 803AEF34 to questwork+0x94. Transfer the authored main record.
            std::copy_n(source.begin() + encounter + 0x394, 0x60, edited.begin() + at + 0x394);
            std::fill(edited.begin() + at + 0x338, edited.begin() + at + 0x348, 0);
            std::fill(edited.begin() + at + 0x3f4, edited.begin() + at + 0x4b4, 0);
            std::fill(edited.begin() + at + 0x8c, edited.begin() + at + 0xde, 0);
        }

        const uint32_t objective = Word(source, encounter + 0x330);

        if (objective != 1 && objective != 0x81)
            return Status::Error("Choose a hunt/capture quest template");

        const uint16_t templateCount = uint16_t(Word(source, encounter + 0x334));

        if (q.count > templateCount)
            return Status::Error("The requested count exceeds the authored monster spawns");

        const auto target = uint16_t(Word(source, encounter + 0x334) >> 16);
        std::fill(edited.begin() + at, edited.begin() + at + 0x2c, 0);
        std::copy(q.name.begin(), q.name.end(), edited.begin() + at);
        edited[at + 0x8a] = q.stars;

        for (const size_t objectiveAt : {size_t(0x330), size_t(0x394)}) {
            PutWord(edited, at + objectiveAt, q.capture ? 0x81 : 1);
            PutHalf(edited, at + objectiveAt + 4, target);
            PutHalf(edited, at + objectiveAt + 6, q.count);
        }

        if (q.minutes)
            PutHalf(edited, at + 0x13a, q.minutes);

        if (q.capture != (Word(source, at + 0x330) == 0x81)) {
            const std::string failure = q.capture ? "Reward hits 0, time expires,\nor target is slain." : "Reward hits 0, or time expires.";
            std::fill(edited.begin() + at + 0x13c, edited.begin() + at + 0x19a, 0);
            std::copy(failure.begin(), failure.end(), edited.begin() + at + 0x13c);
        }

        // The template goal text describes its old count/type. Replace it
        // alongside the objective so both game HUD and launcher stay truthful.
        const std::string goal = (q.capture ? "Capture " : "Hunt ") + std::to_string(q.count) + " target monster" + (q.count > 1 ? "s" : "") + " (ID " + std::to_string(target) + ")";
        std::fill(edited.begin() + at + 0x2e, edited.begin() + at + 0x88, 0);
        std::copy(goal.begin(), goal.end(), edited.begin() + at + 0x2e);
        result = std::move(edited);
        return {};
    }

    return Status::Error("Quest template no longer exists on this disc");
}

namespace {
// The capture support a quest that captures needs.
Status CheckCapture(const CustomQuest& q) {
    if (!q.capture)
        return {};

    bool supported = false;
    MH3_RETURN_IF_ERROR(SupportsCapture(q, supported));

    if (!supported)
        return Status::Error("This template's target has no verified capture support");

    return {};
}
} // namespace

Status SaveCustomQuest(const std::filesystem::path& user, const CustomQuest& q) {
    MH3_RETURN_IF_ERROR(Validate(q));
    MH3_RETURN_IF_ERROR(CheckCapture(q));

    // Validate against the actual disc before writing any user record.
    std::vector<uint8_t> edited;
    MH3_RETURN_IF_ERROR(EditQuestFile(GameDiscImage().ReadFile(QuestPath(q.set)), q, edited));
    const auto directory = Directory(user);
    MH3_RETURN_IF_ERROR(FsStatus::CreateDirectories(directory));
    std::string key = q.storageKey;

    if (key.empty()) {
        unsigned serial = 1;
        bool taken = false;

        do {
            key = Key(q) + "-" + std::to_string(serial++);
            MH3_RETURN_IF_ERROR(FsStatus::Exists(directory / (key + ".mh3quest"), taken));
        } while (taken);
    }

    const auto path = directory / (key + ".mh3quest");
    const auto temporary = directory / (key + ".writing");
    std::ofstream output(temporary, std::ios::trunc);
    output << "mh3-custom-quest-v2\n"
           << unsigned(q.set) << ' ' << q.base << ' ' << q.count << ' ' << q.capture << ' ' << unsigned(q.stars) << ' ' << q.encounter << ' ' << q.minutes << '\n'
           << q.name << '\n';
    output.close();

    if (!output)
        return Status::Error("Cannot save custom quest");

    return FsStatus::Rename(temporary, path);
}

Status LoadCustomQuests(const std::filesystem::path& user, std::vector<CustomQuest>& result) {
    result.clear();
    const auto directory = Directory(user);
    bool exists = false;
    MH3_RETURN_IF_ERROR(FsStatus::Exists(directory, exists));

    if (!exists)
        return {};

    std::error_code error;
    std::filesystem::directory_iterator entries(directory, error);

    if (error)
        return FsStatus::Failure("directory iterator cannot open directory", error, directory);

    for (; entries != std::filesystem::directory_iterator(); entries.increment(error)) {
        const auto& entry = *entries;

        if (entry.path().extension() != ".mh3quest")
            continue;

        std::error_code status;
        const bool regular = entry.is_regular_file(status);

        if (status)
            return FsStatus::Failure("status", status, entry.path());

        if (!regular)
            continue;

        std::ifstream input(entry.path());
        std::string version;
        std::getline(input, version);
        CustomQuest q;
        unsigned set = 0, stars = 0;

        if ((version != "mh3-custom-quest-v1" && version != "mh3-custom-quest-v2") || !(input >> set >> q.base >> q.count >> q.capture >> stars))
            continue;

        if (version == "mh3-custom-quest-v2" && !(input >> q.encounter >> q.minutes))
            continue;

        input.ignore(1024, '\n');
        std::getline(input, q.name);

        if (set > 2 || stars > 255)
            continue;

        q.set = QuestCatalog::Set(set);
        q.stars = stars;
        q.storageKey = entry.path().stem().string();

        // A record that fails validation is skipped.
        if (Validate(q).ok())
            result.push_back(q);
    }

    if (error)
        return Status::Error("filesystem error: directory iterator cannot advance: " + error.message());

    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return {};
}

// Development (--quest-monster): quest `id`'s first large monster replaced by
// `monster` (its id in the game's monster list), with every objective that
// targeted the old one, in the copy of the quest file the disc serves. The
// arena quests make a debug pit for any monster.
Status ReplaceQuestMonster(QuestCatalog::Set set, uint16_t id, uint8_t monster) {
    auto& disc = GameDiscImage();
    std::vector<uint8_t> edited = disc.ReadFile(QuestPath(set));
    size_t at = 0;
    MH3_RETURN_IF_ERROR(Entry(edited, id, at));

    if (edited[at + 0x317] != 1 || !edited[at + 0x314])
        return Status::Error("The quest has no large monster to replace");

    const uint8_t old = edited[at + 0x314];
    edited[at + 0x314] = monster;

    for (const size_t objectives : {size_t(0x330), size_t(0x394)})
        for (size_t k = 0; k < 3; ++k)
            if (Word(edited, at + objectives + 8 * k + 4) >> 16 == old)
                PutHalf(edited, at + objectives + 8 * k + 4, monster);

    std::fprintf(stderr, "MH3 quest %u: large monster %u replaced by %u\n", unsigned(id), unsigned(old), unsigned(monster));
    disc.SetFileOverride(QuestPath(set), std::move(edited));
    return {};
}

Status ActivateCustomQuest(const CustomQuest& q) {
    MH3_RETURN_IF_ERROR(CheckCapture(q));
    auto& disc = GameDiscImage();
    std::vector<uint8_t> edited;
    MH3_RETURN_IF_ERROR(EditQuestFile(disc.ReadFile(QuestPath(q.set)), q, edited));
    disc.SetFileOverride(QuestPath(q.set), std::move(edited));
    return {};
}

Status SupportsCapture(const CustomQuest& q, bool& supported) {
    supported = false;
    const auto templates = GameDiscImage().ReadFile(QuestPath(q.set));
    size_t original = 0;
    MH3_RETURN_IF_ERROR(Entry(templates, q.encounter ? q.encounter : q.base, original));
    const uint16_t target = Word(templates, original + 0x334) >> 16;

    if (!target)
        return {};
    // Derive capture support from authored quests, not guessed monster IDs.
    for (const auto set : {QuestCatalog::Set::Village, QuestCatalog::Set::Online, QuestCatalog::Set::Arena}) {
        const auto bytes = GameDiscImage().ReadFile(QuestPath(set));
        std::vector<size_t> entries;
        MH3_RETURN_IF_ERROR(Entries(bytes, entries));

        for (size_t at : entries)
            for (unsigned i = 0; i < 3; ++i)
                if (Word(bytes, at + 0x330 + i * 8) == 0x81 && (Word(bytes, at + 0x334 + i * 8) >> 16) == target)
                    for (unsigned slot = 0; slot < 3; ++slot)
                        if (bytes[at + 0x314 + slot * 8] == target && bytes[at + 0x316 + slot * 8] == templates[original + 0x316] && bytes[at + 0x319 + slot * 8] == templates[original + 0x319]) {
                            supported = true;
                            return {};
                        }
    }

    return {};
}
} // namespace VitaLauncher
