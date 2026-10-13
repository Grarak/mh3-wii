// The PS Vita's controls as the game's Classic Controller (docs/vita-controls.md).
//
// The pad comes from sceCtrlPeekBufferPositive2, as SDL's Vita driver read it:
// port 1 first (a PS TV controller), port 0
// when that fails. With ...Positive2 the handheld's L/R report as L1/R1 and a
// PS TV controller's triggers as L2/R2. The touch panels are not read (user,
// 2026-10-06): a hand gripping the rear panel or a thumb on the screen never
// reaches the game. The game polls the controller several times a frame
// (WPADProbe per channel, WPADRead, the 5 ms sampling feed), so one sample
// serves 4 ms. While the system's UI is over the game (the quick menu a long
// press of PS opens) the samples still carry the buttons and sticks: the game
// gets a neutral controller then.
#include "controller_host.h"
#include "../native/mh3_classic_map.h"

#include <psp2/appmgr.h>
#include <psp2common/defs.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

namespace {

void Initialize() {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
}

// The system UI (the quick menu, a notification's menu) is over the game, or
// the system took the controls.
bool Overlaid(const SceCtrlData& ctrl) {
    if (ctrl.buttons & SCE_CTRL_INTERCEPTED)
        return true;

    SceAppMgrAppState state{};
    return _sceAppMgrGetAppState(&state, sizeof(state), PSP2_SDK_VERSION) == 0 && state.isSystemUiOverlaid;
}

ClassicControllerSample Sample() {
    SceCtrlData ctrl{};

    if ((sceCtrlPeekBufferPositive2(1, &ctrl, 1) < 0 && sceCtrlPeekBufferPositive2(0, &ctrl, 1) < 0) || Overlaid(ctrl)) {
        ctrl = SceCtrlData{};
        ctrl.lx = ctrl.ly = ctrl.rx = ctrl.ry = 128;
    }

    mh3_vita_pad pad{};
    pad.buttons = ctrl.buttons;
    pad.lx = ctrl.lx;
    pad.ly = ctrl.ly;
    pad.rx = ctrl.rx;
    pad.ry = ctrl.ry;
    pad.lt = ctrl.lt;
    pad.rt = ctrl.rt;
    mh3_classic_state state;
    mh3_vita_pad_to_classic(&pad, &state);
    return {true, state.buttons, state.lx, state.ly, state.rx, state.ry, state.trigger_l, state.trigger_r};
}

} // namespace

ClassicControllerSample ReadVitaController() {
    static bool initialized = false;
    static SceUInt64 sampled = 0;
    static ClassicControllerSample last;

    if (!initialized) {
        Initialize();
        initialized = true;
    }

    const SceUInt64 now = sceKernelGetProcessTimeWide();

    if (!sampled || now - sampled >= 4000) {
        last = Sample();
        sampled = now;
    }

    return last;
}
