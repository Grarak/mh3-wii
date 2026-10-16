// NCD, the Wii's network configuration library (RMHP08 0x8051CBD4..0x8051D8C8;
// decompiled with Ghidra, the call arguments and branches checked in the
// translated C++), ported natively. Its calls lock the library's mutex, send
// an ioctlv to /dev/net/ncd/manage (ios_net.cpp) through a 0x1B60-byte work
// area taken from the IPC heap at first use, and unlock. The socket library
// asks it for the link status before its first call (wii_so.cpp); the game,
// at online startup, for the configuration and the error code to show when
// SOStartup fails. The guest's memcpy and memset stay guest calls.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"

namespace {

enum : uint32_t {
    Memcpy = 0x80004000,
    Memset = 0x80004350,
    IOSOpen = 0x804BC500,
    IOSClose = 0x804BC810,
    IOSIoctlv = 0x804BD310,
    OSRegisterVersion = 0x804CBA00,
    OSReport = 0x804CDCA0,
    OSPanic = 0x804CDD30,
    OSInitMutex = 0x804D2560,
    OSLockMutex = 0x804D25A0,
    OSUnlockMutex = 0x804D2680,
};

enum : uint32_t {
    kFlags = 0x80797D50,     // bit 0: initialized
    kWork = 0x80797D54,      // the work area (0x1B60 bytes from the IPC heap)
    kVersion = 0x807968F0,   // the library's version string
    kIpcHeapLo = 0x807976EC, // IPCGetBufferLo / IPCSetBufferLo
    kIpcHeapHi = 0x807976F0, // IPCGetBufferHi
    kMutex = 0x80768DE0,
    kResult = 0x80768E00,  // the device's answer: result, configuration index
    kVectors = 0x80768E20, // the ioctlv's vectors
};

enum : uint32_t {
    kDevice = 0x80631918,       // "/dev/net/ncd/manage"
    kIfConfigName = 0x806318E8, // "NCDGetCurrentIfConfig"
    kIpConfigName = 0x80631900, // "NCDGetCurrentIpConfig"
    kEnabledName = 0x8063192C,  // "NCDiGetEnabledConfigList"
    kPanicFile = 0x80631948,    // "ncdsystem.c"
    kPanicText = 0x80631954,
    kUnknownError = 0x80631990, // "Unknown SOStartup Error: %d"
};

constexpr uint32_t kCurrentThread = 0x800000E4;
constexpr uint32_t kMsrEe = 0x8000;
constexpr uint32_t kCrFloatArgs = 1u << (31 - 6);
constexpr uint32_t kWorkBytes = 0x1B60;
constexpr uint32_t kConfigBytes = 0x1B5C; // what the device reads and writes
constexpr uint32_t kSlotBytes = 0x91C;    // one of three configurations

// The device's commands and the library's errors.
enum : uint32_t {
    kGetConfig = 3,
    kGetLinkStatus = 7,
    kGetMacAddress = 8,
};

constexpr int32_t kNcdEInval = -3;
constexpr int32_t kNcdENoThread = -5;
constexpr int32_t kNcdEBadSlot = -7;
constexpr int32_t kNcdENoDevice = -8;
constexpr int32_t kNcdEFatal = -2;
constexpr int32_t kNcdEClose = -1;

// 0x8051D360: initializes the library at first use (its work area from the
// IPC heap, which must have room), then locks its mutex.
void Lock(CpuContext* cpu) {
    GuestCall call{cpu};
    const uint32_t msr = cpu->msr;
    cpu->msr &= ~kMsrEe;

    if (!(Memory::Read32(kFlags) & 1)) {
        call.Call<OSRegisterVersion>({Memory::Read32(kVersion)});
        call.Call<OSInitMutex>({kMutex});
        const uint32_t work = (Memory::Read32(kIpcHeapLo) + 0x1F) & ~0x1Fu;

        if (Memory::Read32(kIpcHeapHi) - work < kWorkBytes) {
            cpu->cr &= ~kCrFloatArgs;
            call.Call<OSPanic>({kPanicFile, 1500, kPanicText});
        }

        Memory::Write32(kIpcHeapLo, work + kWorkBytes);
        Memory::Write32(kWork, work);
        call.Call<Memset>({work, 0, kWorkBytes});
        call.Call<Memset>({kResult, 0, 0x20});
        call.Call<Memset>({kVectors, 0, 0x20});
        Memory::Write32(kFlags, Memory::Read32(kFlags) | 1);
    }

    cpu->msr = (msr & kMsrEe) ? cpu->msr | kMsrEe : cpu->msr & ~kMsrEe;
    call.Call<OSLockMutex>({kMutex});
}

void Unlock(CpuContext* cpu) {
    GuestCall{cpu}.Call<OSUnlockMutex>({kMutex});
}

int32_t OpenError(int32_t fd) {
    return fd == -6 ? kNcdENoDevice : kNcdEFatal;
}

// 0x8051D1CC: the configuration through the work area: commands 3 and 5 read
// it (into `data` when given), 4 and 6 write it (from `data`); others do
// nothing. The name is the caller's, for nothing.
int32_t Request(CpuContext* cpu, uint32_t data, int32_t command) {
    if (!Memory::Read32(kCurrentThread))
        return kNcdENoThread;

    Lock(cpu);
    GuestCall call{cpu};
    const int32_t fd = int32_t(call.Call<IOSOpen>({kDevice, 0}));
    int32_t result = 0;

    if (fd < 0) {
        result = OpenError(fd);
    } else {
        const uint32_t work = Memory::Read32(kWork);
        Memory::Write32(kVectors, work);
        Memory::Write32(kVectors + 4, kConfigBytes);
        Memory::Write32(kVectors + 8, kResult);
        Memory::Write32(kVectors + 12, 0x20);

        if (command == 3 || command == 5) {
            const int32_t sent = int32_t(call.Call<IOSIoctlv>({uint32_t(fd), uint32_t(command), 0, 2, kVectors}));
            result = int32_t(Memory::Read32(kResult));

            if (sent < 0)
                result = kNcdEFatal;
            else if (result == 0 && data)
                call.Call<Memcpy>({data, Memory::Read32(kWork), kConfigBytes});
        } else if (command == 4 || command == 6) {
            if (data)
                call.Call<Memcpy>({Memory::Read32(kWork), data, kConfigBytes});

            const int32_t sent = int32_t(call.Call<IOSIoctlv>({uint32_t(fd), uint32_t(command), 1, 1, kVectors}));
            result = int32_t(Memory::Read32(kResult));

            if (sent < 0)
                result = kNcdEFatal;
        }

        if (int32_t(call.Call<IOSClose>({uint32_t(fd)})) < 0)
            result = kNcdEClose;
    }

    Unlock(cpu);
    return result;
}

// The current configuration's slot, or none for an index outside 0..2.
bool CurrentSlot(uint32_t& slot) {
    const int32_t index = int32_t(Memory::Read32(kResult + 4));

    if (index < 0 || index > 2)
        return false;

    slot = Memory::Read32(kWork) + uint32_t(index) * kSlotBytes;
    return true;
}

// 0x8051CBD4: NCDGetCurrentIfConfig(config): the interface (1 wireless, with
// its 0x15C bytes of settings; 2 wired, 4 bytes; flag bit 0 is wired) and the
// work area's byte 6.
int32_t GetCurrentIfConfig(CpuContext* cpu, uint32_t config) {
    if (!config)
        return kNcdEInval;

    Lock(cpu);
    int32_t result = Request(cpu, 0, kGetConfig);
    const uint32_t work = Memory::Read32(kWork);

    if (result == 0) {
        uint32_t slot;

        if (!CurrentSlot(slot)) {
            result = kNcdEBadSlot;
        } else {
            GuestCall call{cpu};
            const bool wired = Memory::Read8(slot + 8) & 1;
            Memory::Write8(config, wired ? 2 : 1);
            call.Call<Memcpy>({config + 2, slot + 0x7C8, wired ? 4u : 0x15Cu});
            Memory::Write8(config + 1, Memory::Read8(work + 6));
        }
    }

    Unlock(cpu);
    return result;
}

// 0x8051CCCC: NCDGetCurrentIpConfig(config): the addresses (12 bytes at
// +0x1C), whether they come by DHCP (+0) with 20 bytes at +8, and the proxy
// (+4, 0x79C bytes at +0x28, zeros without one).
int32_t GetCurrentIpConfig(CpuContext* cpu, uint32_t config) {
    if (!config)
        return kNcdEInval;

    Lock(cpu);
    int32_t result = Request(cpu, 0, kGetConfig);

    if (result == 0) {
        uint32_t slot;

        if (!CurrentSlot(slot)) {
            result = kNcdEBadSlot;
        } else {
            GuestCall call{cpu};
            call.Call<Memcpy>({config + 0x1C, slot + 0x20, 0xC});
            Memory::Write32(config, (Memory::Read8(slot + 8) & 6) ? 1 : 0);
            call.Call<Memcpy>({config + 8, slot + 0xC, 0x14});

            if (Memory::Read8(slot + 8) & 0x10) {
                Memory::Write32(config + 4, 1);
                call.Call<Memcpy>({config + 0x28, slot + 0x2C, 0x79C});
            } else {
                Memory::Write32(config + 4, 0);
                call.Call<Memset>({config + 0x28, 0, 0x79C});
            }
        }
    }

    Unlock(cpu);
    return result;
}

// 0x8051CE24: NCDGetLinkStatus: the device's status, or its second word when
// that is 0 (-1 for a negative one).
int32_t GetLinkStatus(CpuContext* cpu) {
    if (!Memory::Read32(kCurrentThread))
        return kNcdENoThread;

    Lock(cpu);
    GuestCall call{cpu};
    const int32_t fd = int32_t(call.Call<IOSOpen>({kDevice, 0}));
    int32_t result;

    if (fd < 0) {
        result = OpenError(fd);
    } else {
        Memory::Write32(kVectors, kResult);
        Memory::Write32(kVectors + 4, 0x20);

        if (int32_t(call.Call<IOSIoctlv>({uint32_t(fd), kGetLinkStatus, 0, 1, kVectors})) < 0) {
            result = kNcdEFatal;
        } else {
            result = int32_t(Memory::Read32(kResult));

            if (result == 0) {
                result = int32_t(Memory::Read32(kResult + 4));

                if (result < 0)
                    result = kNcdEClose;
            }
        }

        if (int32_t(call.Call<IOSClose>({uint32_t(fd)})) < 0)
            result = kNcdEClose;
    }

    Unlock(cpu);
    return result;
}

// 0x8051CF28: NCDGetWirelessMacAddress(address): 6 bytes.
int32_t GetWirelessMacAddress(CpuContext* cpu, uint32_t address) {
    if (!address)
        return kNcdEInval;

    if (!Memory::Read32(kCurrentThread))
        return kNcdENoThread;

    Lock(cpu);
    GuestCall call{cpu};
    const int32_t fd = int32_t(call.Call<IOSOpen>({kDevice, 0}));
    int32_t result;

    if (fd < 0) {
        result = OpenError(fd);
    } else {
        const uint32_t work = Memory::Read32(kWork);
        Memory::Write32(kVectors, kResult);
        Memory::Write32(kVectors + 4, 0x20);
        Memory::Write32(kVectors + 8, work);
        Memory::Write32(kVectors + 12, 6);
        const int32_t sent = int32_t(call.Call<IOSIoctlv>({uint32_t(fd), kGetMacAddress, 0, 2, kVectors}));
        result = int32_t(Memory::Read32(kResult));

        if (sent < 0)
            result = kNcdEFatal;
        else if (result == 0)
            call.Call<Memcpy>({address, work, 6});

        if (int32_t(call.Call<IOSClose>({uint32_t(fd)})) < 0)
            result = kNcdEClose;
    }

    Unlock(cpu);
    return result;
}

// 0x8051D050: the enabled configurations among the three slots, as bit masks:
// wired ones, and wireless ones split by their byte at +0x7CA (1 or not).
struct EnabledConfigs {
    uint32_t wired = 0, wireless = 0, wirelessOne = 0;
};

int32_t GetEnabledConfigs(CpuContext* cpu, EnabledConfigs& out) {
    Lock(cpu);
    const int32_t result = Request(cpu, 0, kGetConfig);

    if (result == 0) {
        const uint32_t work = Memory::Read32(kWork);

        for (uint32_t i = 0; i < 3; ++i) {
            const uint32_t slot = work + i * kSlotBytes;
            const uint8_t flags = Memory::Read8(slot + 8);

            if (!(flags & 0x80))
                continue;

            if (flags & 1)
                out.wired |= 1u << i;
            else if (Memory::Read8(slot + 0x7CA) != 1)
                out.wireless |= 1u << i;
            else
                out.wirelessOne |= 1u << i;
        }
    }

    Unlock(cpu);
    return result;
}

// 0x8051D450: which kind of configuration the error is about, when only one
// kind is enabled: 20 + the first wired slot, 30 + the first wireless one,
// 40 + the first of the other wireless ones; else 99.
int32_t ConfigKind(uint32_t wired, uint32_t wireless, uint32_t wirelessOne) {
    if (wired && !wireless && !wirelessOne)
        return 20 + __builtin_ctz(wired);

    if (!wired && wireless && !wirelessOne)
        return 30 + __builtin_ctz(wireless);

    if (!wired && !wireless && wirelessOne)
        return 40 + __builtin_ctz(wirelessOne);

    return 99;
}

// 0x8051D748: the error code a failed SOStartup shows (5xxxx), by its error
// and the configuration kind (20..29: wired).
int32_t StartupErrorCode(CpuContext* cpu, int32_t error, int32_t kind) {
    const bool wired = kind >= 20 && kind < 30;
    const int32_t unreachable = wired ? -51400 : -51300;

    if (error >= 0)
        return 0;

    switch (error) {
    case -62:
        return -50400;
    case -111:
        return -52700;
    case -121:
        return wired ? -51400 : -51000;
    case -112:
    case -76:
    case -48:
    case -39:
        return unreachable;
    case -102:
    case -101:
    case -100:
        return -52000;
    case -45:
        return -50200;
    case -28:
        return -50300;
    case int32_t(0x80000000):
        return -50100;
    default:
        break;
    }

    cpu->cr &= ~kCrFloatArgs;
    GuestCall{cpu}.Call<OSReport>({kUnknownError, uint32_t(error)});
    return -50100;
}

} // namespace

