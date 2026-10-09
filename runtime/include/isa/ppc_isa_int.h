#pragma once
// Pure integer PowerPC semantics, together with the declarations of the
// integer, condition/SPR and system helpers the host runtime implements out of
// line. Nothing here depends on another isa/ header beyond the configuration.

#include "ppc_isa_config.h"

#include <cstdint>
#include <cstdlib>
#include <limits>

inline uint32_t PpcRotl32Inline(uint32_t value, uint32_t shift)
{
    return __builtin_rotateleft32(value, shift);
}

extern "C" uint32_t OSSystemCall();
extern "C" int32_t memset_zero_32(int32_t address);
extern "C" void OS_HLE_ProcessAlarms(int maxToProcess);

extern "C" uint32_t PPC_Mcrxr(uint32_t crField);
extern "C" uint32_t PPC_ReadSpr(uint32_t spr);
extern "C" void PPC_WriteSpr(uint32_t spr, uint32_t value);
extern "C" uint32_t PPC_CrSetBit(uint32_t bitIndex, uint32_t value);
extern "C" uint32_t PPC_CrLogical(uint32_t op, uint32_t bt, uint32_t ba, uint32_t bb);
extern "C" uint32_t PPC_Mcrf(uint32_t dstField, uint32_t srcField);

// Time base helpers (PowerPC 'mftb' / 'mftbu') - declared so generated code can call them.
extern "C" uint32_t PPC_Mftb();
extern "C" uint32_t PPC_Mftbu();

// Carry helpers used by translated arithmetic that depends on XER[CA].
extern "C" uint32_t PPC_UpdateCarryAdd(uint32_t lhs, uint32_t rhs, uint32_t carryIn);
extern "C" uint32_t PPC_UpdateCarrySub(uint32_t lhs, uint32_t rhs);
extern "C" uint32_t PPC_UpdateCarryShiftRight(uint32_t value, uint32_t shift);
extern "C" uint32_t PPC_GetCarry();
extern "C" uint32_t PPC_Addo(uint32_t lhs, uint32_t rhs);
extern "C" uint32_t PPC_Addco(uint32_t lhs, uint32_t rhs);
extern "C" uint32_t PPC_Addeo(uint32_t lhs, uint32_t rhs);
extern "C" uint32_t PPC_Addmeo(uint32_t value);
extern "C" uint32_t PPC_Addzeo(uint32_t value);
extern "C" uint32_t PPC_Subfo(uint32_t subtrahend, uint32_t minuend);
extern "C" uint32_t PPC_Subfco(uint32_t subtrahend, uint32_t minuend);
extern "C" uint32_t PPC_Subfeo(uint32_t subtrahend, uint32_t minuend);
extern "C" uint32_t PPC_Subfmeo(uint32_t value);
extern "C" uint32_t PPC_Subfzeo(uint32_t value);
extern "C" uint32_t PPC_Nego(uint32_t value);
extern "C" uint32_t PPC_Mullwo(uint32_t lhs, uint32_t rhs);
extern "C" uint32_t PPC_Divwo(uint32_t lhs, uint32_t rhs);
extern "C" uint32_t PPC_Divwuo(uint32_t lhs, uint32_t rhs);
extern "C" void PPC_Lswi(uint32_t rD, uint32_t addr, uint32_t byteCount);
extern "C" void PPC_Lswx(uint32_t rD, uint32_t addr);
extern "C" void PPC_Stswi(uint32_t rS, uint32_t addr, uint32_t byteCount);
extern "C" void PPC_Stswx(uint32_t rS, uint32_t addr);
extern "C" uint32_t PPC_Lwarx(uint32_t addr);
extern "C" uint32_t PPC_Stwcx(uint32_t addr, uint32_t value);
extern "C" uint32_t PPC_Mcrfs(uint32_t dstField, uint32_t srcField);
extern "C" uint32_t PPC_Eciwx(uint32_t addr);
extern "C" void PPC_Ecowx(uint32_t addr, uint32_t value);
extern "C" void PPC_TrapWord(uint32_t trapOptions, uint32_t lhs, uint32_t rhs);
extern "C" uint32_t PPC_Cntlzw(uint32_t value);

MKW_PPC_FORCE_INLINE uint32_t PPC_CntlzwInline(uint32_t value)
{
    return value == 0 ? 32u : static_cast<uint32_t>(__builtin_clz(value));
}

// Byte-reverse helpers (PowerPC 'lwbrx' / 'stwbrx' / 'lhbrx' / 'sthbrx')
extern "C" uint32_t PPC_LoadWordByteReverse(uint32_t addr);
extern "C" void PPC_StoreWordByteReverse(uint32_t addr, uint32_t value);
extern "C" uint32_t PPC_LoadHalfwordByteReverse(uint32_t addr);
extern "C" void PPC_StoreHalfwordByteReverse(uint32_t addr, uint32_t value);

template <typename T>
inline int32_t CompareUnsigned(T a, T b) {
    uint32_t ua = static_cast<uint32_t>(a);
    uint32_t ub = static_cast<uint32_t>(b);
    if (ua < ub) return -1;
    if (ua > ub) return 1;
    return 0;
}

template <int Bits>
inline int32_t SignExtend(uint32_t val) {
    struct { int32_t x : Bits; } s;
    s.x = val;
    return s.x;
}

inline int32_t ArithmeticShiftRight(int32_t val, int amount) {
    return val >> amount;
}

inline uint32_t PPC_Slw(uint32_t value, uint32_t amount)
{
    return (amount & 0x20u) != 0 ? 0u : value << (amount & 0x1Fu);
}

inline uint32_t PPC_Srw(uint32_t value, uint32_t amount)
{
    return (amount & 0x20u) != 0 ? 0u : value >> (amount & 0x1Fu);
}

inline uint32_t PPC_Sraw(uint32_t value, uint32_t amount)
{
    if ((amount & 0x20u) != 0)
    {
        return (value & 0x80000000u) != 0 ? 0xFFFFFFFFu : 0u;
    }

    return static_cast<uint32_t>(static_cast<int32_t>(value) >> (amount & 0x1Fu));
}

inline uint32_t PPC_Divwu(uint32_t dividend, uint32_t divisor)
{
    // Gekko does not raise a program exception for non-OE division by zero.
    // Match the hardware result used by Dolphin's interpreter/JIT.
    return divisor == 0 ? 0u : dividend / divisor;
}

inline int32_t PPC_Divw(int32_t dividend, int32_t divisor)
{
    if (divisor == 0 || (dividend == std::numeric_limits<int32_t>::min() && divisor == -1))
        return dividend < 0 ? -1 : 0;

    return dividend / divisor;
}
