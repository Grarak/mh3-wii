// Included inside ios.cpp's anonymous namespace. Vita's exFAT filesystem has
// neither xattrs nor openat. Keep Wii attributes in a separate journal beside
// NAND, so host metadata does not count as guest files or NAND clusters.
struct NativeNandHandle { std::string path; bool directory; };
std::map<int, NativeNandHandle> nativeNandHandles;
std::map<std::string, NandMetadata> nativeNandMetadata;
std::filesystem::path nativeMetadataRoot;
int nativeDirectoryHandle = 0x20000000;

int NativeIoError(int result) {
    switch (uint32_t(result)) {
    case 0x80010002: return -106; // ENOENT
    case 0x80010011: return -105; // EEXIST
    case 0x8001000D: return -102; // EACCES
    case 0x8001001C: return -108; // ENOSPC
    default: return -101;
    }
}

bool NativeValidPath(const std::string& path, bool root = false) {
    return ValidNandPath(path, root) && path.find(':') == path.npos && path.find('\\') == path.npos;
}

std::filesystem::path NativeNandPath(const std::string& path) { return NandRoot() / path.substr(1); }

int SyncNandFile(int) {
    const auto root = NandRoot().string();
    const auto colon = root.find(':');

    if (colon == root.npos) return -101;
    const int result = sceIoSync(root.substr(0, colon + 1).c_str(), 0);
    return result >= 0 ? 0 : NativeIoError(result);
}

int EnsureNativeDirectory(const std::filesystem::path& path) {
    const auto text = path.string();

    // Mount roots already exist and cannot be created as ordinary directories.
    if (text.empty() || text.back() == ':' || text.back() == '/') return 0;
    SceIoStat info{};
    const int result = sceIoGetstat(text.c_str(), &info);

    if (result >= 0) return SCE_S_ISDIR(info.st_mode) ? 0 : -102;

    if (uint32_t(result) != 0x80010002) return NativeIoError(result);
    const int parent = EnsureNativeDirectory(path.parent_path());

    if (parent) return parent;
    const int created = sceIoMkdir(text.c_str(), 0777);
    return created >= 0 ? 0 : NativeIoError(created);
}

void LoadNativeMetadata() {
    const auto root = NandRoot().parent_path();

    if (nativeMetadataRoot == root) return;
    nativeNandMetadata.clear();
    nativeMetadataRoot.clear();
    const auto path = root / "nand-metadata.bin";
    const auto backup = root / "nand-metadata.bak";

    // A power interruption between the two renames leaves the previous journal.
    const auto source = std::filesystem::exists(path) ? path : backup;

    if (std::filesystem::exists(source)) {
        std::ifstream input(source, std::ios::binary);
        std::array<char, 6> magic{};
        input.read(magic.data(), magic.size());

        if (magic != std::array<char, 6>{'M', 'H', '3', 'M', '1', '\0'})
            throw std::runtime_error("Invalid Vita NAND metadata journal");

        unsigned entries = 0;

        for (int length; (length = input.get()) != EOF;) {
            if (length < 1 || length >= 64 || ++entries > 10000)
                throw std::runtime_error("Invalid Vita NAND metadata record");

            std::string guest(length, '\0');
            NandMetadata attributes{};
            input.read(guest.data(), guest.size());
            input.read(reinterpret_cast<char*>(attributes.data()), attributes.size());

            if (!input || !NativeValidPath(guest, true))
                throw std::runtime_error("Truncated Vita NAND metadata record");

            nativeNandMetadata[guest] = attributes;
        }

        if (!input.eof()) throw std::runtime_error("Vita NAND metadata read failed");
    }

    nativeMetadataRoot = root;
}

int SaveNativeMetadata() {
    const auto path = nativeMetadataRoot / "nand-metadata.bin";
    const auto staged = nativeMetadataRoot / "nand-metadata.tmp";
    const auto backup = nativeMetadataRoot / "nand-metadata.bak";
    {
        std::ofstream output(staged, std::ios::binary | std::ios::trunc);
        output.write("MH3M1\0", 6);

        for (const auto& [guest, attributes] : nativeNandMetadata) {
            output.put(char(guest.size()));
            output.write(guest.data(), guest.size());
            output.write(reinterpret_cast<const char*>(attributes.data()), attributes.size());
        }

        output.flush();

        if (!output) {
            std::fprintf(stderr, "MH3 NAND journal write failed: %s, errno %d\n", staged.c_str(), errno);
            return -101;
        }
    }

    const int synced = SyncNandFile(-1);

    if (synced) return synced;
    const bool existed = std::filesystem::exists(path);

    if (existed) {
        ::unlink(backup.c_str());

        if (::rename(path.c_str(), backup.c_str()) != 0) return Error();
    }

    if (::rename(staged.c_str(), path.c_str()) != 0) {
        const int error = Error();

        if (existed) ::rename(backup.c_str(), path.c_str());
        return error;
    }

    ::unlink(backup.c_str());
    return 0;
}

