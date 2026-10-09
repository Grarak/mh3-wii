#include "gx_host.h"
#include "gx_panic.h"
#include <array>

namespace {
std::array<uint16_t, 30> cp{};
std::array<uint16_t, 6> pe{};
std::array<uint32_t, 3> pi{};
bool Physical(uint32_t address, uint32_t& p) {
    if ((address >> 30) == 1)
        return false;
    p = address & 0x3FFFFFFF;
    return true;
}
bool CpOffset(uint32_t p) {
    return p == 0x0C000002 || p == 0x0C000004 || p == 0x0C000006 || (p >= 0x0C000020 && p <= 0x0C00003A && !(p & 1));
}
void RejectGxHardware(uint32_t address, const char* operation) {
    uint32_t physical;
    if (!Physical(address, physical))
        return;
    if ((physical >= 0x0C000000 && physical < 0x0C002000) || (physical >= 0x0C00300C && physical < 0x0C003018))
        PanicLegacyGx(operation, address);
}
} // namespace
void ResetGxRegisters() {
    cp.fill(0);
    pe.fill(0);
    pi.fill(0);
}
bool ReadGxRegister(uint32_t address, unsigned width, uint32_t& value) {
    RejectGxHardware(address, "GX CP/PE/FIFO register read");
    uint32_t p;
    if (!Physical(address, p))
        return false;
    if (width == 2 && CpOffset(p)) {
        value = cp[(p - 0x0C000000) / 2];
        return true;
    }
    if (width == 2 && p >= 0x0C001000 && p <= 0x0C00100A && !(p & 1)) {
        value = pe[(p - 0x0C001000) / 2];
        return true;
    }
    if (width == 4 && p >= 0x0C00300C && p <= 0x0C003014 && !(p & 3)) {
        value = pi[(p - 0x0C00300C) / 4];
        return true;
    }
    return false;
}
bool WriteGxRegister(uint32_t address, unsigned width, uint32_t value) {
    RejectGxHardware(address, "GX CP/PE/FIFO register write");
    uint32_t ignored, p;
    if (!ReadGxRegister(address, width, ignored) || !Physical(address, p))
        return false;
    if (p < 0x0C001000) {
        // CP register write masks follow Dolphin's CommandProcessor MMIO map.
        // https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/CommandProcessor.cpp
        if (p == 0x0C000002)
            value &= 0x3F;
        else if (p == 0x0C000004)
            value = 0; // write-only interrupt acknowledgement
        else if (p == 0x0C000006)
            value &= 7;
        else
            value &= (p & 2) ? 0x1FFF : 0xFFE0;
        cp[(p - 0x0C000000) / 2] = value;
    } else if (p < 0x0C002000)
        pe[(p - 0x0C001000) / 2] = value;
    else
        pi[(p - 0x0C00300C) / 4] = value;
    return true;
}
