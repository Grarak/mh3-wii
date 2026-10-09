#include "port_capture.hpp"
#include "cg_shader.hpp"
#include "../gxm/alpha_compare.hpp"
#include <xxhash.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

namespace aurora::gfx::port_capture {
namespace {
struct Capture {
  std::mutex mutex;
  std::filesystem::path root;
  FILE* events = nullptr;
  uint32_t drawInterval = 30;
  std::set<std::string> shaderPairs, cgPairs, shaderOrigins;
  Capture() {
    const char* path = std::getenv("AURORA_PORT_CAPTURE");
    if (!path || !*path) return;
    std::error_code error;
    root = path;
    std::filesystem::create_directories(root / "shaders", error);
    if (error) { root.clear(); return; }
    events = std::fopen((root / "events.tsv").c_str(), "w");
    if (!events) root.clear();
    if (const char* interval = std::getenv("AURORA_PORT_CAPTURE_DRAW_INTERVAL"))
      drawInterval = std::clamp(std::strtoul(interval, nullptr, 10), 1ul,
                                static_cast<unsigned long>(UINT32_MAX));
    std::fprintf(events, "capture_config\tdraw_interval\t%u\n", drawInterval);
  }
  ~Capture() { if (events) std::fclose(events); }
};
Capture& capture() { static Capture value; return value; }
void write(const std::filesystem::path& path, std::string_view source) {
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) throw std::runtime_error("Cannot write shader capture: " + path.string());
  const auto bytes = std::fwrite(source.data(), 1, source.size(), file);
  const auto status = std::fclose(file);
  if (bytes != source.size() || status) throw std::runtime_error("Incomplete shader capture: " + path.string());
}
std::string pair(uint64_t vertex, uint64_t fragment) {
  char name[40];
  std::snprintf(name, sizeof(name), "%016llx-%016llx", (unsigned long long)vertex, (unsigned long long)fragment);
  return name;
}
std::string json(std::string_view input) {
  std::string result = "\"";
  for (const unsigned char ch : input) {
    if (ch == '"' || ch == '\\') { result += '\\'; result += char(ch); }
    else if (ch < 32) { char escape[7]; std::snprintf(escape, sizeof(escape), "\\u%04x", unsigned(ch)); result += escape; }
    else result += char(ch);
  }
  return result + '"';
}
std::string field(std::string_view input) {
  std::string result(input);
  for (auto& ch : result) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
  return result;
}
} // namespace
bool enabled() noexcept { return capture().events != nullptr; }
void pass(uint32_t frame, uint32_t index, const void* color, const void* depth,
          const void* resolve, const void* snapshot, uint32_t width, uint32_t height,
          uint32_t commands) noexcept {
  auto& c = capture();
  // Keep every pass plan: a sampled draw may consume a previous frame's copy.
  if (!c.events) return;
  std::lock_guard lock(c.mutex);
  std::fprintf(c.events, "pass_plan\t%u\t%u\t%p\t%p\t%p\t%p\t%u\t%u\t%u\n",
               frame, index, color, depth, resolve, snapshot, width, height, commands);
}
void schedule(uint32_t frame, uint32_t regions, uint32_t draws,
              uint32_t transitionsBefore, uint32_t transitionsAfter) noexcept {
  auto& c = capture();
  if (!c.events || frame % c.drawInterval != 0) return;
  std::lock_guard lock(c.mutex);
  std::fprintf(c.events, "schedule\t%u\t%u\t%u\t%u\t%u\n", frame, regions, draws,
               transitionsBefore, transitionsAfter);
}
void draw(const DrawState& s, uint64_t vh, uint64_t fh, uint32_t vertices,
          uint32_t indices, uint32_t instances, bool discard, bool depthReplace,
          const std::array<const void*, 8>& textures) noexcept {
  auto& c = capture();
  if (!c.events || s.frame % c.drawInterval != 0) return;
  std::lock_guard lock(c.mutex);
  std::fprintf(c.events,
      "draw\t%u\t%016llx-%016llx\t%u\t%u\t%u\t%u\t%u\t%06x\t%u\t%u\t%08x\t%u\t%08x\t%08x\t%08x\t%08x\n",
      s.frame, (unsigned long long)vh, (unsigned long long)fh, vertices, indices,
      instances, s.primitives, s.flags, s.alphaCompare, discard, depthReplace,
      s.displayList, s.displayListOffset, s.callers[0], s.callers[1], s.callers[2], s.callers[3]);
  // Keep this adjacent to its draw under the same lock. Existing draw readers
  // retain their fixed schema; resource readers join this additional row.
  std::fprintf(c.events, "draw_textures\t%u\t%016llx-%016llx", s.frame,
               (unsigned long long)vh, (unsigned long long)fh);
  for (const auto source : s.textureSources) std::fprintf(c.events, "\t%08x", source);
  std::fputc('\n', c.events);
  std::fprintf(c.events, "draw_bindings\t%u\t%u\t%016llx-%016llx", s.frame, s.passIndex,
               (unsigned long long)vh, (unsigned long long)fh);
  for (const auto texture : textures) std::fprintf(c.events, "\t%p", texture);
  std::fputc('\n', c.events);
}
void native_program(uint32_t id,uint64_t vh,uint64_t fh,bool discard,bool depthReplace,
                    std::string_view label) noexcept {
  auto& c=capture();if (!c.events) return;
  const auto safeLabel=field(label);
  std::lock_guard lock(c.mutex);
  std::fprintf(c.events,"native_program\t%u\t%016llx-%016llx\t%u\t%u\t%s\n",id,
      (unsigned long long)vh,(unsigned long long)fh,discard,depthReplace,safeLabel.c_str());
}
void native_draw(std::string_view kind,const NativeDraw& s) noexcept {
  auto& c=capture();if (!c.events || s.frame%c.drawInterval) return;
  std::lock_guard lock(c.mutex);
  std::fprintf(c.events,"native_draw\t%u\t%.*s\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u",
      s.frame,int(kind.size()),kind.data(),s.program,s.vertices,s.indices,s.instances,
      s.flags,s.depthFunction,s.target,s.depthTarget,s.rasterKnown,s.display);
  for (const auto texture:s.textures) std::fprintf(c.events,"\t%u",texture);
  std::fputc('\n',c.events);
  std::fflush(c.events);
}
void shader(std::string_view label, std::string_view vertex, std::string_view fragment,
            std::string_view origin) noexcept {
  auto& c = capture();
  if (!c.events) return;
  std::lock_guard lock(c.mutex);
  const uint64_t vh = XXH3_64bits(vertex.data(), vertex.size());
  const uint64_t fh = XXH3_64bits(fragment.data(), fragment.size());
  const auto name = pair(vh, fh);
  const auto base = c.root / "shaders" / name;
  const auto safeLabel = field(label), safeOrigin = field(origin);
  const auto originKey = name + '\0' + std::string(label) + '\0' + std::string(origin);
  if (c.shaderOrigins.insert(originKey).second) {
    std::fprintf(c.events, "shader\t%s\t%s\n", name.c_str(), safeLabel.c_str());
    std::fprintf(c.events, "shader_origin\t%s\t%s\t%s\n", name.c_str(), safeLabel.c_str(), safeOrigin.c_str());
  }
  if (!c.shaderPairs.insert(name).second) return;
  try {
    write(base.string() + ".vert", vertex);
    write(base.string() + ".frag", fragment);
    std::string normalized(fragment); std::array<float, 4> alpha{};
    const bool alphaParameterized = gxm::parameterize_alpha_references(normalized, alpha);
    const auto normalizedHash = XXH3_64bits(normalized.data(), normalized.size());
    const auto cgName = pair(vh, normalizedHash);
    if (!c.cgPairs.contains(cgName)) {
      const auto vertexCg = cg::convert(vertex, cg::Stage::Vertex);
      const auto fragmentCg = cg::convert(normalized, cg::Stage::Fragment);
      const auto scalarCg = cg::scalar_fragment(fragmentCg);
      const auto cgBase = c.root / "shaders" / cgName;
      write(cgBase.string() + "_v.cg", vertexCg);
      write(cgBase.string() + "_f.cg", fragmentCg);
      write(cgBase.string() + "_r.cg", scalarCg);
      c.cgPairs.insert(cgName);
    }
    const auto metadata = "{\n  \"generator\": \"aurora::gfx::cg\",\n  \"generator_version\": " + std::to_string(cg::Version) +
      ",\n  \"label\": " + json(label) + ",\n  \"source_origin\": " + json(origin.empty() ? "Aurora ProgramDesc" : origin) +
      ",\n  \"glsl_pair\": " + json(name) + ",\n  \"vita_pair\": " + json(cgName) +
      ",\n  \"vertex_glsl\": " + json(name + ".vert") + ",\n  \"fragment_glsl\": " + json(name + ".frag") +
      ",\n  \"vertex_cg\": " + json(cgName + "_v.cg") + ",\n  \"fragment_cg\": " + json(cgName + "_f.cg") +
      ",\n  \"scalar_fragment_cg\": " + json(cgName + "_r.cg") +
      ",\n  \"alpha_parameterized\": " + (alphaParameterized ? "true" : "false") +
      ",\n  \"alpha_references\": [" + std::to_string(alpha[0]) + ", " + std::to_string(alpha[1]) + "]\n}\n";
    write(base.string() + ".origin.json", metadata);
    std::fprintf(c.events, "shader_cg\t%s\t%s\t%u\n", name.c_str(), cgName.c_str(), cg::Version);
  } catch (const std::exception& error) {
    const auto message = field(error.what());
    std::fprintf(c.events, "shader_cg_error\t%s\t%s\t%s\n", name.c_str(), safeLabel.c_str(), message.c_str());
    std::fprintf(stderr, "Aurora shader capture %s (%s): %s\n", name.c_str(), safeLabel.c_str(), message.c_str());
    try { write(base.string() + ".cg-error.txt", error.what()); } catch (...) {}
  }
  std::fflush(c.events);
}
void texture(const void* id, bool create, uint32_t width, uint32_t height, uint32_t mips,
             uint32_t format, uint32_t bytesPerPixel, std::string_view label) noexcept {
  auto& c = capture();
  if (!c.events) return;
  std::lock_guard lock(c.mutex);
  std::fprintf(c.events, "%s\t%p\t%u\t%u\t%u\t%u\t%u\t%.*s\n", create ? "create" : "destroy",
               id, width, height, mips, format, bytesPerPixel, int(label.size()), label.data());
}
void upload(const void* id, uint32_t mip, uint64_t bytes) noexcept {
  auto& c = capture();
  if (!c.events) return;
  std::lock_guard lock(c.mutex);
  std::fprintf(c.events, "upload\t%p\t%u\t%llu\n", id, mip, (unsigned long long)bytes);
}
void frame(uint32_t index, uint32_t draws, uint32_t merged, uint32_t passes,
           uint64_t vertices, uint64_t uniforms, uint64_t indices, uint64_t storage) noexcept {
  auto& c = capture();
  if (!c.events) return;
  std::lock_guard lock(c.mutex);
  std::fprintf(c.events, "frame\t%u\t%u\t%u\t%u\t%llu\t%llu\t%llu\t%llu\n", index, draws, merged,
               passes, (unsigned long long)vertices, (unsigned long long)uniforms,
               (unsigned long long)indices, (unsigned long long)storage);
  std::fflush(c.events);
}
}
