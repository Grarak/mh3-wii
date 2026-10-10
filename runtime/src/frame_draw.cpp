// The frame's draw, RMHP08: 0x80049308, the frame body's DrawScene call
// (game_frame.cpp), decompiled with Ghidra. While the game runs and a scene
// root exists, it draws the UI sprites behind the scene, the background (a
// grabbed screen or the background texture callback), then the scene between
// the 2D layers: layers 0 to 1 (0 to 4 online in mode 3), the scene (the
// split scene for two players, native_scene_queue.cpp), the rest up to 11;
// in the preview camera mode layers 0 to 7, the preview camera, the scene,
// the camera reset, the scene's view matrices again (0x80082D40), layers 7 to
// 11. Then the UI sprites in front and 0x80048828. MH3_PROFILE=split times
// the 2D layers and the UI parts (the layers and ui figures of the MH3 draw
// split line); MH3_PORT_CHECK=DrawFrame checks the port. In the split draw
// (FrameDraw::SetSceneHandoff, docs/optimizations/threading/pipelined-draw.md) the scene is left to
// the draw thread: at its place the handoff runs and the scene is noted, and
// FrameDraw::RunDeferredScene draws it there.
//
// The 2D layers (0x8004925C, DrawLayers(first, end)): each layer is a list
// of draw entries (the table at 0x8066C3B8; an entry's function at +0, an
// active byte at +4, its argument byte at +5, its data from +8, the next
// entry at +0x28), and the active ones' functions run as function(argument,
// data); layers 8 and 9 flush the fonts first. With MH3_PROFILE=split the
// MH3 layer split line shows the time of each entry function and of the
// font flush (0x8005CA08).
#include "abi_bridge.h"
#include "fatal.h"
#include "frame_split.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <stdexcept>
#include <utility>
#ifdef MH3_DEVELOPMENT
#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#endif

