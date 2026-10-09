#include "model_assets.h"
#include "../native/adapters/model_catalog_bridge.h"
#include "../native/adapters/model_gpu_bridge.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
// One startup pack, with hard admission bounds before any payload allocation.
// Scene-level streaming of the complete model archive is a separate task.
constexpr size_t MaxMetadataBytes = 8 * 1024 * 1024;
constexpr size_t MaxIdentityBytes = 4 * 1024 * 1024;
constexpr size_t MaxGPUBytes = 32 * 1024 * 1024;
struct Assets {
    std::vector<uint8_t> metadata, identity;
    mh3_mesh_pack pack{};
    mh3_model_catalog catalog{};
    uint64_t generation = 0;
    bool gpuRegistered = false, catalogRegistered = false;
    uintptr_t handle() const { return reinterpret_cast<uintptr_t>(this); }
};
std::unique_ptr<Assets> assets;
uint64_t generation = 0;
uint32_t Word(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
[[noreturn]] void Error(const std::filesystem::path& path, const char* reason) {
    throw std::runtime_error("Native model pack " + path.string() + ": " + reason);
}
size_t FileSize(std::ifstream& file, const std::filesystem::path& path, size_t limit) {
    if (!file) Error(path, "cannot open");
    const auto bytes = file.tellg();
    if (bytes < 0 || uint64_t(bytes) > limit) Error(path, "file exceeds admission budget");
    file.seekg(0);
    return size_t(bytes);
}
void Read(std::ifstream& file, const std::filesystem::path& path, void* destination, size_t bytes) {
    if (!file.read(static_cast<char*>(destination), bytes)) Error(path, "truncated read");
}
void Retire(Assets& loaded) {
    if (loaded.catalogRegistered) {
        UnregisterNativeModelPack(loaded.handle());
        loaded.catalogRegistered = false;
    }
    if (loaded.gpuRegistered) {
        UnregisterNativeModelGPU(loaded.handle(), loaded.generation);
        loaded.gpuRegistered = false;
    }
}
}

void LoadNativeModelAssets(const char* discRoot) {
    if (assets) throw std::runtime_error("Native model assets already admitted");
    const char* overridePath = std::getenv("MH3_NATIVE_MODEL_PACK");
    const auto path = overridePath ? std::filesystem::path(overridePath) :
        std::filesystem::path(discRoot ? discRoot : "ux0:/data/mh3-wii") / "native-models/models.bin";
    if (!overridePath && !std::filesystem::exists(path)) return;
    auto next = std::make_unique<Assets>();
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto bytes = FileSize(file, path, MaxMetadataBytes + MaxGPUBytes + 4096);
    std::array<uint8_t, 128> header{};
    Read(file, path, header.data(), header.size());
    if (std::memcmp(header.data(), "M3MP", 4)) Error(path, "invalid M3MP magic");
    const size_t metadataBytes = Word(header.data() + 8);
    const uint64_t gpuOffset = Word(header.data() + 12), gpuBytes = Word(header.data() + 16);
    if (metadataBytes < header.size() || metadataBytes > MaxMetadataBytes || gpuBytes > MaxGPUBytes)
        Error(path, "metadata or GPU payload exceeds admission budget");
    if (gpuOffset < metadataBytes || gpuOffset + gpuBytes != bytes)
        Error(path, "invalid file extent");
    next->metadata.resize(metadataBytes);
    std::memcpy(next->metadata.data(), header.data(), header.size());
    Read(file, path, next->metadata.data() + header.size(), metadataBytes - header.size());
    if (mh3_mesh_pack_open_metadata({next->metadata.data(), next->metadata.size()}, &next->pack) != MH3_PACK_OK)
        Error(path, "invalid compact metadata");
    auto identityPath = path;
    identityPath.replace_extension(".identity.bin");
    std::ifstream identity(identityPath, std::ios::binary | std::ios::ate);
    next->identity.resize(FileSize(identity, identityPath, MaxIdentityBytes));
    Read(identity, identityPath, next->identity.data(), next->identity.size());
    if (mh3_model_catalog_open({next->identity.data(), next->identity.size()}, &next->catalog) != MH3_PACK_OK ||
        mh3_model_catalog_validate_pack(&next->catalog, &next->pack) != MH3_PACK_OK)
        Error(identityPath, "invalid or mismatched source identities");
    if (generation == UINT64_MAX) Error(path, "load generation exhausted");
    next->generation = ++generation;
    const auto name = path.string();
    RegisterNativeModelGPU(next->pack, name.c_str(), next->handle(), next->generation);
    next->gpuRegistered = true;
    try {
        RegisterNativeModelPack(&next->pack, &next->catalog, next->handle(), next->generation);
        next->catalogRegistered = true;
    } catch (...) {
        Retire(*next);
        throw;
    }
    std::fprintf(stderr, "MH3 native model assets: %u models, %u meshes, %zu metadata + %zu identity bytes, %u GPU bytes\n",
        next->catalog.model_count, next->pack.counts[MH3_PACK_MESHES], next->metadata.size(),
        next->identity.size(), next->pack.gpu_bytes);
    assets = std::move(next);
}

void UnloadNativeModelAssets() {
    if (assets) {
        Retire(*assets);
        assets.reset();
    }
}
