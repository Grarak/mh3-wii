// The PS Vita's controls as the game's Classic Controller (docs/vita-controls.md).
//
// The pad comes from sceCtrlPeekBufferPositive2, as SDL's Vita driver read it:
// port 1 first (a PS TV controller, or the devkit harness's CTRLP pad), port 0
// when that fails. With ...Positive2 the handheld's L/R report as L1/R1 and a
// PS TV controller's triggers as L2/R2. The touch panels add ZL/ZR (rear) and
// HOME (front); not while a system dialog (the IME's keyboard, a message) is
// up, whose own touches they would be. The game polls the controller several
// times a frame (WPADProbe per channel, WPADRead, the 5 ms sampling feed), so
// one sample serves 4 ms.
#include "controller_host.h"
#include "../native/mh3_classic_map.h"

#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/message_dialog.h>
#include <psp2/touch.h>

#include <algorithm>

namespace {

struct Panel {
    float x = 0, y = 0, width = 1, height = 1;
};

Panel panels[SCE_TOUCH_PORT_MAX_NUM];

void Initialize() {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);

    for (int port = 0; port < SCE_TOUCH_PORT_MAX_NUM; ++port) {
        sceTouchSetSamplingState(port, SCE_TOUCH_SAMPLING_STATE_START);
        SceTouchPanelInfo info{};

        if (sceTouchGetPanelInfo(port, &info) >= 0 && info.maxAaX > info.minAaX && info.maxAaY > info.minAaY)
            panels[port] = {float(info.minAaX), float(info.minAaY), float(info.maxAaX - info.minAaX), float(info.maxAaY - info.minAaY)};
        else // the front panel's documented active area
            panels[port] = {0, 0, 1920, port == SCE_TOUCH_PORT_FRONT ? 1088.0f : 890.0f};
    }
}

uint8_t Touches(int port, mh3_touch_point* points) {
    SceTouchData data{};

    if (sceTouchPeek(port, &data, 1) < 1)
        return 0;

    const Panel& panel = panels[port];
    const unsigned count = std::min<unsigned>(data.reportNum, MH3_VITA_MAX_TOUCH);

    for (unsigned i = 0; i < count; ++i)
        points[i] = {(data.report[i].x - panel.x) / panel.width, (data.report[i].y - panel.y) / panel.height};

    return uint8_t(count);
}

ClassicControllerSample Sample() {
    SceCtrlData ctrl{};

    if (sceCtrlPeekBufferPositive2(1, &ctrl, 1) < 0 && sceCtrlPeekBufferPositive2(0, &ctrl, 1) < 0) {
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
    const bool dialog = sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING || sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING;

    if (!dialog) {
        pad.front_count = Touches(SCE_TOUCH_PORT_FRONT, pad.front);
        pad.rear_count = Touches(SCE_TOUCH_PORT_BACK, pad.rear);
    }

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
