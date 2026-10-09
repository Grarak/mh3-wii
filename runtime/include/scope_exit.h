#pragma once
#include <utility>

// Runs a cleanup when the scope ends, however it ends: by returning, or (in
// development builds, which keep exceptions) by an exception passing through
// on its way to the report.
template <class Cleanup>
class ScopeExit {
  public:
    explicit ScopeExit(Cleanup cleanup) : cleanup(std::move(cleanup)) {}
    ~ScopeExit() { cleanup(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

  private:
    Cleanup cleanup;
};