int SetMetadata(int fd, const NandMetadata& metadata) {
    LoadNativeMetadata();
    const auto handle = nativeNandHandles.find(fd);

    if (handle == nativeNandHandles.end()) return -101;
    nativeNandMetadata[handle->second.path] = metadata;
    return SaveNativeMetadata();
}

int GetMetadata(int fd, NandMetadata& metadata) {
    LoadNativeMetadata();
    const auto handle = nativeNandHandles.find(fd);

    if (handle == nativeNandHandles.end()) return -101;
    const auto found = nativeNandMetadata.find(handle->second.path);
    metadata = found == nativeNandMetadata.end() ? defaultMetadata : found->second;
    return 0;
}

int CloseNandPath(int fd) {
    const auto found = nativeNandHandles.find(fd);

    if (found == nativeNandHandles.end()) { errno = EBADF; return -1; }
    const bool directory = found->second.directory;
    nativeNandHandles.erase(found);
    return directory ? 0 : ::close(fd);
}

int OpenNandPath(const std::string& path, int flags, bool allowRoot = false) {
    if (!NativeValidPath(path, allowRoot)) return -4;
    const auto host = NativeNandPath(path);
    SceIoStat info{};
    const int result = sceIoGetstat(host.c_str(), &info);

    if (result < 0) return NativeIoError(result);
    const bool directory = SCE_S_ISDIR(info.st_mode);

    if (!directory && !SCE_S_ISREG(info.st_mode)) return -102;

    if ((flags & O_DIRECTORY) && !directory) return -101;
    const int fd = directory ? nativeDirectoryHandle++ : ::open(host.c_str(), flags & ~O_NOFOLLOW);

    if (fd < 0) return Error();
    nativeNandHandles.emplace(fd, NativeNandHandle{path, directory});
    return fd;
}

int InitializeNand() {
    const int root = EnsureNativeDirectory(NandRoot());

    if (root) return root;
    LoadNativeMetadata();
    constexpr const char* directories[]{"/tmp", "/sys", "/shared2", "/title",
        "/title/00010000", "/title/00010001", "/title/00010004",
        "/title/00010000/524d4850", "/title/00010000/524d4850/data"};
    for (const char* path : directories) {
        const auto host = NativeNandPath(path);
        SceIoStat info{};

        if (sceIoGetstat(host.c_str(), &info) >= 0) {
            if (!SCE_S_ISDIR(info.st_mode)) return -102;
            continue;
        }

        const int result = sceIoMkdir(host.c_str(), 0777);

        if (result < 0) {
            std::fprintf(stderr, "MH3 NAND directory create failed: %s\n", host.c_str());
            return NativeIoError(result);
        }

        nativeNandMetadata[path] = defaultMetadata;
    }

    return SaveNativeMetadata();
}

int CreateNandEntry(const std::string& path, bool directory, const NandMetadata& metadata) {
    if (!NativeValidPath(path)) return -101;
    const auto host = NativeNandPath(path);
    const int result = directory ? sceIoMkdir(host.c_str(), 0777) : ::open(host.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);

    if (result < 0) return directory ? NativeIoError(result) : Error();

    if (!directory) ::close(result);
    LoadNativeMetadata();
    nativeNandMetadata[path] = metadata;
    return SaveNativeMetadata();
}

bool PathInTree(const std::string& path, const std::string& tree) {
    return path == tree || (path.starts_with(tree) && path.size() > tree.size() && path[tree.size()] == '/');
}

