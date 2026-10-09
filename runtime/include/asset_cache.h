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
    static constexpr uint64_t StreamCacheBytes = 128ull << 20;

    // What the cache should have of the storage in all, free space plus what
    // it holds: StreamCacheBytes of streams, the rest for converted packs.
    // With less, packs of maps a launch did not load are deleted to make room
    // (they convert again at their next load); a pack that still does not
    // fit stops the game with an error instead of filling the storage.
    static constexpr uint64_t StorageWanted = 384ull << 20;

    // A message for the player when the storage holding root offers the cache
    // less than StorageWanted; empty otherwise.
    static std::string StorageWarning(const std::filesystem::path& root);

    AssetCache(const std::filesystem::path& root, std::vector<SourceFile> sources, std::vector<SourceFile> streams, RawRead read);
    ~AssetCache();
    AssetCache(const AssetCache&) = delete;

    // A game disc read: indexes the source files it touches and patches their
    // native texture tags into the bytes.
    Status Read(uint64_t offset, uint8_t* out, size_t bytes);

    // Whether the textures and models of the source files that [offset,
    // offset + bytes) touches are converted (or failed), waiting up to
    // `limit` for them; files the read did not index count as converted.
    // Disc reads are answered only after this (ios.cpp), so that their
    // consumers find the packs ready.
    bool Converted(uint64_t offset, size_t bytes, std::chrono::milliseconds limit);

    // A map's overlay is loading: from now on stream reads are recorded for
    // `map`, and the streams it read before are extracted ahead of their use.
    void PrefetchStreams(const std::string& map);

    // Called with a source file's texture pack once it is written and the
    // game has read the file (a load), on the reading or the converting
    // thread with the cache locked: it must only hand the path on.
    using PackReady = std::function<void(const std::string& pack)>;
    void SetTexturePackReady(PackReady ready);

    // Likewise with a source file's model pack and its ModelPack key, once
    // the pack is written and the game has read the file.
    using ModelPackReady = std::function<void(uint32_t key, const std::string& pack)>;
    void SetModelPackReady(ModelPackReady ready);

    // The converted pack holding texture asset `id`, once it is written; an
    // error when the asset is unknown or its source failed to convert.
    Status TexturePack(uint32_t id, std::string& pack);

    // The converted model pack of the source file containing partition offset
    // `at`; `key` identifies that file. Empty when the file has no usable
    // models; an error when its conversion failed.
    Status ModelPack(uint64_t at, uint32_t& key, std::filesystem::path& pack);

    // ModelPack without waiting: false while the source file containing `at`
    // is converting. `pack` is empty when `at` is in no source, the source is
    // not checked yet, the file has no usable models or its conversion failed.
    bool ConvertedModelPack(uint64_t at, uint32_t& key, std::filesystem::path& pack);

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
