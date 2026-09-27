#include "video_interface.h"
#include "abi_bridge.h"
#include <array>
#include <chrono>

namespace {
std::array<uint16_t, 60> registers{};
std::array<uint8_t, 256> encoder{};
bool retraceActive = false, secondField = false;
uint16_t retraceLine = 1;
const auto epoch = std::chrono::steady_clock::now();
bool Offset(uint32_t address, uint32_t& offset) {
    // Only physical, cached and uncached aliases of the VI register block.
    if ((address >> 30) == 1) return false;
    const uint32_t physical = address & 0x3FFFFFFF;
    if (physical < 0x0C002000 || physical >= 0x0C002078 || (physical & 1)) return false;
    offset = physical - 0x0C002000;
    return true;
}
}
void ResetVideoInterface() {
    registers.fill(0);
    encoder.fill(0);
    retraceActive = secondField = false;
    // PAL interlaced scanout established by the host boot environment. VIInit
    // reads the active mode to construct its timing and framebuffer state.
    registers[1] = 0x0101;
}
bool ReadVideoRegister16(uint32_t address, uint16_t& value) {
    uint32_t offset;
    if (!Offset(address, offset)) return false;
    if (retraceActive && (offset == 0x2C || offset == 0x2E)) {
        value = offset == 0x2C ? retraceLine : 1;
        return true;
    }
    if (offset == 0x2C || offset == 0x2E) {
        const bool pal = ((registers[1] >> 8) & 3) == 1;
        const uint64_t period = pal ? 20000000 : 16683333;
        const uint64_t lines = pal ? 625 : 525;
        const uint64_t samples = pal ? 864 : 858;
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - epoch).count();
        const uint64_t position = (uint64_t(elapsed) % period) * lines * samples / period;
        value = offset == 0x2C ? position / samples + 1 : position % samples + 1;
    } else value = registers[offset / 2];
    return true;
}
bool WriteVideoRegister16(uint32_t address, uint16_t value) {
    uint32_t offset;
    if (!Offset(address, offset)) return false;
    if (offset != 0x2C && offset != 0x2E) registers[offset / 2] = value;
    return true;
}

// VI's I2C send routine. Preserve encoder register writes; there is no analog
// DAC on the host. Other bus devices are explicitly reported absent.
extern "C" void MH3_VIEncoderSend(CpuContext* ctx) {
    const uint32_t data = ctx->gpr[4], length = ctx->gpr[5];
    if (ctx->gpr[3] != 0xE0 || length < 2 || !Memory::Contains(data, length)) {
        ctx->gpr[3] = 0; return;
    }
    const uint32_t index = Memory::Read8(data);
    if (length - 1 > encoder.size() - index) { ctx->gpr[3] = 0; return; }
    for (uint32_t i = 1; i < length; ++i) encoder[index + i - 1] = Memory::Read8(data + i);
    Memory::Write32(0x80797B48, 1); // r13 - 0x3798, I2C initialized
    Memory::Write32(0x80796648, 1); // r13 - 0x4C98, bus polarity
    ctx->gpr[3] = 1;
}
REGISTER_NATIVE_FUNCTION(0x804E9C20, MH3_VIEncoderSend);

uint64_t VideoRetracePeriodNs() {
    return ((registers[1] >> 8) & 3) == 1 ? 20000000 : 16683333;
}
void BeginVideoRetrace() {
    const bool pal = ((registers[1] >> 8) & 3) == 1;
    const bool interlaced = !(registers[1] & 4);
    retraceLine = interlaced && secondField ? (pal ? 313 : 263) : 1;
    registers[(secondField && interlaced ? 0x34 : 0x30) / 2] |= 0x8000;
    retraceActive = true;
    secondField = !secondField;
}
void EndVideoRetrace() { retraceActive = false; }