int RenameNandEntry(const std::string& from, const std::string& to) {
    if (!NativeValidPath(from) || !NativeValidPath(to)) return -101;
    const int result = sceIoRename(NativeNandPath(from).c_str(), NativeNandPath(to).c_str());

    if (result < 0) return NativeIoError(result);
    LoadNativeMetadata();
    std::map<std::string, NandMetadata> moved;

    for (auto it = nativeNandMetadata.begin(); it != nativeNandMetadata.end();) {
        if (PathInTree(it->first, from)) {
            moved[to + it->first.substr(from.size())] = it->second;
            it = nativeNandMetadata.erase(it);
        } else if (PathInTree(it->first, to)) it = nativeNandMetadata.erase(it);
        else ++it;
    }

    nativeNandMetadata.merge(moved);

    for (auto& [fd, handle] : nativeNandHandles)
        if (PathInTree(handle.path, from)) handle.path = to + handle.path.substr(from.size());

    return SaveNativeMetadata();
}

int NativeWalkTree(const std::filesystem::path& path, uint64_t& clusters, uint64_t& inodes,
                   bool execute, unsigned depth = 0) {
    if (depth > 64) return -101;
    SceIoStat info{};
    int result = sceIoGetstat(path.c_str(), &info);

    if (result < 0) return NativeIoError(result);
    ++inodes;

    if (SCE_S_ISREG(info.st_mode)) {
        clusters += (uint64_t(info.st_size) + 0x3fff) / 0x4000;
        result = execute ? sceIoRemove(path.c_str()) : 0;
    } else if (SCE_S_ISDIR(info.st_mode)) {
        const int dir = sceIoDopen(path.c_str());

        if (dir < 0) return NativeIoError(dir);
        SceIoDirent entry{};

        while ((result = sceIoDread(dir, &entry)) > 0) {
            const std::string_view name(entry.d_name);

            if (name == "." || name == "..") continue;
            result = NativeWalkTree(path / entry.d_name, clusters, inodes, execute, depth + 1);

            if (result) break;
        }

        sceIoDclose(dir);

        if (result == 0 && execute) result = sceIoRmdir(path.c_str());
    } else return -102;
    return result >= 0 ? 0 : (result > -1000 ? result : NativeIoError(result));
}

int DeleteNandEntry(const std::string& path) {
    if (!NativeValidPath(path)) return -101;

    for (const auto& [handle, file] : files) {
        const auto found = nativeNandHandles.find(file.host);

        if (found != nativeNandHandles.end() && PathInTree(found->second.path, path)) return -111;
    }

    uint64_t clusters = 0, inodes = 0;
    const auto host = NativeNandPath(path);
    int result = NativeWalkTree(host, clusters, inodes, false);

    if (!result) result = NativeWalkTree(host, clusters, inodes, true);

    if (result) return result;
    LoadNativeMetadata();

    for (auto it = nativeNandMetadata.begin(); it != nativeNandMetadata.end();)
        it = PathInTree(it->first, path) ? nativeNandMetadata.erase(it) : std::next(it);

    return SaveNativeMetadata();
}

int OpenFile(const std::string& path, uint32_t mode) {
    if (mode < 1 || mode > 3) return -4;
    const int fd = OpenNandPath(path, mode == 1 ? O_RDONLY : mode == 2 ? O_WRONLY : O_RDWR);

    if (fd < 0) return fd;

    if (nativeNandHandles.at(fd).directory) { CloseNandPath(fd); return -101; }
    NandMetadata metadata{};
    int result = GetMetadata(fd, metadata);
    const uint32_t owner = uint32_t(metadata[0]) << 24 | uint32_t(metadata[1]) << 16 | uint32_t(metadata[2]) << 8 | metadata[3];
    const uint16_t group = uint16_t(metadata[4]) << 8 | metadata[5];
    const uint8_t allowed = owner == 0x1000 ? metadata[6] : group == 0x3038 ? metadata[7] : metadata[8];

    if (!result && (allowed & mode) != mode) result = -102;

    if (result) { CloseNandPath(fd); return result; }
    const uint32_t handle = nextHandle++;
    files.emplace(handle, File{fd, mode});
    return handle;
}

int DirectoryUsage(int fd, uint64_t& clusters, uint64_t& inodes, unsigned depth = 0) {
    const auto found = nativeNandHandles.find(fd);

    if (found == nativeNandHandles.end()) return -101;
    const auto path = NativeNandPath(found->second.path);
    CloseNandPath(fd);
    return NativeWalkTree(path, clusters, inodes, false, depth);
}
