#include "abi_bridge.h"
#include "recomp_mod_loader.h"
#include <cstdio>
#include <string_view>

extern "C" void func_80292524(CpuContext*);
extern "C" void func_80296DC8(CpuContext*);

namespace {
void WriteHex(uint32_t address, std::string_view hex) {
    const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'A' + 10; };
    for (size_t i = 0; i < hex.size(); i += 2)
        Memory::Write8(address + i / 2, (digit(hex[i]) << 4) | digit(hex[i + 1]));
}
}

int RunCollisionCheck(CpuContext* cpu) {
    // Captured from the first divergent slope sweep. Dolphin receives the same
    // bytes and returns no candidate. No game loop, renderer, or input timing.
    constexpr uint32_t sweep = 0x81000000, triangle = 0x81000100, position = 0x81000200;
    WriteHex(sweep, "4453C75C43BA37BBC3AEBDC244501D9443BA37BBC3AEBDC242400000416A720000010001");
    WriteHex(triangle, "0020000000000000444D229EC2DD3DCCC369DDCF4440736FC2DD3DCCC31F0301444D229E448CCABFC369DDCFBF53E8A5236DCFA9BF0FA24300000000");
    WriteHex(position, "4453C75C439C37BBC3AEBDC2");
    cpu->gpr[1] = 0x817FF000;
    cpu->gpr[2] = 0x8079FF60;
    cpu->gpr[13] = 0x8079B2E0;
    cpu->gqr[0] = 0;
    RecompMod::RegisterExecutableRange(0x8003F260, 0x8056FAA0, ".text");
    TranslatedFunctionRegistry::Finalize();
    Memory::Write8(0x80797018, 0);
    cpu->gpr[3] = sweep;
    cpu->gpr[4] = triangle;
    cpu->gpr[5] = position;
    func_80292524(cpu);
    const uint32_t accepted = cpu->gpr[3];
    const uint32_t projected = cpu->gpr[1] - 56;
    std::printf("candidate=%u (Dolphin=0), projected=%08x %08x %08x\n", accepted,
                Memory::Read32(projected), Memory::Read32(projected + 4), Memory::Read32(projected + 8));
    WriteHex(position, "445CA69243BA37BBC3A2B6DD");
    cpu->gpr[3] = position;
    cpu->gpr[4] = triangle + 8;
    cpu->gpr[5] = triangle + 44;
    func_80296DC8(cpu);
    std::printf("point-in-triangle=%u (Dolphin=0)\n", cpu->gpr[3]);
    return accepted != 0 || cpu->gpr[3] != 0;
}
