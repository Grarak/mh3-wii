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
#include <dirent.h>
#include <sys/stat.h>
#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#else
#include <sys/xattr.h>
#endif
#include <memory>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {
struct File { int host; uint32_t mode; };
std::map<uint32_t, File> files;
std::map<uint32_t, std::string> devices;
struct EventHook { uint32_t fd, output, callback, argument; };
std::optional<EventHook> eventHook;
struct HidState { bool changed = true; std::optional<EventHook> hook; };
std::map<uint32_t, HidState> hidDevices;
std::deque<IosCompletion> completions;
std::array<uint8_t, 20> ledSettings{}, dimmingSettings{};
uint32_t nextHandle = 100;
uint32_t lastDiscPosition = 0;
DiscImage& Disc() {
    return GameDiscImage();
}
int32_t Error() {
    switch (errno) {
    case ENOENT: return -106;
    case EEXIST: return -105;
    case EACCES: case EPERM: case ELOOP: return -102;
    case ENOSPC: return -108;
    default: return -101;
    }
}
std::filesystem::path NandRoot() {
    const char* user = std::getenv("MH3_USER_DIR");
    return (user ? std::filesystem::path(user) : std::filesystem::path("saves")) / "nand";
}
bool ValidNandPath(const std::string& path, bool allowRoot = false) {
    if (path.empty() || path.size() >= 64 || path[0] != '/' || path.find("//") != std::string::npos ||
        (path.size() == 1 && !allowRoot)) return false;
    for (const auto& part : std::filesystem::path(path.substr(1)))
        if (part == ".." || part == "." || part.empty()) return false;
    return true;
}
// Persist Wii metadata on the inode, so native renames preserve it and NAND
// cluster accounting does not include host-only sidecar files. This port has
// one title: UID 0x1000, group 0x3038 (verified in RMHP08's TMD).
using NandMetadata = std::array<uint8_t, 10>;
constexpr NandMetadata defaultMetadata{0, 0, 0x10, 0, 0x30, 0x38, 3, 3, 3, 0};
#ifndef __vita__
#ifdef __APPLE__
constexpr const char* metadataName = "org.mh3.nand";
#else
constexpr const char* metadataName = "user.mh3.nand";
#endif
int SetMetadata(int fd, const NandMetadata& metadata) {
#ifdef __APPLE__
    const int result = ::fsetxattr(fd, metadataName, metadata.data(), metadata.size(), 0, 0);
#else
    const int result = ::fsetxattr(fd, metadataName, metadata.data(), metadata.size(), 0);
#endif
    return result == 0 ? 0 : Error();
}
int GetMetadata(int fd, NandMetadata& metadata) {
#ifdef __APPLE__
    const auto size = ::fgetxattr(fd, metadataName, metadata.data(), metadata.size(), 0, 0);
    if (size < 0 && errno == ENOATTR) { metadata = defaultMetadata; return 0; }
#else
    const auto size = ::fgetxattr(fd, metadataName, metadata.data(), metadata.size());
    if (size < 0 && errno == ENODATA) { metadata = defaultMetadata; return 0; }
#endif
    return size == metadata.size() ? 0 : size < 0 ? Error() : -101;
}
#endif
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
#ifdef __vita__
#include "../vita/nand.inc"
#else
int CloseNandPath(int fd) { return ::close(fd); }
int SyncNandFile(int fd) { return ::fsync(fd) == 0 ? 0 : Error(); }
int OpenNandPath(const std::string& path, int finalFlags, bool allowRoot = false) {
    if (!ValidNandPath(path, allowRoot)) return -4;
    std::filesystem::path relative = path.substr(1);
    for (const auto& part : relative)
        if (part == ".." || part == "." || part.empty()) return -4;
    const auto root = NandRoot();
    int dir = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dir < 0) return Error();
    auto end = relative.end();
    for (auto it = relative.begin(); it != end; ++it) {
        auto next = it; ++next;
        const bool last = next == end;
        int flags = last ? finalFlags : O_RDONLY | O_DIRECTORY;
        int child = ::openat(dir, it->c_str(), flags | O_NOFOLLOW);
        int saved = errno;
        ::close(dir);
        errno = saved;
        if (child < 0) return Error();
        dir = child;
    }
    return dir;
}
int InitializeNand() {
    const auto root = NandRoot();
    std::filesystem::create_directories(root.parent_path());
    if (::mkdir(root.c_str(), 0700) != 0 && errno != EEXIST) return Error();
    // IOS and title installation normally provide these parents. No game
    // settings or save contents are synthesized, and existing files survive.
    constexpr const char* directories[]{"/tmp", "/sys", "/shared2", "/title",
        "/title/00010000", "/title/00010001", "/title/00010004",
        "/title/00010000/524d4850", "/title/00010000/524d4850/data"};
    for (const auto* path : directories) {
        const std::filesystem::path entry(path);
        const int parent = OpenNandPath(entry.parent_path().string(), O_RDONLY | O_DIRECTORY, true);
        if (parent < 0) return parent;
        const auto name = entry.filename().string();
        const bool created = ::mkdirat(parent, name.c_str(), 0700) == 0;
        if (!created && errno != EEXIST) { const int error = Error(); ::close(parent); return error; }
        const int child = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        const int error = child < 0 ? Error() : created ? SetMetadata(child, defaultMetadata) : 0;
        if (child >= 0) ::close(child);
        ::close(parent);
        if (error) return error;
    }
    return 0;
}
int CreateNandEntry(const std::string& path, bool directory, const NandMetadata& metadata) {
    if (!ValidNandPath(path)) return -101;
    const std::filesystem::path entry(path);
    const int parent = OpenNandPath(entry.parent_path().string(), O_RDONLY | O_DIRECTORY, true);
    if (parent < 0) return parent;
    const auto name = entry.filename().string();
    int child;
    if (directory) {
        if (::mkdirat(parent, name.c_str(), 0700) != 0) {
            const int error = Error(); ::close(parent); return error;
        }
        child = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    } else child = ::openat(parent, name.c_str(), O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW, 0600);
    const int result = child < 0 ? Error() : SetMetadata(child, metadata);
    if (child >= 0) ::close(child);
    ::close(parent);
    return result;
}
int RenameNandEntry(const std::string& from, const std::string& to) {
    if (!ValidNandPath(from) || !ValidNandPath(to)) return -101;
    const std::filesystem::path source(from), target(to);
    const int sourceDir = OpenNandPath(source.parent_path().string(), O_RDONLY | O_DIRECTORY, true);
    if (sourceDir < 0) return sourceDir;
    const int targetDir = OpenNandPath(target.parent_path().string(), O_RDONLY | O_DIRECTORY, true);
    if (targetDir < 0) { ::close(sourceDir); return targetDir; }
    struct stat info{};
    int result = 0;
    if (::fstatat(sourceDir, source.filename().c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) result = Error();
    else if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) result = -102;
    if (!result) {
        if (::fstatat(targetDir, target.filename().c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
            if (!S_ISREG(info.st_mode)) result = -102;
        } else if (errno != ENOENT) result = Error();
    }
    if (!result && ::renameat(sourceDir, source.filename().c_str(), targetDir, target.filename().c_str()) != 0)
        result = Error();
    ::close(sourceDir); ::close(targetDir);
    return result;
}
int RemoveNandTree(int parent, const char* name, bool execute, unsigned depth = 0) {
    if (depth > 64) return -101;
    struct stat info{};
    if (::fstatat(parent, name, &info, AT_SYMLINK_NOFOLLOW) != 0) return Error();
    const bool directory = S_ISDIR(info.st_mode);
    if (!directory && !S_ISREG(info.st_mode)) return -102;
    for (const auto& [handle, file] : files) {
        struct stat opened{};
        if (::fstat(file.host, &opened) == 0 && opened.st_dev == info.st_dev && opened.st_ino == info.st_ino)
            return -111; // ISFS InUse
    }
    if (directory) {
        const int fd = ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (fd < 0) return Error();
        DIR* raw = ::fdopendir(fd);
        if (!raw) { const int error = Error(); ::close(fd); return error; }
        std::unique_ptr<DIR, decltype(&::closedir)> entries(raw, ::closedir);
        while (true) {
            errno = 0;
            const auto* entry = ::readdir(raw);
            if (!entry) { if (errno) return Error(); break; }
            const std::string_view child(entry->d_name);
            if (child == "." || child == "..") continue;
            const int result = RemoveNandTree(fd, entry->d_name, execute, depth + 1);
            if (result) return result;
        }
    }
    if (execute && ::unlinkat(parent, name, directory ? AT_REMOVEDIR : 0) != 0) return Error();
    return 0;
}
int DeleteNandEntry(const std::string& path) {
    if (!ValidNandPath(path)) return -101;
    const std::filesystem::path entry(path);
    const int parent = OpenNandPath(entry.parent_path().string(), O_RDONLY | O_DIRECTORY, true);
    if (parent < 0) return parent;
    // Validate the entire subtree before removing any file (including InUse).
    int result = RemoveNandTree(parent, entry.filename().c_str(), false);
    if (!result) result = RemoveNandTree(parent, entry.filename().c_str(), true);
    ::close(parent);
    return result;
}
int OpenFile(const std::string& path, uint32_t mode) {
    if (mode < 1 || mode > 3) return -4;
    const int dir = OpenNandPath(path, mode == 1 ? O_RDONLY : mode == 2 ? O_WRONLY : O_RDWR);
    if (dir < 0) return dir;
    struct stat info{};
    if (::fstat(dir, &info) != 0) { const int error = Error(); ::close(dir); return error; }
    if (!S_ISREG(info.st_mode)) { ::close(dir); return -101; }
    NandMetadata metadata;
    const int metadataResult = GetMetadata(dir, metadata);
    if (metadataResult) { ::close(dir); return metadataResult; }
    const uint32_t owner = (uint32_t(metadata[0]) << 24) | (uint32_t(metadata[1]) << 16) |
                           (uint32_t(metadata[2]) << 8) | metadata[3];
    const uint16_t group = (uint16_t(metadata[4]) << 8) | metadata[5];
    const uint8_t allowed = owner == 0x1000 ? metadata[6] : group == 0x3038 ? metadata[7] : metadata[8];
    if ((allowed & mode) != mode) { ::close(dir); return -102; }
    const uint32_t handle = nextHandle++;
    files.emplace(handle, File{dir, mode});
    return handle;
}
int DirectoryUsage(int fd, uint64_t& clusters, uint64_t& inodes, unsigned depth = 0) {
    // Own each descriptor even on error, and never follow a host symlink.
    DIR* raw = ::fdopendir(fd);
    if (!raw) { const int error = Error(); ::close(fd); return error; }
    std::unique_ptr<DIR, decltype(&::closedir)> directory(raw, ::closedir);
    if (depth > 64) return -4;
    ++inodes;
    while (true) {
        errno = 0;
        const auto* entry = ::readdir(raw);
        if (!entry) return errno ? Error() : 0;
        const std::string_view name(entry->d_name);
        if (name == "." || name == "..") continue;
        struct stat info{};
        if (::fstatat(fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) return Error();
        if (S_ISREG(info.st_mode)) {
            ++inodes;
            clusters += (uint64_t(info.st_size) + 0x3FFF) / 0x4000;
        } else if (S_ISDIR(info.st_mode)) {
            const int child = ::openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            if (child < 0) return Error();
            const auto result = DirectoryUsage(child, clusters, inodes, depth + 1);
            if (result) return result;
        } else return -102;
    }
}
#endif
}

std::optional<IosCompletion> TakeIosCompletion() {
    if (completions.empty()) return std::nullopt;
    auto result = completions.front();
    completions.pop_front();
    return result;
}
void QueueIosCompletion(IosCompletion completion) {
    if (completion.callback) completions.push_back(completion);
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
        path == "/dev/fs" || path == "/dev/es" || path == "/dev/di" || path == "/dev/usb/hid") {
        if (path == "/dev/di") (void)Disc();
        if (path == "/dev/fs") {
            const int result = InitializeNand();
            if (result) {
                std::fprintf(stderr, "MH3 NAND initialization failed: %d (errno %d), root %s\n",
                             result, errno, NandRoot().c_str());
                ctx->gpr[3] = result; return;
            }
        }
        uint32_t handle = nextHandle++;
        devices.emplace(handle, path);
        if (path == "/dev/usb/hid") hidDevices.emplace(handle, HidState{});
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
        if (const auto hid = hidDevices.find(ctx->gpr[3]); hid != hidDevices.end()) {
            if (hid->second.hook) {
                const auto hook = *hid->second.hook;
                Memory::Write32(hook.output, 0xFFFFFFFF);
                completions.push_back({hook.callback, -1, hook.argument});
            }
            hidDevices.erase(hid);
        }
        ctx->gpr[3] = 0;
        return;
    }
    auto it = files.find(ctx->gpr[3]);
    if (it == files.end()) { ctx->gpr[3] = -4; return; }
    const int syncError = (it->second.mode & 2) ? SyncNandFile(it->second.host) : 0;
    int result = CloseNandPath(it->second.host);
    files.erase(it);
    ctx->gpr[3] = syncError ? syncError : result == 0 ? 0 : Error();
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
void SeekFile(CpuContext* ctx) {
    const auto it = files.find(ctx->gpr[3]);
    if (it == files.end() || ctx->gpr[5] > 2) { ctx->gpr[3] = -101; return; }
    struct stat info{};
    const auto current = ::lseek(it->second.host, 0, SEEK_CUR);
    if (current < 0 || ::fstat(it->second.host, &info) != 0) { ctx->gpr[3] = Error(); return; }
    // IOS uses 32-bit offsets, including modular relative/end offsets, and
    // rejects seeks beyond EOF (unlike POSIX).
    const uint32_t base = ctx->gpr[5] == 0 ? 0 : ctx->gpr[5] == 1 ? current : info.st_size;
    const uint32_t position = base + ctx->gpr[4];
    if (position > uint64_t(info.st_size)) { ctx->gpr[3] = -101; return; }
    const auto result = ::lseek(it->second.host, position, SEEK_SET);
    ctx->gpr[3] = result < 0 ? Error() : result;
}

// STM event hooks remain pending until a real host reset/power event. In
// particular, installing the hook must not immediately call the guest handler.
// Protocol reference: Dolphin Core/IOS/STM/STM.cpp, IOCTL_STM_EVENTHOOK.
extern "C" void MH3_IOSIoctl(CpuContext*);
extern "C" void MH3_IOSIoctlAsync(CpuContext* ctx) {
    auto it = devices.find(ctx->gpr[3]);
    if (it != devices.end() && it->second == "/dev/usb/hid" && ctx->gpr[4] == 0) {
        // HID v4 GETDEVICECHANGE: enumerate once, then retain the subscription
        // until a change or shutdown. There are no Wii USB HID peripherals;
        // keyboard/gamepad input is supplied separately through WPAD.
        if (!ctx->gpr[7] || ctx->gpr[8] != 0x600 ||
            !Memory::Contains(ctx->gpr[7], 0x600) || !ctx->gpr[9]) {
            ctx->gpr[3] = -4; return;
        }
        auto& state = hidDevices.at(ctx->gpr[3]);
        if (state.hook) { ctx->gpr[3] = -8; return; }
        if (state.changed) {
            Memory::Write32(ctx->gpr[7], 0xFFFFFFFF); // end of device list
            completions.push_back({ctx->gpr[9], 0, ctx->gpr[10]});
            state.changed = false;
        } else {
            state.hook = EventHook{ctx->gpr[3], ctx->gpr[7], ctx->gpr[9], ctx->gpr[10]};
        }
        ctx->gpr[3] = 0; return;
    }
    if (files.contains(ctx->gpr[3]) || (it != devices.end() &&
        (it->second == "/dev/di" || it->second == "/dev/stm/immediate" || it->second == "/dev/fs" ||
         it->second == "/dev/usb/hid"))) {
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
    throw std::runtime_error("IOS async ioctl needs a host implementation: " +
        (it == devices.end() ? std::string("invalid descriptor") : it->second) + " command " + std::to_string(ctx->gpr[4]));
}
REGISTER_NATIVE_FUNCTION(0x804BCE70, MH3_IOSIoctlAsync);

extern "C" void MH3_IOSIoctl(CpuContext* ctx) {
    if (const auto file = files.find(ctx->gpr[3]); file != files.end()) {
        if (ctx->gpr[4] != 11) throw std::runtime_error("IOS file ioctl not implemented: " + std::to_string(ctx->gpr[4]));
        if (ctx->gpr[8] < 8 || !Memory::Contains(ctx->gpr[7], 8)) { ctx->gpr[3] = -101; return; }
        struct stat info{};
        const auto offset = ::lseek(file->second.host, 0, SEEK_CUR);
        if (offset < 0 || ::fstat(file->second.host, &info) != 0) { ctx->gpr[3] = Error(); return; }
        Memory::Write32(ctx->gpr[7], info.st_size);
        Memory::Write32(ctx->gpr[7] + 4, offset);
        ctx->gpr[3] = 0; return;
    }
    const auto device = devices.find(ctx->gpr[3]);
    if (device == devices.end()) { ctx->gpr[3] = -4; return; }
    const uint32_t command = ctx->gpr[4];
    if (device->second == "/dev/usb/hid") {
        // IOS55 HID v4 protocol: Dolphin Core/IOS/USB/USB_HID/HIDv4.cpp
        // and RMHP08 HID initialization at 0x80528AF0.
        if (command == 6) { ctx->gpr[3] = 0x40001; return; }
        if (command == 1) { ctx->gpr[3] = 0; return; } // SET_SUSPEND is an IOS no-op
        if (command == 7) {
            auto& state = hidDevices.at(ctx->gpr[3]);
            if (state.hook) {
                const auto hook = *state.hook;
                Memory::Write32(hook.output, 0xFFFFFFFF);
                completions.push_back({hook.callback, -1, hook.argument});
                state.hook.reset(); state.changed = true;
            }
            ctx->gpr[3] = 0; return;
        }
        if ((command >= 2 && command <= 5) || command == 8) {
            const uint32_t expected = command == 8 ? 8 : 32;
            ctx->gpr[3] = !ctx->gpr[5] || ctx->gpr[6] != expected ||
                !Memory::Contains(ctx->gpr[5], expected) ? -4 : -6;
            return; // No attached device matches any IOS ID.
        }
        throw std::runtime_error("USB HID ioctl needs a host implementation: " + std::to_string(command));
    }
    if (device->second == "/dev/fs") {
        const uint32_t input = ctx->gpr[5], inputSize = ctx->gpr[6], output = ctx->gpr[7], outputSize = ctx->gpr[8];
        if (command == 7) {
            if (inputSize < 64 || !Memory::Contains(input, 64)) { ctx->gpr[3] = -101; return; }
            const auto path = GuestPath(input);
            ctx->gpr[3] = DeleteNandEntry(path);
            if (std::getenv("MH3_TRACE_ERRORS"))
                std::cerr << "MH3 NAND delete: " << path << " -> " << int32_t(ctx->gpr[3]) << '\n';
            return;
        }
        if (command == 8) {
            if (inputSize < 128 || !Memory::Contains(input, 128)) { ctx->gpr[3] = -101; return; }
            const auto from = GuestPath(input), to = GuestPath(input + 64);
            ctx->gpr[3] = RenameNandEntry(from, to);
            if (std::getenv("MH3_TRACE_ERRORS"))
                std::cerr << "MH3 NAND rename: " << from << " -> " << to << ": " << int32_t(ctx->gpr[3]) << '\n';
            return;
        }
        if (command == 3 || command == 9 || command == 5) {
            if (inputSize < 74 || !Memory::Contains(input, inputSize)) { ctx->gpr[3] = -101; return; }
            auto metadata = defaultMetadata;
            for (unsigned i = 0; i < 4; ++i) metadata[6 + i] = Memory::Read8(input + 70 + i);
            if (metadata[6] > 3 || metadata[7] > 3 || metadata[8] > 3) { ctx->gpr[3] = -101; return; }
            const auto path = GuestPath(input + 6);
            int result;
            if (command == 5) {
                for (unsigned i = 0; i < 6; ++i) metadata[i] = Memory::Read8(input + i);
                const int fd = OpenNandPath(path, O_RDONLY);
                result = fd < 0 ? fd : SetMetadata(fd, metadata);
                if (fd >= 0) CloseNandPath(fd);
            } else result = CreateNandEntry(path, command == 3, metadata);
            if (std::getenv("MH3_TRACE_ERRORS"))
                std::cerr << "MH3 NAND command " << command << ": " << path << " -> " << result << '\n';
            ctx->gpr[3] = result; return;
        }
        if (command == 6) {
            if (inputSize < 64 || outputSize < 74 || !Memory::Contains(input, 64) || !Memory::Contains(output, outputSize)) {
                ctx->gpr[3] = -101; return;
            }
            const auto path = GuestPath(input);
            const int fd = OpenNandPath(path, O_RDONLY, true);
            if (fd < 0) { ctx->gpr[3] = fd; return; }
            NandMetadata metadata;
            const int result = GetMetadata(fd, metadata);
            CloseNandPath(fd);
            if (result) { ctx->gpr[3] = result; return; }
            for (unsigned i = 0; i < 74; ++i) Memory::Write8(output + i, 0);
            for (unsigned i = 0; i < 6; ++i) Memory::Write8(output + i, metadata[i]);
            for (size_t i = 0; i < path.size(); ++i) Memory::Write8(output + 6 + i, path[i]);
            for (unsigned i = 0; i < 4; ++i) Memory::Write8(output + 70 + i, metadata[6 + i]);
            ctx->gpr[3] = 0; return;
        }
    }
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
    if (device->second == "/dev/fs" && command == 12) {
        if (inputs != 1 || outputs != 2 || Memory::Read32(vectors + 4) != 64 ||
            Memory::Read32(vectors + 12) != 4 || Memory::Read32(vectors + 20) != 4) {
            ctx->gpr[3] = -4; return;
        }
        const uint32_t pathAddress = Memory::Read32(vectors);
        const uint32_t clusterOut = Memory::Read32(vectors + 8), inodeOut = Memory::Read32(vectors + 16);
        if (!Memory::Contains(pathAddress, 64) || !Memory::Contains(clusterOut, 4) || !Memory::Contains(inodeOut, 4)) {
            ctx->gpr[3] = -4; return;
        }
        const auto path = GuestPath(pathAddress);
        const int directory = OpenNandPath(path, O_RDONLY | O_DIRECTORY, true);
        if (directory < 0) { ctx->gpr[3] = directory; return; }
        uint64_t clusters = 0, inodes = 0;
        const auto result = DirectoryUsage(directory, clusters, inodes);
        if (result) { ctx->gpr[3] = result; return; }
        if (clusters > UINT32_MAX || inodes > UINT32_MAX) { ctx->gpr[3] = -101; return; }
        Memory::Write32(clusterOut, clusters);
        Memory::Write32(inodeOut, inodes);
        ctx->gpr[3] = 0; return;
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
extern "C" void MH3_IOSWriteAsync(CpuContext* ctx) {
    AsyncFile(ctx, MH3_IOSWrite, ctx->gpr[6], ctx->gpr[7]);
}
REGISTER_NATIVE_FUNCTION(0x804BCB80, MH3_IOSWriteAsync);
extern "C" void MH3_IOSSeekAsync(CpuContext* ctx) {
    AsyncFile(ctx, SeekFile, ctx->gpr[6], ctx->gpr[7]);
}
REGISTER_NATIVE_FUNCTION(0x804BCD90, MH3_IOSSeekAsync);
extern "C" void MH3_IOSCloseAsync(CpuContext* ctx) {
    AsyncFile(ctx, MH3_IOSClose, ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION(0x804BC750, MH3_IOSCloseAsync);
extern "C" void MH3_IOSIoctlvAsync(CpuContext* ctx) {
    AsyncFile(ctx, MH3_IOSIoctlv, ctx->gpr[8], ctx->gpr[9]);
}
REGISTER_NATIVE_FUNCTION(0x804BD220, MH3_IOSIoctlvAsync);

// Boot's SCCheckStatus loop is a safe event-pump boundary before OS scheduling
// starts. Preserve the polling caller's full CPU state around each callback.
// Process a bounded batch so callbacks that resubmit cannot starve the caller.
void PumpIosCompletions(CpuContext* ctx) {
    static thread_local bool pumping = false;
    if (pumping) return;
    pumping = true;
    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{pumping};
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

// MH3 polls DVD command status while waiting for module/asset reads. Deliver
// queued IOS completions before querying the original SDK state machine.
extern "C" void func_804AACC0(CpuContext*);
extern "C" void MH3_DVDGetCommandBlockStatus(CpuContext* ctx) {
    PumpIosCompletions(ctx);
    func_804AACC0(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x804AACC0, MH3_DVDGetCommandBlockStatus, "MH3_DVDGetCommandBlockStatus");

// DVDGetDriveStatus (0x804AAD90), decompiled with Ghidra. The frame counter's
// retrace callbacks sample it every retrace.
extern "C" void MH3_DVDGetDriveStatus(CpuContext* ctx) {
    enum : int32_t {
        kFatalError = -0x3D74,    // s32 FatalErrorFlag
        kPausing = -0x3D78,       // s32 PausingFlag
        kExecuting = -0x3CF0,     // DVDCommandBlock* executing
    };
    constexpr uint32_t kDummyCommandBlock = 0x80748A20;
    const CpuContext saved = *ctx;
    const uint32_t sda = saved.gpr[13];
    ctx->msr &= ~0x8000u; // OSDisableInterrupts (inlined)
    uint32_t status;
    if (Memory::Read32(sda + uint32_t(kFatalError))) status = 0xFFFFFFFF;
    else if (Memory::Read32(sda + uint32_t(kPausing))) status = 8; // DVD_STATE_PAUSING
    else {
        const uint32_t executing = Memory::Read32(sda + uint32_t(kExecuting));
        if (!executing || executing == kDummyCommandBlock) status = 0;
        else {
            ctx->gpr[3] = executing;
            InvokeIndirectCpu(0x804AACC0, ctx); // DVDGetCommandBlockStatus
            status = ctx->gpr[3];
        }
    }
    *ctx = saved; // OSRestoreInterrupts
    ctx->gpr[3] = status;
}
REGISTER_NATIVE_FUNCTION_AS(0x804AAD90, MH3_DVDGetDriveStatus, "DVDGetDriveStatus");
