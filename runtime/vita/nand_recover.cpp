// Recovery of NAND replaces interrupted between their two renames
// (nand_recover.h, runtime/vita/nand.inc).
#include "nand_recover.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {
bool RecoverDirectory(const std::filesystem::path& directory, unsigned depth) {
    constexpr std::string_view suffix = ".replaced";
    const int dir = depth < 16 ? sceIoDopen(directory.string().c_str()) : -1;

    if (dir < 0)
        return false;

    std::vector<std::string> asides, directories;
    SceIoDirent entry{};

    while (sceIoDread(dir, &entry) > 0) {
        const std::string_view name(entry.d_name);

        if (name == "." || name == "..")
            continue;

        if (SCE_S_ISDIR(entry.d_stat.st_mode))
            directories.emplace_back(name);
        else if (name.size() > suffix.size() && name.ends_with(suffix))
            asides.emplace_back(name);
    }

    sceIoDclose(dir);
    bool changed = !asides.empty();

    for (const auto& name : asides) {
        const auto aside = (directory / name).string();
        const auto original = (directory / name.substr(0, name.size() - suffix.size())).string();
        SceIoStat info{};

        if (sceIoGetstat(original.c_str(), &info) >= 0)
            sceIoRemove(aside.c_str());
        else {
            std::fprintf(stderr, "MH3 NAND: restoring %s from an interrupted replace\n", original.c_str());
            sceIoRename(aside.c_str(), original.c_str());
        }
    }

    for (const auto& name : directories)
        changed |= RecoverDirectory(directory / name, depth + 1);

    return changed;
}
} // namespace

extern "C" void MH3_RecoverNandReplaces(const char* nandRoot) {
    if (!RecoverDirectory(nandRoot, 0))
        return;

    // The restored names on the card before anything reads them.
    const std::string root = nandRoot;

    if (const auto colon = root.find(':'); colon != root.npos)
        sceIoSync(root.substr(0, colon + 1).c_str(), 0);
}

void RecoverNandSaves() {
    const char* user = std::getenv("MH3_USER_DIR");
    MH3_RecoverNandReplaces(((user ? std::filesystem::path(user) : std::filesystem::path("saves")) / "nand").string().c_str());
}
