#pragma once
// Every opened native texture pack, by asset ID. Packs are immutable once
// published, so records stay valid for the process lifetime. Payload reads open
// the pack file per load: a set may hold one pack per disc file, more than the
// open-file limit. The resolver publishes the pack of an unknown ID (and may
// wait for its conversion); it runs without the set's lock held.
#include <aurora/fatal.hpp>
#include "native_texture_pack.hpp"
#include <algorithm>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace aurora::gfx {
class NativeTextureSet {
public:
  using Resolver = void (*)(uint32_t id);
  struct Entry {
    const NativeTextureRecord* record;
    const std::string* path;
    uint32_t version;
  };

  void set_resolver(Resolver resolver) {
    std::lock_guard guard(m_lock);
    m_resolver = resolver;
  }

  // A pack's header word 4 is the ID before its first record (0 for a
  // whole-disc pack). Re-adding the same pack is a no-op.
  void add(const std::string& path) {
    File file(path);
    std::array<uint8_t, 32> header{};
    file.read(header.data(), 0, header.size());
    const uint32_t version = pack_u32(header.data() + 4), count = pack_u32(header.data() + 8), stride = pack_u32(header.data() + 12), base = pack_u32(header.data() + 16);

    if (pack_u32(header.data()) != 0x5456334d || (version != 1 && version != 2) || !count || count > 32768 || base > 32768 - count || stride != (version == 1 ? 40u : 64u) ||
        pack_u32(header.data() + 20) || pack_u32(header.data() + 24) || pack_u32(header.data() + 28) || 32 + uint64_t(count) * stride > file.size)
      AURORA_THROW(std::runtime_error("Invalid native texture pack header: " + path));
    auto pack = std::make_unique<Pack>();
    pack->path = path;
    pack->version = version;
    pack->records.resize(count);
    std::vector<uint8_t> table(size_t(count) * stride);
    file.read(table.data(), 32, table.size());
    uint64_t end = 32 + table.size();

    for (uint32_t i = 0; i < count; i++) {
      auto& r = pack->records[i];
      r = decode_native_texture_record({table.data() + size_t(i) * stride, stride}, version);
      validate_native_texture_record(r, version, base + i + 1, end, file.size);
      end = r.offset + r.bytes;
    }

    std::lock_guard guard(m_lock);
    const auto existing = m_packs.find(base + 1);

    if (existing != m_packs.end()) {
      if (existing->second->path != path || existing->second->records.size() != count)
        AURORA_THROW(std::runtime_error("Native texture packs overlap: " + path));

      return;
    }

    const auto next = m_packs.lower_bound(base + 1);

    if ((next != m_packs.end() && next->first <= base + count) ||
        (next != m_packs.begin() && std::prev(next)->first + std::prev(next)->second->records.size() > base + 1))
      AURORA_THROW(std::runtime_error("Native texture packs overlap: " + path));
    m_packs.emplace(base + 1, std::move(pack));
  }

  bool empty() const {
    std::lock_guard guard(m_lock);
    return m_packs.empty() && !m_resolver;
  }

  Entry find(uint32_t id) {
    if (auto entry = lookup(id); entry.record)
      return entry;

    Resolver resolver;
    {
      std::lock_guard guard(m_lock);
      resolver = m_resolver;
    }

    if (resolver)
      resolver(id);

    if (auto entry = lookup(id); entry.record)
      return entry;

    AURORA_THROW(std::runtime_error("Unknown native texture asset " + std::to_string(id)));
  }

  // Reads the record's payload into `out` (r.bytes long).
  static void read(const Entry& entry, void* out) { File(*entry.path).read(out, entry.record->offset, entry.record->bytes); }

private:
  struct Pack {
    std::string path;
    uint32_t version;
    std::vector<NativeTextureRecord> records;
  };

  Entry lookup(uint32_t id) const {
    std::lock_guard guard(m_lock);
    auto it = m_packs.upper_bound(id);

    if (!id || it == m_packs.begin())
      return {};

    --it;
    const auto& pack = *it->second;

    if (id - it->first >= pack.records.size())
      return {};

    return {&pack.records[id - it->first], &pack.path, pack.version};
  }

  class File {
  public:
    uint64_t size = 0;
    explicit File(const std::string& path) : m_path(path) {
#ifdef __vita__
      m_fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
      SceIoStat st{};

      if (m_fd < 0 || sceIoGetstatByFd(m_fd, &st) < 0)
        fail("open");

      size = st.st_size;
#else
      m_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
      struct stat st{};

      if (m_fd < 0 || fstat(m_fd, &st) < 0)
        fail("open");

      size = st.st_size;
#endif
    }

    ~File() {
#ifdef __vita__
      if (m_fd >= 0)
        sceIoClose(m_fd);
#else
      if (m_fd >= 0)
        ::close(m_fd);
#endif
    }

    void read(void* out, uint64_t offset, size_t bytes) const {
      if (offset > size || bytes > size - offset)
        fail("read past the end of");

      auto* p = static_cast<uint8_t*>(out);

      for (size_t done = 0; done < bytes;) {
        const size_t amount = std::min<size_t>(bytes - done, 1u << 20);
#ifdef __vita__
        const int n = sceIoPread(m_fd, p + done, amount, offset + done);
#else
        const auto n = ::pread(m_fd, p + done, amount, off_t(offset + done));

        if (n < 0 && errno == EINTR)
          continue;
#endif
        if (n <= 0)
          fail("read");

        done += size_t(n);
      }
    }

  private:
    [[noreturn]] void fail(const char* what) const { AURORA_THROW(std::runtime_error(std::string("Cannot ") + what + " native texture pack " + m_path)); }
    std::string m_path;
    int m_fd = -1;
  };

  mutable std::mutex m_lock;
  std::map<uint32_t, std::unique_ptr<Pack>> m_packs; // by first ID
  Resolver m_resolver = nullptr;
};
} // namespace aurora::gfx
