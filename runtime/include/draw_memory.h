// Guest memory as the model draw path sees it. On the game thread this is live
// guest memory. A model instance drawn away from the game thread installs a
// view (mh3_res_memory with regions): owned captured bytes overlay the guest
// windows, and writes land only inside writable regions, never in live memory.
#pragma once
// Include after memory.h: native tests substitute their own Memory.
#include "runtime/native/mh3_model_runtime.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace DrawMemory {
namespace detail {
// Never reset: each thread enables the gate before installing its own view.
extern std::atomic<bool> viewsEverEnabled;
// Keep the TLS expression out of inline callers: LLVM can otherwise hoist
// emulated-TLS address resolution above the gate when several reads share it.
__attribute__((noinline)) const mh3_res_memory* EnabledView();
}
// The default live-memory path does not access emulated TLS. After the first
// view is installed, every thread consults its independent installed view.
inline const mh3_res_memory* CurrentView() {
    if (!detail::viewsEverEnabled.load(std::memory_order_relaxed))
        return nullptr;
    return detail::EnabledView();
}
// All view writes go through this function so the gate cannot miss a view.
void InstallView(const mh3_res_memory* next);
// A recording view over live memory: writes outside its regions go to live
// memory instead of failing.
extern thread_local bool liveWrites;
// Host bytes of [address, address + length) in the installed view; throws when
// the range is unmapped, or for a write outside a writable region.
uint8_t* ViewPointer(uint32_t address, size_t length, bool write);

inline const uint8_t* Pointer(uint32_t address, size_t length) {
    return CurrentView() ? ViewPointer(address, length, false) : Memory::GetPointer(address, length);
}
inline uint8_t* Mutable(uint32_t address, size_t length) {
    return CurrentView() ? ViewPointer(address, length, true) : Memory::GetPointer(address, length);
}
inline uint8_t Read8(uint32_t address) {
    return CurrentView() ? *ViewPointer(address, 1, false) : Memory::Read8(address);
}
inline uint16_t Read16(uint32_t address) {
    if (!CurrentView())
        return Memory::Read16(address);
    const uint8_t* p = ViewPointer(address, 2, false);
    return uint16_t(p[0] << 8 | p[1]);
}
inline uint32_t Read32(uint32_t address) {
    if (!CurrentView())
        return Memory::Read32(address);
    const uint8_t* p = ViewPointer(address, 4, false);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline float ReadFloat32(uint32_t address) {
    if (!CurrentView())
        return Memory::ReadFloat32(address);
    const uint32_t bits = Read32(address);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
inline void Write8(uint32_t address, uint8_t value) {
    if (CurrentView())
        *ViewPointer(address, 1, true) = value;
    else
        Memory::Write8(address, value);
}
inline void Write32(uint32_t address, uint32_t value) {
    if (!CurrentView())
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
    explicit Scope(const mh3_res_memory* next) : previous(CurrentView()) { InstallView(next); }
    ~Scope() { InstallView(previous); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
} // namespace DrawMemory
