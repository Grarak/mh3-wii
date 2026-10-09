#pragma once

#include "status.h"
#include <filesystem>
#include <string>
#include <system_error>

// std::filesystem operations whose failure a caller recovers from: the
// error_code forms, with the message libstdc++'s filesystem_error would carry.
namespace FsStatus {
inline Status Failure(const char* operation, const std::error_code& error, const std::filesystem::path& first, const std::filesystem::path* second = nullptr) {
    std::string message = std::string("filesystem error: ") + operation + ": " + error.message() + " [" + first.string() + "]";

    if (second)
        message += " [" + second->string() + "]";

    return Status::Error(std::move(message));
}

inline Status Exists(const std::filesystem::path& path, bool& exists) {
    std::error_code error;
    exists = std::filesystem::exists(path, error);
    return error ? Failure("status", error, path) : Status{};
}

inline Status Rename(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code error;
    std::filesystem::rename(from, to, error);
    return error ? Failure("cannot rename", error, from, &to) : Status{};
}

inline Status RemoveAll(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    return error ? Failure("cannot remove all", error, path) : Status{};
}

inline Status CreateDirectories(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return error ? Failure("cannot create directories", error, path) : Status{};
}

inline Status Absolute(const std::filesystem::path& path, std::filesystem::path& absolute) {
    std::error_code error;
    absolute = std::filesystem::absolute(path, error);
    return error ? Failure("cannot make absolute path", error, path) : Status{};
}
} // namespace FsStatus
