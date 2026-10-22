#include "vita_launcher.h"
#include "fatal.h"
#include "disc_image.h"
#include "fs_status.h"
#include "quest_catalog.h"
#include "quest_placement.h"
#include <cstdio>
#include <algorithm>
#include <array>
#include <cstdlib>
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

    if (!q.map)
        return {};

    const auto monsters = ScratchMonsters(q.map);
    const auto allowed = [&](uint8_t monster) { return std::find(monsters.begin(), monsters.end(), monster) != monsters.end(); };
    CustomQuest carrier = q;
    SetScratchCarrier(carrier);

    if (!allowed(q.monsters[0]) || (q.monsters[1] && !allowed(q.monsters[1])) || (q.monsters[2] && !allowed(q.monsters[2])) || (q.monsters[2] && !q.monsters[1]) || q.rank > 2 ||
        q.size < 50 || q.size > 200 || carrier.set != q.set || carrier.base != q.base)
        return Status::Error("Invalid custom quest settings");

    if (q.monsters[1] && (ScratchAlone(q.monsters[0]) || ScratchAlone(q.monsters[1]) || ScratchAlone(q.monsters[2])))
        return Status::Error("Ceadeus, Alatreon and Jhen Mohran can only be hunted alone");

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

// Quests from scratch.
namespace {
constexpr uint8_t kCeadeus = 20, kAlatreon = 24, kJhenMohran = 25;
constexpr std::array<uint8_t, 18> kLargeMonsters{1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 14, 15, 16, 18, 19, kCeadeus, kAlatreon, kJhenMohran};
constexpr std::array<uint8_t, 3> kSwimmers{15, 16, 18}; // the water arena's (their spawn lists' map 12 entries)
constexpr std::array<uint8_t, 10> kScratchMaps{1, 2, 3, 4, 5, 9, 10, 11, 6, 7};
constexpr size_t kSlots = 0x314, kObjectives = 0x330, kRecords = 0x394, kRecordBytes = 0x60, kLayout = 0x390;

// The monster's memory class (0x805A22C8: 3 the bosses, 1 Great Jaggi and
// Great Baggi, 2 the others): a heap slot of the class holds its files.
int MemoryClass(uint8_t monster) {
    return monster == kCeadeus || monster == kAlatreon || monster == kJhenMohran ? 3 : monster == 12 || monster == 14 ? 1 : 2;
}

// The rank of an authored quest: 0 village, 1 low, 2 high, 3 the arena.
unsigned Rank(size_t set, uint8_t stars) {
    return set == size_t(QuestCatalog::Set::Village) ? 0 : set == size_t(QuestCatalog::Set::Online) ? (stars <= 3 ? 1 : 2) : 3;
}

std::string WithArticle(const std::string& name) {
    return (std::string("AEIOU").find(name.empty() ? 'x' : name[0]) != std::string::npos ? "an " : "a ") + name;
}

struct Source {
    size_t set = 0, at = 0, slot = 0, record = 0;
};

// The authored slot of `monster` in the quest nearest `rank`, then `stars`
// (the first of equals), with the quest's objective record for it (else its
// first).
Status StrengthSource(const std::array<std::vector<uint8_t>, 3>& sets, uint8_t monster, uint8_t rank, uint8_t stars, Source& found) {
    unsigned best = ~0u;

    for (size_t set = 0; set < sets.size(); ++set) {
        std::vector<size_t> entries;
        MH3_RETURN_IF_ERROR(Entries(sets[set], entries));

        for (size_t at : entries)
            for (size_t slot = 0; slot < 3; ++slot) {
                const auto& file = sets[set];

                if (file[at + kSlots + slot * 8] != monster)
                    continue;

                const unsigned questRank = Rank(set, file[at + 0x8a]);
                const unsigned distance = (questRank > rank ? questRank - rank : rank - questRank) * 16 + unsigned(std::abs(int(file[at + 0x8a]) - int(stars)));

                if (distance >= best)
                    continue;

                best = distance;
                found = {set, at, slot, 0};

                for (size_t k = 0; k < 3; ++k)
                    if ((Word(file, at + kRecords + k * kRecordBytes + 4) >> 16) == monster) {
                        found.record = k;
                        break;
                    }
            }
    }

    if (best == ~0u)
        return Status::Error("No quest on this disc has this monster");

    return {};
}

std::array<std::vector<uint8_t>, 3> DiscQuestFiles() {
    std::array<std::vector<uint8_t>, 3> sets;

    for (size_t set = 0; set < sets.size(); ++set)
        sets[set] = GameDiscImage().ReadFile(QuestPath(QuestCatalog::Set(set)));

    return sets;
}

// The quest's file as the game is to read it.
Status CustomQuestFile(const CustomQuest& q, std::vector<uint8_t>& edited) {
    if (q.map)
        return ScratchQuestFile(DiscQuestFiles(), q, edited);

    return EditQuestFile(GameDiscImage().ReadFile(QuestPath(q.set)), q, edited);
}
} // namespace