extern "C" void MH3_NCDGetCurrentIfConfig(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(GetCurrentIfConfig(cpu, cpu->gpr[3]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051CBD4, MH3_NCDGetCurrentIfConfig, "NCDGetCurrentIfConfig");

extern "C" void MH3_NCDGetCurrentIpConfig(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(GetCurrentIpConfig(cpu, cpu->gpr[3]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051CCCC, MH3_NCDGetCurrentIpConfig, "NCDGetCurrentIpConfig");

extern "C" void MH3_NCDGetLinkStatus(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(GetLinkStatus(cpu));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051CE24, MH3_NCDGetLinkStatus, "NCDGetLinkStatus");

extern "C" void MH3_NCDGetWirelessMacAddress(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(GetWirelessMacAddress(cpu, cpu->gpr[3]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051CF28, MH3_NCDGetWirelessMacAddress, "NCDGetWirelessMacAddress");
REGISTER_NATIVE_FUNCTION_AS(0x8051D8C8, MH3_NCDGetWirelessMacAddress, "NCDGetWirelessMacAddress2");

extern "C" void MH3_NCDGetEnabledConfigs(CpuContext* cpu) {
    const uint32_t wired = cpu->gpr[3], wireless = cpu->gpr[4], wirelessOne = cpu->gpr[5];
    EnabledConfigs configs;
    const int32_t result = GetEnabledConfigs(cpu, configs);

    if (wired)
        Memory::Write32(wired, configs.wired);

    if (wireless)
        Memory::Write32(wireless, configs.wireless);

    if (wirelessOne)
        Memory::Write32(wirelessOne, configs.wirelessOne);

    cpu->gpr[3] = uint32_t(result);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051D050, MH3_NCDGetEnabledConfigs, "NCDiGetEnabledConfigList");

extern "C" void MH3_NCDConfigKind(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(ConfigKind(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051D450, MH3_NCDConfigKind, "NCDiConfigKind");

// 0x8051D6C8: the error code for a failed SOStartup's error, less the kind.
extern "C" void MH3_NCDStartupErrorCode(CpuContext* cpu) {
    int32_t error = int32_t(cpu->gpr[3]);
    EnabledConfigs configs;
    int32_t kind = 99;

    if (GetEnabledConfigs(cpu, configs) >= 0)
        kind = ConfigKind(configs.wired, configs.wireless, configs.wirelessOne);

    if (kind < 0) {
        error = int32_t(0x80000000);
        kind = 99;
    }

    cpu->gpr[3] = uint32_t(StartupErrorCode(cpu, error, kind) - kind);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051D6C8, MH3_NCDStartupErrorCode, "NCDGetStartupErrorCode");

extern "C" void MH3_NCDStartupErrorCodeOf(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(StartupErrorCode(cpu, int32_t(cpu->gpr[3]), int32_t(cpu->gpr[4])));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051D748, MH3_NCDStartupErrorCodeOf, "NCDiStartupErrorCode");

extern "C" void MH3_NCDLock(CpuContext* cpu) {
    Lock(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051D360, MH3_NCDLock, "NCDiLock");

extern "C" void MH3_NCDRequest(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Request(cpu, cpu->gpr[4], int32_t(cpu->gpr[5])));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051D1CC, MH3_NCDRequest, "NCDiRequest");
