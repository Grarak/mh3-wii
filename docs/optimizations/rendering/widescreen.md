# The game's own 16:9 mode at 960×544

## What the original does

RMHP08 reads the console's aspect setting once (`SCGetAspectRatio`
0x804DD060, SYSCONF item `IPL.AR`) in `RenderInit` (0x8003F794) and fills
its screen record at 0x8065B4BC: in 16:9 the 2D width becomes 854 instead
of 640 and the camera aspect 16/9, and 38 call sites of `ck_WideMode`
switch layouts. The EFB stays 640×448, so the 16:9 picture is
anamorphic: the TV stretches it back.

## What the port does instead

1. `SCGetAspectRatio` is a native (`runtime/src/render_mode.cpp`) that
   returns the host's setting, 16:9 by default (`MH3_WIDESCREEN=0` for
   4:3). Everything else follows from the game's own code.
2. Every host renders for the Vita's 960×544 display. In 16:9 the native
   target covers the whole surface, and logical coordinates (viewports,
   scissors, scene captures, glare, CPU framebuffer reads) are scaled per
   axis (×1.5, ×1.214), so the game draws at the display's resolution
   instead of into a 640×448 image that is stretched afterwards. In 4:3 the
   target keeps 640:448, pillarboxed (777×544 on the Vita).
3. `RenderInit` writes 960/544 to the game's 16:9 camera aspect constant
   (0x80797F68), because the display is 1.7647:1, not 1.7778:1. The HUD
   keeps its 854×480 layout, 0.8% narrower than square on 960×544.

Outside the Vita two settings drop the 960×544 picture
(`runtime/src/graphics_host.cpp`):

- **`MH3_RESOLUTION=native`**: the target follows the window's (on the web
  the canvas's) pixel size: the largest 16:9 (or 4:3) picture that fits,
  centred. The per-axis scaling above takes any size.
- **`MH3_ASPECT=window`** (ultrawide): the picture takes the window's
  aspect. Once a frame the game's camera aspect (its constant 0x80797F68 and
  the screen record's +0x0C copy) follows the target's, so the 3D view
  widens instead of stretching. The 2D layer keeps its 854×480 aspect
  (`runtime/src/native_render_state.cpp`): orthographic draws, and 3D drawn
  in a viewport smaller than the screen (a menu's preview), land in a
  16:9 box; the field HUD's parts put their box against an edge (the clock
  and gauges left, the map, the item box and the quest menu right:
  `kAnchors` in `hud_draw.cpp`, the draw state's `anchor`), the rest in the
  middle. Text a part queues for the font flush keeps its part's edge (the
  queue positions it appended, `native_ui.cpp`). A draw whose vertices
  span the whole picture (a fade, a loading backdrop) fills the width
  (`NativeWideDrawScope`), and a draw without a viewport of its own takes
  the whole screen's instead of the backend's last one, which may be a
  box. The pointer maps onto the middle box, where its cursor is drawn.

## Why

The Vita's screen is 960×544; drawing the game's widescreen mode at that
size uses the whole display without stretching a 640×448 image, and keeps
the game's own widescreen layouts. Changing the 2D width would have broken
the places that hard-code 854.

## Example

The screen record fields the port relies on (from the game, not changed by
the port):

| Field | 4:3 | 16:9 |
| --- | --- | --- |
| +0x00/+0x02 s16 EFB size | 640×448 | 640×448 |
| +0x04 f32 2D width | 640 | 854 |
| +0x0C f32 camera aspect | 4/3 | 16/9 (960/544 in the port) |
| +0x19 u8 wide flag | 0 | 1 |

## Applying it to another Wii game

Generic: making the system report 16:9, rendering the EFB at the display's
resolution with per-axis scaling of logical coordinates, and fixing the
camera aspect constant.

Game-specific: whether the game has a widescreen mode at all, where it
reads the setting (SYSCONF through `SCGetAspectRatio` is the SDK way), which
constant holds its camera aspect, and which 2D paths hard-code 4:3
coordinates. Audit the game's visibility tests at 16:9: objects culled
against a 4:3 frustum pop in at the edges.
