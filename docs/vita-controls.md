# Controls

The game always sees one Wii Remote with a Classic Controller (RVL-005, not
the Pro) on channel 0, and plays fully on it: no motion or Nunchuk prompts
([controller input](optimizations/system/controller-input.md)). The PS
Vita's controls, SDL gamepads and the keyboard are mapped onto that
controller, and the front touch screen (the mouse elsewhere) is the remote's
pointer. Recordings, playback and the control file (`MH3_INPUT`) hold the
resulting Classic report, so they replay the same on every host; the pointer
stays live during playback.

## PS Vita

| Classic Controller | PS Vita | PS TV controller | In the game |
| --- | --- | --- | --- |
| Left stick | Left stick | Left stick | Move |
| Right stick | Right stick | Right stick | Attack (the game's default) or camera (its camera option); turns the hunter in character creation |
| A | ○ | ○ | Confirm, act |
| B | × | × | Back |
| X | △ | △ | Per control style |
| Y | □ | □ | Per control style |
| D-pad | D-pad | D-pad | Menu cursor; camera (the game's default) |
| L (trigger, fully pressed) | L | L1 | Per control style; menu tab |
| R (trigger, fully pressed) | R | R1 | Per control style; menu tab |
| ZL | — | L2 | Per control style |
| ZR | — | R2 | Per control style |
| + | START | START | Pause menu |
| − | SELECT | SELECT | Per control style |
| HOME | — | L3 + R3 | HOME Menu |

"Per control style": the game's Classic Style 1/2 options decide the field
actions. Every Classic control stays where it is on the original controller,
so both styles work as the game's help screens show them.

- **Face buttons by position**: ○ confirms and acts, × rolls and backs out,
  as in the PSP games. The launcher's own menus keep the system convention
  (× selects).
- **L/R are the Classic L/R triggers**, reported fully pressed.
- **Touch**: the front screen is the Wii Remote's pointer (below); the rear
  panel is not read. The handheld has no control for ZL, ZR or HOME; a PS TV
  controller has them on L2/R2 and L3 + R3.
- **System menus**: while the quick menu (a long press of PS) or a
  notification's menu is over the game, the game sees a neutral controller.
- **Button prompts** show the Vita's buttons: the Classic's a, b, y, + and −
  draw as ○, ×, □, START and SELECT (`runtime/src/button_glyphs.cpp`).

### Pointer

In the village and the field the game lets the hunter point the Wii Remote
at a monster and drag it onto the log on the HUD, which notes it in the
save (online, a hunter's entry goes into a list the same way). On the Wii
the remote's B shows the cursor, A over a target takes it, and A held drags
it. With the touch screen:

1. Touch a monster on the screen: the cursor opens under the finger (B).
2. Keep the finger down: once the cursor has opened (under a second) and
   rests on the monster, it takes it (A).
3. Drag it onto the log and lift the finger.

A target that can't be taken plays a refusal sound until the finger lifts.
Code: `runtime/src/wpad_services.cpp` (the remote's B and A, the sensor bar
dots), `runtime/src/pad_pointer.cpp` (the game's KPAD and cursor) and
`runtime/src/log_pointer.cpp` (the pointing).

### Sticks

The game reads a stick's length in a narrow window (raw 90..187 of the
report's −512..511). The Vita mapping spreads that window over the stick's
travel: a 30 % radial dead zone, then the game's threshold rising linearly
to its full length at 78 % of the travel, then the rest of the report range,
so full tilt reports what a real Classic stick does. The hunter walks at
about 12–50 % of the travel and runs beyond. In the default layout a partial
tilt of the right stick already draws the weapon and attacks.

Code: `runtime/vita/vita_controller.cpp` (the pad, port 1 first so a PS TV
controller works) and `runtime/native/mh3_classic_map.c` (the mapping;
`tests/native/test_mh3_classic_map.c`).

## Linux (SDL)

The first SDL gamepad is the same Classic Controller, by position:

| Classic Controller | Xbox layout | PlayStation layout |
| --- | --- | --- |
| A / B / X / Y | B / A / Y / X | ○ / × / △ / □ |
| L / R (analog triggers) | LT / RT | L2 / R2 |
| ZL / ZR | LB / RB | L1 / R1 |
| + / − / HOME | Menu / View / Guide | Options / Create (Share) / PS |
| Sticks, D-pad | Sticks, D-pad | Sticks, D-pad |

Nintendo layouts land on the same physical buttons. The triggers set the
digital L/R past half way; the sticks use the Vita's curve with a 15 % dead
zone.

Keyboard and mouse, while the window has focus, after Monster Hunter:
World's defaults where the game has the action:

| Keys and mouse | Classic Controller | In the game |
| --- | --- | --- |
| W A S D | Left stick | Move |
| Mouse (captured) | — | Camera, turned by the mouse itself (below) |
| Left button (captured) | X | Draw the weapon, attack |
| Right button (captured) | A | Second attack |
| Space | B | Evade, back |
| Left Shift | R (fully pressed) | Dash; the weapon's guard or charge |
| E | Y | Use an item, sheathe |
| F / Enter | A | Talk, gather, confirm |
| Q | L (fully pressed) | Camera behind the hunter; held, the item bar |
| Q held + left / right button (captured) | — | Item bar: the previous / next item |
| Z / C | ZL / ZR | Per control style |
| Escape | + | Pause menu |
| Tab / M | − | Quest menu (item pouch, map, signal) |
| Backspace | B | Back |
| Arrow keys | D-pad | Menu cursor |
| I J K L | Right stick | Camera or attack, as the game's option |
| Home | HOME | HOME Menu |

Keyboard and gamepad sticks combine per stick (a held key wins). There is no
remapping yet: the tables live in `mh3_classic_map.c` (Vita) and
`controller_host.cpp` (SDL, keyboard).

### Mouse look

A click in the window captures the mouse; Left Alt held frees the cursor
(the window or the browser keeps it otherwise). Captured, the mouse turns
the field camera directly instead of standing in for the right stick, whose
camera only turns at fixed speeds:

- across, the yaw turns with the mouse, one to one and without the stick's
  ramp (22 angle units a pixel, 0.12°), and holds off the camera's swing
  back behind the hunter as a turning stick does;
- up and down, on land, where the stick only steps between the camera's
  five distance rows (0 high and close, 1 high and far, 2 the middle, 3 low
  and far with the narrower view, 4 low and close), the mouse moves between
  them continuously (96 pixels a row): the camera's row is the blend of the
  two nearest, written to 32 bytes the runtime reserves in MEM2, so the
  camera's collision and limits see the same row; swimming, it tilts the
  pitch as the stick does.

While L is held (Q) the item bar is open, and the captured buttons scroll it
instead of attacking: the runtime replays the bar's own scroll
(`runtime/src/item_bar.cpp`, the game's 0x8025E12C), its sound, the next
usable pouch slot back or forward and the HUD's turn.

`MH3_MOUSE_SENSITIVITY` scales both (default 1) and `MH3_MOUSE_INVERT_Y=1`
turns the vertical round; the game's own camera inversion options apply too.
Free, the left button over the game's picture is the Wii Remote's pointer: a
finger on the Vita's screen ([pointer](#pointer)). Code:
`runtime/src/controller_host.cpp` (capture, `TakeMouseMotion`),
`runtime/src/camera_input.cpp` and `field_camera.cpp` (the camera), and the
camera helpers ported for it in `runtime/src/camera_controls.cpp`.