namespace {

constexpr uint32_t kSystem = 0x8065AA60; // game system flags
enum : uint32_t {
    kRunning = 0x21,      // u8
    kOnlineOnly = 0x86F,  // u8: draw only in online mode 1
    kGrabbed = 0x869,     // u8 1: draw the grabbed screen as the background
    kBackground = 0x866,  // u8: run the background texture callback
};

enum : int32_t {
    kRoot = -0x44AC,               // r13: the scene root (_pRoot)
    kBackgroundCallback = -0x45A8, // r13: _bg_tex_disp_func
};

enum : uint32_t {
    GetOnlineMode = 0x800CF26C,
    DrawGrabbedScreen = 0x80058C34, // (1 background, 0 only)
    BeginFrameDraw = 0x80047DD0,
    DrawUISprites = 0x800489C8, // (0 behind, 1 in front)
    TwoPlayers = 0x800470BC,
    PreviewCamera = 0x800D0788,
    DrawLayers = 0x8004925C, // (first, end)
    DrawSplitScene = 0x80049594,
    DrawSceneOnly = 0x80049498,
    PreviewCameraSetup = 0x802BBCB4,
    PreviewCameraReset = 0x802BBE88,
    CalcRootView = 0x80082D40,
    EndFrameDraw = 0x80048828,
};

constexpr uint32_t kLayerLists = 0x8066C3B8;
enum : uint32_t {
    kEntryActive = 0x04, // u8: 1 when drawn
    kEntryArgument = 0x05, // u8
    kEntryData = 0x08,
    kEntryNext = 0x28,
};

constexpr uint32_t FontFlush = 0x8005CA08;
#ifdef MH3_DEVELOPMENT
// Per entry function: its time and calls (MH3 layer split line).
struct LayerTime {
    uint32_t function = 0, calls = 0;
    double ms = 0;
};

std::array<LayerTime, 48> layerTimes{};
#endif

// A layer's call: the font flush or an entry function.
void Timed(GuestCall& call, uint32_t function, std::initializer_list<uint32_t> arguments) {
#ifdef MH3_DEVELOPMENT
    const bool timed = mh3SplitDetail;
    const auto start = timed ? MH3SplitClock::now() : MH3SplitClock::time_point{};
#endif
    call(function, arguments);
#ifdef MH3_DEVELOPMENT
    if (!timed)
        return;

    const double ms = std::chrono::duration<double, std::milli>(MH3SplitClock::now() - start).count();

    for (LayerTime& t : layerTimes) {
        if (t.function == function || t.function == 0) {
            t.function = function;
            t.ms += ms;
            ++t.calls;
            break;
        }
    }
#endif
}

void DrawLayersPort(CpuContext* cpu) {
    const int32_t first = int32_t(cpu->gpr[3]), end = int32_t(cpu->gpr[4]);
    GuestFrame frame(cpu, 0x20);
    GuestCall call{cpu};

    for (int32_t layer = first; layer < end; ++layer) {
        uint32_t entry = Memory::Read32(kLayerLists + 4 * uint32_t(layer));

        if (uint32_t(layer - 8) <= 1)
            Timed(call, FontFlush, {});

        for (; entry; entry = Memory::Read32(entry + kEntryNext)) {
            if (Memory::Read8(entry + kEntryActive) == 1)
                Timed(call, Memory::Read32(entry), {Memory::Read8(entry + kEntryArgument), entry + kEntryData});
        }
    }
}

void Layers(GuestCall& call, uint32_t first, uint32_t end) {
    MH3_SPLIT_SCOPE(MH3_SPLIT_LAYERS);
    call(DrawLayers, {first, end});
}

// The preview camera (an equipment preview, Ghidra): camera 5 of the local
// player's set draws the hunter into the preview window and is put back to
// the full view after it. Only the camera records change; the scene's
// ScnRoot::SetGlbSettings applies them.
constexpr uint32_t kSplitCameras = 0x8065B4D6; // u8: the two players' camera sets
constexpr uint32_t kPlayerIndex = 0x8065AA87;  // s8: MyPlayerIndex
constexpr uint32_t kViewAspect = 0x8065B4C8;   // f32; per player from 0x8065B504 with split cameras
constexpr uint32_t kCameraRecords = 0xF8, kCameraRecordBytes = 268; // ScnRoot's CameraData
enum : uint32_t {
    kCameraFlags = 0x70,       // u32: 0x80 projection valid, 0x20 perspective, 0xF0 the type bits
    kCameraProjection = 0xA8,  // u32 type 0 (perspective), f32 fovy, aspect, near, far
    kCameraViewport = 0xDC,    // f32 x, y, width, height, near, far
    kCameraScissor = 0xF4,     // u32 x, y, width, height
};

enum : uint32_t {
    PreviewRectangle = 0x8021A6C8, // (s16* x, y, width, height): the layout's preview window
    HudScaleX = 0x802D9FD4,
    ViewCameraFovy = 0x802BE024,
};

constexpr uint32_t kHeightScale = 0x8079CAA0; // f32 448/480
constexpr uint32_t kOne = 0x8079C9AC, kPreviewFovy = 0x8079CA64, kNear = 0x8079C9DC, kFar = 0x8079CAA4;

// 0x8004737C: the local player's camera `index` (8 + index for player 0 and
// 16 + index for player 1 with split cameras), ScnRoot::GetCamera.
uint32_t PlayerCamera(uint32_t r13, uint32_t index) {
    if (Memory::Read8(kSplitCameras))
        index += int8_t(Memory::Read8(kPlayerIndex)) == 0 ? 8 : 16;

    const uint32_t root = Memory::Read32(r13 + kRoot);

    if (!root)
        MH3_THROW(std::runtime_error("Preview camera without a scene root"));

    return root + kCameraRecords + index * kCameraRecordBytes;
}

// 0x800750E0 Camera::SetViewport: the viewport, and the same rectangle as
// the scissor (0x80075050, __cvt_fp2unsigned).
void SetCameraViewport(uint32_t camera, const float rectangle[4]) {
    for (uint32_t i = 0; i < 4; ++i) {
        Memory::WriteFloat32(camera + kCameraViewport + 4 * i, rectangle[i]);
        Memory::Write32(camera + kCameraScissor + 4 * i, rectangle[i] > 0 ? uint32_t(rectangle[i]) : 0);
    }
}

// 0x80074ED0 Camera::SetPerspective.
void SetCameraPerspective(uint32_t camera, float fovy, float aspect) {
    Memory::Write32(camera + kCameraProjection, 0);
    Memory::WriteFloat32(camera + kCameraProjection + 4, fovy);
    Memory::WriteFloat32(camera + kCameraProjection + 8, aspect);
    Memory::WriteFloat32(camera + kCameraProjection + 12, Memory::ReadFloat32(kNear));
    Memory::WriteFloat32(camera + kCameraProjection + 16, Memory::ReadFloat32(kFar));
    Memory::Write32(camera + kCameraFlags, (Memory::Read32(camera + kCameraFlags) & ~0xF0u) | 0x20);
}

// 0x802BBCB4: camera 5 views the preview window (the layout's rectangle in
// 640x480 layout units, its height to the 448-line screen and its x to the
// HUD's width) with a 30-degree perspective of the window's own aspect.
void PreviewCameraSetupPort(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x20);
    GuestCall call{cpu};
    call(PreviewRectangle, {frame[0x8], frame[0xA], frame[0xC], frame[0xE]});
    int16_t x = int16_t(Memory::Read16(frame[0x8])), y = int16_t(Memory::Read16(frame[0xA]));
    int16_t width = int16_t(Memory::Read16(frame[0xC])), height = int16_t(Memory::Read16(frame[0xE]));
    const float aspect = float(width) / float(height);
    const float heightScale = Memory::ReadFloat32(kHeightScale);
    y = int16_t(int32_t(float(y) * heightScale));
    height = int16_t(int32_t(float(height) * heightScale));
    call(HudScaleX);
    const float widthScale = float(double(Memory::ReadFloat32(kOne)) / double(call.F1()));
    x = int16_t(int32_t(float(x) * widthScale));
    width = int16_t(int32_t(float(width) * widthScale));
    const uint32_t camera = PlayerCamera(frame.Arg(13), 5);
    const float rectangle[4] = {float(x), float(y), float(width), float(height)};
    SetCameraViewport(camera, rectangle);
    SetCameraPerspective(camera, Memory::ReadFloat32(kPreviewFovy), aspect);
}

