#include "abi_bridge.h"
#include "ios_host.h"
#include "disc_image.h"
#include <array>
#include <cerrno>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
struct File { int host; uint32_t mode; };
std::map<uint32_t, File> files;
std::map<uint32_t, std::string> devices;
struct EventHook { uint32_t fd, output, callback, argument; };
std::optional<EventHook> eventHook;
std::deque<IosCompletion> completions;
std::array<uint8_t, 20> ledSettings{}, dimmingSettings{};
uint32_t nextHandle = 100;
uint32_t lastDiscPosition = 0;
DiscImage& Disc() {
    const char* path = std::getenv("MH3_DISC_DIR");
    static DiscImage image(path ? path : "Assets/DATA");
    return image;
}
int32_t Error() {
    switch (errno) {
    case ENOENT: return -106;
    case EACCES: case EPERM: case ELOOP: return -102;
    case ENOSPC: return -108;
    default: return -101;
    }
}
std::string GuestPath(uint32_t address) {
    std::string path;
    for (unsigned i = 0; i < 64; ++i) {
        const char c = Memory::Read8(address + i);
        if (!c) return path;
        path += c;
    }
    return {};
}
// IOS_Open does not create files. ISFS creation will be handled independently.
// Resolve every component relative to the private NAND root without following
// symlinks, and reject traversal before opening any host file.
int OpenFile(const std::string& path, uint32_t mode) {
    if (path.size() < 2 || path[0] != '/' || mode < 1 || mode > 3) return -4;
    std::filesystem::path relative = path.substr(1);
    for (const auto& part : relative)
        if (part == ".." || part == "." || part.empty()) return -4;
    const char* user = std::getenv("MH3_USER_DIR");
    auto root = (user ? std::filesystem::path(user) : std::filesystem::path("saves")) / "nand";
    int dir = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dir < 0) return Error();
    auto end = relative.end();
    for (auto it = relative.begin(); it != end; ++it) {
        auto next = it; ++next;
        const bool last = next == end;
        int flags = last ? (mode == 1 ? O_RDONLY : mode == 2 ? O_WRONLY : O_RDWR) : O_RDONLY | O_DIRECTORY;
        int child = ::openat(dir, it->c_str(), flags | O_NOFOLLOW);
        int saved = errno;
        ::close(dir);
        errno = saved;
        if (child < 0) return Error();
        dir = child;
    }
    const uint32_t handle = nextHandle++;
    files.emplace(handle, File{dir, mode});
    return handle;
}
}

std::optional<IosCompletion> TakeIosCompletion() {
    if (completions.empty()) return std::nullopt;
    auto result = completions.front();
    completions.pop_front();
    return result;
}
void TriggerStmEvent(uint32_t event) {
    if (!eventHook) return;
    Memory::Write32(eventHook->output, event);
    completions.push_back({eventHook->callback, 0, eventHook->argument});
    eventHook.reset();
}

// IPCWriteReg's initialization acknowledges stale status and enables replies.
// Actual requests are delivered through the IOS HLE entry points below.
extern "C" void MH3_IPCWriteReg(CpuContext* ctx) {
    if (ctx->gpr[3] != 1 || ctx->gpr[4] != 0x38)
        throw std::runtime_error("Unimplemented direct IPC register operation");
}
REGISTER_NATIVE_FUNCTION(0x804BB950, MH3_IPCWriteReg);

