#include "vita_launcher.h"
#include "disc_image.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void Word(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        b.at(at + i) = v >> (24 - i * 8);
}
uint32_t ReadWord(const std::vector<uint8_t>& b, size_t at) {
    return uint32_t(b.at(at)) << 24 | uint32_t(b.at(at + 1)) << 16 | uint32_t(b.at(at + 2)) << 8 | b.at(at + 3);
}
void Save(const std::filesystem::path& p, const std::vector<uint8_t>& b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), b.size());
    if (!f)
        throw std::runtime_error("test fixture write");
}
std::vector<uint8_t> Quest() {
    std::vector<uint8_t> q(16 + 0x4c0);
    Word(q, 0, 0x4f4d4f4d);
    Word(q, 4, 1);
    Word(q, 8, 16);
    Word(q, 12, 0x4c0);
    Word(q, 16 + 0x2c, 1060u << 16);
    Word(q, 16 + 0x330, 1);
    Word(q, 16 + 0x334, (20u << 16) | 1);
    q[16 + 0x8a] = 2;
    q[16 + 0x8b] = 1;
    return q;
}
void SaveTests(const std::filesystem::path& root) {
    std::vector<uint8_t> file(0x14000);
    Word(file, 0, 1);
    Word(file, 0x48 + 0x6000, 1);
    Word(file, 4, 1);
    Check(!VitaLauncher::ValidSave(file, 0), "empty character must be rejected");
    Check(VitaLauncher::ValidSave(file, 1), "populated second character must be found");
    auto dir = root / "nand/title/00010000/524d4850/data/nocopy";
    std::filesystem::create_directories(dir);
    Save(dir / "data04", file);
    const auto slots = VitaLauncher::ProbeSaves(root);
    Check(slots.size() == 1 && slots[0].file == 4 && slots[0].character == 1, "real MH3 slot probe");
    file.back() = 1;
    Check(!VitaLauncher::ValidSave(file, 1), "bad checksum must be rejected");
    file.resize(12);
    Check(!VitaLauncher::ValidSave(file, 1), "short file must be rejected");
    Check(std::filesystem::file_size(dir / "data04") == 0x14000, "probe must not change the save");
}
void QuestTests() {
    const auto original = Quest();
    VitaLauncher::CustomQuest custom{"Own quest", QuestCatalog::Set::Village, 1060, 1, true, 7};
    const auto edited = VitaLauncher::EditQuestFile(original, custom);
    Check(edited.size() == original.size() && std::string(edited.begin() + 16, edited.begin() + 25) == "Own quest", "quest name/extent");
    Check(edited[16 + 0x333] == 0x81 && edited[16 + 0x337] == 1 && edited[16 + 0x8a] == 7, "actual objective changes");
    Check(edited[16 + 0x397] == 0x81 && edited[16 + 0x399] == 20 && edited[16 + 0x39b] == 1, "live expanded objective must match summary");
    Check(edited[16 + 0x8b] == original[16 + 0x8b] && edited[16 + 0x335] == 20, "template map/monster retained");
    custom.count = 3;
    bool impossible = false;
    try {
        VitaLauncher::EditQuestFile(original, custom);
    } catch (const std::exception&) {
        impossible = true;
    }
    Check(impossible, "unsupported additional spawns rejected");
    auto bad = original;
    Word(bad, 8, 0xfffffff0);
    bool caught = false;
    try {
        VitaLauncher::EditQuestFile(bad, custom);
    } catch (const std::exception&) {
        caught = true;
    }
    Check(caught, "bad quest extent rejected");
    auto multiple = original;
    Word(multiple, 16 + 0x334, (20u << 16) | 2);
    custom.count = 1;
    custom.minutes = 25;
    const auto fewer = VitaLauncher::EditQuestFile(multiple, custom);
    Check(fewer[16 + 0x337] == 1 && fewer[16 + 0x39b] == 1 && fewer[16 + 0x13b] == 25, "smaller authored objective and live time limit");
    custom.stars = 8;
    caught = false;
    try {
        VitaLauncher::EditQuestFile(original, custom);
    } catch (const std::exception&) {
        caught = true;
    }
    Check(caught, "star bucket overflow rejected");
    // Two independently authored encounters on one ordinary map. Donor primary
    // rewards move with its monster; unsupported old subquests are removed.
    constexpr size_t extent = 0x4c0, first = 24, second = first + extent;
    std::vector<uint8_t> composition(24 + extent * 2);
    Word(composition, 0, 0x4f4d4f4d);
    Word(composition, 4, 2);
    Word(composition, 8, first);
    Word(composition, 12, extent);
    Word(composition, 16, second);
    Word(composition, 20, extent);
    for (size_t at : {first, second}) {
        Word(composition, at + 0x310, 0x300c5);
        composition[at + 0x8b] = 2;
        composition[at + 0x317] = 1;
        Word(composition, at + 0x330, 1);
        Word(composition, at + 0x394, 1);
    }
    Word(composition, first + 0x2c, 1060u << 16);
    composition[first + 0x314] = 20;
    Word(composition, first + 0x334, (20u << 16) | 1);
    Word(composition, second + 0x2c, 2001u << 16);
    composition[second + 0x314] = 3;
    Word(composition, second + 0x334, (3u << 16) | 1);
    Word(composition, second + 0x398, (3u << 16) | 1);
    composition[second + 0x39c] = 0x7a;
    Word(composition, first + 0x338, 0x204);
    Word(composition, first + 0x3f4, 0x204);
    composition[first + 0x8c] = 'S';
    composition[first + 0xb5] = 'S';
    composition[first + 0xde] = 'C';
    composition[first + 0x4bc] = 0x5a;
    custom = {"Own encounter", QuestCatalog::Set::Village, 1060, 1, false, 3};
    custom.encounter = 2001;
    Check(VitaLauncher::QuestCompositionOptions(composition, 1060).size() == 2, "same-map authored donors offered");
    const auto composed = VitaLauncher::EditQuestFile(composition, custom);
    Check(composed[first + 0x314] == 3 && composed[first + 0x335] == 3 && composed[first + 0x399] == 3 && composed[first + 0x39c] == 0x7a, "monster slot/live objective/rewards transferred together");
    Check(composed[first + 0x33b] == 0 && composed[first + 0x3f7] == 0 && composed[first + 0x4bc] == 0x5a && composed.size() == composition.size(),
          "old subquests removed; base small spawns and disc extent preserved");
    Check(composed[first + 0x8c] == 0 && composed[first + 0xb5] == 0 && composed[first + 0xde] == 'C', "subquest text cleared; completion text preserved");
    composition[second + 0x8b] = 3;
    caught = false;
    try {
        VitaLauncher::EditQuestFile(composition, custom);
    } catch (const std::exception&) {
        caught = true;
    }
    Check(caught, "monster from incompatible map rejected");
    Word(composition, first + 0x310, 0x30105);
    Check(VitaLauncher::QuestCompositionOptions(composition, 1060).empty(), "scripted quest composition excluded");
}
void DiscTests(const std::filesystem::path& root) {
    const auto disc = root / "disc";
    std::filesystem::create_directories(disc / "sys");
    std::vector<uint8_t> boot(0x440);
    std::copy_n("RMHP08", 6, boot.begin());
    Word(boot, 0x18, 0x5d1c9ea3);
    Word(boot, 0x420, 0x3000 / 4);
    Word(boot, 0x424, 0x3400 / 4);
    const auto quest = Quest();
    // Root/05/quest00_eng.bin and its raw DVD byte range.
    std::vector<uint8_t> fst(36 + 20);
    Word(fst, 0, 0x01000000);
    Word(fst, 8, 3);
    Word(fst, 12, 0x01000000);
    Word(fst, 20, 3);
    Word(fst, 24, 3);
    Word(fst, 28, 0x4000 / 4);
    Word(fst, 32, quest.size());
    std::copy_n("05\0quest00_eng.bin\0", 19, fst.begin() + 36);
    Word(boot, 0x428, fst.size() / 4);
    // FST is aligned to its boot header's size units.
    fst.resize((fst.size() + 3) & ~size_t(3));
    Word(boot, 0x428, fst.size() / 4);
    std::vector<uint8_t> partition(0x7c00);
    std::copy(boot.begin(), boot.end(), partition.begin());
    std::copy(fst.begin(), fst.end(), partition.begin() + 0x3400);
    std::copy(quest.begin(), quest.end(), partition.begin() + 0x4000);
    Save(disc / "sys/boot.bin", boot);
    Save(disc / "sys/bi2.bin", std::vector<uint8_t>(0x2000));
    Save(disc / "sys/apploader.img", std::vector<uint8_t>(0x20));
    Save(disc / "sys/main.dol", std::vector<uint8_t>(0x100));
    Save(disc / "sys/fst.bin", fst);
    Save(disc / "partition.bin", partition);
    {
        std::ofstream layout(disc / "disc-layout.txt");
        layout << "mh3-partition-v1\n";
    }
    DiscImage image(disc);
    const auto edited = VitaLauncher::EditQuestFile(quest, {"Own quest", QuestCatalog::Set::Village, 1060, 1, false, 3});
    image.SetFileOverride("05/quest00_eng.bin", edited);
    Check(image.Read(0x4000 + 16, 9) == std::vector<uint8_t>(edited.begin() + 16, edited.begin() + 25), "partial raw DVD read must see custom quest");
    Check(image.ReadFile("05/quest00_eng.bin") == edited, "full quest read override");
    bool caught = false;
    try {
        image.SetFileOverride("05/quest00_eng.bin", {0});
    } catch (const std::exception&) {
        caught = true;
    }
    Check(caught, "resized overlay rejected");
    // A real decrypted ISO sector and partition metadata, processed through
    // the production bounded reader/extractor. Empty texture inventory is valid.
    constexpr size_t partitionAt = 0x50000, dataAt = 0x70000;
    std::vector<uint8_t> iso(dataAt + 0x8000);
    std::copy(boot.begin(), boot.begin() + 0x100, iso.begin());
    iso[0x61] = 1; // nodtool's decrypted-ISO marker: read without the RVZ/WIA decoder
    Word(iso, 0x40000, 1);
    Word(iso, 0x40004, 0x40020 / 4);
    Word(iso, 0x40020, partitionAt / 4);
    Word(iso, partitionAt + 0x2b8, (dataAt - partitionAt) / 4);
    Word(iso, partitionAt + 0x2bc, 0x8000 / 4);
    Word(iso, partitionAt + 0x2b4, 0x2c0 / 4);
    std::copy(partition.begin(), partition.end(), iso.begin() + dataAt + 0x400);
    Save(root / "game.iso", iso);
    unsigned updates = 0;
    VitaLauncher::ProcessDisc(root / "game.iso", root / "processed", [&](const auto&, uint64_t, uint64_t) { ++updates; });
    Check(updates > 1 && VitaLauncher::DiscReady(root / "processed"), "processing publishes validated game data");
    Check(std::filesystem::file_size(root / "game.iso") == iso.size(), "ROM remains untouched");
    Check(!std::filesystem::exists(root / "processed/partition.bin") && std::filesystem::exists(root / "processed/rom.txt"), "the partition stays in the disc image");
    {
        DiscImage processed(root / "processed");
        Check(processed.ReadFile("05/quest00_eng.bin") == quest, "processed partition exact bytes");
    }
    std::filesystem::resize_file(root / "game.iso", iso.size() + 0x8000);
    Check(!VitaLauncher::DiscReady(root / "processed"), "a changed disc image must not be ready");
    std::filesystem::resize_file(root / "game.iso", iso.size());
    Check(VitaLauncher::DiscReady(root / "processed"), "the restored disc image is ready again");
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--disc-ready") {
            return VitaLauncher::DiscReady(argv[2]) ? 0 : 1;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--prepare-textures") {
            VitaLauncher::PrepareTextures(argv[2], [](const std::string&, uint64_t, uint64_t) {});
            Check(VitaLauncher::DiscReady(argv[2]), "prepared external texture readiness");
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--quest-audit") {
            setenv("MH3_DISC_DIR", argv[2], 1);
            unsigned combinations = 0;
            for (unsigned set = 0; set < 3; ++set) {
                const auto file = GameDiscImage().ReadFile("05/quest0" + std::to_string(set == 2 ? 3 : set) + "_eng.bin");
                for (unsigned i = 0; i < ReadWord(file, 4); ++i) {
                    const size_t at = ReadWord(file, 8 + i * 8);
                    const uint16_t id = ReadWord(file, at + 0x2c) >> 16;
                    const auto options = VitaLauncher::QuestCompositionOptions(file, id);
                    for (uint16_t donor : options) {
                        size_t source = 0;
                        for (unsigned j = 0; j < ReadWord(file, 4); ++j)
                            if ((ReadWord(file, ReadWord(file, 8 + j * 8) + 0x2c) >> 16) == donor)
                                source = ReadWord(file, 8 + j * 8);
                        VitaLauncher::CustomQuest q{"Own encounter", QuestCatalog::Set(set), id, uint16_t(ReadWord(file, source + 0x334)), false, file[at + 0x8a]};
                        q.encounter = donor;
                        q.minutes = 25;
                        for (unsigned capture = 0; capture < 2; ++capture) {
                            q.capture = capture;
                            if (capture && !VitaLauncher::SupportsCapture(q))
                                continue;
                            const auto edited = VitaLauncher::EditQuestFile(file, q);
                            Check(edited.size() == file.size(), "authored composition preserves complete disc extent");
                            Check(ReadWord(edited, at + 0x330) == ReadWord(edited, at + 0x394) && ReadWord(edited, at + 0x334) == ReadWord(edited, at + 0x398),
                                  "authored composition summary/live objective match");
                            Check(std::equal(file.begin() + source + 0x314, file.begin() + source + 0x31c, edited.begin() + at + 0x314), "authored placement transferred exactly");
                            if (donor != id)
                                Check(ReadWord(edited, at + 0x338) == 0 && ReadWord(edited, at + 0x3f4) == 0, "authored composition clears unsupported subobjective");
                            ++combinations;
                        }
                    }
                }
            }
            Check(combinations > 0, "external disc has composable quest encounters");
            std::cout << "Authored quest composition audit passed: " << combinations << " configurations\n";
            return 0;
        }
        if (argc == 4 && std::string_view(argv[1]) == "--process") {
            std::string last;
            VitaLauncher::ProcessDisc(argv[2], argv[3], [&](const std::string& phase, uint64_t done, uint64_t total) {
                if (phase != last) {
                    std::cout << phase << " (" << total << ")\n";
                    last = phase;
                }
            });
            Check(VitaLauncher::DiscReady(argv[3]), "processed external disc readiness");
            return 0;
        }
        if (argc != 2)
            throw std::runtime_error("temporary directory argument required");
        const std::filesystem::path root = std::filesystem::path(argv[1]) / ("launcher-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        SaveTests(root / "user");
        QuestTests();
        DiscTests(root);
        std::filesystem::remove_all(root);
        std::cout << "MH3 launcher save, quest overlay and ISO processing tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
