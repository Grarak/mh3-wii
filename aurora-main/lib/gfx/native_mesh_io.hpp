#pragma once
#include <aurora/fatal.hpp>
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace aurora::gfx {
// Read directly into the final allocation. Callers publish the immutable
// buffer only after the entire range has been read successfully.
inline void read_mesh_payload(const char* path, uint64_t offset, void* destination, size_t bytes) {
  if (!path || !destination || !bytes || offset > LONG_MAX)
    AURORA_THROW(std::runtime_error("Invalid native mesh payload range"));

  std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(path, "rb"), &std::fclose);

  if (!file || std::fseek(file.get(), long(offset), SEEK_SET))
    AURORA_THROW(std::runtime_error("Cannot open or seek native mesh payload"));

  auto* out = static_cast<unsigned char*>(destination);

  while (bytes) {
    const size_t count = std::fread(out, 1, std::min(bytes, size_t(1u << 20)), file.get());

    if (!count)
      AURORA_THROW(std::runtime_error("Native mesh payload is truncated or unreadable"));

    out += count;
    bytes -= count;
  }
}

// A buffer's payload: read from the file range, or copied from `payload`.
inline void fill_mesh_payload(const char* path, uint64_t offset, const void* payload, void* destination, size_t bytes) {
  if (payload)
    std::memcpy(destination, payload, bytes);
  else
    read_mesh_payload(path, offset, destination, bytes);
}
} // namespace aurora::gfx
