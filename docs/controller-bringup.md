# Native Classic Controller input

The Metal build exposes one virtual Classic Controller on channel 0, backed by
keyboard input and the first SDL gamepad. The original game code still performs
button transitions and stick processing. The bridge supplies raw WPAD reports,
probe results, data-format changes, controller information, deferred callbacks,
and a 5 ms sampling feed into the SDK's registered auto-sampling ring.

The guest layout and channel-control offsets were verified from RMHP08's
`WPADRead` (`0x804EF9D0`), `WPADSetAutoSamplingBuf` (`0x804EFAC0`), and
`WPADGetLatestIndexInBuf` (`0x804EFC70`). The 0x36-byte Classic report and
0x18-byte controller-info structure also agree with
[devkitPro's WPAD declarations](https://github.com/devkitPro/wut/blob/master/include/padscore/wpad.h).

Keyboard controls apply while the game window has keyboard focus:

| Host keys | Classic Controller |
| --- | --- |
| Space / Left Shift | A / B |
| E / R | X / Y |
| WASD / IJKL | Left stick / right stick |
| Arrow keys | D-pad |
| Q / F | L / R |
| Z / X | ZL / ZR |
| Enter / Backspace / Escape | Plus / Minus / Home |

SDL gamepad face buttons follow the physical Nintendo layout: east=A, south=B,
north=X, west=Y. Sticks, D-pad, shoulders, triggers, Start, Back and Guide map to
the corresponding Classic controls. Disconnecting the gamepad leaves keyboard
input available. Physical Wii Remote motion, IR, speaker output and controller
rumble are not implemented by this bridge.

For unattended validation, `MH3_INPUT_FILE=/absolute/path` replaces the host
source with a diagnostic Classic report. The file contains a hexadecimal button
mask followed by four decimal stick axes in [-512, 511], for example
`10 0 0 0 0` presses Classic A and `0 0 0 0 0` releases all controls. Replace the
file atomically when changing it during a run. This exercises the same WPAD
report and sampling path; it does not prove physical keyboard/gamepad behavior.

Runtime tests cover device probing, report byte order and bounds, supported
formats, press/release samples, ring wraparound, disconnect state and queued
sampling/info callbacks. Unsupported IR enable requests return an error.

Both native and Metal runtime/audio test suites pass. A real Metal boot with
diagnostic input skipped the opening movie, opened the title menu with A, and
changed the highlighted entry with the D-pad. Evidence: `logs/controller-boot.log`
and its numbered frame captures. Physical host input and gameplay require further
validation.

A separate Metal-build CTest now attaches an SDL virtual gamepad and exercises
`ReadHostController()` without the diagnostic-file or sample override. It passes
face-button/D-pad mapping, stick ranges and Y inversion, analog/digital trigger
mapping, release, disconnect fallback, and reconnection. It uses SDL's dummy video
driver and does not require an unlocked desktop. Physical hardware and focused
keyboard input are still not manually verified.
