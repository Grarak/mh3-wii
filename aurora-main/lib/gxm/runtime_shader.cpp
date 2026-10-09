#include "runtime_shader.hpp"
#include "shaders.hpp"
#include "../gfx/cg_shader.hpp"
#include <vitashark.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <malloc.h>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace aurora::gxm {
namespace {
constexpr const char* Directory = "ux0:/data/mh3-wii/shaders";
constexpr const char* CacheVersion = "mh3-gxm-cg-v1-O3-fastmath-nofastprecision-fastint";
constexpr size_t MaxProgramBytes = 1024 * 1024;
constexpr size_t MaxResidentBytes = 16 * 1024 * 1024;
using Key = std::pair<uint64_t, RuntimeShaderStage>;
struct Entry {
  std::string source, origin;
  bool cg = false;
  std::unique_ptr<void, decltype(&std::free)> binary{nullptr, std::free};
  size_t bytes = 0;
};
struct Store {
  std::map<Key, Entry> entries;
  size_t residentBytes = 0;
  bool compiler = false;
};
Store& store() {
  static Store value;
  return value;
}
char suffix(RuntimeShaderStage stage) {
  return stage == RuntimeShaderStage::Vertex ? 'v' : stage == RuntimeShaderStage::Fragment ? 'f' : 'r';
}
std::string id(uint64_t hash, RuntimeShaderStage stage) {
  char text[24];
  std::snprintf(text, sizeof(text), "%016llx_%c", (unsigned long long)hash, suffix(stage));
  return text;
}
std::string json_string(std::string_view value) {
  std::string result = "\"";
  const char hex[] = "0123456789abcdef";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') {
      result += '\\';
      result += char(c);
    } else if (c < 32) {
      result += "\\u00";
      result += hex[c >> 4];
      result += hex[c & 15];
    } else
      result += char(c);
  }
  return result + '"';
}
bool embedded(uint64_t hash, const ShaderBinary* list, size_t count) {
  const auto* entry = std::lower_bound(list, list + count, hash, [](const ShaderBinary& entry, uint64_t value) { return entry.sourceHash < value; });
  return entry != list + count && entry->sourceHash == hash;
}
std::string read_text(const std::filesystem::path& path, size_t limit) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    return {};
  const auto size = file.tellg();
  if (size < 0 || static_cast<uint64_t>(size) > limit)
    return {};
  std::string result(static_cast<size_t>(size), '\0');
  file.seekg(0);
  if (!file.read(result.data(), result.size()))
    return {};
  return result;
}
void write_file(const std::filesystem::path& path, const void* bytes, size_t size) {
  const auto temporary = path.string() + ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "wb");
  if (!file)
    throw std::runtime_error("Cannot create shader cache file: " + temporary);
  const bool written = std::fwrite(bytes, 1, size, file) == size;
  const int closed = std::fclose(file);
  if (!written || closed || std::rename(temporary.c_str(), path.c_str())) {
    std::remove(temporary.c_str());
    throw std::runtime_error("Cannot publish shader cache file: " + path.string());
  }
}
void write_text(const std::filesystem::path& path, std::string_view text) {
  write_file(path, text.data(), text.size());
}
bool valid_program(const void* bytes, size_t size, RuntimeShaderStage stage) {
  if (!bytes || size < 64 || size > MaxProgramBytes || std::memcmp(bytes, "GXP\0", 4))
    return false;
  const auto* program = static_cast<const SceGxmProgram*>(bytes);
  // libshacccg returns word-padded blobs: a legal program can declare305
  // bytes while its compiler output contains308. Check the declared extent
  // against the owned allocation instead of rejecting trailing padding.
  const size_t declared = sceGxmProgramGetSize(program);
  if (declared < 64 || declared > size || sceGxmProgramCheck(program) < 0)
    return false;
  return sceGxmProgramGetType(program) == (stage == RuntimeShaderStage::Vertex ? SCE_GXM_VERTEX_PROGRAM : SCE_GXM_FRAGMENT_PROGRAM);
}
void compiler_log(const char* message, shark_log_level level, int line) {
  std::fprintf(stderr, "MH3 shader compiler [%d:%d] %s\n", int(level), line, message);
}
void start_compiler(Store& s) {
  if (s.compiler)
    return;
  shark_set_allocators(std::malloc, std::free);
  if (shark_init(nullptr) < 0 && shark_init("ur0:data/external/libshacccg.suprx") < 0)
    throw std::runtime_error("Missing runtime shader compiler: install libshacccg.suprx (DSVita setup)");
  s.compiler = true;
  shark_install_log_cb(compiler_log);
  shark_set_warnings_level(SHARK_WARN_HIGH);
}
void remember(Store& s, Key key, std::string_view source, std::string_view origin, bool cg) {
  auto found = s.entries.find(key);
  if (found != s.entries.end()) {
    if (found->second.source != source)
      throw std::runtime_error("Conflicting GLSL for runtime shader identity");
    return;
  }
  const size_t extra = source.size() + origin.size();
  if (extra > MaxResidentBytes - s.residentBytes)
    throw std::runtime_error("Runtime shader source/cache budget exhausted (16 MiB)");
  Entry entry;
  entry.source = source;
  entry.origin = origin;
  entry.cg = cg;
  s.entries.emplace(key, std::move(entry));
  s.residentBytes += extra;
}
} // namespace

