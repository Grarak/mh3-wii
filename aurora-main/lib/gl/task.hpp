#pragma once

// A move-only `void()` callable invoked once as an rvalue
// (`std::move(task)()`), for work posted to the renderer. The project builds
// as C++20, which has no std::move_only_function, and std::function needs
// copyable callables while posted work owns move-only state (staging sets,
// a nested task). Callables of up to four pointers that move without
// throwing live inline; larger ones on the heap.

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace aurora::gl {

class Task {
  public:
    Task() noexcept = default;
    Task(std::nullptr_t) noexcept {}
    template <typename F, typename D = std::decay_t<F>,
              typename = std::enable_if_t<!std::is_same_v<D, Task> && std::is_invocable_r_v<void, D&&>>>
    Task(F&& f) {
        if constexpr (kInline<D>) {
            ::new (static_cast<void*>(m_storage)) D(std::forward<F>(f));
        } else {
            *reinterpret_cast<D**>(m_storage) = new D(std::forward<F>(f));
        }

        m_ops = &kOps<D>;
    }

    Task(Task&& other) noexcept { take(other); }
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            reset();
            take(other);
        }

        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() { reset(); }

    explicit operator bool() const noexcept { return m_ops != nullptr; }
    void operator()() && { m_ops->invoke(m_storage); }

  private:
    static constexpr size_t kStorageSize = 4 * sizeof(void*);
    template <typename D>
    static constexpr bool kInline = sizeof(D) <= kStorageSize && alignof(D) <= alignof(std::max_align_t) &&
                                    std::is_nothrow_move_constructible_v<D>;

    struct Ops {
      void (*invoke)(void* storage);

      // Moves the callable from `from` into the empty `to` and ends `from`.
      void (*relocate)(void* to, void* from) noexcept;
      void (*destroy)(void* storage) noexcept;
    };

    template <typename D>
    static D& callable(void* storage) noexcept {
        if constexpr (kInline<D>)
            return *std::launder(reinterpret_cast<D*>(storage));
        else
            return **reinterpret_cast<D**>(storage);
    }

    template <typename D>
    static constexpr Ops kOps{
        [](void* storage) { std::move(callable<D>(storage))(); },
        [](void* to, void* from) noexcept {
            if constexpr (kInline<D>) {
                ::new (to) D(std::move(callable<D>(from)));
                callable<D>(from).~D();
            } else {
                *reinterpret_cast<D**>(to) = *reinterpret_cast<D**>(from);
            }
        },
        [](void* storage) noexcept {
            if constexpr (kInline<D>)
                callable<D>(storage).~D();
            else
                delete *reinterpret_cast<D**>(storage);
        },
    };

    void take(Task& other) noexcept {
        if (other.m_ops != nullptr) {
            other.m_ops->relocate(m_storage, other.m_storage);
            m_ops = other.m_ops;
            other.m_ops = nullptr;
        }
    }

    void reset() noexcept {
        if (m_ops != nullptr) {
            m_ops->destroy(m_storage);
            m_ops = nullptr;
        }
    }

    const Ops* m_ops = nullptr;
    alignas(std::max_align_t) unsigned char m_storage[kStorageSize];
};

} // namespace aurora::gl
