#pragma once
// The owners of native draw state: the game thread records draws against
// Game; the render thread restores each packet's snapshots into Render,
// prepares deferred model draws against it and draws. Shadow is a logical-only
// copy the game thread uses to check deferred preparation against the original
// (MH3_RENDER_PREP=2); it never touches the graphics API. Code reached from
// several owners names its owner; nothing looks up the thread.
enum class NativeOwner : unsigned { Game, Render, Shadow };
constexpr unsigned NativeOwnerCount = 3;