void register_runtime_shader_sources(uint64_t vertexHash, uint64_t fragmentHash, std::string_view vertex, std::string_view fragment, std::string_view origin, bool cg) {
  auto& s = store();
  const std::string pair = id(vertexHash, RuntimeShaderStage::Vertex).substr(0, 16) + "-" + id(fragmentHash, RuntimeShaderStage::Fragment).substr(0, 16);
  const std::string metadata = "{\"program\":" + json_string(origin) + ",\"source_pair\":" + json_string(pair) + "}";
  if (!embedded(vertexHash, VertexShaders, VertexShaderCount))
    remember(s, {vertexHash, RuntimeShaderStage::Vertex}, vertex, metadata, cg);
  // Retain one fragment source for both RGBA and lazy scalar-target compilation.
  if (!embedded(fragmentHash, FragmentShaders, FragmentShaderCount) || !embedded(fragmentHash, FragmentScalarShaders, FragmentScalarShaderCount))
    remember(s, {fragmentHash, RuntimeShaderStage::Fragment}, fragment, metadata, cg);
}

const SceGxmProgram* runtime_shader_binary(uint64_t hash, RuntimeShaderStage stage) {
  auto& s = store();
  const Key key{hash, stage};
  auto found = s.entries.find(key);
  if (stage == RuntimeShaderStage::ScalarFragment && found == s.entries.end()) {
    const auto fragment = s.entries.find({hash, RuntimeShaderStage::Fragment});
    if (fragment != s.entries.end()) {
      // The scalar entry shares source identity but has its own declared output.
      remember(s, key, fragment->second.source, fragment->second.origin, fragment->second.cg);
      found = s.entries.find(key);
    }
  }
  if (found == s.entries.end())
    throw std::runtime_error("Missing runtime GLSL origin for GXM shader " + id(hash, stage));
  auto& entry = found->second;
  if (entry.binary)
    return static_cast<const SceGxmProgram*>(entry.binary.get());
  std::string cg = entry.cg ? entry.source : gfx::cg::convert(entry.source, stage == RuntimeShaderStage::Vertex ? gfx::cg::Stage::Vertex : gfx::cg::Stage::Fragment);
  if (stage == RuntimeShaderStage::ScalarFragment)
    cg = gfx::cg::scalar_fragment(cg);
  std::error_code error;
  std::filesystem::create_directories(Directory, error);
  if (error)
    throw std::runtime_error("Cannot create ux0:/data/mh3-wii/shaders/");
  const auto base = std::filesystem::path(Directory) / id(hash, stage);
  const auto cgPath = base.string() + ".cg", binaryPath = base.string() + ".gxp";
  const std::string cacheVersion = std::string(CacheVersion) + "-translator" + std::to_string(gfx::cg::Version);
  const std::string metadata = "{\"cache_version\":" + json_string(cacheVersion) + ",\"stage\":" + json_string(std::string(1, suffix(stage))) + ",\"origin\":" + entry.origin + "}\n";
  // Only a matching translator output/options cache can bypass compilation.
  const bool matching = read_text(base.string() + ".version", 256) == cacheVersion && read_text(cgPath, MaxProgramBytes) == cg;
  std::string cached = matching ? read_text(binaryPath, MaxProgramBytes) : std::string{};
  if (cached.size() > MaxResidentBytes - s.residentBytes)
    throw std::runtime_error("Runtime shader source/cache budget exhausted (16 MiB)");
  std::unique_ptr<void, decltype(&std::free)> candidate(nullptr, std::free);
  if (!cached.empty()) {
    candidate.reset(memalign(64, cached.size()));
    if (!candidate)
      throw std::bad_alloc();
    std::memcpy(candidate.get(), cached.data(), cached.size());
    if (!valid_program(candidate.get(), cached.size(), stage)) {
      candidate.reset();
      cached.clear();
    }
  }
  // Invalidate the old transaction before publishing changed source. A failed
  // compilation must never make an old GXP appear to match newly written Cg.
  if (!candidate)
    write_text(base.string() + ".version", "incomplete");
  write_text(cgPath, cg);
  write_text(base.string() + ".origin.json", metadata);
  size_t size = cached.size();
  if (!candidate) {
    start_compiler(s);
    std::fprintf(stderr, "MH3 compiling missing GXM shader %s (%s)\n", id(hash, stage).c_str(), entry.origin.c_str());
    if (cg.size() > MaxProgramBytes)
      throw std::runtime_error("Runtime Cg source exceeds 1 MiB");
    // vitaShaRK takes source length through size, then replaces it with GXP
    // length. Its returned program borrows the compiler output; never free it.
    uint32_t compiledSize = static_cast<uint32_t>(cg.size());
    struct CompilerOutput {
      ~CompilerOutput() { shark_clear_output(); }
    } output;
    const SceGxmProgram* compiled = shark_compile_shader_extended(cg.c_str(), &compiledSize, stage == RuntimeShaderStage::Vertex ? SHARK_VERTEX_SHADER : SHARK_FRAGMENT_SHADER, SHARK_OPT_FAST,
                                                                  SHARK_ENABLE, SHARK_DISABLE, SHARK_ENABLE);
    std::fprintf(stderr, "MH3 shader compiler output %s pointer=%p alignment=%u bytes=%u\n", id(hash, stage).c_str(), static_cast<const void*>(compiled),
                 unsigned(reinterpret_cast<uintptr_t>(compiled) & 63), compiledSize);
    if (!compiled || compiledSize < 64 || compiledSize > MaxProgramBytes)
      throw std::runtime_error("Runtime GXM compile failed; Cg/origin saved for " + id(hash, stage));
    size = compiledSize;
    if (size > MaxResidentBytes - s.residentBytes)
      throw std::runtime_error("Runtime shader source/cache budget exhausted (16 MiB)");
    candidate.reset(memalign(64, size));
    if (!candidate)
      throw std::bad_alloc();
    std::memcpy(candidate.get(), compiled, size);
    const auto* program = static_cast<const SceGxmProgram*>(candidate.get());
    std::fprintf(stderr, "MH3 shader compiler GXP %s magic=%02x%02x%02x%02x declared=%u check=%08x type=%d\n", id(hash, stage).c_str(), static_cast<const uint8_t*>(candidate.get())[0],
                 static_cast<const uint8_t*>(candidate.get())[1], static_cast<const uint8_t*>(candidate.get())[2], static_cast<const uint8_t*>(candidate.get())[3], sceGxmProgramGetSize(program),
                 unsigned(sceGxmProgramCheck(program)), int(sceGxmProgramGetType(program)));
    if (!valid_program(candidate.get(), size, stage))
      throw std::runtime_error("Runtime GXM compiler returned invalid GXP; Cg/origin saved for " + id(hash, stage));
    write_file(binaryPath, candidate.get(), size);
    write_text(base.string() + ".version", cacheVersion);
  } else
    std::fprintf(stderr, "MH3 loaded cached GXM shader %s\n", id(hash, stage).c_str());
  entry.binary = std::move(candidate);
  entry.bytes = size;
  s.residentBytes += size;
  return static_cast<const SceGxmProgram*>(entry.binary.get());
}

void clear_runtime_shaders() noexcept {
  auto& s = store();
  s.entries.clear();
  s.residentBytes = 0;
  if (s.compiler) {
    shark_end();
    s.compiler = false;
  }
}
} // namespace aurora::gxm