extern "C" void MH3_IOSOpen(CpuContext* ctx) {
    const std::string path = GuestPath(ctx->gpr[3]);
    // No WiiConnect24 background-download daemon is running on the host.
    // Let the SDK's existing unavailable-service path handle offline startup.
    if (path == "/dev/net/kd/request" || path == "/dev/net/kd/time") {
        ctx->gpr[3] = -6; return;
    }
    if (path == "/dev/stm/immediate" || path == "/dev/stm/eventhook" ||
        path == "/dev/fs" || path == "/dev/es" || path == "/dev/di") {
        if (path == "/dev/di") (void)Disc();
        uint32_t handle = nextHandle++;
        devices.emplace(handle, path);
        ctx->gpr[3] = handle;
        return;
    }
    if (path.starts_with("/dev/"))
        throw std::runtime_error("IOS device needs a host implementation: " + path);
    ctx->gpr[3] = OpenFile(path, ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION(0x804BC500, MH3_IOSOpen);
extern "C" void MH3_IOSClose(CpuContext* ctx) {
    if (devices.erase(ctx->gpr[3])) {
        if (eventHook && eventHook->fd == ctx->gpr[3]) eventHook.reset();
        ctx->gpr[3] = 0;
        return;
    }
    auto it = files.find(ctx->gpr[3]);
    if (it == files.end()) { ctx->gpr[3] = -4; return; }
    int result = ::close(it->second.host);
    files.erase(it);
    ctx->gpr[3] = result == 0 ? 0 : Error();
}
REGISTER_NATIVE_FUNCTION(0x804BC810, MH3_IOSClose);
extern "C" void MH3_IOSRead(CpuContext* ctx) {
    auto it = files.find(ctx->gpr[3]);
    if (it == files.end()) { ctx->gpr[3] = -4; return; }
    if (!(it->second.mode & 1)) { ctx->gpr[3] = -102; return; }
    uint32_t address = ctx->gpr[4], size = ctx->gpr[5];
    if (!Memory::Contains(address, size)) { ctx->gpr[3] = -4; return; }
    // Use guest writes so executable-memory checks cannot be bypassed.
    std::vector<uint8_t> buffer(size);
    ssize_t count = ::read(it->second.host, buffer.data(), size);
    if (count < 0) { ctx->gpr[3] = Error(); return; }
    for (ssize_t i = 0; i < count; ++i) Memory::Write8(address + i, buffer[i]);
    ctx->gpr[3] = count;
}
REGISTER_NATIVE_FUNCTION(0x804BCA70, MH3_IOSRead);
extern "C" void MH3_IOSWrite(CpuContext* ctx) {
    auto it = files.find(ctx->gpr[3]);
    if (it == files.end()) { ctx->gpr[3] = -4; return; }
    if (!(it->second.mode & 2)) { ctx->gpr[3] = -102; return; }
    ssize_t count = ::write(it->second.host, Memory::GetPointer(ctx->gpr[4], ctx->gpr[5]), ctx->gpr[5]);
    ctx->gpr[3] = count < 0 ? Error() : count;
}
REGISTER_NATIVE_FUNCTION(0x804BCC80, MH3_IOSWrite);

// STM event hooks remain pending until a real host reset/power event. In
// particular, installing the hook must not immediately call the guest handler.
// Protocol reference: Dolphin Core/IOS/STM/STM.cpp, IOCTL_STM_EVENTHOOK.
extern "C" void MH3_IOSIoctl(CpuContext*);
extern "C" void MH3_IOSIoctlAsync(CpuContext* ctx) {
    auto it = devices.find(ctx->gpr[3]);
    if (it != devices.end() && it->second == "/dev/di") {
        const uint32_t callback = ctx->gpr[9], argument = ctx->gpr[10];
        if (!callback) { ctx->gpr[3] = -4; return; }
        MH3_IOSIoctl(ctx);
        completions.push_back({callback, static_cast<int32_t>(ctx->gpr[3]), argument});
        ctx->gpr[3] = 0;
        return;
    }
    if (it != devices.end() && it->second == "/dev/stm/eventhook" && ctx->gpr[4] == 0x1000) {
        if (ctx->gpr[8] < 4 || !Memory::Contains(ctx->gpr[7], ctx->gpr[8]) || !ctx->gpr[9]) {
            ctx->gpr[3] = -4; return;
        }
        if (eventHook) {
            completions.push_back({ctx->gpr[9], -8, ctx->gpr[10]});
            ctx->gpr[3] = 0;
            return;
        }
        eventHook = EventHook{ctx->gpr[3], ctx->gpr[7], ctx->gpr[9], ctx->gpr[10]};
        ctx->gpr[3] = 0;
        return;
    }
    throw std::runtime_error("IOS async ioctl needs a host implementation");
}
REGISTER_NATIVE_FUNCTION(0x804BCE70, MH3_IOSIoctlAsync);

extern "C" void MH3_IOSIoctl(CpuContext* ctx) {
    const auto device = devices.find(ctx->gpr[3]);
    if (device == devices.end()) { ctx->gpr[3] = -4; return; }
    const uint32_t command = ctx->gpr[4];
    if (device->second == "/dev/di") {
        // IOS DI command buffer contains byte length and word-addressed offset.
        // DI uses success=1 (unlike ordinary IOS success=0).
        const uint32_t input = ctx->gpr[5], output = ctx->gpr[7], capacity = ctx->gpr[8];
        if (ctx->gpr[6] != 0x20 || !Memory::Contains(input, 0x20)) {
            ctx->gpr[3] = 32; return;
        }
        if (command == 0x12) {
            if (capacity < 0x20 || !Memory::Contains(output, 0x20)) { ctx->gpr[3] = 32; return; }
            for (uint32_t i = 0; i < 0x20; ++i) Memory::Write8(output + i, 0);
            // Retail Wii inquiry values recorded in Dolphin DVDInterface.cpp.
            Memory::Write32(output, 2);
            Memory::Write32(output + 4, 0x20060526);
            Memory::Write32(output + 8, 0x41000000);
            ctx->gpr[3] = 1; return;
        }
        if (command == 0x70 || command == 0x71) {
            const uint32_t length = command == 0x70 ? 0x20 : Memory::Read32(input + 4);
            const uint32_t position = command == 0x70 ? 0 : Memory::Read32(input + 8);
            if (length > capacity || !Memory::Contains(output, length)) { ctx->gpr[3] = 32; return; }
            auto bytes = Disc().Read(uint64_t(position) * 4, length);
            for (uint32_t i = 0; i < length; ++i) Memory::Write8(output + i, bytes[i]);
            lastDiscPosition = position;
            ctx->gpr[3] = 1; return;
        }
        if (command == 0x7A || command == 0x88 || command == 0x83 || command == 0xE0) {
            if (capacity < 4 || !Memory::Contains(output, 4)) { ctx->gpr[3] = 32; return; }
            Memory::Write32(output, command == 0x88 ? 2 : command == 0x83 ? lastDiscPosition : 0);
            ctx->gpr[3] = 1; return;
        }
        // The host-mounted disc is present and has no physical cover interrupt.
        if (command == 0x86 || command == 0x85 || command == 0x87 || command == 0x89 || command == 0x7E) {
            ctx->gpr[3] = 1; return;
        }
    }
    if (device->second == "/dev/stm/immediate") {
        // Hardware presentation settings, retained without driving Wii LEDs.
        // IDs/layout: Dolphin Core/IOS/STM/STM.h and MH3 0x804D6230.
        if (command == 0x6002 || command == 0x5001) {
            if (ctx->gpr[6] < 20 || !Memory::Contains(ctx->gpr[5], 20)) {
                ctx->gpr[3] = -4; return;
            }
            auto& settings = command == 0x6002 ? ledSettings : dimmingSettings;
            for (uint32_t i = 0; i < 20; ++i) settings[i] = Memory::Read8(ctx->gpr[5] + i);
            ctx->gpr[3] = 0;
            return;
        }
        if (command == 0x3002) {
            if (!eventHook) { ctx->gpr[3] = -6; return; }
            TriggerStmEvent(0);
            ctx->gpr[3] = 0;
            return;
        }
    }
    throw std::runtime_error("IOS ioctl needs a host implementation: " + device->second +
                             " command " + std::to_string(command));
}
REGISTER_NATIVE_FUNCTION(0x804BCFB0, MH3_IOSIoctl);

// ES_GetTitleID and ES_GetDataDir, verified at MH3's ESP wrappers
// 0x804B0060 and 0x804AFFC0. Title ID comes from this disc's signed TMD.
extern "C" void MH3_IOSIoctlv(CpuContext* ctx) {
    const auto device = devices.find(ctx->gpr[3]);
    if (device == devices.end()) { ctx->gpr[3] = -4; return; }
    const uint32_t command = ctx->gpr[4], inputs = ctx->gpr[5], outputs = ctx->gpr[6];
    const uint32_t vectors = ctx->gpr[7];
    if (inputs > 16 || outputs > 16 || !Memory::Contains(vectors, (inputs + outputs) * 8)) {
        ctx->gpr[3] = -4; return;
    }
    if (device->second == "/dev/es" && command == 0x16 && inputs == 1 && outputs == 2) {
        // New host NAND has no ticket-consumption records. The original SDK
        // still checks the ticket's own time-limit fields after this query.
        const uint32_t countOut = Memory::Read32(vectors + 16);
        if (Memory::Read32(vectors + 4) != 8 || !Memory::Contains(Memory::Read32(vectors), 8) ||
            Memory::Read32(vectors + 20) != 4 || !Memory::Contains(countOut, 4)) {
            ctx->gpr[3] = -4; return;
        }
        Memory::Write32(countOut, 0);
        ctx->gpr[3] = 0; return;
    }
    if (device->second == "/dev/es" && command == 0x1B && inputs == 1 && outputs == 1) {
        const uint32_t input = Memory::Read32(vectors), inputSize = Memory::Read32(vectors + 4);
        const uint32_t output = Memory::Read32(vectors + 8), outputSize = Memory::Read32(vectors + 12);
        if (outputSize != 0xD8 || !Memory::Contains(output, outputSize)) { ctx->gpr[3] = -4; return; }
        std::array<uint8_t, 0x2A4> ticket{};
        if (!input && !inputSize) {
            const char* root = std::getenv("MH3_DISC_DIR");
            std::ifstream file((root ? std::filesystem::path(root) : "Assets/DATA") / "ticket.bin", std::ios::binary);
            file.read(reinterpret_cast<char*>(ticket.data()), ticket.size());
            if (!file) throw std::runtime_error("Unable to read disc ticket for ES view");
        } else {
            if (inputSize != ticket.size() || !Memory::Contains(input, inputSize)) { ctx->gpr[3] = -4; return; }
            for (uint32_t i = 0; i < ticket.size(); ++i) ticket[i] = Memory::Read8(input + i);
        }
        // RSA2048 ticket v0 view: version, 3 padding bytes, then ticket ID
        // through time limits. Layout verified against Dolphin ES/Formats.h.
        if (ticket[0] || ticket[1] != 1 || ticket[2] || ticket[3] != 1 || ticket[0x1BC] != 0) {
            ctx->gpr[3] = -4; return;
        }
        Memory::Write32(output, 0);
        for (uint32_t i = 0; i < 0xD4; ++i) Memory::Write8(output + 4 + i, ticket[0x1D0 + i]);
        ctx->gpr[3] = 0; return;
    }
    if (device->second == "/dev/es" && outputs == 1 &&
        ((command == 0x20 && inputs == 0) || (command == 0x1D && inputs == 1))) {
        const uint32_t out = Memory::Read32(vectors + inputs * 8);
        const uint32_t size = Memory::Read32(vectors + inputs * 8 + 4);
        constexpr uint64_t title = 0x00010000524D4850ull;
        constexpr char directory[] = "/title/00010000/524d4850/data";
        const uint32_t required = command == 0x20 ? 8 : sizeof(directory);
        if (size < required || !Memory::Contains(out, size)) { ctx->gpr[3] = -4; return; }
        if (command == 0x20) Memory::Write64(out, title);
        else {
            const uint32_t in = Memory::Read32(vectors);
            if (Memory::Read32(vectors + 4) != 8 || !Memory::Contains(in, 8)) {
                ctx->gpr[3] = -4; return;
            }
            if (Memory::Read64(in) != title) { ctx->gpr[3] = -106; return; }
            for (uint32_t i = 0; i < sizeof(directory); ++i) Memory::Write8(out + i, directory[i]);
        }
        ctx->gpr[3] = 0;
        return;
    }
    throw std::runtime_error("IOS ioctlv needs a host implementation: " + device->second +
                             " command " + std::to_string(command));
}
REGISTER_NATIVE_FUNCTION(0x804BD310, MH3_IOSIoctlv);

namespace {
void AsyncFile(CpuContext* ctx, void (*operation)(CpuContext*), uint32_t callback, uint32_t argument) {
    if (!callback) { ctx->gpr[3] = -4; return; }
    operation(ctx);
    completions.push_back({callback, static_cast<int32_t>(ctx->gpr[3]), argument});
    ctx->gpr[3] = 0;
}
}
extern "C" void MH3_IOSOpenAsync(CpuContext* ctx) {
    AsyncFile(ctx, MH3_IOSOpen, ctx->gpr[5], ctx->gpr[6]);
}
REGISTER_NATIVE_FUNCTION(0x804BC3E0, MH3_IOSOpenAsync);
extern "C" void MH3_IOSReadAsync(CpuContext* ctx) {
    AsyncFile(ctx, MH3_IOSRead, ctx->gpr[6], ctx->gpr[7]);
}
REGISTER_NATIVE_FUNCTION(0x804BC970, MH3_IOSReadAsync);
extern "C" void MH3_IOSCloseAsync(CpuContext* ctx) {
    AsyncFile(ctx, MH3_IOSClose, ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION(0x804BC750, MH3_IOSCloseAsync);

// Boot's SCCheckStatus loop is a safe event-pump boundary before OS scheduling
// starts. Preserve the polling caller's full CPU state around each callback.
// Process a bounded batch so callbacks that resubmit cannot starve the caller.
void PumpIosCompletions(CpuContext* ctx) {
    for (unsigned i = 0; i < 32; ++i) {
        auto completion = TakeIosCompletion();
        if (!completion) break;
        const CpuContext saved = *ctx;
        try {
            ctx->gpr[3] = completion->result;
            ctx->gpr[4] = completion->argument;
            InvokeIndirectCpu(completion->callback, ctx);
        } catch (...) { *ctx = saved; throw; }
        *ctx = saved;
    }
}
extern "C" void func_804DB770(CpuContext*);
extern "C" void MH3_SCCheckStatus(CpuContext* ctx) {
    PumpIosCompletions(ctx);
    func_804DB770(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x804DB770, MH3_SCCheckStatus, "MH3_SCCheckStatus");
