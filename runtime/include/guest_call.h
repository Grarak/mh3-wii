// Calling guest functions from native ports: integer arguments go in r3..,
// floating-point ones in f1..; the result is r3 (F1() for f1).
#pragma once

#include "abi_bridge.h"
#include "memory.h"

#include <initializer_list>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

struct GuestCall {
    CpuContext* cpu;

    uint32_t operator()(uint32_t fn, std::initializer_list<uint32_t> ints = {}, std::initializer_list<double> floats = {}) {
        uint32_t r = 3, f = 1;

        for (uint32_t v : ints)
            cpu->gpr[r++] = v;

        for (double v : floats)
            cpu->fpr[f++].d = v;

        InvokeIndirectCpu(fn, cpu);
        return cpu->gpr[3];
    }

    // Calls the virtual function at `slot` in `object`'s vtable (at offset 0).
    uint32_t Virtual(uint32_t object, uint32_t slot, std::initializer_list<uint32_t> ints = {}, std::initializer_list<double> floats = {}) {
        const uint32_t fn = Memory::Read32(Memory::Read32(object) + slot);
        uint32_t r = 3, f = 1;
        cpu->gpr[r++] = object;

        for (uint32_t v : ints)
            cpu->gpr[r++] = v;

        for (double v : floats)
            cpu->fpr[f++].d = v;

        InvokeIndirectCpu(fn, cpu);
        return cpu->gpr[3];
    }

    // operator() for a constant target: the first call after the registry is
    // published binds the target's entry point once, so later calls skip the
    // dispatch lookup and the translated-address bookkeeping. Targets that may
    // change the nonvolatile FPRs keep the guarded dispatch.
    template <uint32_t Target>
    [[gnu::always_inline]] uint32_t Call(std::initializer_list<uint32_t> ints = {}, std::initializer_list<double> floats = {}) {
        uint32_t r = 3, f = 1;

        for (uint32_t v : ints)
            cpu->gpr[r++] = v;

        for (double v : floats)
            cpu->fpr[f++].d = v;

        static void (*bound)(CpuContext*) = nullptr;

        if (bound) [[likely]]
            bound(cpu);
        else
            bound = Bind(Target, cpu, &Dispatch<Target>);

        return cpu->gpr[3];
    }

    // Virtual() for a constant slot: remembers the last function the slot
    // resolved to, so a loop over objects of one class binds it once.
    template <uint32_t Slot>
    [[gnu::always_inline]] uint32_t Virtual(uint32_t object, std::initializer_list<uint32_t> ints = {}, std::initializer_list<double> floats = {}) {
        const uint32_t fn = Memory::Read32(Memory::Read32(object) + Slot);
        uint32_t r = 3, f = 1;
        cpu->gpr[r++] = object;

        for (uint32_t v : ints)
            cpu->gpr[r++] = v;

        for (double v : floats)
            cpu->fpr[f++].d = v;

        static const RawDispatchRecord* memo = nullptr;

        if (const RawDispatchRecord* record = memo; record && record->address == fn) [[likely]]
            record->entry(cpu);
        else
            Rebind(fn, cpu, memo);

        return cpu->gpr[3];
    }

    // Virtual() and operator() with the call site's own memo (a static the
    // port declares): sites whose objects' classes differ from those of the
    // other Virtual<Slot> sites of the same slot would evict their memo on
    // every call.
    [[gnu::always_inline]] uint32_t VirtualAt(const RawDispatchRecord*& memo, uint32_t object, uint32_t slot, std::initializer_list<uint32_t> ints = {},
                                              std::initializer_list<double> floats = {}) {
        const uint32_t fn = Memory::Read32(Memory::Read32(object) + slot);
        uint32_t r = 3, f = 1;
        cpu->gpr[r++] = object;

        for (uint32_t v : ints)
            cpu->gpr[r++] = v;

        for (double v : floats)
            cpu->fpr[f++].d = v;

        if (const RawDispatchRecord* record = memo; record && record->address == fn) [[likely]]
            record->entry(cpu);
        else
            Rebind(fn, cpu, memo);

        return cpu->gpr[3];
    }