bool ScratchAlone(uint8_t monster) {
    return MemoryClass(monster) == 3;
}

std::optional<std::array<int8_t, 6>> ScratchLayout(const CustomQuest& q) {
    // A slot per distinct monster (the same one twice shares its files).
    std::vector<int8_t> classes;

    for (size_t i = 0; i < q.monsters.size(); ++i)
        if (q.monsters[i] && std::find(q.monsters.begin(), q.monsters.begin() + i, q.monsters[i]) == q.monsters.begin() + i)
            classes.push_back(int8_t(MemoryClass(q.monsters[i])));

    // Up to two: the disc's layouts 2 and 5.
    if (classes.size() < 3)
        return std::nullopt;

    // Three large slots and three small ones (the heap grows for them:
    // SetMonsterHeaps' port).
    std::sort(classes.rbegin(), classes.rend());
    return std::array<int8_t, 6>{classes[0], classes[1], classes[2], 0, 0, 0};
}

std::vector<uint8_t> ScratchMaps() {
    return {kScratchMaps.begin(), kScratchMaps.end()};
}

std::vector<uint8_t> ScratchMonsters(uint8_t map) {
    if (map == 6)
        return {kJhenMohran};

    if (map == 7)
        return {kCeadeus};

    if (map == 10)
        return {kSwimmers.begin(), kSwimmers.end()};

    if (std::find(kScratchMaps.begin(), kScratchMaps.end(), map) == kScratchMaps.end())
        return {};

    std::vector<uint8_t> monsters;

    for (uint8_t monster : kLargeMonsters)
        if (monster != kCeadeus && monster != kJhenMohran)
            monsters.push_back(monster);
    return monsters;
}

void SetScratchCarrier(CustomQuest& q) {
    // A hunt quest of each field map; the boss maps' and arenas' own quests.
    struct Carrier {
        uint8_t map;
        QuestCatalog::Set set;
        uint16_t id;
    };

    constexpr Carrier carriers[]{{1, QuestCatalog::Set::Village, 1036}, {2, QuestCatalog::Set::Village, 1046}, {3, QuestCatalog::Set::Village, 1026}, {4, QuestCatalog::Set::Village, 1045},
                                 {5, QuestCatalog::Set::Village, 1044}, {6, QuestCatalog::Set::Online, 14002},  {7, QuestCatalog::Set::Village, 2006}, {9, QuestCatalog::Set::Arena, 9004},
                                 {10, QuestCatalog::Set::Arena, 9005},  {11, QuestCatalog::Set::Online, 19001}};

    for (const auto& carrier : carriers)
        if (carrier.map == q.map) {
            q.set = carrier.set;
            q.base = carrier.map == 6 && q.rank == 2 ? 16001 : carrier.id;
        }
}

Status ScratchCapture(const CustomQuest& q, bool& supported) {
    supported = q.map >= 1 && q.map <= 5;
    const auto sets = DiscQuestFiles();

    for (const uint8_t monster : q.monsters) {
        bool captured = !monster;

        for (const auto& file : sets) {
            std::vector<size_t> entries;
            MH3_RETURN_IF_ERROR(Entries(file, entries));

            for (size_t at : entries)
                for (size_t k = 0; k < 3; ++k)
                    captured |= Word(file, at + kObjectives + k * 8) == 0x81 && (Word(file, at + kObjectives + k * 8 + 4) >> 16) == monster;
        }

        supported &= captured;
    }

    return {};
}

Status ScratchFromTemplate(CustomQuest& q) {
    if (q.map)
        return {};

    const auto file = GameDiscImage().ReadFile(QuestPath(q.set));
    size_t at = 0, placement = 0;
    MH3_RETURN_IF_ERROR(Entry(file, q.base, at));
    MH3_RETURN_IF_ERROR(Entry(file, q.encounter ? q.encounter : q.base, placement));
    const auto monsters = ScratchMonsters(file[at + 0x8b]);

    if (std::find(monsters.begin(), monsters.end(), file[placement + kSlots]) == monsters.end())
        return Status::Error("This template's monster cannot be set up from scratch");

    q.map = file[at + 0x8b];
    q.monsters = {file[placement + kSlots], 0, 0};
    q.rank = uint8_t(std::min(Rank(size_t(q.set), file[at + 0x8a]), 2u));
    q.count = 1;
    q.encounter = 0;
    SetScratchCarrier(q);
    return {};
}

