# Controls: the Classic Controller on every host — 2026-10-03

The game always sees one Wii Remote with a Classic Controller (the original
RVL-005, not the Pro) on channel 0. The PS Vita's controls, SDL gamepads and
the keyboard are mapped onto that controller's buttons, sticks and analog L/R
triggers. Recordings and playback (`MH3_INPUT=record=`/`playback=`) and the
control file (`file=`) hold the resulting Classic report, so they replay the
same on every host.

## How the game reads the Classic Controller

RMHP08 never falls back to Wii Remote and Nunchuk controls while a Classic
Controller is attached. What it does, per frame (`PollControllers`
0x80041F6C → `PadReadWii` 0x80042088 for each of the four channels; pad records
of 0x350 bytes at 0x8065B7D0):

- `WPADProbe` gives the device type (+0x34). Our WPAD bridge
  (`runtime/src/wpad_services.cpp`) answers 2, Classic, on channel 0 and "no
  controller" on 1–3, so `get_ControlType` (0x80043BAC) is 2 everywhere.
  The connect and extension callbacks (`PadConnectCallback` 0x8004351C,
  `PadExtensionCallback` 0x8004378C) then select data format 8 and turn the
  pointer (IR/DPD) off. There is no Nunchuk or "connect a controller" prompt.
- `WPADRead` fills the Classic report (+0x62). The game reads it directly;
  KPAD is only initialised for the HOME Menu.
- Buttons go through a fixed table (0x80581670, copied to 0x8065D2D0 at
  start-up by `PadInit` 0x80041B08) into the game's Classic bits at +0xD6
  (`get_rcSwitch_on` returns the presses): ↑ 0x2000, ↓ 0x1000, ← 0x800,
  → 0x400, Y 0x200, X 0x100, R 0x80, B 0x40, A 0x20, ZR 0x10, L 0x8, ZL 0x4,
  HOME 0x2, + 0x8000, − 0x4000. The analog trigger bytes are not read; the
  game uses the L/R full-press bits only.
- Sticks: `WPADClampStick` (circle: 60 units of play per axis, radius 248),
  then the stick reader `PadClassicSticks` (0x80042FDC; its computation is
  `mh3_classic_read_sticks` in `runtime/native/mh3_classic_map.c`, not yet
  registered as a native) stores direction, length (0 below 30, at most
  127) and 8-way bits (from length 37) for each stick. So of the report range
  −512..511 only raw 90..187 along an axis changes the result; a real Classic
  stick's full tilt (about 308) is far past saturation.
- Menus (`MenuKeysFromPad` 0x80449C58) take the Classic bits: A confirms,
  B goes back, the D-pad moves, + and −, X, Y, L, R, ZL and ZR have their own
  keys (page tabs, sorting and so on, per menu).
- Character creation turns the preview with the right stick (`CharMakeTurn
  Preview` 0x8043AB60 reads its 8-way bits); its pointer checks
  (0x8043890C, 0x80438984) are off for the Classic Controller.
- A few menus ask for the pointer (`PadPointerOn` 0x80044BA0 calls
  `WPADControlDpd(on)`); the bridge refuses, the game records "no pointer"
  and the Classic Controller keeps working there. The Wii Remote's own
  buttons (+0xCA, read by 0x802DC954 for a pointer menu) stay 0.

So a session is a clean Classic Controller session; no game code needed
changing for it.

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
actions; the mapping keeps every Classic control where it is on the original
controller, so both styles work as the game's own help screens show them.
The field roles of L, R, ZL, ZR and − were not checked in a run yet.

Why this layout:

- **Face buttons by position.** The Classic Controller's diamond (X top, A
  right, B bottom, Y left) sits where the Vita's △ ○ × □ are, and MH3's
  Classic scheme is the Monster Hunter Portable layout: ○ confirms and acts,
  × rolls and backs out, as in the PSP games. (The Vita launcher's menus
  keep the system convention, × to select; once the game starts, ○ is A.)
- **L/R are the Classic L/R triggers.** They are the shoulder controls the
  game uses most, and on the original controller the outer triggers that
  the index fingers rest on. A press reports the trigger fully down (the
  digital L/R bit and an analog value of 255).
- **No touch input** (user, 2026-10-06). The front and rear panels are not
  read: a hand gripping the rear panel or a thumb resting on the screen never
  reaches the game. The handheld has no control left for the Classic
  Controller's ZL/ZR (small extra buttons inside its triggers) or HOME (the
  PS button belongs to the system); a PS TV controller has them on L2/R2 and
  L3 + R3. Until 2026-10-06 the rear panel's upper quarters were ZL/ZR and a
  tap at the top centre of the screen was HOME.

