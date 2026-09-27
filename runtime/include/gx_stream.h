#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

// Frames original big-endian GX commands before handing them to the host.
// Pointer-bearing commands stay rejected until their conversion is implemented.
class GuestGxStream {
public:
    using Sink = std::function<void(std::span<const uint8_t>)>;
    struct ArrayView { uintptr_t address; uint32_t size; };
    using ArrayResolver = std::function<ArrayView(uint32_t)>;
    struct DisplayCopy { uintptr_t destination; uint32_t width, height; bool clear; };
    using CopySink = std::function<void(const DisplayCopy&)>;
    void Write(std::span<const uint8_t> bytes, const Sink& sink, const ArrayResolver& resolver = {}, const CopySink& copy = {});
    size_t PendingBytes() const { return pending.size(); }
private:
    std::vector<uint8_t> pending;
    std::array<uint32_t, 256> bp{};
    std::array<bool, 8> textureBound{};
    uint32_t bpMask = 0xFFFFFF, textureRevision = 0;
    std::array<uint8_t, 37> TexturePacket(unsigned slot, const ArrayResolver& resolver);
};
