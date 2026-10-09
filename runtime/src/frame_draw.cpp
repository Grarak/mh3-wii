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
// split line); MH3_PORT_CHECK=DrawFrame checks the port.
#include "abi_bridge.h"
#include "frame_split.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

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

void Layers(GuestCall& call, uint32_t first, uint32_t end) {
    MH3_SPLIT_SCOPE(MH3_SPLIT_LAYERS);
    call(DrawLayers, {first, end});
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

    const auto scene = [&] { call(call(TwoPlayers) != 0 ? DrawSplitScene : DrawSceneOnly); };

    if (uint8_t(call(PreviewCamera)) == 0) {
        const uint32_t split = uint8_t(call(GetOnlineMode)) == 3 ? 4 : 1;
        Layers(call, 0, split);
        scene();
        Layers(call, split, 0xB);
    } else {
        Layers(call, 0, 7);
        call(PreviewCameraSetup);
        scene();
        call(PreviewCameraReset);
        call(CalcRootView, {Memory::Read32(r13 + kRoot)});
        Layers(call, 7, 0xB);
    }

    MH3_SPLIT_SCOPE(MH3_SPLIT_UI);
    call(DrawUISprites, {1});
    call(EndFrameDraw);
}

} // namespace

extern "C" void func_80049308(CpuContext* cpu);
extern "C" void MH3_DrawFrame(CpuContext* cpu) {
    PortCheck::Call(cpu, "DrawFrame", func_80049308, DrawFrame);
}
REGISTER_NATIVE_FUNCTION_AS(0x80049308, MH3_DrawFrame, "DrawFrame");
