// Object ages, RMHP08. ObjectAgesUpdate (0x80345188) runs each step over the
// ten 60-byte objects at 0x806C1520; for each active one (+0 set, +8 not 255,
// +22 not 1) ObjectAgesTick (0x80344EBC) adds 1 to its three s16 step counts:
// +0x34 and +0x1C while below 0x7FFF, +0x1E while in 0..0x7FFE (a negative
// count there stays as it is).
//
// Here they count the whole ticks of game time in the step, so they keep time
// at any step length; at the stock 1/30 s step (one tick) this is the
// original.
#include "game_step.h"
#include "memory.h"
#include "port_check.h"

#include <algorithm>

namespace {

constexpr int32_t kMaxAge = 0x7FFF;

void Age(uint32_t address) {
    const int32_t age = int16_t(Memory::Read16(address));
    if (age < kMaxAge) Memory::Write16(address, uint16_t(std::min(age + int32_t(GameStep::Ticks()), kMaxAge)));
}

void ObjectAgesTickPort(CpuContext* ctx) {
    const uint32_t object = ctx->gpr[3];
    Age(object + 0x34);
    Age(object + 0x1C);
    if (int16_t(Memory::Read16(object + 0x1E)) >= 0) Age(object + 0x1E);
}

} // namespace

extern "C" void func_80344EBC(CpuContext* ctx);
extern "C" void MH3_ObjectAgesTick(CpuContext* ctx) {
    PortCheck::Call(ctx, "ObjectAgesTick", func_80344EBC, ObjectAgesTickPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80344EBC, MH3_ObjectAgesTick, "ObjectAgesTick");
