#include "port_capture.hpp"
#include <xxhash.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>

namespace aurora::gfx::port_capture {
namespace {
struct Capture {
  std::mutex mutex;
  std::filesystem::path root;
  FILE* events = nullptr;
  Capture() {
    const char* path = std::getenv("AURORA_PORT_CAPTURE");
    if (!path || !*path) return;
    std::error_code error;
    root = path;
    std::filesystem::create_directories(root / "shaders", error);
    if (error) { root.clear(); return; }
    events = std::fopen((root / "events.tsv").c_str(), "w");
    if (!events) root.clear();
  }
  ~Capture() { if (events) std::fclose(events); }
};
Capture& capture() { static Capture value; return value; }
void write(const std::filesystem::path& path, std::string_view source) {
  if (FILE* file = std::fopen(path.c_str(), "wb")) {
    std::fwrite(source.data(), 1, source.size(), file);
    std::fclose(file);
  }
}
}
void shader(std::string_view label, std::string_view vertex, std::string_view fragment) noexcept {
  auto& c = capture();
  if (!c.events) return;
  std::lock_guard lock(c.mutex);
  const uint64_t vh = XXH3_64bits(vertex.data(), vertex.size());
  const uint64_t fh = XXH3_64bits(fragment.data(), fragment.size());
  char name[40];
  std::snprintf(name, sizeof(name), "%016llx-%016llx", (unsigned long long)vh, (unsigned long long)fh);
  const auto base = c.root / "shaders" / name;
  write(base.string() + ".vert", vertex);
  write(base.string() + ".frag", fragment);
  std::fprintf(c.events, "shader\t%s\t%.*s\n", name, int(label.size()), label.data());
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
