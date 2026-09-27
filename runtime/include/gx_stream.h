#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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
    struct TextureCopy {
        uintptr_t destination;
        uint32_t x, y, sourceWidth, sourceHeight, width, height, format;
        bool halfScale, clear;
    };
    using TextureCopySink = std::function<void(const TextureCopy&)>;
    void Write(std::span<const uint8_t> bytes, const Sink& sink, const ArrayResolver& resolver = {},
               const CopySink& copy = {}, const TextureCopySink& textureCopy = {});
    unsigned PixelFormat() const { return bp[0x43] & 7; }
    size_t PendingBytes() const { return pending.size(); }
private:
    std::vector<uint8_t> pending;
    std::array<uint32_t, 256> bp{};
    std::array<uint32_t, 256> cp{};
    std::array<ArrayView, 16> boundArrays{};
    std::array<bool, 8> textureBound{};
    unsigned displayListDepth = 0;
    uint32_t bpMask = 0xFFFFFF, paletteRevision = 0;
    struct PaletteMemory {
        std::array<uint8_t, 1024 * 1024> bytes{};
        std::array<bool, 32768> loadedLines{};
        uint32_t revision = 0;
    };
    std::shared_ptr<PaletteMemory> palettes;
    struct PaletteBinding { uint32_t revision = 0, descriptor = 0, entries = 0; };
    std::array<PaletteBinding, 8> paletteBindings{};
    void LoadPalette(const ArrayResolver& resolver);
    void BindDrawPalettes(const Sink& sink, const ArrayResolver& resolver);
    std::array<uint8_t, 37> TexturePacket(unsigned slot, const ArrayResolver& resolver);
    struct Attribute { unsigned descriptor = 0, bytes = 0, indices = 1; };
    std::array<Attribute, 12> Attributes(unsigned format) const;
    static std::array<uint8_t, 16> ArrayPacket(unsigned slot, ArrayView view);
    void BindDrawArrays(std::span<const uint8_t> draw, const Sink& sink, const ArrayResolver& resolver);
    size_t VertexSize(unsigned format) const;
};
