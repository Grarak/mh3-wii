// Development option strings (docs/flags.md). Each development tool reads one
// variable holding a comma-separated list of items: a bare word (a flag the
// tool knows, or a list entry such as a path or a port name) or name=value:
//
//   MH3_CAPTURE=logs/frame.bmp,interval=150
//   MH3_PORT_CHECK=VecAddInPlace,YureChain,every
//
// `known` names the tool's flags and options; an unknown name=value item is
// reported, and so is an unknown bare word when the tool takes no list.
#pragma once

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

    DevOptions(const char* variable, std::initializer_list<std::string_view> known, Words words = Words::None) : variable_(variable) {
        const char* text = std::getenv(variable);

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
    void Unknown(std::string_view item) const {
        std::fprintf(stderr, "MH3: %s: unknown option '%.*s' (docs/flags.md)\n", variable_, int(item.size()), item.data());
    }

    const char* variable_;
    bool set_ = false;
    std::vector<std::string> flags_, words_;
    std::vector<std::pair<std::string, std::string>> options_;
};

// Variables that a family replaced or that were removed: reported at startup,
// so that a stale script or runtime.env does not silently lose its effect.
inline void ReportRetiredVariables() {
    static constexpr struct {
        const char* name;
        const char* family; // nullptr: removed
    } kRetired[] = {
        {"MH3_CAPTURE_PATH", "MH3_CAPTURE"},
        {"MH3_CAPTURE_INTERVAL", "MH3_CAPTURE"},
        {"MH3_CAPTURE_START", "MH3_CAPTURE"},
        {"MH3_CAPTURE_FRAME", "MH3_CAPTURE"},
        {"MH3_AUDIO_DUMP", "MH3_CAPTURE"},
        {"MH3_PORT_CHECK_EVERY", "MH3_PORT_CHECK"},
        {"MH3_PORT_CHECK_DUMP", "MH3_PORT_CHECK"},
        {"MH3_PORT_FUZZ", "MH3_PORT_CHECK"},
        {"MH3_PORT_FUZZ_AFTER", "MH3_PORT_CHECK"},
        {"MH3_PORT_FUZZ_SAMPLES", "MH3_PORT_CHECK"},
        {"MH3_SOUND_ORIGINALS", "MH3_PORT_CHECK"},
        {"MH3_RAZOR_LIVE", "MH3_RAZOR"},
        {"MH3_RAZOR_LIVE_START_FRAME", "MH3_RAZOR"},
        {"MH3_RAZOR_LIVE_FRAMES", "MH3_RAZOR"},
        {"MH3_RAZOR_GROUP", "MH3_RAZOR"},
        {"MH3_RAZOR_HIDE_HUD", "MH3_RAZOR"},
        {"MH3_RAZOR_SCENE_TRACE", "MH3_RAZOR"},
        {"MH3_RAZOR_CPU_TRACE", "MH3_RAZOR"},
        {"MH3_RAZOR_CAPTURE", "MH3_RAZOR"},
        {"MH3_RAZOR_CAPTURE_FRAME", "MH3_RAZOR"},
        {"MH3_RAZOR_SALVAGE", "MH3_RAZOR"},
        {"MH3_STEP_PROBE_TICK", "MH3_STEP_PROBE"},
        {"MH3_STEP_PROBE_SETTLE", "MH3_STEP_PROBE"},
        {"MH3_STEP_PROBE_ALL", "MH3_STEP_PROBE"},
        {"MH3_WRITE_WATCH_LIST", "MH3_WRITE_WATCH"},
        {"MH3_WRITE_WATCH_MS", "MH3_WRITE_WATCH"},
        {"MH3_CALL_TRACE_GATE", "MH3_CALL_TRACE"},
        {"MH3_PROFILE_FRAMES", "MH3_PROFILE"},
        {"MH3_MODEL_SPLIT", "MH3_PROFILE"},
        {"MH3_RENDER_SYNC", "MH3_PROFILE"},
        {"MH3_ANIM_WHOLE_FRAMES", nullptr},
        {"MH3_EFFECT_TICK_CURVES", nullptr},
        {"MH3_STEP_COUNTER_RAW", nullptr},
        {"MH3_FIELD_WAIT", nullptr},
        {"MH3_NATIVE_MODEL_DRAW", nullptr},
        {"MH3_MODEL_TRACE", nullptr},
        {"MH3_MODEL_POSE_TRACE", nullptr},
        {"MH3_GX_DRAW_LOG", nullptr},
        {"MH3_GX_SOURCE_CAPTURE", nullptr},
        {"MH3_GX_DEBUG_CONTROL", nullptr},
        {"MH3_TRACE_EFB", nullptr},
        {"MH3_MISSING_TARGET_MEM1_DUMP", nullptr},
        {"MH3_MOTION_LOG", nullptr},
        {"MH3_ROOT_CACHE_CHECK", nullptr},
        {"MH3_ANM_KEYS_CHECK", nullptr},
        {"MH3_ANM_BAKED_CHECK", nullptr},
        {"MH3_EFFECT_CURVE_CHECK", nullptr},
        {"MH3_TICK_SURVEY", nullptr},
        {"MH3_STEP_LOG", nullptr},
        {"MH3_STEP_LOG_WORDS", nullptr},
        {"MH3_SLOW_PATH_TRACE", nullptr},
        {"MH3_AUDIO_TRACE", nullptr},
    };

    for (const auto& retired : kRetired) {
        if (!std::getenv(retired.name))
            continue;

        if (retired.family)
            std::fprintf(stderr, "MH3: %s is ignored; it is now part of %s (docs/flags.md)\n", retired.name, retired.family);
        else
            std::fprintf(stderr, "MH3: %s is ignored; it was removed (docs/flags.md)\n", retired.name);
    }
}

// Families read in more than one place; each is parsed (and reported) once.

// MH3_CAPTURE=[PATH][,interval=N][,start=N][,frame=N][,audio=FILE]: frame
// dumps (graphics_host.cpp) and the audio output (audio_output.cpp).
inline const DevOptions& CaptureOptions() {
    static const DevOptions options("MH3_CAPTURE", {"interval", "start", "frame", "audio"}, DevOptions::Words::Allowed);
    return options;
}

// MH3_PROFILE=[frames=N][,split][,sync][,skip=KIND[+KIND...]][,fill][,disc]:
// the frame profile and GX counters every N frames (graphics_host.cpp), the
// per-draw split timers (game_frame.cpp), serial frames and dropped draw kinds
// (native_frame_packet.cpp), material geometry screen area (native_material.cpp),
// the disc image's read speed (rom_image.cpp).
inline const DevOptions& ProfileOptions() {
    static const DevOptions options("MH3_PROFILE", {"frames", "split", "sync", "skip", "fill", "disc"});
    return options;
}

// MH3_VERIFY=[pose]: in-run checks that state reused instead of copied equals
// a fresh copy: `pose` compares every reused pose version with the live view
// arrays (model_pose.cpp).
inline const DevOptions& VerifyOptions() {
    static const DevOptions options("MH3_VERIFY", {"pose"});
    return options;
}