The pad is read with `sceCtrlPeekBufferPositive2` (port 1 first, as SDL's
Vita driver did, then port 0), so the devkit harness's CTRLP pad still
drives the game. One sample serves 4 ms of the game's several reads per frame. Code:
`runtime/vita/vita_controller.cpp` (Vita APIs) and
`runtime/native/mh3_classic_map.c` (the mapping, host-testable).

### Sticks

The game reads the stick's length in a narrow window (above). A linear
mapping (the Vita's 0..255 times four, as the SDL path did) put that whole
window into 18–37 % of the Vita stick's travel: the rest was dead or
saturated, so slight tilts jumped from standing to running. The mapping now
spreads the window over the travel:

- radial dead zone of 30 % (the sticks rest within about ±10 of centre; 12 %
  made them too sensitive, user 2026-10-06);
- just past it the length is the game's threshold (31), growing linearly to
  127 at 78 % of the travel (80 % of the travel past the dead zone up to the
  90 % taken as full);
- from there to the edge the report goes on to the full −512..511 range, so
  full tilt reports what it always did (511 along an axis; KPAD readers
  such as the HOME Menu see a full stick);
- WPADClampStick's 60 units of play are added back per axis, so the game
  sees the stick's real direction.

`tests/native/test_mh3_classic_map.c` checks the dead zone, the window,
monotonic length, direction error under 0.05 rad, full deflection and every
button. The game's own responses are not changed here; only
how far the Vita stick has to move to reach a given game length.

On the VM (input playback of the mapped values, quest 10057 area 6), the
hunter walks at one pace (about 2.8 units a 30 Hz frame) at lengths 42, 61
and 80 and runs (about 15) from 100 on, so with the Vita curve the walk
takes about 12–50 % of the travel and the run the rest; the hunter stands
still at rest. In the game's default Classic layout the right stick draws
the weapon and attacks (a partial tilt, length 46, already does) and the
D-pad turns the camera; the right stick turns the camera only with the
game's camera option set to it.

## Linux hosts (SDL)

The first SDL gamepad is the same Classic Controller, by position:

| Classic Controller | Xbox layout | PlayStation layout |
| --- | --- | --- |
| A / B / X / Y | B / A / Y / X | ○ / × / △ / □ |
| L / R (analog triggers) | LT / RT | L2 / R2 |
| ZL / ZR | LB / RB | L1 / R1 |
| + / − / HOME | Menu / View / Guide | Options / Create (Share) / PS |
| Sticks, D-pad | Sticks, D-pad | Sticks, D-pad |

Face buttons go by position (SDL's east, south, north, west), so Nintendo
layouts land on the same physical buttons.
The analog triggers report their travel as the Classic's analog L/R and set
the digital L/R bit past half way. Sticks use the same window as the Vita
with a 15 % dead zone.

Keyboard, while the window has focus:

| Keys | Classic Controller |
| --- | --- |
| W A S D | Left stick |
| I J K L | Right stick |
| Arrow keys | D-pad |
| Space / Left Shift | A / B |
| E / F | X / Y |
| Q / R | L / R (fully pressed) |
| Z / C | ZL / ZR |
| Enter / Backspace | + / − |
| Escape or Home | HOME |

Keyboard sticks and gamepad sticks combine per stick (a key held wins).

## Devkit checklist

Checked on the dev VM only (playback and the SDL/keyboard path); none of the
Vita-side reading has run on hardware yet. On the devkit:

1. `--playback` with the usual log: the recorded axes still reach ±511
   (`saw_move`, `saw_camera`, the run-forward and camera phases), and the
   playback finishes. A CTRLP run, if used, still drives the game (port 1
   first, then port 0).
2. The launcher still selects with × (it reads the pad itself); in the
   game ○ confirms and × goes back, the D-pad moves menu cursors.
3. Left stick: resting does not move the hunter; a slight tilt walks, the
   run starts at about half tilt (not at the first touch); full tilt in
   every direction runs.
4. Right stick, default layout: resting does nothing, a tilt draws the
   weapon and attacks. With the game's camera option on the right stick it
   turns the camera instead. Character creation turns the hunter. The D-pad
   turns the camera by default.
5. L and R act as the Classic L/R triggers (field actions per control
   style, menu tabs); START opens the pause menu (+), SELECT is −.
6. Touching the screen or the rear panel does nothing in the game.
7. PS TV, if one is at hand: L2/R2 are ZL/ZR, L3 + R3 is HOME; check whether
   the HOME Menu opens and can be closed with the Classic Controller.
8. The vprof profile shows no measurable time in `ReadVitaController`.

## Not configurable yet

There is no runtime settings file for remapping; the tables above live in
`mh3_classic_map.c` (Vita) and `controller_host.cpp` (SDL, keyboard).
