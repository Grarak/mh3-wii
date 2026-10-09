// MSL string routines, RMHP08, on host pointers into guest memory.
//
//   0x8045FD04 strcmp   r3 = the difference of the first differing bytes
//                       (unsigned), 0 for equal strings
//
// The original compares a word at a time when both strings share their
// alignment, then finds the differing byte one at a time; the byte loop gives
// the same value. MH3_PORT_CHECK=strcmp checks every call.
#include "abi_bridge.h"
#include "guest_flat_memory.h"
#include "port_check.h"

#include <cstdint>

extern "C" void func_8045FD04(CpuContext* ctx);

namespace {

uint32_t Strcmp(uint32_t a, uint32_t b) {
    const uint8_t* p = GuestFlat::Host(a);
    const uint8_t* q = GuestFlat::Host(b);

    for (;; ++p, ++q) {
        const uint32_t x = *p, y = *q;

        if (x != y)
            return x - y;

        if (!x)
            return 0;
    }
}

void StrcmpPort(CpuContext* ctx) {
    ctx->gpr[3] = Strcmp(ctx->gpr[3], ctx->gpr[4]);
}

uint32_t StrcmpChecked(uint32_t r1, uint32_t r3, uint32_t r4) {
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("strcmp");

    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        PortCheck::RunPure(c, "strcmp", PortCheck::Returns::Gpr3, func_8045FD04, StrcmpPort);
        return c->gpr[3];
    }
#else
    (void)r1;
#endif
    return Strcmp(r3, r4);
}

} // namespace

extern "C" void MH3_Strcmp(CpuContext* ctx) {
    ctx->gpr[3] = StrcmpChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8045FD04, MH3_Strcmp, "strcmp");