    [[gnu::always_inline]] uint32_t CallAt(const RawDispatchRecord*& memo, uint32_t fn, std::initializer_list<uint32_t> ints = {}, std::initializer_list<double> floats = {}) {
        uint32_t r = 3, f = 1;

        for (uint32_t v : ints)
            cpu->gpr[r++] = v;

        for (double v : floats)
            cpu->fpr[f++].d = v;

        if (const RawDispatchRecord* record = memo; record && record->address == fn) [[likely]]
            record->entry(cpu);
        else
            Rebind(fn, cpu, memo);

        return cpu->gpr[3];
    }

    float F1() const { return float(cpu->fpr[1].d); }

  private:
    // Runs `fn` through the generic dispatch and remembers its record when
    // later calls may enter it directly. The memo is one pointer, checked
    // against the record's address, so the game thread and the guest worker
    // can share a call site without entering each other's function.
    [[gnu::noinline]] static void Rebind(uint32_t fn, CpuContext* cpu, const RawDispatchRecord*& memo) {
        if (TranslatedFunctionRegistry::IsLookupPublished() && fn < 0x80800000u && PpcThread::IsCurrent(cpu)) {
            const RawDispatchRecord* record = TranslatedFunctionRegistry::FindRawByAddressPtr(fn);

            if (record && record->entry && (record->nonvolatileFprWriteMask & kPpcAllNonvolatileFprMask) == 0)
                memo = record;
        }

        InvokeIndirectCpu(fn, cpu);
    }

    template <uint32_t Target>
    static void Dispatch(CpuContext* cpu) {
        InvokeIndirectCpu(Target, cpu);
    }

    // Runs `target` through the generic dispatch and returns what later calls
    // enter: its entry point when that is safe, else `dispatch` (the generic
    // path); null before the registry is published. Overlay code can unload,
    // so overlay targets always dispatch.
    [[gnu::noinline]] static void (*Bind(uint32_t target, CpuContext* cpu, void (*dispatch)(CpuContext*)))(CpuContext*) {
        void (*entry)(CpuContext*) = nullptr;

        if (TranslatedFunctionRegistry::IsLookupPublished()) {
            entry = dispatch;
            const RawDispatchRecord* record = TranslatedFunctionRegistry::FindRawByAddressPtr(target);

            if (target < 0x80800000u && PpcThread::IsCurrent(cpu) && record && record->entry && (record->nonvolatileFprWriteMask & kPpcAllNonvolatileFprMask) == 0)
                entry = record->entry;
        }

        InvokeIndirectCpu(target, cpu);
        return entry;
    }
};

// A native port's guest stack frame, as the original's: `size` bytes below the
// caller's stack pointer with the back chain stored. Restores the caller's
// GPRs and CR when the port returns. Everything else the port leaves as the
// original would: its guest calls keep the nonvolatile FPRs (the dispatch
// guards them) and the rest is volatile. The copies are inline: a whole
// CpuContext each way was two memcpy calls per port call on the Cortex-A9.
class GuestFrame {
  public:
    GuestFrame(CpuContext* cpu, uint32_t size) : cpu_(cpu), cr_(cpu->cr) {
        Copy(gpr_, cpu->gpr);
        sp_ = (gpr_[1] - size) & ~0xFu;
        cpu->gpr[1] = sp_;
        Memory::Write32(sp_, gpr_[1]);
    }

    ~GuestFrame() {
        Copy(cpu_->gpr, gpr_);
        cpu_->cr = cr_;
    }

    GuestFrame(const GuestFrame&) = delete;
    GuestFrame& operator=(const GuestFrame&) = delete;
    uint32_t operator[](uint32_t offset) const { return sp_ + offset; }
    uint32_t Arg(int gpr) const { return gpr_[gpr]; }

  private:
    // NEON: the compiler turns a plain copy (even __builtin_memcpy_inline,
    // once it splits the array) back into a memcpy call.
    static void Copy(uint32_t (&to)[32], const uint32_t (&from)[32]) {
#if defined(__ARM_NEON)
        uint32x4_t v[8];

        for (int i = 0; i < 8; ++i)
            v[i] = vld1q_u32(from + 4 * i);

        for (int i = 0; i < 8; ++i)
            vst1q_u32(to + 4 * i, v[i]);
#else
        for (int i = 0; i < 32; ++i)
            to[i] = from[i];
#endif
    }

    CpuContext* cpu_;
    uint32_t gpr_[32];
    uint32_t cr_;
    uint32_t sp_;
};
