// The web build's files. The page imports the disc image
// into the browser's Origin Private File System (OPFS); WasmFS mounts it at
// /opfs, where the prepared game data and the saves stay between visits:
//
//   /opfs/mh3/disc.rvz    the image the page imported (any name)
//   /opfs/mh3/disc/       disc metadata and converted assets (--rom writes it
//                         "next to the executable", /proc/self/exe below)
//   /opfs/mh3/user/       MH3_USER_DIR: the NAND saves and graphics cache
#include "fatal.h"

#include <emscripten/wasmfs.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

// fsync through WasmFS's own sync, keeping libc's out of the link: libc's
// fsync declares __wasi_fd_sync an import from wasi_snapshot_preview1, which
// WasmFS defines instead, and a ThinLTO link (release builds) stops on the
// mismatch ("import module mismatch for symbol: __wasi_fd_sync").
extern "C" int __syscall_fdatasync(int fd);

extern "C" int fsync(int fd) {
    const int result = __syscall_fdatasync(fd);

    if (result < 0) {
        errno = -result;
        return -1;
    }

    return 0;
}

namespace {
void Directory(const char* path) {
    if (mkdir(path, 0777) != 0 && errno != EEXIST)
        MH3_THROW(std::runtime_error(std::string("Cannot create ") + path + ": " + std::strerror(errno)));
}
} // namespace

// The game thread, first thing in main(): the OPFS backend cannot be created
// on the browser's main thread.
void MountWebFiles() {
    backend_t opfs = wasmfs_create_opfs_backend();

    if (!opfs || wasmfs_create_directory("/opfs", 0777, opfs) != 0)
        MH3_THROW(std::runtime_error("Cannot mount the Origin Private File System"));

    Directory("/opfs/mh3");
    Directory("/opfs/mh3/user");
    Directory("/proc");
    Directory("/proc/self");

    if (symlink("/opfs/mh3/MH3", "/proc/self/exe") != 0 && errno != EEXIST)
        MH3_THROW(std::runtime_error("Cannot link the executable path"));

    setenv("MH3_USER_DIR", "/opfs/mh3/user", 0);
    std::fprintf(stderr, "MH3 web: files in /opfs/mh3 (Origin Private File System)\n");
}
