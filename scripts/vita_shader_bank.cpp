// GXM shader bank sources (docs/optimizations/rendering/offline-shader-bank.md): writes the Cg of every
// program stage the Vita build can ever bind, named by its runtime key, plus a
// manifest. scripts/vita-shader-bank.py compiles them with psp2cgc and embeds
// the result. Host tool; standard library and xxHash only.
//
//   mh3-gxm-shader-bank OUTPUT UTILITY_DIR
//
// Keys match aurora-main/lib/gxm: XXH3-64 of the ProgramDesc vertex/fragment
// text (Cg for fixed native shaders, GLSL after alpha parameterization for the
// shared GLSL programs, converted by the same translator the runtime used), or
// the fixed small IDs of the hand-written utility programs.
#include "../runtime/src/native_shader_source.h"
#include "gfx/cg_shader.hpp"
#include "gfx/fixed_shader_sources.hpp"
#include "gxm/alpha_compare.hpp"
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>

namespace {
namespace fs = std::filesystem;

struct Stage {
    std::string cg, kind, name;
};

// (stage letter, key) -> stage; 'v' vertex, 'f' fragment, 'r' scalar fragment.
using Bank = std::map<std::pair<char, uint64_t>, Stage>;

std::string Hex(uint64_t key) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(key));
    return text;
}

uint64_t Key(const std::string& source) {
    return XXH3_64bits(source.data(), source.size());
}

void Add(Bank& bank, char stage, uint64_t key, std::string cg, std::string kind, std::string name) {
    auto [it, inserted] = bank.try_emplace({stage, key}, Stage{cg, kind, name});

    if (!inserted && it->second.cg != cg)
        throw std::runtime_error("Two shader bank sources share key " + Hex(key) + stage + ": " + it->second.name + ", " + name);
}

std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);

    if (!file)
        throw std::runtime_error("Cannot read " + path.string());

    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// Rewrites only changed files so the build's restat keeps later steps idle.
void Write(const fs::path& path, const std::string& text) {
    if (fs::exists(path) && Read(path) == text)
        return;

    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);

        if (!(file << text))
            throw std::runtime_error("Cannot write " + temporary);
    }

    fs::rename(temporary, path);
}

// The native material programs: the complete key domain of each stage.
void AddShaded(Bank& bank) {
    for (const auto& key : NativeShaderVertexKeys())
        Add(bank, 'v', Key(NativeShaderCgVertex(key)), NativeShaderCgVertex(key), "shade", NativeShaderVertexName(key));

    for (const auto& key : NativeShaderFragmentKeys())
        Add(bank, 'f', Key(NativeShaderCgFragment(key)), NativeShaderCgFragment(key), "shade", NativeShaderFragmentName(key));
}

// GLSL programs of the shared renderer modules, keyed and converted exactly as
// gxm/renderer.cpp Program and gxm/runtime_shader.cpp do.
void AddFixed(Bank& bank) {
    for (auto program : aurora::gfx::fixed_shader_sources()) {
        std::array<float, 4> references{};
        aurora::gxm::parameterize_alpha_references(program.fragment, references);
        Add(bank, 'v', Key(program.vertex), aurora::gfx::cg::convert(program.vertex, aurora::gfx::cg::Stage::Vertex), "fixed", program.label);
        Add(bank, 'f', Key(program.fragment), aurora::gfx::cg::convert(program.fragment, aurora::gfx::cg::Stage::Fragment), "fixed", program.label);
    }
}

// Hand-written utility Cg: VVVV-FFFF_s.cg names the fixed program IDs.
void AddUtility(Bank& bank, const fs::path& directory) {
    std::vector<fs::path> files;

    for (const auto& entry : fs::directory_iterator(directory))
        if (entry.is_regular_file() && entry.path().extension() == ".cg")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());

    if (files.empty())
        throw std::runtime_error("No utility Cg in " + directory.string());

    for (const auto& path : files) {
        const auto stem = path.stem().string();

        if (stem.size() != 35 || stem[16] != '-' || stem[33] != '_' || (stem[34] != 'v' && stem[34] != 'f'))
            throw std::runtime_error("Unexpected utility shader name " + path.string());

        const char stage = stem[34];
        const uint64_t key = std::stoull(stem.substr(stage == 'v' ? 0 : 17, 16), nullptr, 16);
        const auto cg = Read(path);
        Add(bank, stage, key, cg, "utility", path.filename().string());
    }
}
} // namespace

int main(int argc, char** argv) try {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s OUTPUT UTILITY_DIR\n", argv[0]);
        return 2;
    }

    const fs::path output = argv[1], utility = argv[2];
    Bank bank;
    AddShaded(bank);
    AddFixed(bank);
    AddUtility(bank, utility);

    const fs::path cg = output / "cg";
    fs::create_directories(cg);
    std::set<std::string> current;

    // The source column changes with any Cg change, also of a fixed-ID utility.
    std::string manifest = "# key\tstage\tkind\tsource\tname (generated by mh3-gxm-shader-bank)\n";
    std::map<char, size_t> counts;

    for (const auto& [id, stage] : bank) {
        const auto file = Hex(id.second) + "_" + id.first + ".cg";
        current.insert(file);
        Write(cg / file, stage.cg);
        manifest += Hex(id.second) + "\t" + id.first + "\t" + stage.kind + "\t" + Hex(Key(stage.cg)) + "\t" + stage.name + "\n";
        ++counts[id.first];
    }

    for (const auto& entry : fs::directory_iterator(cg))
        if (!current.count(entry.path().filename().string()))
            fs::remove(entry.path());
    Write(output / "manifest.tsv", manifest);
    std::printf("GXM shader bank sources: %zu vertex, %zu fragment, %zu scalar fragment stages\n", counts['v'], counts['f'], counts['r']);
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "mh3-gxm-shader-bank: %s\n", error.what());
    return 1;
}
