// Development option strings. Each development tool reads one
// variable holding a comma-separated list of items: a bare word (a flag the
// tool knows, or a list entry such as a path or a port name) or name=value:
//
//   MH3_CAPTURE=logs/frame.bmp,interval=150
//   MH3_PORT_CHECK=VecAddInPlace,YureChain,every
//
// `known` names the tool's flags and options; an unknown name=value item is
// reported, and so is an unknown bare word when the tool takes no list.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class DevOptions {
public:
    enum class Words {
        None,   // every item is a known flag or option
        Allowed // other bare words form the tool's list (Words())
    };

    DevOptions(const char* variable, std::initializer_list<std::string_view> known, Words words = Words::None)
        : variable_(variable) {
        Parse(known, words);
    }

    // `known` kept in an array that other code reads too.
    template <size_t N>
    DevOptions(const char* variable, const std::array<std::string_view, N>& known) : variable_(variable) {
        Parse(known, Words::None);
    }

    // The variable is set and not empty.
    bool Set() const { return set_; }
    // A bare flag.
    bool Has(std::string_view flag) const {
        for (const std::string& f : flags_)
            if (f == flag)
                return true;
        return false;
    }

    // The value of name=value (the last one given), or nullptr.
    const char* Value(std::string_view name) const {
        for (auto it = options_.rbegin(); it != options_.rend(); ++it)
            if (it->first == name)
                return it->second.c_str();
        return nullptr;
    }

    // A numeric value (decimal, or 0x hexadecimal), or `fallback` when absent.
    uint64_t Number(std::string_view name, uint64_t fallback) const {
        const char* value = Value(name);

        if (!value)
            return fallback;

        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(value, &end, 0);

        if (end == value || *end || *value == '-') {
            std::fprintf(stderr, "MH3: %s: %.*s=%s is not a number; using %llu\n", variable_, int(name.size()), name.data(), value, static_cast<unsigned long long>(fallback));
            return fallback;
        }

        return parsed;
    }

    // The bare words that are not flags, in order.
    const std::vector<std::string>& List() const { return words_; }
    bool Listed(std::string_view word) const {
        for (const std::string& w : words_)
            if (w == word)
                return true;
        return false;
    }

private:
    template <typename Known>
    void Parse(const Known& known, Words words) {
        const char* text = std::getenv(variable_);

        if (!text)
            return;

        set_ = *text != 0;
        const std::string_view all(text);

        for (size_t at = 0; at < all.size();) {
            size_t end = all.find(',', at);

            if (end == std::string_view::npos)
                end = all.size();

            const std::string_view item = all.substr(at, end - at);
            at = end + 1;

            if (item.empty())
                continue;

            const size_t equals = item.find('=');
            const std::string_view name = item.substr(0, equals);
            bool listed = false;

            for (const std::string_view k : known)
                listed |= k == name;

            if (equals != std::string_view::npos) {
                if (!listed)
                    Unknown(item);

                options_.emplace_back(std::string(name), std::string(item.substr(equals + 1)));
            } else if (listed) {
                flags_.emplace_back(item);
            } else {
                if (words == Words::None)
                    Unknown(item);

                words_.emplace_back(item);
            }
        }
    }

    void Unknown(std::string_view item) const {
        std::fprintf(stderr, "MH3: %s: unknown option '%.*s'\n", variable_, int(item.size()), item.data());
    }

    const char* variable_;
    bool set_ = false;
    std::vector<std::string> flags_, words_;
    std::vector<std::pair<std::string, std::string>> options_;
};

// Families read in more than one place; each is parsed (and reported) once.

// MH3_CAPTURE=[PATH][,interval=N][,start=N][,frame=N][,audio=FILE][,ram=N[,ram-frames=N]]:
// frame dumps (graphics_host.cpp), the audio output (audio_output.cpp) and
// RAM dumps (frame_pacer.cpp).
inline const DevOptions& CaptureOptions() {
    static const DevOptions options("MH3_CAPTURE", {"interval", "start", "frame", "audio", "ram", "ram-frames"}, DevOptions::Words::Allowed);
    return options;
}

// MH3_PROFILE=[split][,render][,sync][,skip=KIND[+KIND...]][,fill][,disc][,census]:
// the per-draw split timers (game_frame.cpp), the render thread's sampled split,
// serial frames and dropped draw kinds (native_frame_packet.cpp), material
// geometry screen area (native_material.cpp), the disc image's read speed
// (disc_image.cpp) and the census of the model draws (native_material.cpp).
inline const DevOptions& ProfileOptions() {
    static const DevOptions options("MH3_PROFILE", {"split", "render", "sync", "skip", "fill", "disc", "census"});
    return options;
}

// MH3_TRACE=[input][,monsters][,actions][,particles][,loading][,effect-objects][,net][,threads]:
// the input as the game read it once per tick of game time (frame_pacer.cpp),
// each monster's work and position once a second (field_camera.cpp), the
// actions monsters start (monster_control.cpp), every third tick of game
// time the live particles of each emitter resource (effect_step.cpp), each
// loading-screen frame's progress counters (load_progress.cpp), each effect
// object update callback and kind the first time it runs (field_lists.cpp),
// the network devices' requests, sockets, lookups and TLS sessions
// (ios_net.cpp), every guest thread switch (guest_threads.cpp) and how far
// the game's random streams moved per tick (game_step.cpp).
inline constexpr auto kTraceItems = std::to_array<std::string_view>({"input", "monsters", "actions", "particles", "loading", "effect-objects", "net", "threads", "demo", "movie", "rand"});

inline const DevOptions& TraceOptions() {
    static const DevOptions options("MH3_TRACE", kTraceItems);
    return options;
}

// A MH3_TRACE item as a switch that the debug port turns on and off while the
// game runs (`trace ITEM on|off`): a site keeps one and tests it each time.
class TraceFlag {
public:
    explicit TraceFlag(std::string_view item) : on_(Switch(item)) {}

    operator bool() const { return on_ && on_->load(std::memory_order_relaxed); }

    // The switch of an item, nullptr for an unknown one; each starts as
    // MH3_TRACE has it.
    static std::atomic<bool>* Switch(std::string_view item) {
        static std::atomic<bool> switches[kTraceItems.size()];
        [[maybe_unused]] static const bool loaded = [] {
            for (size_t i = 0; i < kTraceItems.size(); ++i)
                switches[i] = TraceOptions().Has(kTraceItems[i]);

            return true;
        }();

        for (size_t i = 0; i < kTraceItems.size(); ++i)
            if (kTraceItems[i] == item)
                return &switches[i];

        return nullptr;
    }

private:
    std::atomic<bool>* on_;
};

