# Controller input: one Classic Controller from any host

## What the original does

MH3 reads Wii Remotes through the SDK's WPAD library, once per frame
(`PollControllers` 0x80041F6C → `PadReadWii` 0x80042088 for each channel).
With a Classic Controller attached it reads the Classic report directly
(`WPADRead` 0x804EF9D0) and never falls back to the Wii Remote and Nunchuk:
buttons go through a table (0x80581670) into the game's Classic bits, the
sticks through `WPADClampStick` (a circle with 60 units of play per axis)
and the game's stick reader (0x80042FDC), which reports a length of 0 below
30 and at most 127. Of the report's -512..511 range, only raw values of
about 90..187 along an axis change the result ([controls](../../vita-controls.md)).

## What the port does instead

- **WPAD bridge** (`runtime/src/wpad_services.cpp`): channel 0 is a Wii
  Remote with a Classic Controller, channels 1-3 are empty. The bridge
  supplies probe results, data-format changes, controller information,
  deferred callbacks, raw Classic reports, and a 5 ms feed into the SDK's
  auto-sampling ring (WPAD bridge). The game's own
  button and stick processing runs unchanged. Motion, speaker and rumble
  are not implemented.
- **The pointer**: the Vita's front touch screen (the mouse's left button
  on other hosts) is the Wii Remote's IR pointer. While a finger is down
  the reports carry two dots of a sensor bar, placed by inverting the
  game's own copy of KPAD (`pad_pointer.cpp`) so its cursor lands under the
  finger (the picture's place on the screen from `aurora_present_rect`),
  and the remote's own B and A buttons for the one feature that reads
  them: pointing at monsters for the log ([controls](../../vita-controls.md#pointer)).
- **Host mappings** (`runtime/native/mh3_classic_map.c` for the Vita,
  `runtime/src/controller_host.cpp` for SDL gamepads and the keyboard): face
  buttons by position, the Classic L/R triggers on the shoulders; on a PS TV
  controller L2/R2 are ZL/ZR and L3 + R3 is HOME. The rear touch panel is
  not read: a hand gripping it never reaches the game
  ([controls](../../vita-controls.md)).
- **A stick curve** spreads the game's narrow input window over the host
  stick's travel: a radial dead zone, then the game's threshold rising
  linearly to full length at 80% of the travel past the dead zone, then the
  rest of the report range; `WPADClampStick`'s play is added back so the
  game sees the stick's real direction.
- **Scripted input** (development builds): `MH3_INPUT=record=,playback=`
  logs and replays the resulting Classic report indexed by game time, so a
  recording replays the same on every host and frame rate.

## Why

The hosts have no Wii Remote or motion sensors, and the game is fully
playable with the Classic Controller; only pointing at monsters for the log
needs the remote's pointer, which a touch screen or mouse stands in for.
Supplying it at the WPAD level keeps the game's input code, menus and help
screens as they are. The stick
curve fixes a real problem: a linear mapping (as the SDL path first did) put
the game's whole input window into 18-37% of the Vita stick's travel, so a
slight tilt jumped from standing to running ([controls](../../vita-controls.md)).

## Example

`mh3_classic_stick` (`runtime/native/mh3_classic_map.c`), the curve:

```c
const mh3_stick_curve mh3_vita_stick_curve = {0.30f, 0.90f, 0.80f}; /* dead zone, saturation, window */

const float t = clampf((length - curve->dead_zone) / (curve->saturation - curve->dead_zone), 0.0f, 1.0f);
const float first = MH3_CL_GAME_DEAD + 1.0f, full = MH3_CL_GAME_FULL;
const float reach = 512.0f - MH3_CL_STICK_PLAY;
...
if (t <= window)
    target = first + (full - first) * (t / window);
else
    target = full + (reach - full) * ((t - window) / (1.0f - window));
```

`tests/native/test_mh3_classic_map.c` checks the dead zone, the window,
monotonic length, the direction error, full deflection and every button.

## Applying it to another Wii game

Generic: the WPAD bridge (report layout, probe, auto-sampling feed) works
for any game that reads WPAD; the host mappings and the curve function take
a curve per device.

Game-specific: which extension the game supports well without pointer or
motion. The sensor bar dots need the game's KPAD settings (sensor bar
place, scale, distances) to land the cursor where the player touched; a
game on the SDK's KPAD keeps them in its KPADInsideStatus as this one does.
Motion controls would need a gesture mapping, which this port does not
have. Find
the game's own stick reader and its thresholds (here 30 and 127 after the
clamp), and set the curve so that the game's window covers most of the host
stick's travel.
