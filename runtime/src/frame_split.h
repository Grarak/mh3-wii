// Development-only game-thread frame split (game_frame.cpp). MH3_SPLIT_SCOPE
// adds the scope's wall time to one draw-pass family; nested scopes of the
// same family count once. Release builds compile both away.
#pragma once
#ifdef MH3_DEVELOPMENT
#include <chrono>
#include <cstdint>

enum MH3SplitFamily : unsigned {
    MH3_SPLIT_TRAVERSE,
    MH3_SPLIT_MODEL,
    MH3_SPLIT_EFFECT,
    MH3_SPLIT_NODE_CALLBACK,
    MH3_SPLIT_FONT,
    MH3_SPLIT_FADE,
    MH3_SPLIT_HUD,
    MH3_SPLIT_MOVIE,
    // Parts of the model family: material scope preparation, shape/item
    // preparation (pose, matrices, selectors) and the packet record.
    MH3_SPLIT_MATERIAL_PREP,
    MH3_SPLIT_SHAPE_PREP,
    MH3_SPLIT_MODEL_RECORD,
    MH3_SPLIT_FAMILIES
};
void MH3FrameSplitCall(unsigned kind);
void MH3FrameSplitTime(unsigned family, std::chrono::steady_clock::duration elapsed);
extern uint8_t mh3SplitDepth[MH3_SPLIT_FAMILIES];
extern uint32_t mh3SplitFrame; // frame bodies started
struct MH3FrameSplitScope {
    unsigned family;
    std::chrono::steady_clock::time_point start;
    explicit MH3FrameSplitScope(unsigned value) : family(value) {
        if (!mh3SplitDepth[family]++)
            start = std::chrono::steady_clock::now();
    }
    ~MH3FrameSplitScope() {
        if (!--mh3SplitDepth[family])
            MH3FrameSplitTime(family, std::chrono::steady_clock::now() - start);
    }
};
// Per-draw parts (material/shape preparation, packet record) run about a
// hundred times per frame; timing each costs two clock reads (a system call on
// the Vita, ~1 ms/frame there). MH3_MODEL_SPLIT=1 enables them.
extern const bool mh3SplitDetail;
struct MH3FrameSplitDetailScope {
    unsigned family;
    bool active;
    std::chrono::steady_clock::time_point start;
    explicit MH3FrameSplitDetailScope(unsigned value) : family(value), active(mh3SplitDetail) {
        if (active && !mh3SplitDepth[family]++)
            start = std::chrono::steady_clock::now();
    }
    ~MH3FrameSplitDetailScope() {
        if (active && !--mh3SplitDepth[family])
            MH3FrameSplitTime(family, std::chrono::steady_clock::now() - start);
    }
};
#define MH3_SPLIT_JOIN2(a, b) a##b
#define MH3_SPLIT_JOIN(a, b) MH3_SPLIT_JOIN2(a, b)
#define MH3_SPLIT_SCOPE(family) MH3FrameSplitScope MH3_SPLIT_JOIN(mh3SplitScope, __LINE__)(family)
#define MH3_SPLIT_DETAIL_SCOPE(family) MH3FrameSplitDetailScope MH3_SPLIT_JOIN(mh3SplitScope, __LINE__)(family)
#else
#define MH3_SPLIT_SCOPE(family)
#define MH3_SPLIT_DETAIL_SCOPE(family)
#endif