Status ScratchQuestFile(const std::array<std::vector<uint8_t>, 3>& sets, const CustomQuest& q, std::vector<uint8_t>& result) {
    MH3_RETURN_IF_ERROR(Validate(q));

    if (!q.map)
        return Status::Error("Not a quest from scratch");

    if (q.capture) {
        bool supported = false;
        MH3_RETURN_IF_ERROR(ScratchCapture(q, supported));

        if (!supported)
            return Status::Error("This quest's monsters cannot be captured here");
    }

    const auto& carrierFile = sets.at(size_t(q.set));
    size_t at = 0;
    MH3_RETURN_IF_ERROR(Entry(carrierFile, q.base, at));
    std::vector<uint8_t> edited = carrierFile;
    const size_t count = q.monsters[2] ? 3 : q.monsters[1] ? 2 : 1;
    const uint32_t carrierMode = Word(carrierFile, at + 0x310), carrierObjective = Word(carrierFile, at + kObjectives);
    const bool huntCarrier = (carrierMode & 0xF) == 4 || (carrierMode & 0xF) == 5;
    const uint32_t type = q.capture ? 0x81 : carrierObjective == 0x101 ? 0x101 : 1;

    // The monsters, their objectives and their reward records; the unused
    // slots, objectives and records empty.
    std::fill(edited.begin() + at + kSlots, edited.begin() + at + 0x330, 0);
    std::fill(edited.begin() + at + kObjectives, edited.begin() + at + 0x348, 0);
    std::fill(edited.begin() + at + kRecords, edited.begin() + at + kRecords + 3 * kRecordBytes, 0);

    for (size_t i = 0; i < count; ++i) {
        const uint8_t monster = q.monsters[i];
        Source source;
        MH3_RETURN_IF_ERROR(StrengthSource(sets, monster, q.rank, q.stars, source));
        const auto& file = sets[source.set];
        const size_t slot = at + kSlots + i * 8, objective = at + kObjectives + i * 8, record = at + kRecords + i * kRecordBytes;
        std::copy_n(file.begin() + source.at + kSlots + source.slot * 8, 8, edited.begin() + slot);
        std::copy_n(file.begin() + source.at + kRecords + source.record * kRecordBytes, kRecordBytes, edited.begin() + record);

        // The spawn record (each its own, QuestPlacement fills a list's empty
        // ones), the count and the size in percent.
        edited[slot + 1] = uint8_t(i);
        edited[slot + 3] = 1;
        edited[slot + 6] = q.size;

        for (const size_t to : {objective, record}) {
            PutWord(edited, to, type);
            PutHalf(edited, to + 4, monster);
            PutHalf(edited, to + 6, 1);
        }
    }

    // Monsters at once: 0x02000000 (Mating Season, the arena's pairs; three
    // too). 0x04000000 would bring them one at a time (Arena Free-for-all).
    uint32_t mode = (carrierMode & ~0x06000000u) | (count > 1 ? 0x02000000 : 0);

    // Hunts without sub-quests (0xC0, set where an authored hunt has them).
    if (huntCarrier)
        mode = (mode & ~0xFFu) | (q.capture ? 4 : 5);

    PutWord(edited, at + 0x310, mode);
    PutWord(edited, at + 0x32c, 0);
    PutWord(edited, at + kLayout, ScratchLayout(q) ? 4 : MemoryClass(q.monsters[0]) == 3 ? 5 : 2);

    // Its texts: name, goal, no sub-quests, failure, description.
    const auto names = QuestCatalog::MonsterNames();
    const auto name = [&](uint8_t monster) { return monster < names.size() && !names[monster].empty() ? names[monster] : "monster " + std::to_string(monster); };
    const auto text = [&](size_t from, size_t to, const std::string& value) {
        std::fill(edited.begin() + at + from, edited.begin() + at + to, 0);
        std::copy_n(value.begin(), std::min(value.size(), to - from - 1), edited.begin() + at + from);
    };

    const std::string verb = q.capture ? "Capture" : type == 0x101 ? "Slay" : "Hunt";
    std::string targets, lines;

    for (size_t i = 0; i < count; ++i) {
        const std::string separator = i == 0 ? "" : i + 1 == count ? " and " : ", ";
        targets += separator + WithArticle(name(q.monsters[i]));
        lines += (i == 0 ? " " : i + 1 == count ? "\nand " : ",\n") + WithArticle(name(q.monsters[i]));
    }

    text(0, 0x2c, q.name);
    text(0x2e, 0x88, verb + " " + targets);
    text(0x8c, 0xb5, "None");
    text(0xb5, 0xde, "None");
    text(0x13c, 0x19a, q.capture ? "Reward hits 0, time expires,\nor target is slain." : "Reward hits 0, or time expires.");
    text(0x1cd, 0x30c, "A quest of your own design.\n" + verb + lines + "\nLocation: " + QuestCatalog::MapName(q.map) + ".");
    edited[at + 0x8a] = q.stars;

    if (q.minutes)
        PutHalf(edited, at + 0x13a, q.minutes);

    result = std::move(edited);
    return {};
}

