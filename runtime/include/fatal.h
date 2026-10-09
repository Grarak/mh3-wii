#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>

// What main sets to add to a fatal report: the guest CPU state, and the
// report in the crash log (on the Vita stderr reaches only a devkit's TTY).
inline void (*fatalReportHook)(const char* what) = nullptr;

// Stops the game with a message, as an exception that reached main's report
// did: the message, then the hook's report, then exit status 1, at once (other
// threads are running, so no static destructors). A second failure, during the
// report or on another thread, leaves the first one's report.
[[noreturn]] inline void MH3Fatal(const char* what) noexcept {
    static std::atomic<bool> stopping{false};

    if (stopping.exchange(true)) {
        std::fprintf(stderr, "MH3 runtime stopped again: %s\n", what);
        std::fflush(nullptr);
        std::_Exit(1);
    }

    std::fprintf(stderr, "MH3 runtime stopped: %s\n", what);
    std::fflush(stderr);

    if (fatalReportHook)
        fatalReportHook(what);

    std::fflush(nullptr);
    std::_Exit(1);
}

// A failure that stops the game. Builds with exceptions (development) throw
// it on to main's report, where a self-test may expect it; builds without
// them (release) stop at the throw site with the same report.
#if defined(__cpp_exceptions)
#define MH3_THROW(...) throw __VA_ARGS__
#else
#define MH3_THROW(...) MH3Fatal((__VA_ARGS__).what())
#endif
