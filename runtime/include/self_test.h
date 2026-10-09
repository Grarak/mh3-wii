#pragma once

#include <stdexcept>

// The self-tests' negative checks: whether an operation is rejected with an
// Exception. Development builds keep exceptions. Release builds have none and
// a rejection stops the process there, so CheckRejections is false: a check
// that expects a rejection is skipped, and an operation expected to pass just
// runs (Rejects returns false).
#if defined(__cpp_exceptions)
inline constexpr bool CheckRejections = true;

template <class Exception = std::runtime_error, class Operation>
bool Rejects(Operation&& operation) {
    try {
        operation();
    } catch (const Exception&) {
        return true;
    }

    return false;
}
#else
inline constexpr bool CheckRejections = false;

template <class Exception = std::runtime_error, class Operation>
bool Rejects(Operation&& operation) {
    operation();
    return false;
}
#endif