namespace {
// The capture support a quest that captures needs.
Status CheckCapture(const CustomQuest& q) {
    if (!q.capture)
        return {};

    bool supported = false;
    MH3_RETURN_IF_ERROR(q.map ? ScratchCapture(q, supported) : SupportsCapture(q, supported));

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
    MH3_RETURN_IF_ERROR(CustomQuestFile(q, edited));
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
    output << "mh3-custom-quest-v3\n"
           << unsigned(q.set) << ' ' << q.base << ' ' << q.count << ' ' << q.capture << ' ' << unsigned(q.stars) << ' ' << q.encounter << ' ' << q.minutes << ' ' << unsigned(q.map) << ' '
           << unsigned(q.monsters[0]) << ' ' << unsigned(q.monsters[1]) << ' ' << unsigned(q.monsters[2]) << ' ' << unsigned(q.rank) << ' ' << unsigned(q.size) << '\n'
           << q.name << '\n';
    output.close();

    if (!output)
        return Status::Error("Cannot save custom quest");

    // An edited quest replaces its file. The Vita's rename refuses an existing
    // target, so remove the old record first.
    bool exists = false;
    MH3_RETURN_IF_ERROR(FsStatus::Exists(path, exists));

    if (exists) {
        std::error_code error;
        std::filesystem::remove(path, error);

        if (error)
            return FsStatus::Failure("cannot remove", error, path);
    }

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

        if ((version != "mh3-custom-quest-v1" && version != "mh3-custom-quest-v2" && version != "mh3-custom-quest-v3") || !(input >> set >> q.base >> q.count >> q.capture >> stars))
            continue;

        if (version != "mh3-custom-quest-v1" && !(input >> q.encounter >> q.minutes))
            continue;

        if (version == "mh3-custom-quest-v3") {
            unsigned map = 0, first = 0, second = 0, third = 0, rank = 0, size = 0;

            if (!(input >> map >> first >> second >> third >> rank >> size) || map > 255 || first > 255 || second > 255 || third > 255 || rank > 255 || size > 255)
                continue;

            q.map = map;
            q.monsters = {uint8_t(first), uint8_t(second), uint8_t(third)};
            q.rank = rank;
            q.size = size;
        }

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

// --quest-monster: quest `id`'s first large monster replaced by
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

    // The field's monster heap slots (+0x390, a layout of 0x805A23F8 + 1): a
    // monster of memory class 3 (0x805A22C8: Ceadeus, Alatreon, Jhen Mohran)
    // loads only into layout 5's class 3 slot.
    if (monster == 20 || monster == 24 || monster == 25)
        PutWord(edited, at + 0x390, 5);

    QuestPlacement::Add(monster, edited[at + 0x8b], edited[at + 0x315]);
    std::fprintf(stderr, "MH3 quest %u: large monster %u replaced by %u\n", unsigned(id), unsigned(old), unsigned(monster));
    disc.SetFileOverride(QuestPath(set), std::move(edited));
    return {};
}

// Development (--quest-small): quest `id`'s small monster spawns (the area
// spawn tables at +0x4B8: u32 count, u32 header size, u32, then {offset from
// +0x4B8, size} pairs of 0x30-byte spawns, +0 u32 monster id) made `monster`.
// Only named monsters other than the quest's large ones change; the arena's
// own kind 40 stays. The arena quest 9000 spawns four Jaggi.
Status ReplaceQuestSmallMonsters(QuestCatalog::Set set, uint16_t id, uint8_t monster) {
    constexpr size_t kTables = 0x4B8, kSpawnSize = 0x30;
    constexpr uint32_t kNamed = 38; // Bnahabra, the list's last
    auto& disc = GameDiscImage();
    std::vector<uint8_t> edited = disc.ReadFile(QuestPath(set));
    size_t at = 0;
    MH3_RETURN_IF_ERROR(Entry(edited, id, at));
    const uint32_t tables = at + kTables + 12 <= edited.size() ? Word(edited, at + kTables) : 0;
    unsigned replaced = 0;
    std::string where, kinds; // "table:kind,kind,..." of the tables (one per area) that had small spawns

    for (uint32_t t = 0; t < tables && t < 64; ++t) {
        const unsigned before = replaced;
        const size_t pair = at + kTables + 12 + 8 * t;

        if (pair + 8 > edited.size())
            break;

        const uint32_t spawns = Word(edited, pair), bytes = Word(edited, pair + 4);

        for (uint32_t b = 0; b + kSpawnSize <= bytes; b += kSpawnSize) {
            const size_t spawn = at + kTables + spawns + b;

            if (spawn + 4 > edited.size())
                break;

            const uint32_t old = Word(edited, spawn);
            bool large = false;

            for (size_t k = 0; k < 3; ++k)
                large |= edited[at + 0x314 + 8 * k] == old;

            if (old == 0 || old > kNamed || large)
                continue;

            edited[spawn] = edited[spawn + 1] = edited[spawn + 2] = 0;
            edited[spawn + 3] = monster;
            ++replaced;
            kinds += (kinds.empty() ? "" : ",") + std::to_string(old);
        }

        if (replaced > before)
            where += " " + std::to_string(t) + ":" + kinds;

        kinds.clear();
    }

    if (!replaced)
        return Status::Error("The quest has no small monster to replace");

    std::fprintf(stderr, "MH3 quest %u: %u small monster spawns made %u (table:kinds%s)\n", unsigned(id), replaced, unsigned(monster), where.c_str());
    disc.SetFileOverride(QuestPath(set), std::move(edited));
    return {};
}

// Development (--quest-patch): bytes of quest `id`'s entry replaced. `patches`
// is OFFSET=HEX[,OFFSET=HEX...]: an offset into the entry and the bytes from
// there, both in hex ("310=80000000" sets the flag word).
Status PatchQuest(QuestCatalog::Set set, uint16_t id, std::string_view patches) {
    auto& disc = GameDiscImage();
    std::vector<uint8_t> edited = disc.ReadFile(QuestPath(set));
    size_t at = 0;
    MH3_RETURN_IF_ERROR(Entry(edited, id, at));
    std::vector<size_t> entries;
    MH3_RETURN_IF_ERROR(Entries(edited, entries));
    size_t end = edited.size();

    for (size_t other : entries)
        if (other > at)
            end = std::min(end, other);

    while (!patches.empty()) {
        const size_t comma = std::min(patches.find(','), patches.size());
        const std::string_view patch = patches.substr(0, comma);
        patches.remove_prefix(std::min(comma + 1, patches.size()));
        const size_t equals = patch.find('=');
        const std::string_view bytes = equals == std::string_view::npos ? std::string_view() : patch.substr(equals + 1);

        if (equals == std::string_view::npos || bytes.empty() || bytes.size() % 2 != 0)
            return Status::Error("--quest-patch takes OFFSET=HEX[,OFFSET=HEX...]");

        const size_t offset = std::stoul(std::string(patch.substr(0, equals)), nullptr, 16);

        if (at + offset + bytes.size() / 2 > end)
            return Status::Error("--quest-patch: offset past the quest's entry");

        for (size_t i = 0; i < bytes.size(); i += 2)
            edited[at + offset + i / 2] = uint8_t(std::stoul(std::string(bytes.substr(i, 2)), nullptr, 16));

        std::fprintf(stderr, "MH3 quest %u: +0x%zX = %.*s\n", unsigned(id), offset, int(bytes.size()), bytes.data());
    }

    disc.SetFileOverride(QuestPath(set), std::move(edited));
    return {};
}

Status ActivateCustomQuest(const CustomQuest& q) {
    MH3_RETURN_IF_ERROR(CheckCapture(q));
    std::vector<uint8_t> edited;
    MH3_RETURN_IF_ERROR(CustomQuestFile(q, edited));

    // Spawn points (each slot's record) for monsters on maps they never
    // visit, and the heap layout, at boot.
    for (size_t i = 0; i < q.monsters.size(); ++i)
        if (q.monsters[i])
            QuestPlacement::Add(q.monsters[i], q.map, uint8_t(i));

    if (const auto layout = ScratchLayout(q))
        QuestPlacement::Layout() = *layout;

    GameDiscImage().SetFileOverride(QuestPath(q.set), std::move(edited));
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