// 0x802BBE88: camera 5 back to camera 4's viewport and the view camera's
// perspective (its fovy, the view's aspect: 0x80040300).
void PreviewCameraResetPort(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x10);
    GuestCall call{cpu};
    const uint32_t full = PlayerCamera(frame.Arg(13), 4), camera = PlayerCamera(frame.Arg(13), 5);
    float rectangle[4];

    for (uint32_t i = 0; i < 4; ++i)
        rectangle[i] = Memory::ReadFloat32(full + kCameraViewport + 4 * i);

    SetCameraViewport(camera, rectangle);
    const uint32_t aspect = Memory::Read8(kSplitCameras) ? 0x8065B504 + 4 * uint32_t(int32_t(int8_t(Memory::Read8(kPlayerIndex)))) : kViewAspect;
    call(ViewCameraFovy);
    SetCameraPerspective(camera, call.F1(), Memory::ReadFloat32(aspect));
}

// The split draw (docs/optimizations/threading/pipelined-draw.md): the scene, with the preview
// camera's setup and reset around it, is left to the draw thread.
void (*sceneHandoff)() = nullptr;
void (*scenePrepareLaunch)(CpuContext* cpu) = nullptr;

struct DeferredScene {
    bool pending = false, twoPlayers = false, preview = false, prepared = false;
} deferredScene;

void Scene(GuestCall& call, uint32_t r13, bool preview) {
    const bool twoPlayers = call(TwoPlayers) != 0;

    if (sceneHandoff) {
        // A single view's scene-graph walk stays here (it reads what the
        // next logic step changes, and sets the camera's state the scene
        // starts from); the draw thread draws its buckets.
        const bool prepared = !twoPlayers && !preview;
        deferredScene = {true, twoPlayers, preview, prepared};

        // The draw thread walks the graph from the handoff's state while the
        // game thread draws the layers after the scene.
        if (prepared && scenePrepareLaunch) {
            sceneHandoff();
            scenePrepareLaunch(call.cpu);
            return;
        }

        if (prepared)
            SceneQueue::Prepare(call.cpu);

        sceneHandoff();

        // The draw thread walks a preview's or the two players' scene; the
        // layers after it start, as in the original, from the state the scene
        // leaves: the preview camera reset and the root's global settings
        // (camera 5's full viewport, projection and scissor). Without them
        // the game side's text kept the state from before the scene, or none
        // (straight into the village): it drew in the preview window.
        if (!prepared) {
            if (preview)
                call(PreviewCameraReset);

            call(CalcRootView, {Memory::Read32(r13 + kRoot)});
        }

        return;
    }

    if (preview)
        call(PreviewCameraSetup);

    call(twoPlayers ? DrawSplitScene : DrawSceneOnly);

    if (preview) {
        call(PreviewCameraReset);
        call(CalcRootView, {Memory::Read32(r13 + kRoot)});
    }
}

