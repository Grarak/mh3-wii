#include "vita_launcher.h"
#include "button_glyphs.h"
#include "disc_image.h"
#include "load_progress.h"
#include "launcher_convert.h"
#include "launcher_texture_inventory.h"
#include "gfx/native_texture_decode.hpp"
#include "gfx/texture_convert.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <stdexcept>

// The asset cache reports conversions to the loading screen, which the tests
// do not have.
void LoadProgress::ConversionQueued(uint64_t) {}
void LoadProgress::ConversionDone(uint64_t) {}

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

// The edited quest file; a failure stops the tests.
std::vector<uint8_t> Edit(std::span<const uint8_t> source, const VitaLauncher::CustomQuest& quest) {
    std::vector<uint8_t> edited;
    CheckStatus(VitaLauncher::EditQuestFile(source, quest, edited));
    return edited;
}

// Whether the edit is rejected.
bool Rejected(std::span<const uint8_t> source, const VitaLauncher::CustomQuest& quest) {
    std::vector<uint8_t> edited;
    return !VitaLauncher::EditQuestFile(source, quest, edited).ok();
}

std::vector<uint16_t> Options(std::span<const uint8_t> source, uint16_t base) {
    std::vector<uint16_t> options;
    CheckStatus(VitaLauncher::QuestCompositionOptions(source, base, options));
    return options;
}

