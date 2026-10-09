#pragma once
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
    void Write(std::span<const uint8_t> bytes, const Sink& sink, const ArrayResolver& resolver = {});
    size_t PendingBytes() const { return pending.size(); }
private:
    std::vector<uint8_t> pending;
};
