// Pl_Skill_ck (0x80271374, plw, u16 skill), RMHP08: 1 when the hunter has the
// skill. C reconstruction from the disassembly. The hunter's eight active
// skills are u16s at plw +0x5F2, or at +0x61A in the online modes that use
// the online set (online mode 2 with the lobby state 6, or 15 while the
// lobby's session byte is 7 or 8: 0x80363A90). The cheat wrapper registered
// over it (cheat_movement.cpp) calls this port.
#include "abi_bridge.h"
#include "memory.h"
#include "port_check.h"

#include <cstdint>

namespace {

constexpr uint32_t kOnlineMode = 0x8065AA84u; // u8, what GetOnlineMode (0x800CF26C) returns
constexpr uint32_t kLobby = 0x806ACFC4u;      // lobby_w: u8 state, +0xAC the session

bool OnlineSkillSet() {
    if (MemoryInline::FlatRead8(kOnlineMode) != 2)
        return false;

    const uint32_t state = MemoryInline::FlatRead8(kLobby);

    if (state == 6)
        return true;

    if (state != 15)
        return false;

    const uint32_t session = MemoryInline::FlatRead8(MemoryInline::FlatRead32(kLobby + 0xAC));
    return session == 7 || session == 8;
}

uint32_t HasSkill(uint32_t plw, uint32_t skill) {
    const uint32_t skills = plw + (OnlineSkillSet() ? 0x61A : 0x5F2);

    for (uint32_t i = 0; i < 8; ++i)
        if (MemoryInline::FlatRead16(skills + 2 * i) == (skill & 0xFFFF))
            return 1;

    return 0;
}

void HasSkillPort(CpuContext* p) {
    p->gpr[3] = HasSkill(p->gpr[3], p->gpr[4]);
}

} // namespace

extern "C" void func_80271374(CpuContext* ctx);

// The result for r3; with MH3_PORT_CHECK naming PlSkillCheck, checked
// against the original with RunPure.
uint32_t PlSkillCheck([[maybe_unused]] uint32_t r1, uint32_t plw, uint32_t skill) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("PlSkillCheck")) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = plw;
        c->gpr[4] = skill;
        PortCheck::RunPure(c, "PlSkillCheck", PortCheck::Returns::Gpr3, func_80271374, HasSkillPort);
        return c->gpr[3];
    }
#endif
    return HasSkill(plw, skill);
}
