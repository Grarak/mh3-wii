#pragma once

#include <stdexcept>
#include <string>
#include <utility>

// The explicit error path of an operation whose caller may recover from its
// failure: success, or the message that says what failed. Nothing recovers
// from an exception; a caller that cannot go on reports the message as a fatal
// error instead.
class [[nodiscard]] Status {
  public:
    Status() = default;

    static Status Error(std::string message) {
        Status status;
        status.failed = true;
        status.text = std::move(message);
        return status;
    }

    bool ok() const noexcept { return !failed; }

    const std::string& message() const noexcept { return text; }

  private:
    std::string text;
    bool failed = false;
};

// Returns a failed Status from the enclosing function.
#define MH3_RETURN_IF_ERROR(expression)                    \
    do {                                                   \
        if (Status mh3_status_ = (expression); !mh3_status_.ok()) \
            return mh3_status_;                            \
    } while (0)

// For a caller that cannot go on: a failure stops the game with its message.
inline void CheckStatus(const Status& status) {
    if (!status.ok())
        throw std::runtime_error(status.message());
}
