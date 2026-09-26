#include "abi_bridge.h"
#include "ios_host.h"
#include <cerrno>
#include <cstdlib>
#include <deque>
#include <filesystem>
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
uint32_t nextHandle = 100;
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
    if (path == "/dev/stm/immediate" || path == "/dev/stm/eventhook") {
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
extern "C" void MH3_IOSIoctlAsync(CpuContext* ctx) {
    auto it = devices.find(ctx->gpr[3]);
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
