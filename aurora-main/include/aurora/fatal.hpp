#pragma once

#include <cstdio>
#include <cstdlib>

namespace aurora {
// The embedding program's report of a failure that stops it (AURORA_THROW in
// a build without exceptions). Without one, or if it returns, the message goes
// to stderr and the process aborts.
using FatalHandler = void (*)(const char* what);
inline FatalHandler fatal_handler = nullptr;

[[noreturn]] inline void fatal_exception(const char* what) noexcept {
  if (fatal_handler)
    fatal_handler(what);

  std::fprintf(stderr, "[aurora] fatal: %s\n", what);
  std::fflush(nullptr);
  std::abort();
}
} // namespace aurora

// A failure that stops the program. Builds with exceptions throw it; builds
// without them (release) report it through aurora::fatal_handler.
#if defined(__cpp_exceptions)
#define AURORA_THROW(...) throw __VA_ARGS__
#else
#define AURORA_THROW(...) ::aurora::fatal_exception((__VA_ARGS__).what())
#endif