void DrawFrame(CpuContext* cpu) {
    const uint32_t r13 = cpu->gpr[13];

    if (!Memory::Read8(kSystem + kRunning) || !Memory::Read32(r13 + kRoot))
        return;

    GuestFrame frame(cpu, 0x10);
    GuestCall call{cpu};

    if (Memory::Read8(kSystem + kOnlineOnly) && uint8_t(call(GetOnlineMode)) != 1) {
        call(DrawGrabbedScreen, {0});
        return;
    }

    {
        MH3_SPLIT_SCOPE(MH3_SPLIT_UI);
        call(BeginFrameDraw);
        call(DrawUISprites, {0});
    }

    if (call(TwoPlayers) == 0) {
        if (Memory::Read8(kSystem + kGrabbed) == 1)
            call(DrawGrabbedScreen, {1});
        else if (Memory::Read8(kSystem + kBackground) != 0) {
            if (const uint32_t callback = Memory::Read32(r13 + kBackgroundCallback))
                call(callback);
        }
    }

    if (uint8_t(call(PreviewCamera)) == 0) {
        const uint32_t split = uint8_t(call(GetOnlineMode)) == 3 ? 4 : 1;
        Layers(call, 0, split);
        Scene(call, r13, false);
        Layers(call, split, 0xB);
    } else {
        Layers(call, 0, 7);
        Scene(call, r13, true);
        Layers(call, 7, 0xB);
    }

    MH3_SPLIT_SCOPE(MH3_SPLIT_UI);
    call(DrawUISprites, {1});
    call(EndFrameDraw);
}

} // namespace

namespace FrameDraw {
void SetSceneHandoff(void (*handoff)()) {
    sceneHandoff = handoff;
}

void SetScenePrepareLaunch(void (*launch)(CpuContext* cpu)) {
    scenePrepareLaunch = launch;
}

bool DeferredSceneWalksGraph() {
    return deferredScene.pending && !deferredScene.prepared;
}

bool RunDeferredScene(CpuContext* cpu) {
    if (!std::exchange(deferredScene.pending, false))
        return false;

    if (deferredScene.prepared) {
        SceneQueue::DrawPrepared(cpu);
        return true;
    }

    GuestFrame frame(cpu, 0x10);
    GuestCall call{cpu};

    if (deferredScene.preview)
        call(PreviewCameraSetup);

    call(deferredScene.twoPlayers ? DrawSplitScene : DrawSceneOnly);

    if (deferredScene.preview) {
        call(PreviewCameraReset);
        call(CalcRootView, {Memory::Read32(frame.Arg(13) + kRoot)});
    }

    return true;
}
} // namespace FrameDraw

extern "C" void func_80049308(CpuContext* cpu);
extern "C" void MH3_DrawFrame(CpuContext* cpu) {
    PortCheck::Call(cpu, "DrawFrame", func_80049308, DrawFrame);
}
REGISTER_NATIVE_FUNCTION_AS(0x80049308, MH3_DrawFrame, "DrawFrame");

extern "C" void func_802BBCB4(CpuContext* cpu);
extern "C" void MH3_PreviewCameraSetup(CpuContext* cpu) {
    PortCheck::Call(cpu, "PreviewCameraSetup", func_802BBCB4, PreviewCameraSetupPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x802BBCB4, MH3_PreviewCameraSetup, "PreviewCameraSetup");

extern "C" void func_802BBE88(CpuContext* cpu);
extern "C" void MH3_PreviewCameraReset(CpuContext* cpu) {
    PortCheck::Call(cpu, "PreviewCameraReset", func_802BBE88, PreviewCameraResetPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x802BBE88, MH3_PreviewCameraReset, "PreviewCameraReset");

extern "C" void func_8004925C(CpuContext* cpu);
extern "C" void MH3_DrawLayers(CpuContext* cpu) {
    PortCheck::Call(cpu, "DrawLayers", func_8004925C, DrawLayersPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8004925C, MH3_DrawLayers, "DrawLayers");

#ifdef MH3_DEVELOPMENT
void MH3PrintLayerSplit(unsigned frames) {
    auto sorted = layerTimes;
    std::sort(sorted.begin(), sorted.end(), [](const LayerTime& a, const LayerTime& b) { return a.ms > b.ms; });
    std::string line = "MH3 layer split (ms/frame, calls/frame):";
    char part[64];

    for (const LayerTime& t : sorted) {
        if (!t.function || !frames)
            break;

        std::snprintf(part, sizeof part, " %08x %.2f (%.1f)", t.function, t.ms / frames, double(t.calls) / frames);
        line += part;
    }

    std::printf("%s\n", line.c_str());
    layerTimes = {};
}
#endif
