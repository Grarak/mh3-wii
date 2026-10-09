#pragma once
// The owners of native draw state: the game thread records draws against
// Game; the render thread restores each packet's snapshots into Render,
// prepares deferred model draws against it and draws. Shadow is a logical-only
// copy the game thread uses to check deferred preparation against the original
// (MH3_RENDER_PREP=2); it never touches the graphics API. Code reached from
// several owners names its owner. Draw is the draw thread's copy of Game
// (docs/optimizations/threading/pipelined-draw.md): the scene it records starts from the game's state
// at the handoff (CopyNativeGameStateForDraw), and Game named on the draw
// thread means Draw. Only the state stores look up the thread
// (NativeOwnerIndex); code that tells the recording owner from the render
// thread's keeps comparing with Game.
#include "isa/ppc_isa_thread_state.h"

enum class NativeOwner : unsigned { Game, Render, Shadow, Draw };
constexpr unsigned NativeOwnerCount = 4;

// The storage index of `owner` for the calling thread.
inline unsigned NativeOwnerIndex(NativeOwner owner) noexcept {
    if (owner == NativeOwner::Game && PpcThread::CurrentThread() == PpcThread::Draw)
        return unsigned(NativeOwner::Draw);

    return unsigned(owner);
}

// The handoff: Draw's state (draw, texture, material and palette stores, and
// their revisions) becomes a copy of Game's.
void CopyNativeDrawStoreForDraw();
void CopyNativeTextureStoreForDraw();
void CopyNativeMaterialStoreForDraw();
void CopyNativePaletteStoreForDraw();

inline void CopyNativeGameStateForDraw() {
    CopyNativeDrawStoreForDraw();
    CopyNativeTextureStoreForDraw();
    CopyNativeMaterialStoreForDraw();
    CopyNativePaletteStoreForDraw();
}
