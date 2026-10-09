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
    MH3_SPLIT_MESH_SUBMIT,
    MH3_SPLIT_FONT,
    MH3_SPLIT_FADE,
    MH3_SPLIT_HUD,
    MH3_SPLIT_MOVIE,
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
#define MH3_SPLIT_JOIN2(a, b) a##b
#define MH3_SPLIT_JOIN(a, b) MH3_SPLIT_JOIN2(a, b)
#define MH3_SPLIT_SCOPE(family) MH3FrameSplitScope MH3_SPLIT_JOIN(mh3SplitScope, __LINE__)(family)
#else
#define MH3_SPLIT_SCOPE(family)
#endif
