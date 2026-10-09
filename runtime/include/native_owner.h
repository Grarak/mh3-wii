#pragma once
// The two owners of native draw state: the game thread records draws against
// Game; the render thread restores each packet's snapshots into Render and
// draws. Code reached from both names its owner; nothing looks up the thread.
enum class NativeOwner : unsigned { Game, Render };
