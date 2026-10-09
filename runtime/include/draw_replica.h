// Host-side model draw state has one replica per drawing thread. The game
// thread uses its own; the render thread uses a second one for the model
// instances it draws: catalog provenance, texture and palette objects, vertex
// source revisions and light sets. Game-thread mutations are mirrored to the
// render replica in frame order through the frame packet task queues (before
// the draws of the frame that made them), so the render replica always
// matches the frame it draws. Render-thread lookups only fill its own caches.
#pragma once
#include <functional>
#include <utility>

namespace DrawReplica {
enum Replica : unsigned { Game, Render, Count };
// Replica selection and mutation nesting share one compiler TLS lookup.
// Only the outermost mutation on this thread is mirrored.
struct State {
    unsigned current = Game;
    unsigned depth = 0;
};
inline thread_local constinit State state{};
inline unsigned Current() { return state.current; }
inline void SetCurrent(unsigned replica) { state.current = replica; }
// Queues a mirrored mutation (after: once the frame's draws are done). Set
// by the game thread while the render replica is live, otherwise null.
using Sink = void (*)(std::function<void()> apply, bool after);
inline Sink sink = nullptr;

struct Scope {
    unsigned previous;
    explicit Scope(unsigned replica) {
        auto& local = state;
        previous = local.current;
        local.current = replica;
    }
    ~Scope() { state.current = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
// Marks one host-state mutation. apply repeats it on the render replica; it
// must touch only replicated host state, never guest memory.
class Mutation {
public:
    template <class Apply>
    explicit Mutation(Apply&& apply, bool after = false) {
        auto& local = state;
        if (local.current == Game && !local.depth && sink)
            sink(
                [apply = std::forward<Apply>(apply)] {
                    Scope scope(Render);
                    apply();
                },
                after);
        ++local.depth;
    }
    ~Mutation() { --state.depth; }
    Mutation(const Mutation&) = delete;
    Mutation& operator=(const Mutation&) = delete;
};
} // namespace DrawReplica
