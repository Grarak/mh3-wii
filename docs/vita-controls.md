# Controls

The game always sees one Wii Remote with a Classic Controller (RVL-005, not
the Pro) on channel 0, and plays fully on it: no pointer, motion or Nunchuk
prompts ([controller input](optimizations/system/controller-input.md)). The
PS Vita's controls, SDL gamepads and the keyboard are mapped onto that
controller. Recordings, playback and the control file (`MH3_INPUT`) hold the
resulting Classic report, so they replay the same on every host.

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
- **No touch input**: the front and rear panels are not read. The handheld
  has no control for ZL, ZR or HOME; a PS TV controller has them on L2/R2
  and L3 + R3.
- **System menus**: while the quick menu (a long press of PS) or a
  notification's menu is over the game, the game sees a neutral controller.
- **Button prompts** show the Vita's buttons: the Classic's a, b, y, + and −
  draw as ○, ×, □, START and SELECT (`runtime/src/button_glyphs.cpp`).

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

Keyboard and gamepad sticks combine per stick (a held key wins). There is no
remapping yet: the tables live in `mh3_classic_map.c` (Vita) and
`controller_host.cpp` (SDL, keyboard).
