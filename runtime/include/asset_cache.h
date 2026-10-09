#pragma once
#include "status.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Load-time native asset conversion. The first game read of a texture/model
// source file parses it, assigns its texture and model IDs (kept for good in
// root/cache/index.log) and queues its conversion on worker threads that are
// free to run on any core. Converted packs are written under root/cache/ and
// reused by later launches. Consumers that need a pack before its conversion
// finishes wait for it; disc reads are answered after the conversion of the
// files they touch, so the game's loading screens absorb that time. Audio
// stream files are extracted the same way, at their first read (the quest's
// loading screen), to plain files that later reads use directly.
class AssetCache {
  public:
    struct SourceFile {
        std::string path;
        uint64_t offset;
        uint32_t bytes;
    };

    // Reads decrypted partition bytes without native texture tags.
    using RawRead = std::function<Status(uint64_t offset, uint8_t* out, size_t bytes)>;

    // Streams are copied out to cache/files at their first read and then read
    // from there (least recently used copies make room).
    AssetCache(const std::filesystem::path& root, std::vector<SourceFile> sources, std::vector<SourceFile> streams, RawRead read);
    ~AssetCache();
    AssetCache(const AssetCache&) = delete;

    // A game disc read: indexes the source files it touches and patches their
    // native texture tags into the bytes.
    void Read(uint64_t offset, uint8_t* out, size_t bytes);

    // Whether the textures and models of the source files that [offset,
    // offset + bytes) touches are converted (or failed), waiting up to
    // `limit` for them; files the read did not index count as converted.
    // Disc reads are answered only after this (ios.cpp), so that their
    // consumers find the packs ready.
    bool Converted(uint64_t offset, size_t bytes, std::chrono::milliseconds limit);

    // A map's overlay is loading: from now on stream reads are recorded for
    // `map`, and the streams it read before are extracted ahead of their use.
    void PrefetchStreams(const std::string& map);

    // The converted pack holding texture asset `id`, once it is written.
    std::string TexturePack(uint32_t id);

    // The converted model pack of the source file containing partition offset
    // `at`; `key` identifies that file. Empty when the file has no usable models.
    std::filesystem::path ModelPack(uint64_t at, uint32_t& key);

    // The extent of the indexed source file containing partition offset `at`
    // when it has models (converted yet or not). Never waits.
    bool ModelFile(uint64_t at, uint64_t& offset, uint32_t& bytes);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// The game's asset cache: active when GameDiscImage() has no prepared native
// texture map; null otherwise.
AssetCache* GameAssetCache();