void QuestTests() {
    const auto original = Quest();
    VitaLauncher::CustomQuest custom{"Own quest", QuestCatalog::Set::Village, 1060, 1, true, 7};
    const auto edited = Edit(original, custom);
    Check(edited.size() == original.size() && std::string(edited.begin() + 16, edited.begin() + 25) == "Own quest", "quest name/extent");
    Check(edited[16 + 0x333] == 0x81 && edited[16 + 0x337] == 1 && edited[16 + 0x8a] == 7, "actual objective changes");
    Check(edited[16 + 0x397] == 0x81 && edited[16 + 0x399] == 20 && edited[16 + 0x39b] == 1, "live expanded objective must match summary");
    Check(edited[16 + 0x8b] == original[16 + 0x8b] && edited[16 + 0x335] == 20, "template map/monster retained");
    custom.count = 3;
    Check(Rejected(original, custom), "unsupported additional spawns rejected");
    auto bad = original;
    Word(bad, 8, 0xfffffff0);
    Check(Rejected(bad, custom), "bad quest extent rejected");
    auto multiple = original;
    Word(multiple, 16 + 0x334, (20u << 16) | 2);
    custom.count = 1;
    custom.minutes = 25;
    const auto fewer = Edit(multiple, custom);
    Check(fewer[16 + 0x337] == 1 && fewer[16 + 0x39b] == 1 && fewer[16 + 0x13b] == 25, "smaller authored objective and live time limit");
    custom.stars = 8;
    Check(Rejected(original, custom), "star bucket overflow rejected");

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
    Check(Options(composition, 1060).size() == 2, "same-map authored donors offered");
    const auto composed = Edit(composition, custom);
    Check(composed[first + 0x314] == 3 && composed[first + 0x335] == 3 && composed[first + 0x399] == 3 && composed[first + 0x39c] == 0x7a, "monster slot/live objective/rewards transferred together");
    Check(composed[first + 0x33b] == 0 && composed[first + 0x3f7] == 0 && composed[first + 0x4bc] == 0x5a && composed.size() == composition.size(),
          "old subquests removed; base small spawns and disc extent preserved");
    Check(composed[first + 0x8c] == 0 && composed[first + 0xb5] == 0 && composed[first + 0xde] == 'C', "subquest text cleared; completion text preserved");
    composition[second + 0x8b] = 3;
    Check(Rejected(composition, custom), "monster from incompatible map rejected");
    Word(composition, first + 0x310, 0x30105);
    Check(Options(composition, 1060).empty(), "scripted quest composition excluded");
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
    const auto edited = Edit(quest, {"Own quest", QuestCatalog::Set::Village, 1060, 1, false, 3});
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
    CheckStatus(VitaLauncher::ProcessDisc(root / "game.iso", root / "processed", [&](const auto&, uint64_t, uint64_t) { ++updates; }));
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

// Every disc texture's converted payload, decoded as the GPU samples it,
// against the GX decode: exact formats must match, CMPR's BC blocks within
// the difference of the in-between colours (GX blends them 3:5, BC1 1:2).
void CheckTextures(const std::filesystem::path& root) {
    DiscImage disc(root, false);
    std::map<uint32_t, std::pair<unsigned, uint64_t>> formats;
    unsigned textures = 0, patched = 0;

    for (const auto& path : VitaLauncher::TextureInventory::SourceNames(disc.FileNames())) {
        const auto data = disc.ReadFile(path);
        std::vector<VitaLauncher::TextureInventory::Texture> parsed;
        CheckStatus(VitaLauncher::TextureInventory::Parse(data, disc.FileOffset(path), parsed, 1));

        for (const auto& s : parsed) {
            VitaLauncher::TextureConversion converted;
            CheckStatus(VitaLauncher::ConvertTexture(s, data, disc.FileOffset(path), path, converted));
            uint64_t planned = 0;
            CheckStatus(VitaLauncher::TexturePayloadBytes(s, data, disc.FileOffset(path), planned));
            Check(planned == converted.payload.size(), "planned texture bytes differ from the payload");
            const aurora::ArrayRef<uint8_t> bytes{data.data() + s.offset, s.bytes};
            auto source = s.entries ? aurora::gfx::convert_texture_palette(s.format, s.width, s.height, s.mips, bytes, GXTlutFmt(s.paletteFormat), s.entries,
                                                                          {data.data() + s.palette, size_t(s.entries) * 2})
                                    : aurora::gfx::convert_texture(s.format, s.width, s.height, s.mips, bytes);

            // The atlases with the Classic Controller's buttons carry the Vita's.
            if (source.format == aurora::gl::Format::RGBA8 && ButtonGlyphs::Patch(s.name, s.width, s.height, source.data.data())) {
                ++patched;
                std::cout << "button atlas " << path << " " << s.name << " " << s.width << "x" << s.height << " GX format " << s.format << " level 0 hash " << std::hex
                          << ButtonGlyphs::SourceHash({data.data() + s.offset, size_t(s.width) * s.height * (s.format == GX_TF_RGBA8 ? 4 : 2)}) << std::dec << "\n";
            }

            const auto& r = converted.record;
            const unsigned tolerance = r.format == 2 || r.format == 3 ? 11 : 0;
            size_t at = 0;

            for (uint32_t mip = 0; mip < r.mips; ++mip) {
                const auto level = aurora::gfx::native_texture_rgba_level(r, converted.payload, mip);

                for (size_t i = 0; i < level.size(); ++i)
                    if (unsigned(std::abs(int(level[i]) - int(source.data.data()[at + i]))) > tolerance) {
                        std::cerr << path << " texture " << s.id << " (GX format " << s.format << ", " << s.width << "x" << s.height << ", tag " << r.format << ") mip " << mip
                                  << " texel " << i / 4 << " channel " << i % 4 << ": " << int(level[i]) << " instead of " << int(source.data.data()[at + i]) << "\n";
                        throw std::runtime_error("converted texture samples differently");
                    }

                at += level.size();
            }

            auto& count = formats[r.format];
            ++count.first;
            count.second += r.bytes;
            ++textures;
        }
    }

    for (const auto& [format, count] : formats)
        std::cout << "tag " << format << ": " << count.first << " textures, " << count.second / 1048576.0 << " MiB\n";

    std::cout << textures << " textures sample as their source, " << patched << " atlases with the Vita's buttons\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--disc-ready") {
            return VitaLauncher::DiscReady(argv[2]) ? 0 : 1;
        }

        if (argc == 3 && std::string_view(argv[1]) == "--check-textures") {
            CheckTextures(argv[2]);
            return 0;
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
                    const auto options = Options(file, id);

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

                            if (capture) {
                                bool supported = false;
                                CheckStatus(VitaLauncher::SupportsCapture(q, supported));

                                if (!supported)
                                    continue;
                            }

                            const auto edited = Edit(file, q);
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

            // Quests from scratch: every map and rank, one to three of the
            // map's monsters, small and large, hunted and captured.
            std::array<std::vector<uint8_t>, 3> sets;

            for (unsigned set = 0; set < 3; ++set)
                sets[set] = GameDiscImage().ReadFile("05/quest0" + std::to_string(set == 2 ? 3 : set) + "_eng.bin");

            unsigned scratch = 0;

            for (const uint8_t map : VitaLauncher::ScratchMaps()) {
                const auto allowed = VitaLauncher::ScratchMonsters(map);

                for (uint8_t rank = 0; rank < 3; ++rank)
                    for (size_t i = 0; i < allowed.size(); ++i)
                        for (size_t count = 1; count <= std::min<size_t>(3, allowed.size()); ++count)
                            for (unsigned capture = 0; capture < 2; ++capture) {
                                VitaLauncher::CustomQuest q{"Scratch"};
                                q.map = map;
                                q.rank = rank;
                                q.stars = 4;
                                q.minutes = 30;
                                q.size = count == 2 ? 50 : count == 3 ? 200 : 100;
                                q.capture = capture;

                                for (size_t k = 0; k < count; ++k)
                                    q.monsters[k] = allowed[(i + k) % allowed.size()];

                                if (count > 1 && std::any_of(q.monsters.begin(), q.monsters.end(), VitaLauncher::ScratchAlone))
                                    continue;

                                VitaLauncher::SetScratchCarrier(q);
                                bool supported = !capture;

                                if (capture)
                                    CheckStatus(VitaLauncher::ScratchCapture(q, supported));

                                if (!supported)
                                    continue;

                                std::vector<uint8_t> edited;
                                CheckStatus(VitaLauncher::ScratchQuestFile(sets, q, edited));
                                const auto& file = sets[unsigned(q.set)];
                                Check(edited.size() == file.size(), "scratch quest keeps the quest file's size");
                                size_t at = 0;

                                for (unsigned j = 0; j < ReadWord(edited, 4); ++j)
                                    if ((ReadWord(edited, ReadWord(edited, 8 + j * 8) + 0x2c) >> 16) == q.base)
                                        at = ReadWord(edited, 8 + j * 8);

                                Check(at != 0 && edited[at + 0x8b] == map, "scratch quest is on its map");

                                for (size_t k = 0; k < 3; ++k) {
                                    const size_t slot = at + 0x314 + k * 8;
                                    Check(edited[slot] == q.monsters[k] && (!q.monsters[k] || (edited[slot + 1] == k && edited[slot + 3] == 1 && edited[slot + 6] == q.size)),
                                          "scratch quest slot holds its monster, record and size");
                                    Check((ReadWord(edited, at + 0x334 + k * 8) >> 16) == q.monsters[k] && ReadWord(edited, at + 0x330 + k * 8) == ReadWord(edited, at + 0x394 + k * 0x60),
                                          "scratch quest objective targets its monster");
                                }

                                ++scratch;
                            }
            }

            Check(scratch > 0, "quests from scratch build");
            std::cout << "Authored quest composition audit passed: " << combinations << " configurations, " << scratch << " quests from scratch\n";
            return 0;
        }

        if (argc == 4 && std::string_view(argv[1]) == "--process") {
            std::string last;
            CheckStatus(VitaLauncher::ProcessDisc(argv[2], argv[3], [&](const std::string& phase, uint64_t done, uint64_t total) {
                if (phase != last) {
                    std::cout << phase << " (" << total << ")\n";
                    last = phase;
                }
            }));
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
