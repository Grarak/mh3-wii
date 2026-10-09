// Guest memory as the model draw path sees it. On the game thread this is live
// guest memory. A model instance drawn away from the game thread installs a
// view (mh3_res_memory with regions): owned captured bytes overlay the guest
// windows, and writes land only inside writable regions, never in live memory.
#pragma once
// Include after memory.h: native tests substitute their own Memory.
#include "runtime/native/mh3_model_runtime.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace DrawMemory {
// The installed view, or nullptr for live guest memory.
extern thread_local const mh3_res_memory* view;
// A recording view over live memory: writes outside its regions go to live
// memory instead of failing.
extern thread_local bool liveWrites;
// Host bytes of [address, address + length) in the installed view; throws when
// the range is unmapped, or for a write outside a writable region.
uint8_t* ViewPointer(uint32_t address, size_t length, bool write);

inline const uint8_t* Pointer(uint32_t address, size_t length) {
    return view ? ViewPointer(address, length, false) : Memory::GetPointer(address, length);
}
inline uint8_t* Mutable(uint32_t address, size_t length) {
    return view ? ViewPointer(address, length, true) : Memory::GetPointer(address, length);
}
inline uint8_t Read8(uint32_t address) {
    return view ? *ViewPointer(address, 1, false) : Memory::Read8(address);
}
inline uint16_t Read16(uint32_t address) {
    if (!view)
        return Memory::Read16(address);
    const uint8_t* p = ViewPointer(address, 2, false);
    return uint16_t(p[0] << 8 | p[1]);
}
inline uint32_t Read32(uint32_t address) {
    if (!view)
        return Memory::Read32(address);
    const uint8_t* p = ViewPointer(address, 4, false);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline float ReadFloat32(uint32_t address) {
    if (!view)
        return Memory::ReadFloat32(address);
    const uint32_t bits = Read32(address);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
inline void Write8(uint32_t address, uint8_t value) {
    if (view)
        *ViewPointer(address, 1, true) = value;
    else
        Memory::Write8(address, value);
}
inline void Write32(uint32_t address, uint32_t value) {
    if (!view)
        return Memory::Write32(address, value);
    uint8_t* p = ViewPointer(address, 4, true);
    p[0] = uint8_t(value >> 24), p[1] = uint8_t(value >> 16), p[2] = uint8_t(value >> 8), p[3] = uint8_t(value);
}
inline void WriteFloat32(uint32_t address, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    Write32(address, bits);
}
// Installs a view for the current thread for one scope.
struct Scope {
    const mh3_res_memory* previous;
    explicit Scope(const mh3_res_memory* next) : previous(view) { view = next; }
    ~Scope() { view = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
} // namespace DrawMemory
