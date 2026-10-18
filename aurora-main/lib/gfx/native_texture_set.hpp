#pragma once
// Every opened native texture pack, by asset ID. Packs are immutable once
// published, so records stay valid for the process lifetime. Payload reads open
// the pack file per load: a set may hold one pack per disc file, more than the
// open-file limit. The resolver publishes the pack of an unknown ID (and may
// wait for its conversion); it runs without the set's lock held.
#include <aurora/fatal.hpp>
#include "native_texture_pack.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <atomic>
#include <chrono>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
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
  struct Pack;

  struct Entry {
    const NativeTextureRecord* record;
    const std::string* path;
    uint32_t version;
    Pack* pack = nullptr;
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

    {
      std::lock_guard guard(m_lock);

      if (const auto existing = m_packs.find(base + 1); existing != m_packs.end()) {
        if (existing->second->path != path || existing->second->records.size() != count)
          AURORA_THROW(std::runtime_error("Native texture packs overlap: " + path));

        return;
      }
    }

    // The payloads into memory now, in one read (the asset cache adds a pack
    // while the game loads): a texture's first use then copies from RAM. On
    // the Vita's card a read at first use stalled the render thread 12-30 ms
    // each, most of it opening the file.
    pack->first = 32 + table.size();
    const uint64_t payloadBytes = end - pack->first;
    uint8_t* region = nullptr;

    if (Arena& arena = resident_arena(); payloadBytes && payloadBytes <= arena.size) {
      std::lock_guard guard(m_lock);
      region = reserve(arena, payloadBytes);
    }

    // Read outside the lock: the region's earlier packs were evicted first,
    // and a reader copies under the lock, so none still reads them.
    if (region)
      file.read(region, pack->first, payloadBytes);

    // A resident pack's file closes: the process's open files are limited
    // (the Vita's sceIo), and a session reads hundreds of packs. One evicted
    // later reads from its file reopened (read below).
    if (!region)
      pack->file = std::make_unique<File>(std::move(file));

    std::lock_guard guard(m_lock);
    std::erase_if(m_filling, [&](const Filling& f) { return f.region == region; });

    if (m_packs.count(base + 1))
      return;

    if (region) {
      pack->resident = region;
      pack->residentBytes = payloadBytes;
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

  // Reads the record's payload into `out` (r.bytes long): from memory while
  // the pack is resident, else from its open file, counted in read_stats.
  void read(const Entry& entry, void* out) const {
    {
      // Under the lock: a pack added meanwhile may take over this region.
      std::lock_guard guard(m_lock);

      if (const uint8_t* resident = entry.pack->resident) {
        std::memcpy(out, resident + (entry.record->offset - entry.pack->first), entry.record->bytes);
        return;
      }
    }

    const auto start = std::chrono::steady_clock::now();

    // An evicted pack's file was closed: open it for this read only.
    if (entry.pack->file)
      entry.pack->file->read(out, entry.record->offset, entry.record->bytes);
    else
      File(entry.pack->path).read(out, entry.record->offset, entry.record->bytes);

    const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    auto& s = read_stats();
    s.count.fetch_add(1, std::memory_order_relaxed);
    s.bytes.fetch_add(entry.record->bytes, std::memory_order_relaxed);
    s.ns.fetch_add(ns, std::memory_order_relaxed);

    for (uint64_t longest = s.longestNs.load(std::memory_order_relaxed); ns > longest && !s.longestNs.compare_exchange_weak(longest, ns, std::memory_order_relaxed);) {
    }
  }

  struct ReadStats {
    std::atomic<uint64_t> count{0}, bytes{0}, ns{0}, longestNs{0};
  };

  static ReadStats& read_stats() {
    static ReadStats stats;
    return stats;
  }

private:
  class File;

public:
  struct Pack {
    std::string path;
    uint32_t version;
    std::vector<NativeTextureRecord> records;
    uint64_t first = 0;               // file offset of the payloads
    uint8_t* resident = nullptr;      // the payloads in the arena, until evicted (lock held)
    uint64_t residentBytes = 0;
    std::unique_ptr<File> file;       // kept open while the pack is not resident
  };

private:
  // The resident payloads share one 48 MiB ring of main memory (on the Vita
  // its own memory block, outside the libc heap). Packs go in at the head;
  // the head wrapping round evicts the oldest, which read from their files
  // again.
  struct Arena {
    uint8_t* base = nullptr;
    uint64_t size = 0, head = 0;
  };

  static constexpr uint64_t ArenaBytes = uint64_t(48) << 20;

  static Arena& resident_arena() {
    static Arena arena = [] {
      Arena a;
#ifdef __vita__
      const SceUID block = sceKernelAllocMemBlock("MH3 texture packs", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, ArenaBytes, nullptr);
      void* base = nullptr;

      if (block >= 0 && sceKernelGetMemBlockBase(block, &base) >= 0)
        a = {static_cast<uint8_t*>(base), ArenaBytes, 0};
      else
        std::fprintf(stderr, "aurora: no memory for resident texture packs (%08x); they read from their files\n", unsigned(block));
#else
      a = {new uint8_t[ArenaBytes], ArenaBytes, 0};
#endif
      return a;
    }();
    return arena;
  }

  // Lock held: `bytes` at the arena's head, evicting the packs there; null
  // when another add is still reading into that part.
  uint8_t* reserve(Arena& arena, uint64_t bytes) {
    if (arena.head + bytes > arena.size)
      arena.head = 0;

    const uint64_t from = arena.head, to = from + bytes;

    for (const auto& f : m_filling)
      if (f.from < to && from < f.from + f.bytes)
        return nullptr;

    for (auto& [first, pack] : m_packs) {
      if (!pack->resident)
        continue;

      const uint64_t at = uint64_t(pack->resident - arena.base);

      if (at < to && from < at + pack->residentBytes)
        pack->resident = nullptr;
    }

    arena.head = to;
    m_filling.push_back({arena.base + from, from, bytes});
    return arena.base + from;
  }

  Entry lookup(uint32_t id) const {
    std::lock_guard guard(m_lock);
    auto it = m_packs.upper_bound(id);

    if (!id || it == m_packs.begin())
      return {};

    --it;
    const auto& pack = *it->second;

    if (id - it->first >= pack.records.size())
      return {};

    return {&pack.records[id - it->first], &pack.path, pack.version, it->second.get()};
  }

  class File {
  public:
    uint64_t size = 0;

    File(File&& other) noexcept : size(other.size), m_path(std::move(other.m_path)), m_fd(other.m_fd.exchange(-1)) {}
    explicit File(const std::string& path) : m_path(path) {
#ifdef __vita__
      m_fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
      SceIoStat st{};

      if (m_fd < 0) {
        char what[32];
        std::snprintf(what, sizeof(what), "open (0x%08X)", unsigned(m_fd));
        fail(what);
      }

      if (sceIoGetstatByFd(m_fd, &st) < 0)
        fail("stat");

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
#ifdef __vita__
      bool reopened = false;
#endif

      for (size_t done = 0; done < bytes;) {
        const size_t amount = std::min<size_t>(bytes - done, 1u << 20);
#ifdef __vita__
        const int fd = m_fd.load(std::memory_order_acquire);
        const int n = sceIoPread(fd, p + done, amount, offset + done);

        // A suspend invalidates the descriptors of files open under ux0:data
        // (ENODEV after the resume, Sony's Development Process Overview): open
        // the pack again once and retry.
        if (n < 0 && !reopened) {
          reopened = true;

          if (reopen(fd))
            continue;
        }
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
#ifdef __vita__
    // Replaces `stale` with a new descriptor, unless another reader already
    // did; false when the pack cannot be opened again.
    bool reopen(int stale) const {
      static std::mutex lock;
      std::lock_guard guard(lock);

      if (m_fd.load(std::memory_order_relaxed) != stale)
        return true;

      const int fd = sceIoOpen(m_path.c_str(), SCE_O_RDONLY, 0);

      if (fd < 0)
        return false;

      sceIoClose(stale);
      m_fd.store(fd, std::memory_order_release);
      return true;
    }
#endif

    [[noreturn]] void fail(const char* what) const { AURORA_THROW(std::runtime_error(std::string("Cannot ") + what + " native texture pack " + m_path)); }
    std::string m_path;
    mutable std::atomic<int> m_fd{-1};
  };

  mutable std::mutex m_lock;
  std::map<uint32_t, std::unique_ptr<Pack>> m_packs; // by first ID
  // Arena regions being read into (outside the lock).
  struct Filling {
    uint8_t* region;
    uint64_t from, bytes;
  };

  std::vector<Filling> m_filling;
  Resolver m_resolver = nullptr;
};
} // namespace aurora::gfx
