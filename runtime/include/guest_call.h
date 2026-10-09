// Calling guest functions from native ports: integer arguments go in r3..,
// floating-point ones in f1..; the result is r3 (F1() for f1).
#pragma once

#include "abi_bridge.h"
#include "memory.h"

#include <initializer_list>

struct GuestCall {
    CpuContext* cpu;

    uint32_t operator()(uint32_t fn, std::initializer_list<uint32_t> ints = {},
                        std::initializer_list<double> floats = {}) {
        uint32_t r = 3, f = 1;
        for (uint32_t v : ints) cpu->gpr[r++] = v;
        for (double v : floats) cpu->fpr[f++].d = v;
        InvokeIndirectCpu(fn, cpu);
        return cpu->gpr[3];
    }
    // Calls the virtual function at `slot` in `object`'s vtable (at offset 0).
    uint32_t Virtual(uint32_t object, uint32_t slot, std::initializer_list<uint32_t> ints = {},
                     std::initializer_list<double> floats = {}) {
        const uint32_t fn = Memory::Read32(Memory::Read32(object) + slot);
        uint32_t r = 3, f = 1;
        cpu->gpr[r++] = object;
        for (uint32_t v : ints) cpu->gpr[r++] = v;
        for (double v : floats) cpu->fpr[f++].d = v;
        InvokeIndirectCpu(fn, cpu);
        return cpu->gpr[3];
    }
    float F1() const { return float(cpu->fpr[1].d); }
};

// A native port's guest stack frame, as the original's: `size` bytes below the
// caller's stack pointer with the back chain stored. Restores the caller's
// registers when the port returns.
class GuestFrame {
public:
    GuestFrame(CpuContext* cpu, uint32_t size) : cpu_(cpu), saved_(*cpu) {
        sp_ = (saved_.gpr[1] - size) & ~0xFu;
        cpu->gpr[1] = sp_;
        Memory::Write32(sp_, saved_.gpr[1]);
    }
    ~GuestFrame() { *cpu_ = saved_; }
    uint32_t operator[](uint32_t offset) const { return sp_ + offset; }
    uint32_t Arg(int gpr) const { return saved_.gpr[gpr]; }

private:
    CpuContext* cpu_;
    CpuContext saved_;
    uint32_t sp_;
};
