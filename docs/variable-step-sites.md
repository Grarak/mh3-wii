# Variable step: what is continuous and what is still discrete

MH3's logic was written for a fixed 1/30 s step (a **tick**). The port runs
each logic step for the time that really elapsed. This document lists every
place the port adapts the game to that. Each is either **continuous** (the
amount follows the step's length) or **discrete** (the thing still happens on
whole ticks only). The last sections list what is known to be left and where
discrete handling may look lumpy at high frame rates.

This covers the native ports of the
ambient flutterer (0x803BC584), the sway chain solver (0x802AC4E8), the
input path (section 7: pad repeats, the field camera and aiming) and the
monster intros, roars, attacks and waits of the pattern sweep (sections 9
and 10).

---

## 1. Mechanisms

### 1.1 GameStep (`runtime/include/game_step.h`, `runtime/src/game_step.cpp`)

The frame pacer (`runtime/src/frame_pacer.cpp`, the wait 0x800401A8) measures
the host time of the step that just ran. It calls `GameStep::Begin(s)`, where
`s` is that time in ticks (half the elapsed retraces, capped by `kMaxStep`).
The next step is assumed to be as long as this one. With the frame limit on
the Vita (GXM, VSync) a shown frame's step is the display's instead
(`DisplayStep`): one retrace (two at the 30 fps limit), since the packet
ring and display queue show frames a VBlank apart (two) whenever the game
thread started them, plus the VBlanks
the display kept a frame for because the next was late (`gxm::repeated_vblanks`,
from `sceDisplayGetVcount` at each flip), paid back an eighth of what is owed
a frame (fading out; past 4 retraces behind, the rest at once).
Loading screens, the limit off and GL (no VSync) keep the host time.
`MH3_VARIABLE_STEP=0` restores fixed steps (`Begin(budget / 2)`). The records
below name the development switches each finding was made with; those not in
[flags.md](flags.md) (`MH3_STEP_SCALE`, and trace items such as `srt`,
`camera-path` or `bind-camera`) were removed once their findings were in.

| API | Meaning | Kind |
|---|---|---|
| `Scale()` | this step's length in ticks (1.0 at the stock step) | continuous |
| `Share(k)` | this step's share of an integer amount `k` per tick: `round(k·elapsed) − round(k·(elapsed − s))`, so the shares add up exactly | continuous (integer-quantised) |
| `Fraction()` | the part of a tick left over after the last whole tick | continuous |
| `Elapsed()` | game time since boot, in ticks | continuous |
| `Ticks()` | whole ticks that finished in this step (0 on short steps, more than 1 on long ones) | discrete |
| `Variable()` | whether the pacer runs variable steps | — |

`Begin()` also writes two guest floats:

- **`nw4r::g3d::FrameCtrl::smBaseUpdateRate` (0x80793628) = s.** Every g3d
  animation (character motions, material, texture and visibility animations)
  advances by `rate · s`, so all of it is continuous.
- **The fps float (0x8065B4D0) = 30.** At variable step it is not `30 / s`.
  The game turns seconds into frame counts with it, and those counts are then
  counted down in whole ticks.

### 1.2 Tick sites (`runtime/src/tick_sites.inc`, `runtime/src/tick_sites.cpp`)

Each entry `{pc, kind[, limit[, module]]}` names a guest load that feeds an
inline `x ± 1` or `x + k`. The translator wraps that load in `MH3_TickLoad`
(integer kinds), `MH3_EaseDivisor`, `MH3_PerStep` or `MH3_Decay` (float `lfs`
kinds).

A boot module and an RSO overlay can share addresses: demo_data (the title),
map_village, net_data and map_town (after a quest) all load at 0x8097D080, and
quest_data's field profile at lobby_data's base. A site there names its module
(`"demo"`, `"net_data"`, `"quest_field"`): `scripts/tick_sites_filter.py`
gives the base translation the untagged sites and the boot modules' and each
overlay profile the untagged sites and its own (`build/tick_sites-base.inc`,
`build/rso-overlays/PROFILE/tick_sites.inc`). Before the tags the title's sites
also hooked net_data's loads at the same pcs: 0x809808F8 (Rate) scaled the
connecting window's seconds and 0x80981654 (Rate) the terms list's first
line, and 0x8097EFEC (Countup) moved a menu item by a pixel without a tick. Two
modules' sites can't share one pc (`MH3_TickLoad` looks sites up by pc; a
static_assert keeps the pcs sorted and unique).

| Kind | What the guest does | Port | Class |
|---|---|---|---|
| `Countdown` | `x − 1` while `x > limit` | moves by `Ticks()`, stops at the limit | discrete |
| `CountdownPastZero` | `x − 1` until `< 0` | moves by `Ticks()` | discrete |
| `Countup` | `x + 1` (up to the limit) | moves by `Ticks()` | discrete |
| `CountupEvery` / `CountdownEvery` | `x ± 1`, then `% N == 0` (or a one-shot `== N`) | moves by `Ticks()` and lands on the multiple of N it passed, so the test passes once per N ticks | discrete |
| `Advance` | `x + k` (a constant k: spin, phase) | moves by `Share(k)` | continuous |
| `Rate` | `x + k` with k kept in memory | k becomes `Share(k)` | continuous |
| `PerStep` | float amount applied once per step | `amount · s` | continuous |
| `EaseDivisor` | `x += (goal − x) / d` | `d' = 1 / (1 − (1 − 1/d)^s)` | continuous |
| `Decay` | `x *= f` | `f^s` | continuous |

### 1.3 Step-counter periodic tests (`runtime/src/step_counter.cpp`)

The global step counter `system_w+0xC` (0x8065AA6C) is incremented by RunTasks
(0x800416F8). Here it advances by `Ticks()`. About 280 game sites read it, and
so do the scene counters. These tests are discrete by design.

- **`% N == k` tests** are hooked (`MH3_HookedLoad32`) at the sites listed in
  the generated `runtime/src/step_counter_sites.inc`
  (`scripts/step_counter_sites.py`). They pass on exactly one step per N ticks.
- **Duty cycles and phases** (`% N < k`, `& bit`, `counter << s`) read the
  tick-count counter itself, so they follow whole ticks of game time.

### 1.4 Native ports

A hot or step-dependent guest function is replaced by C
(`REGISTER_NATIVE_FUNCTION_AS`). The port either:

- scales an argument by call site (`ctx->lr`), as in `shell_motion.cpp` and
  `enemy_turn.cpp`; or
- reimplements the body with `Scale()`, `Share()` or `Fraction()` (continuous),
  or runs it once per whole tick with `Ticks()` (discrete).

---

## 2. Tick sites: inventory

There are 514 entries in total. The ambient flutterer 0x803BC584 has none
left: its native port (`ambient_flutter.cpp`) scales its angles and counts its
waits itself.

### 2.1 Counts per kind

| Kind | Count | Class |
|---|---:|---|
| Countdown | 170 | discrete |
| Countup | 142 | discrete |
| CountdownPastZero | 70 | discrete |
| CountupEvery | 6 (0x803BBB00, 0x802F3420, 0x8028CB4C and 0x80258588, N = 32; 0x80113DC4, N = 5; 0x801143E4, N = 30) | discrete |
| CountdownEvery | 2 (0x80264500, N = 90; 0x808F7A7C, N = 10) | discrete |
| **Discrete total** | **390** | |
| Advance | 45 | continuous |
| PerStep | 60 | continuous |
| Rate | 11 | continuous |
| EaseDivisor | 7 | continuous |
| Decay | 1 | continuous |
| **Continuous total** | **124** | |

### 2.2 By subsystem

Groups follow the entries' comments. Entries without a comment belong to the
group of the commented entry before them.

| Subsystem | Entries | Discrete | Continuous | Examples |
|---|---:|---|---|---|
| Sound handles / sound countdowns (0x800DE3D8) | 32 | 32 Countdown | — | 0x800DE41C (life s16 +0x48), 0x800DEDFC |
| Stream volume fade (0x800EB378 StreamUpdateVolume) | 5 | 5 Countup (steps vs length) | — | 0x800EB40C, 0x800EB514 |
| Enemies / large monsters (DOL 0x800FB…–0x801BA…, plus em_data RSO 0x809735B4) | 50 | 29 Countdown, 8 Countup, 1 CountdownPastZero | 9 PerStep, 2 Advance, 1 Decay | enemy update 0x801382A0: the height offset (+0x1AC) easing to 0 by 10 0x8013896C (PerStep) and `*= 0.9` 0x801389AC (Decay) (the model offset it feeds, cleared each step, takes it whole: 0x801389C0 is not a site); flying enemy vertical speed 0x80134ED0/0x80134FA8, y += 10 0x8014F6F0 (PerStep); enemy spin 0x801BA674 (+5461) and 0x801BA938 (+2731) (Advance); ramps 0x801A9810… (PerStep); action countdowns 0x801A1488, 0x801AC6D8; the timers of the senses step (0x801298E0, every monster every step, also while frozen): paint 0x80133704 (s16 +0x810), the status buildup decay 0x801333B4 (every 300 ticks), +0x444..+0x44A 0x80131D08… |
| Small monsters (0x801AFE24, 0x8034F588–0x80355A44) | 15 | 6 Countdown, 9 Countup | — | wait 0x801AFF6C, state steps 0x80350EE8…0x80356120 |
| Effects (spawner, effect life/age, debris, markers) | 11 | 2 Countup, 1 CountdownPastZero | 7 Advance, 1 PerStep | effect age 0x80308050, life 0x8030805C; debris gravity 0x80308A00 (PerStep), spin 0x80308A18 (−2185); marker bob/spin 0x8031F728/0x8031F734; online markers 0x803C37D4…0x803C3A34 |
| Objects (generic, volcano, village, desert, stage, 0x8035AF48, the field actor 0x8038595C, the field object step 0x8032C574) | 20 | 8 Countdown, 5 CountdownPastZero, 7 Countup | — | 0x8010A538, volcano 0x8011D02C/0x8011D31C/0x8037F62C (up to 450), desert 0x80367920/0x80368ACC/0x80369360/0x80369BA8, stage 0x802B4BA8; the field actor's wander wait 0x803A451C (its turns: `field_actor.cpp`) |
| Uncommented (in the field player range) | 3 | 2 Countdown, 1 CountdownPastZero | — | 0x8024ACD0, 0x8024C224 (limit 1), 0x8024C314 |
| Hunter moves and timers (the sweep of every weapon's actions and the hunter's evades and moves) | 52 | 9 Countdown, 3 Countup, 1 CountdownPastZero, 1 CountupEvery | 32 PerStep, 6 Advance | pushes applied once a step in a frame window: the rolls' Evade Distance push 0x80247C44/0x8025A2F0, the weapon-drawn evades 0x80248D54…0x80248FD8 (by weapon kind), the lance's step 0x80289130, the bowguns' side steps (push 0x8028ADC4/0x8028B100, move angle ±364 a step 0x8028ADA0…0x8028B0EC, Advance), the switch axe's turn ±128 0x8034DCCC/0x8034DCF0; the attack lunges K · cos(90° · frame / N) of 0x802556F8, 0x80255A50, 0x80255EFC, 0x8025779C, 0x80257BE0 (by weapon kind), sword and shield 0x802839AC and long sword 0x80331DA8; carrying 0x8025CAC0/0x8025CCB0, swims 0x80254010/0x802545B8 (and the swim dash wait 0x802544D8), the ledge push 0x80247D58, 0x8025D7DC, the velocity move 0x8026A548 (0x8026A54C…) and the knockback 0x8027AF98 (steps +0xB4, velocity +0x9C…); timers: idle 0x80246C64, +0x3F4 0x802581F0, +0x34 0x8025C3B8, the bound hunter's escape 0x802587FC with its mash cooldown 0x802587B8 and shake every 32 0x80258588, the long sword actions' input cooldowns (+7) 0x803323B0…0x803342E8 |
| Grilling meat (PlGrillStep 0x8024AAB8) | 1 | 1 Countup | — | the cook time 0x8024AE74 (s16 +0x5B8) against the meat's raw/done/burnt steps |
| Village and city NPCs | 4 | 1 Countdown, 2 Countup | 1 Advance (yaw +512) | 0x80201884, 0x80203744; the script's steps in an entry (s16 +0x20A, NpcScriptStep 0x801FFD04) 0x801FFE40/0x801FFE74. Their eased and timed turns are `npc_turn.cpp` (section 4) |
| Talk windows (typing) | 2 | 2 Countup | — | the village talk (0x8021D258, VillageTalkTypeStep, 0x806ACC2C) and quest NPC talk (0x803C80F4, QuestNpcTalkStep, +0x10C) count steps and show steps / 2 + 1 letters (FontTypeText 0x8005CAC8) |
| Player (anchored action, conditions, side timer, input) | 7 | 4 Countdown, 1 CountdownEvery, 1 Countdown (0x802EF2AC) | 1 PerStep | anchored slide offset 0x80259EFC (motion 422, PerStep); conditions 0x80264378, 0x80264500 (% 90), 0x80264760; input lock (s16 +0x352) 0x8026B018 and the Classic chord window (u8 +0x137) 0x8026C0C4 (section 7) |
| Camera | 23 | 9 Countdown, 1 CountdownPastZero, 5 Countup | 7 EaseDivisor, 1 PerStep | field camera height eases 0x802B638C…0x802B6610 and ease timer 0x802B65BC/0x802B65FC (these 9 now act only on the translated original that port checks run: `field_camera.cpp` does the same itself); climb view countdowns 0x802B7960/0x802B799C (0x802B7880, run twice a step); collision camera (CameraCollide): ticks a correction is held 0x802B832C…0x802B8578 and its release blends 0x802B8850; scope zoom 0x802B99F0 (PerStep); aim view blend 0x802B9F50; zone camera blends 0x802BACE8/0x802BAF18 (the blends' curve 0x802BB0B0 reads the step's time, `field_camera.cpp`); script wait 0x802BCDC8; path hold 0x802BD878 and frame 0x802BD8B0 (evaluated continuously by `camera_script.cpp`) |
| Area ambience (0x802E4CC0) | 3 | 3 Countup (cycle steps) | — | 0x802E4D60, 0x802E4DB4, 0x802E4E24 |
| Creatures and fish | 29 | 7 Countdown, 5 CountdownPastZero, 7 Countup | 10 PerStep | fish (0x8033FF88's states): appear scale 0x803407D0…0x803407FC and steps 0x80340810, bait approach and lunge speeds 0x80341064…0x80341988, waits 0x803410AC…0x80341FB4 (their turns and the swim state: `fish_effects.cpp`); creature 0x80358BA4 frame hold 0x80358EB4; volcano creature colour fade / steps 0x80382498…0x803826EC |
| Effect objects (the sweep of the callbacks live in Deserted Island area 10) | 45 | 7 Countdown, 14 CountdownPastZero, 16 Countup | 3 Advance, 5 PerStep | lives and end delays 0x803089C0, 0x80106608, 0x8010A310, 0x801E3EDC; hunter and weapon effect sparks 0x8010F760, 0x80109C70; colour key frames 0x8010920C, 0x801E36BC; a joint effect's spin 0x8010CC48 (PerStep, 2048); a proximity glow's alpha 0x8011A9F0/0x8011AA30/0x8011AA8C (Advance 13, −5, −3); glowing parts' pulse 0x802FA510 |
| Effect objects of the Alatreon quest, the arena and the village (a second sweep, `MH3_TRACE=effect-objects`) | 36 | 4 Countdown, 9 CountdownPastZero, 17 Countup, 3 CountupEvery | 2 Advance, 1 PerStep | the hunter effects' switch 0x80113990 (by kind +2): ages for colour keys 0x80113A64…0x80114788, lives 0x80113BC8/0x80113C48/0x80114420, part waits 0x80113D04 and 0x801143E4 (every 30); Alatreon's pieces 0x802F2D14 (delay, wait, growth 0x802F3094, colour phases 0x802F30D4…0x802F3134) and curves 0x802F3420; monster effects 0x8030555C/0x803055E4; spins 0x80308748/0x8030889C (Advance 5461); village effects 0x80393FD4/0x80393FF8/0x80396700 |
| Ambient creatures (0x803B9AE8, 0x803BB7F8, 0x803BB984, 0x803BBDA4) | 17 | 8 Countdown, 5 CountdownPastZero, 1 Countup, 1 CountupEvery | 1 Advance, 1 Rate | glow phase 0x803BB824 (−1024, Advance), glow pulse 0x803BB838 (Rate); sound every 32: 0x803BBB00. The flutterer 0x803BC584 is a native port (section 4) |
| Online sync objects (the ten 60-byte objects at 0x806C1520, ObjectAgesUpdate 0x80345188) and the online quest load | 3 | 1 Countup (limit 899), 1 CountupEvery (32), 1 Countdown | — | 0x803440A8 (in 0x80343F50): steps since the object's last sync message (s16 +6); at 900 (30 s) it resyncs (state 8 sends one, 0x803385C8). It counted steps, so at 60 fps every 15 s |
| Actor actions | 4 | 2 CountdownPastZero, 2 Countup | — | 0x803327A4, 0x8034E5EC |
| Quest, quest clock, result screen, event demo | 16 | 3 Countdown, 13 Countup | — | quest clock 0x803B0C00/0x803B0CE4; result screen 0x80398008/0x80398080; timer 0x806C4898 at 0x80383AB0; event-demo clock 0x803C477C (0x806D4FA4) |
| Quest objects (quest_data RSO, 0x808C…–0x808F…) | 94 | 18 Countdown, 24 CountdownPastZero, 1 CountdownEvery, 30 Countup | 21 Advance | countdown 0x808F7A7C (`% 10`, kind 123 effects); spins 0x808E1C64 (−5461), 0x808E6044…0x808E60F0 (13 spins by kind), 0x808F4848 (+9102); phase 0x808E1E6C (+128) |
| Title (demo_data RSO, 0x8097…–0x80993…) | 30 | 14 Countdown, 4 Countup | 10 Rate, 2 Advance | idle to attract movie 0x8097EF14 (up to 900); pulse phase Rate 0x8097EF54…0x809831C8; character view turn 0x8097DDE0/0x8097DDF8 (±1024); logo timer 0x80993528 |
| Online menus (net_data RSO, 0x8097F…–0x80981…) | 9 | 5 Countdown, 4 Countup | — | the connecting window's Felyne frame flip every 20 (0x80980520, +0x12C; 0x8097F2C0 in another window) and message cycle every 150 (0x8098054C, +0x12E); seconds waiting (+0x128, shown / 30) 0x80980EA0, 0x80980EF8, 0x8098114C; the text list's arrow bumps 0x8097F778/0x8097F794 and hold repeat from 12 held steps 0x8097F830 (+0x2C, limit 11) |

**Pattern.** Every continuous tick site is a motion or visual amount: spins,
phases, ramps, eases, fall speeds. Every discrete one is a timer, a
steps-in-state count, or a periodic trigger.

---

## 3. Step counters (discrete)

### 3.1 Counters advanced by ticks (`runtime/src/step_counter.cpp`)

| Guest function | Name | What advances by `Ticks()` |
|---|---|---|
| 0x800416F8 | RunTasks | `system_w+0xC` (0x8065AA6C) moved on by `Ticks() − 1` before the guest's +1 |
| 0x800D2F80 | PlayTimeTick | play-time steps (0x8065B2F4), and seconds every 30 (0x8065B2F0); one pass per tick |
| 0x80212680 | VillageStepCounters | village countdown (u8 +0x28), ramp to ±60 (s16 +0x34), step counter (+0x30) |
| 0x8021DC88 | VillageTalkStep | village talk steps (+0x150) |
| 0x802E4C88 | AreaStepCounters | three area step counters (0x806C04E8, 0x806C02D8, 0x806C046C) |
| (tick sites 0x803B0C00 / 0x803B0CE4) | quest clock | quest work+0x28 (0x806C7D00) |
| `player_timers.cpp` | PlayerStepTimers | the player's step counter PLW+0x20 |

### 3.2 Hooked periodic tests (`runtime/src/step_counter_sites.inc`)

The file is generated by `scripts/step_counter_sites.py` and has 299 entries:

- **285 of kind 1**, `counter % N == k` on a global or scene counter: 262 in
  the DOL and 23 in RSO overlays.
- **12 of kind 2**, the same test on the player's step counter PLW+0x20: 5 in
  the DOL and 7 in quest_field. These are the script's `MANUAL` entries, for
  example 0x802645B0 (N 30), 0x8026461C (N 32), and 0x808D11B0…0x808D12E0
  (damage-condition effects).
- **2 of kind 0**, another `% N` use such as `< k`. They are read as the
  tick-count counter: 0x80214ACC (48) and 0x802E490C (32). The 12 equality
  tests in quest_field's pl_effect_sub (0x808D15D4…0x808D56F4) were first
  read as kind 0: llvm-objdump prints their branches with a prediction
  hint as `bc 13, 2` rather than `bt 2`, which the script did not read, so
  at steps without a tick they passed every step (kind 20 effects about 16
  times as often in the Alatreon quest).

The most common N are 8 (101 entries), 4 (75), 32 (25), 16 (22) and 2 (21).
The largest is 2048. One site has N = 0, where N is only known at run time.

Nine sites sit inside native ports (`PORTED` in the script:
`enemy_timers.cpp`). Those ports test the counter themselves with
`StepCounter::Every(n, k)`.

All of these are **discrete**. A test passes on one step per N ticks. On steps
shorter than a tick it fails, and a long step can pass at most once.

---

## 4. Native ports that use GameStep

C = continuous, D = discrete (whole ticks). Guest addresses come from the
`REGISTER_NATIVE_FUNCTION_AS` lines.

| File | Guest function(s) | Continuous | Discrete |
|---|---|---|---|
| `shell_motion.cpp` | 0x802AB3A8 ShellRateAdd, 0x802AB3B8 ShellRateAddG | C: shell position += vel·s, vel += acc·s | — |
| | 0x80073FCC VecAddInPlace | C: the addend × s at the 30 `kRateCalls` return addresses: enemy position/velocity (0x8013548C, 0x80135558, 0x8013562C, 0x80328BDC), desert enemy swims, 0x801BA438, 0x801CBD94, 0x801CF124, 0x801D2208, 0x802C3444, 0x802D32E0, camera demo actor 0x802D853C, 0x802F511C, debris 0x80308960, 0x8031DF1C, desert object group 0x803692F0 | the other ~200 callers add whole (offsets, push-outs); correct, since they are not rates |
| | 0x8013547C EnemyAddVelocity | C: enemy +0x188 += +0x310 · s, always | — |
| | 0x80130460 EnemyHeightAdd | C: the height offset (+0x1AC) += amount · s, always (its 37 callers raise or lower a body a step at a time: burrowing, surfacing, landing; a state sets it through 0x80130450 and adds each step) | — |
| | 0x80050F88 VecMagWrapper | C: length × s at the three desert-enemy budget calls (0x801A07E4, 0x801A0AEC, 0x801A0EC4) | — |
| | 0x802E77EC PlayerTintPulse | C: tint phases +0x13E/+0x140 moved by `Share(advance)` after the original | colours computed inside the original lead by up to one tick's advance |
| `enemy_turn.cpp` | 0x80133E14 EnemyTurnToward, 0x80134884 EnemyTurnStep | C: max turn → `Share()` | — |
| | 0x80133FB0 EnemyTurnOverMotion | C: the per-tick turn (2 × angle / window) by `Share()` (Deviljho's tail spin 7.1 and five more kinds' spins) | — |
| | 0x80133EA0 EnemyTurnWhile | C: the motion's advance as rate × s inside the frame window (75 callers: turns within attacks) | — |
| | 0x8012F8C4 EnemyBlendToward | C: lean step × s | — |
| | 0x8012B66C EnemyAwarenessAdd, 0x8012B9A8 / 0x8012B9EC / 0x8012BA20 gauges | C: `Share(amount)` only from the per-step caller (lr 0x8012B898, 0x8012BFB4, 0x8012C02C, 0x8012C100) | the event callers (near 0x8011F39C) add whole, which is correct |
| `enemy_glide.cpp` | 0x80135064 EnemyGlide | C: speed brake/gain × s, yaw and roll steps `Share()`, lean and pitch through the scaled helpers | +0x786 pass count (event) |
| `enemy_timers.cpp` | 0x801298C8, 0x80129A3C, 0x801307C4, 0x801556C8, 0x80133B64, 0x80133920, 0x80132D50, 0x8011E750, 0x80130D4C, 0x801C8978, 0x8038CC84, 0x8015574C, 0x8035BFC8, 0x801311B4 | — | D: every counter by `Ticks()`; periodic checks through `StepCounter::Every` (AI checks, retarget every 32, sight every 2, periodic effects) |
| `player_turn.cpp` | 0x80260304 PlayerTurn, 0x8020A258 VillagePlayerTurn | C: yaw, lean, pitch and turn blend by `Share()` | — |
| `player_timers.cpp` | 0x80260B9C PlayerStepTimers | — | D: the whole body once per tick (about 40 countdowns, stamina recovery +0x264, health recovery every N, hot/cold zones every 128) |
| `cheat_hunter.cpp` | 0x80284430 PlHammerChargeStep | — | D: the hammer's charge count (+0x310, up to 999) by `Ticks()`; its level is the count over the 30-tick charge time (0x802844D8) |
| `stamina.cpp` | 0x8027695C StaminaChange, 0x80276AA0 StaminaDrain | — | D: once per tick, each tick seeing its own PLW+0x20 |
| `reward_equip.cpp` | 0x80398FE0 RewardPages (the quest result's reward pages), 0x80398264 RewardsAnimate | — | D: the page countdown +0x2FF4 (seconds × 30) by `Ticks()`, and a tick at a time the fade-in +0x3026, the equipment page's build delay +0x3032, the points and record rows (+0x305E, +0x3061), page 8's row delay +0x3020, the blink +0x3034 and the taken item's flash +0x3036; they counted steps, so a page ran out early above 30 steps a second |
| `player_work.h` | helpers `CountDown`, `CountDownToZero`, `CountUp`, `Every(n)` | — | D: PLW+0x28 by `Ticks()`; `Every` on PLW+0x20 |
| `village_player_actions.cpp` | 21 actions 0x8020A800 … 0x8020E1F4, GlideTo 0x8020A0BC, GlideStep 0x8020A1BC | C: the runs' forward push (1 a tick, the run start 5) × s (0x8020D10C, 0x8020D260, 0x8020E0A0, 0x8020E1F4); a glide to a spot moves by the part of a tick spent; another console's player's yaw ease 1 − (3/4)^s | D: all timers and counts (idle steps +0x36, countdowns +0x28); the glide's steps (+0xB4) count ticks |
| `npc_turn.cpp` | 0x801FDEA0 NpcYawTo (by return address: the village and city NPCs' yaw eases in 0x802014C8, 0x80202F64, 0x80205110, 0x802057C8, 0x80207738), 0x801FDED8 NpcTimedTurnStep | C: an ease's gap scaled by d·(1 − (1 − 1/d)^s), so the caller's gap / d closes the step's share (d 10, 5, 4 or 2 by the action); a timed turn by `Share(amount)` | D: the timed turn's steps (+0x1E4) count `Ticks()` |
| `field_actor.cpp` | 0x803A4148 ActorTurnToward, 0x803A41D4 ActorFaceGoal (the field actor 0x8038595C: one in the Tundra quest 10057 at 0x806C76E8) | C: the goal heading turns by `Share(256)` toward a point while it wanders, the heading by `Share(4004)` (`Share(1820)` in action 0.4) toward the goal | D: the wander wait by `Ticks()` (tick site 0x803A451C) |
| `field_player_actions.cpp` | 55 actions and helpers (0x802535B8 … 0x80258AC8, 0x8026A57C) | C: forward moves × s (Slide221 0x8025D5F8, Sink206 0x80259C9C speed −4·s and z +10·s, Walk52 0x802595A0, Walk53 0x802596E8, Run119 0x80253038, Walk101 0x80252C78: the drawn walk and the swim; the R dashes Run39 0x80253750 1.5·s, Run3 0x80252704 and Run38 0x802539F8 5·s, which made the dash 25 % faster at 1.8 steps a tick); the hammer's lunging charged swings HammerSwing 0x80285144, HammerSwimSwing 0x8028648C and HammerSwimSwing1 0x802866A0, 18 or 27 · cos(90° · frame / 24) · s until frame 30, which went 38 % farther at 1.8 steps a tick); SwimPitch 0x80252B50 pitch `Share(0x400)`, levelling `Share(0x300)`; FallMove (Fall 0x802598C4, Fall2 0x8025CECC, Jump15 0x80254A28); VelocityStep 0x8026A57C (ClimbOver, KnockedFlying, Grabbed, Charge1101, jumps); Sleep 0x80250630 heal `Share(2)`; Rest 0x8024A7C0 stamina `Share(4)`; Spin1103/1162 steering `Share(192)` | D: action timers +0x28, ButtonHold counts 0x8026AB00, Motion39 +0x3F4, Escape inputs, charge steps 0x8028138C, cooldown bytes +6/+7, Sleep's 1-in-128 roll per tick, Effect13 every 50 (0x802580D4); stamina drains through `stamina.cpp` |
| `mhchar_motion.cpp` | 0x800E19D4 MHcharRootMotion, 0x800E1EB4 MHcharChannelBlend, 0x800E1D2C MHcharRootMotionDelta, 0x800E1740 MHcharFrameCheck | C: root motion over `rate · smBaseUpdateRate` frames (windows join up); cross-fade frames left −= rate·s; the pause (hit stop, +0xF1) and hold (+0xF2) counters use up exact partial ticks (`Spend`); the frame check's advance ahead (enemies' `em_frame_check` 0x8012F980, the hunter's `Pl_frame_check` 0x8026A378) spans rate·s, so a frame event fires on the one step that crosses it | the u8 counters themselves stay integral (the partial tick is kept host-side) |
| `alatreon_actions.cpp` | 0x803559F0 Alatreon's intro action (em024, quest 19001), 0x80353EBC its meteor breath | C: its poses through `demo_pose.cpp` | D: the breath effect every 8 **ticks** while the motion is in [56, 138] (its count +0x20 by `Ticks()`, `Em::Enemy::CountEvery`); the three shells on demo steps 834/844/854 once (`Em::DemoAt`) |
| `ceadeus_intro.cpp` | 0x80371730 Ceadeus's intro action (em020) | — | D: the swim-in spray every 17 **ticks** while the motion is in [82, 484] (its countdown +0x20 by `Ticks()`); the surfacing foam on demo steps that are multiples of 10, once per tick (the demo clock crossing one) |
| `monster_intros.cpp` | ten first-encounter intro actions: 0x8038C188 kind 9, 0x801C8228 kind 4, 0x8032F5B4 em026, 0x802FE5B4 em023, 0x8018B47C em016/017/021, 0x80162564 kind 8, 0x801BF050 kind 6, 0x80150830 kinds 1/2, 0x8017E56C em015, 0x801D42F0 kind 5 | C: the kind 9/kind 4/em026 cutscene paths posed by their own count (now ticks) plus `Fraction()` through `demo_pose.cpp` | D: the counts +0x20 by `Ticks()`; dust, sprays and splashes on demo steps `& 3`, `& 7`, `% 3` and `== K` once per tick (`Em::DemoEvery`, `Em::DemoAt`); roar steam every 8 ticks (`CountEvery`) |
| `monster_roars.cpp` | eleven roar actions: 0x8014858C, 0x8014B3DC kinds 1/2; 0x8017766C, 0x80178EBC em015; 0x801BE084 kind 6; 0x801C344C, 0x801C4AD0, 0x801C5DEC kind 4; 0x801CB3B4 kind 5; 0x8036F238 em020; 0x803798A0 em019 | — | D: the steam every 8 ticks while the motion is in range (count +0x20 by `Ticks()`, `CountEvery`); puffs on the step counter's multiples of 4 through `StepCounter::Every` |
| `monster_attacks.cpp` | 0x8014BBA4, 0x8014C4C8 kinds 1/2 spits; 0x8017B7AC, 0x8017C6F4 em015 discharges; 0x80188548 the Ludroth kinds' spit | C: the spit's blend from motion channel 0 to 1 at 0.1 × (ticks + `Fraction()`) | D: the hiss every 8 ticks before frame 56, the sparks (0x80306AFC) every 3 ticks; counts +0x20 by `Ticks()` |
| `monster_waits.cpp` | waits: 0x8015611C kind 3, 0x80168430 kind 10, 0x8019474C em018, 0x801ACA48 em027, 0x801B2578 kinds 30/31, 0x801C7570 kind 4, 0x8034FA1C em024, 0x80372994 em020, 0x8039E9EC em028, and about 70 more from the monster sweep's watch pass (10-06, listed in the file's header); repeats: 0x8017AA5C em015, 0x8019F3DC em025, 0x803519B8 / 0x8035455C em024 | C: em020's pitch turn (45.512 × the rate a tick) by `Share()` | D: the waits (count +0x20) by `Ticks()`, a test of an exact count passing on the step that crosses it; sparks every 5, steam every 10, bursts every 7, sparks every 6 ticks |
| `demo_pose.cpp` | 0x8014620C DemoPose, 0x80146308 DemoPoseAxes (a monster's position and angles in an in-engine cutscene, from keyframe tables at frame 2·steps) | C: the tables are read at the steps plus `Fraction()`, so a monster in a cutscene moves on every step as the interpolated camera path does | the demo clock (or the action's own count) stays whole |
| `effect_step.cpp` (+ `particle_manager_calc.cpp`) | 0x800A4C20 EffectCalc, 0x800A8AC0 EmitterCalcEmitter, 0x800A8E5C EmitterCalcEmission, 0x800AC35C ParticleManagerCalc, 0x800A1D0C AnimCurveExecuteChild | C: particles by dt: velocity += fields·dt, position += … · momentum·dt, fractional age, linear curves at the fractional age | D: `Effect::Calc` once per whole tick (a particles-only pass on steps with no tick); emitter curves and emission on ticks; particle values that jump at a tick (random picks, key corners) and texture offsets/flipbooks held to the tick; child emission and retirement when the age crosses a tick |
| `ambient_hover.cpp` | 0x803BB570 AmbientHover | C: phase +0x1A4 by `Share(rate)`; height y += speed·s; x/z from the angle; at a bound the phase rests until the next whole tick (`Back()`), as the Wii's clamp does | D: the 1-in-4 orbit angle jump (+4096) and the 1-in-32 retarget, rolled once per tick |
| `ambient_flutter.cpp` | 0x803BC584 AmbientFlutter | C: flap angles +0x1D8/+0x1DC by `Share(4096)`, orbit +0x1F0 by `Share(8192)`, position += velocity·s | D: the wait and the flight length (+0x10) count `Ticks()` |
| `yure_chain.cpp` | 0x802AC4E8 YureChain (running state; the set-up step runs the original) | C: nodes += velocity·s; rest pull, gravity and springs × s; damping (1 − d)^s | the anchor's drag (its motion since the last step) is already the step's share |
| `creature_fade.cpp` | 0x803822D8 CreatureFade | C: fade += (1/frames)·s | the direction flip is an event |
| `group_shuttle.cpp` | 0x80368DCC GroupShuttle | C: member z moved by amount·s | D: step counts (+0x40) by `Ticks()`; the speed ramp (count/2) and state changes follow whole ticks |
| `hit_timers.cpp` | 0x80299F5C HitTimerStep | C: delay, active time and phase floats −= amount·s | a phase's start step (marking bit 1/2) is still one step |
| `swing_effect.cpp` | 0x80116B64 SwingEffectUpdate | C: yaw evaluated at phase + `Fraction()` | D: flip-book joint, phase counter and end holds once per tick |
| `pulse.cpp` | 0x802D9F18 PulseUpdate | C: value at phase + `Fraction()` | D: the u8 phase stores whole ticks |
| `screen_fade.cpp` | 0x800581A4 TickFades | C: level += `Share(change)` | D: steps left −= `Ticks()` |
| `camera_script.cpp` | 0x802BD3A0 CameraScriptInterpolate, 0x802BD938 CameraPathEvaluate | C: script waits interpolated at `left − (Ticks + Fraction − 1)`; paths blended between the two frames around the fractional frame, except across a cut (two keys less than a frame apart: the blend flew the camera between shots, the quest intros' "zoom"), where the earlier frame holds; the path evaluation is native (kinds without the orbit or ground adjustment), walking the keys from where the last step's lowest frame stopped instead of from the first key | D: the counters themselves (tick sites 0x802BCDC8, 0x802BD8B0) |
| `texture_scroll.cpp` | 0x803750E8 SeaSurfaceUpdate, 0x801A9274 Mat6ScrollUpdate, 0x8031ED54 ObjectListScrollUpdate (plus GetEffectMtx/SetEffectMtx 0x80095434 / 0x800953A8) | C: UV offsets −= / += const·s; the sea surface U offset = sin of (`system_w+0xC` + `Fraction()`) << 7 | D: ObjectList's random scale/+48 re-rolled once per tick (a step without a whole tick keeps each part's last roll), its pass count +5 by `Ticks()` |
| `quest_clock.cpp` | 0x803A9014 QuestClockTick | C: at variable step, frames −= s·(1e6/30)/period instead of the OSGetTime delta (the fixed step keeps the original's OSGetTime delta, at least 1 frame) | the s32 HUD copy truncates |
| `fps_timers.cpp` | 0x802EA658 PlayerSkillTimers | — | D: the two skill countdowns once per tick |
| | 0x8012E9CC TimedCheckByKind, 0x8039D174 QuestResultInit | — | seconds × fps float (fixed at 30), so not step-dependent |
| `rumble_timers.cpp` | 0x80044AF4 TickSystemTimers | — | D: rumble countdowns by `Ticks()` |
| `pad_input.cpp` | 0x80042088 PadReadWii, 0x800429A4 PadReadGc, 0x80044368 PadGameKeys | — | D: held-button repeat (13 ticks after a change, then every 3) of the Wii Remote, Classic, Nunchuk, GameCube and game key sets and of the stick directions; the game keys' locks (Psw+0x336) and the WPAD info wait (system_w+0x7E0) by `Ticks()` |
| `camera_input.cpp` | 0x802B6864 CameraYawInput, 0x802B6D30 CameraPitchInput, 0x802BDC90 CameraAngleEase | C: yaw turn by stick or d-pad (1035/843/747 a tick at full speed), swimming pitch tilt (560/368) and the swing back behind the player by `Share()`; angle eases close 1 − (1 − 2^−n)^s of the gap, the whole units dithered over steps | D: the speed ramps (5 or 10 ticks yaw, 20 pitch; at least 1 on the first step held) and the 30-tick pitch hold |
| `field_camera.cpp` | 0x802B5CBC FieldCameraUpdate | C: pitch levelling (10/30 a tick) and the look-up tilt (48/144) by `Share()`; target and eye heights through `MH3_EaseDivisor`; a reset blend keeps t^s of the old position each step | D: the settle timer (+0xA8) and the reset count (+0x44) by `Ticks()` |
| `player_aim.cpp` | 0x8027A5E0 PlAimInput | C: aim yaw (384/512, or 76 to the stick table's) and pitch (5, or 1 to the table's) by `Share()`. **Unverified in play**: checked against the original at the stock step through the player fuzz only, never played with a bowgun or at a variable step | — |
| `map_sound.cpp` | 0x800DFE30 MapSoundUpdate | following the listener runs every step | D: repeat countdowns by `Ticks()` |
| `object_ages.cpp` | 0x80344EBC ObjectAgesTick | — | D: ages +0x34/+0x1C/+0x1E by `Ticks()` |
| `effect_objects.cpp` | 0x800FF938 EffectSpawnerUpdate | — | D: each spawn slot's wait (+0x10) by `Ticks()` (map effects such as the Deserted Island shore waves) |
| `loading_screen.cpp` | 0x800D09E4 LoadingLetters | C: hopping letters move by 8 units and 1820 of tilt per tick times s (fractional hop progress) | D: sequencing (a letter starting every other frame, the 4-frame holds, the restarts) on whole frames |
| `movie_draw.cpp` | 0x804344F8 MovieDrawFrame | — | D: decodes `Ticks()` video frames (29.97 fps content) |
| `step_counter.cpp` | see §3 | — | D |

The remaining files are infrastructure, not game logic:

- `frame_pacer.cpp` sets the step (`SetVariable`, `Begin`).
- `game_frame.cpp` runs the step probe (`Save`/`Restore`, 1.0 against
  2 × 0.5).
- `port_check.cpp` and `port_check.h` check ports only when `Ticks() == 1`.

---

## 5. Open work and known leftovers

- Settle probes of Moga (1060), Sandy Plains (1046), Flooded Forest (1038),
  Tundra (1045), Volcano (1048), the sandship (14002) and Alatreon (19001) are
  clean apart from known noise (recorded on the earlier GX renderer; rerun the
  relevant ones on the current renderer):
  - particle-slot reuse;
  - scratch (80ff9xxx–80ffcxxx, 807adfxx, 807aexxx);
  - the quest clock's OSGetTime doubles (806ce720/728) and an allocator pointer
    (8069af74);
  - the ambient hover phase near 0 or 0x8000.
- **Online** (Pi 5 arm32, a local MH3SP server: a poster and a joiner
  through `MH3_ONLINE_AUTO`, the Tundra quest 10057, settle probes every
  ~1150 steps from the city to 4 minutes into the quest, writers named by
  `MH3_WRITE_WATCH` on the poster): the city NPCs' eased and timed turns and
  script steps (`npc_turn.cpp`, tick sites 0x801FFE40/0x801FFE74), kind 8's
  rise at the quest's start (EnemyHeightAdd, `shell_motion.cpp`), a field
  actor's wait and turns (`field_actor.cpp`, 0x803A451C) and the sync
  objects' resync count (0x803440A8). A write watch on the joiner then named
  three more step counts: the quest load's member sync every 32 steps
  (0x8028CB4C), the 30-step input lock at the quest's start (0x8028D064) and
  a field object's random wait (0x8032C5A8). Left in the last probes: the sway
  chain's values (YureChain's integration differs a little with the step
  length) and the joint matrices of the bones it moves (their ratios spread
  from 0.3 to 24, most at 1), effect particles on the worker, scratch
  (80ff9xxx, 807afxxx) and two request queues (8069c9ec, 8069cad0, written by
  0x800E6740 into a slot it is handed).
- **Not probed:** the arena beyond 9001/9002; scenes that need input
  (close-range fights, carts, cutscenes and event demos).

Found since:

| Item | Where | State |
|---|---|---|
| Enemy and hunter frame events fired on several steps (effects, sounds keyed to a motion frame: four times at 120 fps) | 0x800E1740 MHcharFrameCheck | fixed: native port in `mhchar_motion.cpp`, the advance ahead is the step's (rate·s) instead of the tick's; port check matches at the stock step (Ceadeus quest 2006 on the Pi); the intro's 34 one-off effect sites spawn as often as at the fixed step |
| Ceadeus intro splash 3-4 times as dense at 120 fps (the user's "splash at the rocks") | 0x80371730 (em020 intro action) | fixed: native port `ceadeus_intro.cpp`: the spray countdown in ticks, the foam's `demo steps % 10` once per tick; the frame-check fix covers the rest. Spray spawns over the intro 54 fixed / 198 variable before, 54 / 53 after (`MH3_TRACE=particles`) |
| Loading screen letters moved only on whole ticks | 0x800D09E4 | smoothed: fractional hop progress (port check matches at the stock step) |
| Ambient flutterer angle adds (+0x1D8, +0x1DC +4096; +0x1F0 +8192 a step) | 0x803BC584 | fixed: native port `ambient_flutter.cpp`; built and played; port check and arena settle probe not run yet |
| Sway chain solver damping and springs | 0x802AC2D8 → 0x802AC4E8 | fixed: native port `yure_chain.cpp`; built and played; port check and arena settle probe not run yet |
| Small flying-enemy y drift in Moga (1060) | enemy +0x18C | traced, no rate bug: every writer of y, the velocity (+0x310..), pitch, roll and speed is step-scaled or a floor clamp, and the lift curve is read at the animation frame (+0x98, advanced by `MHcharRootMotion`). The probe difference is the pursuing AI diverging between the two runs |
| Effect object fade per step (kind 7) | 0x80308D64 (countdown +0xC at 0x80308DB4) | ported (EffectFadeUpdate, effect_objects.cpp): the countdown by the step's ticks, the alpha (data +0xF) by its share of +8 a tick, clamped at 0 |
| Hunter weapon trail one step per sample (about 14 times too short at 400 fps) | 0x80115AE4 (six Mtx34 per trail model, data +0xC) | fixed: HunterTrailUpdate (effect_objects.cpp) shifts the history once a tick; Pi 5 sword-and-shield swings at the variable step draw the long arc |
| Camera during a monster's hold of the hunter ran per step (13 times as fast at 400 fps) | 0x802B8ED0 BindCamera: the count +0x114 picks the time in keyed paths (kinds 4, 7) or drives orbit eases (kinds 1, 2) | fixed: the original once per tick; past the last tick (the step's fraction) it runs once more for the next tick's place with its state (+0x114..+0x145) put back, and the eye, target and field of view sit that fraction of the way from the last tick's place (`field_camera.cpp`, `BindPlace`); port check matches. Deviljho's grab (7.10 forced at the hunter in the arena pit): held 181 ticks with the count at 180, and 411 ticks with 410, at the variable step (the count once a tick; it was once a step); with the fraction the hold's eye and target moved on 6863 of its 6870 steps (`MH3_TRACE=camera-blends`, `bind`; before, only on steps with a tick) |
| Collision camera: how long a correction is held (to a row's 20..60) and its release blends (20..50) counted steps | CameraCollide 0x802B8244 (+0xA2, +0xA5; +0x100..+0x103) | fixed: tick sites; the release blends' curve reads the step's time (below). Not seen in play yet (no wall hit by the camera in the test walks) |
| Scope zoom 1.75 a step (15..60) | ScopeCamera 0x802B988C (+0x1DC, +0x1E0) | fixed: PerStep site 0x802B99F0; unverified in play |
| Aim view blend in (9 steps) and its side offset easing 6/64 a step | AimCamera 0x802B9C68 (+0x234; +0x23E toward +-0x800) | fixed: tick site 0x802B9F50; the offset by AimCameraPort (the gap shrunk by (58/64)^s); port check matches at the stock step; unverified in play |
| Zone camera blends (the village's and the camps' fixed cameras, 0x805D4070's regions): the angle blend held between ticks, the point blend counted steps | 0x802BAC0C ZoneAngleBlend, 0x802BAE68 ZonePointBlend (zone +0x44 steps left) | fixed: tick site 0x802BAF18; CameraResetCurve 0x802BB0B0 reads the blends' steps left at the step's time. Village start blend 58.7 ticks variable / 58.0 fixed; the camp's point camera (kind 1, quest 1000) 48.0 ticks at both steps, moved on all 621 steps |
| Monster effects 0x803048A4 kinds 1 and 103 ended about 10 ticks early at the variable step (Alatreon: 13 against 24 ticks), and kind 20 hunter effects (0x80107E40) 2.75 | 0x8030524C, 0x80108AD8 | no rate bug (`MH3_TRACE=effect-states`, Pi 5 Alatreon area 1, 120 s each): neither has a counter; they live while the ef effect has particle managers (EffectMove 0x800F9984), and that part matched: kind 1 12.97 ticks variable / 13.00 fixed, kind 103 13.05 / 13.0, kind 176 50.6 / 51, kind 177 12.5 / 13. The rest of a life is the create, ending and release states, a step each (3 ticks at the fixed step, ~0.24 at 380 fps): kind 20's 19.00 / 16.24. The 24 ticks were the trace's: at the fixed step a slot released and taken again in one step kept its callback, so two or three lives counted as one; a state 0 after another state now starts a new life. Left: kinds 101 and 182 (sparks, 7 ticks) stay in state 1 1.2–1.4 ticks longer at the variable step (8.4 / 7.0–7.3); ef effects start and end on whole ticks while the object's states change on steps |
| g3d animation frames set by game code | FrameCtrl::SetFrame 0x80061F28 | probed in Deserted Island area 10, the arena (9004), the village and Alatreon (19001): no calls; animation objects move by UpdateFrame at the scaled base rate (texture SRT: `MH3_TRACE=srt`), and AnmObjChrRes::SetFrame is native |
| Ambient hover phase bounce at 0 / 0x8000 | `ambient_hover.cpp` `Back()` | rests at the bound until the next whole tick (as the Wii's clamp does); built and played; not settle-probed |

Found while writing this inventory (from reading the code; not verified at
runtime):

| Item | Where | Why |
|---|---|---|
| Soft-reset fade speed | `reset_fade.cpp`, 0x800D2230 ResetFade | fixed: each call stepped the fade level by `step` or `2·step`, and the hold by 1 or 2 (the retrace budget `kRetracesPerFrame` and the movie flag), so the fade ran faster at high fps; now a step applies its `GameStep::Share` of those per-tick amounts. Not played (the reset needs HOME) |
| Effect-45 part flicker | `texture_scroll.cpp` ObjectListPort, 0x8031ED54 | fixed: the random scale (`Rand() % 20`) and +48 were re-rolled, and the pass count +5 moved on by 1, on every call (once per step). Now each part rolls once per whole tick the step covers (two RNG 0 draws each, so a step of two ticks rolls twice and the stream moves as at 30 Hz); a step without a whole tick keeps each part's last roll, and a part first seen between ticks shows the next roll without drawing it |
| Integer `Share()` quantisation | every `Share(k)` user | shares are whole units. With a small k at a high fps (for example `Share(2)` heal at s ≈ 0.25) the value moves once every few steps. It is correct on average; the steps are visible only if k is small and the value is drawn directly |

Found with the input work (section 7), not changed:

| Item | Where | Why |
|---|---|---|
| WPAD DPD switch-off wait | 0x80044C68, small-data s32 0x80796CF8 (300 from 0x800440BC) | counted once per step: the IR camera of a Wii Remote is turned off after 300 steps. The host has no IR, so nothing visible |
| Accelerometer history | `pad_input.cpp`, Psw+0x2A4 | one sample a step (16 kept); swing checks read the last 4. The host's remote is stationary, so the history is constant |
| Camera follow lag | `camera_input.cpp` CameraAngleEase | the stock ease (`+= gap >> 2` after the target moved) trails a turning target by 3·v; the continuous ease (1 − (3/4)^s) trails by v / ln(4/3) ≈ 3.5·v at short steps and by ≈ 2.4·v at 2.5-tick steps. Measured: 3102 units after a full-speed turn at the stock step, 3365–3579 variable, 2458 at 2.5-tick steps. The static gap closes at the Wii's rate |
| 0x803C87F8 wait (+0x109) | quest/online UI helper | counts down once per call; not traced to what it gates |
| Character-creation UI animation | f32 0x80797F2C (+2 a step), heap f32 words +1 a step and an int +8 a step (0x92AF.., 0x92B16EDC, 0x92B16F58, 0x92B2F048) | seen by step probes while the keyboard opens (ticks 1083–1095); not traced. Probably layout animation frames; nothing looked wrong in play |

---

## 6. Discrete things that may look lumpy at high frame rates

These change visible state (position, orientation, UV or colour) only when a
whole tick passes, so at 60–240 fps they update at 30 Hz. They are ordered by
how likely they are to show and how easy they are to convert.

1. **Sea surface U offset** (`texture_scroll.cpp`, 0x803750E8): fixed, the
   phase is the counter plus `Fraction()` ("Swing Ceadeus's texture offset at
   the step's game time").
2. **Other step-counter phases** (`counter << s` sines, `step_counter.cpp`
   comment). Any such read of `system_w+0xC` or the scene counters is a
   whole-tick phase. They are not listed; the script's "other read" output
   (stderr of `scripts/step_counter_sites.py`) enumerates the candidates. Fix
   as in item 1, per site or by a hook that returns the counter plus the
   fraction for phase reads.
3. **Loading screen letters** (`loading_screen.cpp`, 0x800D09E4): fixed, each
   hopping letter's progress is fractional and its height, slide and tilt
   follow it; the sequencing stays on whole frames.
4. **Effect emitters** (`effect_step.cpp`). Emitter curves (the emitter's own
   animated transform and parameters) run only in the per-tick
   `Effect::Calc`. Particle texture offsets and flipbooks are held to the tick
   on purpose, because blending them made fire churn. Particles themselves are
   continuous. Scrolling-UV particles and emitters with animated transforms
   step at 30 Hz. Only the emitter-curve part is worth considering.
5. **Visual values driven by whole-tick counters (tick sites)**. The counter
   is discrete, so whatever is interpolated from it steps at 30 Hz unless the
   reader interpolates, as `camera_script.cpp` does for camera paths:
   - the stream volume fade 0x800EB378 (StreamUpdateVolume; once listed as
     an MHchar colour fade): a volume, set once a tick as on the Wii;
   - volcano creature colour fades 0x80382498, 0x80382520, 0x80382614,
     0x803826EC (u8 +0x2C);
   - the title's fade to the attract movie 0x8097EFEC;
   - the zone camera's angle blend 0x802BACE8 (the village's fixed-angle
     camera; not a shake): fixed, its curve 0x802BB0B0 reads the step's
     time, as do the collision camera's release blends and the aim view's
     (section 5);
   - quest result screen counters 0x80398008, 0x8039E568;
   - the event demo clock 0x803C477C: in-engine cutscene actors and scripts
     keyed to the demo steps, other than camera paths, which are interpolated.

   Each needs a check of what reads the counter before converting.
6. **Group shuttle speed ramp** (`group_shuttle.cpp`). Position moves by
   speed·s, but the ramp speed (count/2) changes only on ticks: velocity is a
   30 Hz staircase and position is smooth. Low priority.
7. **Village ramp ±60** (0x80212680, village work +0x34). It counts whole
   ticks. If it drives a visible blend, that blend steps at 30 Hz. Its use has
   not been checked.

These are discrete by design and not lumpy relative to the Wii:

- the ambient hover orbit jumps (+4096 on random ticks), which jump on the Wii
  too;
- the swing-effect and creature (0x80358EB4) flipbooks;
- blink duty cycles such as 0x8035B21C and the title menu blink;
- the movie at 29.97 fps;
- sound, rumble and AI timers;
- per-tick random rolls.

---

## 7. Input: from the host to the game

Every path from the host's buttons to something that moves, and how each
handles the step length. Measured with `MH3_TRACE=input` (`frame_pacer.cpp`,
development builds: once per game tick the sticks and keys as read, the field
camera's yaw/pitch, the player's position and command bits, and the repeats
since the last line) and `MH3_INPUT=playback=` in Moga area 1
(`--quest 1060 --area 1`), comparing phases by game ticks.

### 7.1 The path

1. **Host** (`controller_host.cpp`): SDL keyboard and gamepad, or
   `MH3_INPUT` (`file=`, `playback=`), become one Classic Controller sample.
   Playback and recording are indexed by game time in 30 Hz frames.
2. **WPAD bridge** (`wpad_services.cpp`): `WPADRead` copies the current
   sample each call; the auto-sampling ring is fed every 5 ms of host time.
   Nothing accumulates per step: a press is seen from the first step after
   it, a release likewise. Not step-dependent; unchanged.
3. **PollControllers** (0x80041F6C, once a step from the game frame):
   `PadReadWii`, `PadReadGc` and `PadGameKeys` turn the reports into button
   sets, presses, releases, **repeats** and the game keys with their
   **locks**. The stick directions (0x80042FDC, 0x800432C0) are stateless:
   angle, tilt (dead zone 30, no hysteresis) and direction bits. KPAD is not
   used by the game; there is no IR pointer and no motion on the host.
4. **Player input** (0x8026AF6C PlReadInput, from the player update): the
   game keys and sticks become PLW+0xB8.. and command bits (+0xDC, +0xE0..):
   edges and levels, no amounts. Step counts in it: the input lock +0x352
   and the Classic chord window +0x137 (tick sites), the button hold counts
   (`PlButtonHoldUpdate`, `field_player_actions.cpp`, already in ticks).
5. **Consumers** that apply an amount while a command is held: the field
   camera (`camera_input.cpp`, `field_camera.cpp`), the item bar and menus
   (presses and repeats from 3), and the gunner's aim (`player_aim.cpp`).
   Movement is root motion (`mhchar_motion.cpp`) and the player's turn
   (`player_turn.cpp`), both already continuous. In the village the run
   actions also push the hunter forward each step (section 7.3).
6. **Name-entry keyboard** (character creation): it ignores input for 15
   steps after it opens (the wait at small-data s32 0x8079722C; the same word
   holds its 2-step close delay). Counted in steps, the
   wait ended within the press that opened it, which then typed a "1". It is
   now three tick sites (KeyboardOpenLayout 0x80449B00, KeyboardUpdate
   0x8044A038, KeyboardCloseStep 0x8044C72C).

### 7.2 Measured

Held 120 ticks unless noted. "Before" is the build without these ports at
the variable step (~40 steps a tick on arm64 Linux); "stock" is
`MH3_VARIABLE_STEP=0`; "short"/"long" force 0.1- and 2.5-tick steps with
`MH3_STEP_SCALE` (playback stays in real time, so their phases differ in
length and are compared per tick).

| Input | Stock | Before | Variable | Short (0.1) | Long (2.5) |
|---|---:|---:|---:|---:|---:|
| d-pad held: repeats (pad and game keys) | 36 | 1517–1628 | 36 | 5 in 28 ticks, 20 in 73 | 618 in 1867 ticks |
| 4-frame L tap: repeats | 0 | 12 | 0 | — | — |
| d-pad camera turn: yaw target per tick | 1018 | aliased (≥ 15×) | 1018 | 961 (28 ticks, ramp included) | 1034 |
| first 25 ticks of the turn: yaw target | 23805 | — | 23745 (scaled) | 23805 | 24322 |
| menu (Minus, d-pad down 90 ticks): repeats | 26 | 267–380 | 26 | — | — |
| L tap: camera snaps behind | yes | no | yes | — | — |
| running, steady speed per tick | 14.2 | 14.1 | 14.1 | 15.0 | 14.9 |

The running speed in short/long differs by the direction run in (the camera
turned for a different time before). Port checks (`MH3_PORT_CHECK`) of the
seven camera and pad ports match the originals at the stock step;
`PlAimInput` matched through the player fuzz (`MH3_PORT_CHECK=P:0x8027A5E0`,
64 states).

Name entry (no save, `MH3_INPUT=playback=` with A presses 3 s apart, then
right, A, right held 30 ticks, A, Plus): at the stock step the keyboard opens
empty and the name becomes "29". At the variable step with the pad ports alone
the right tap and hold moved the cursor as at the stock step (1 and 7 keys),
but the A that opened the keyboard also typed a "1" ("129"); with the keyboard
tick sites the keyboard opens empty and each later A types one key.

Not run: swimming (the pitch tilt), a gunner's aim in play, a camera reset
blend (view 2), the
right-stick camera option (option 7 = 1; it sets the same commands 6/8 as
the d-pad), and the Vita.

### 7.3 Village movement

In the village the hunter is moved by root motion and, on top of it, by the
run actions themselves (`village_player_actions.cpp`): every step the run
(0x8020D10C, action 1/1, motion 3) adds 1 unit along the yaw and the run's
start (0x8020D260, 1/2, motion 3 at speed 1.25) adds 5; kind 12 has the same
pair with motions 53/59 (0x8020E0A0, 0x8020E1F4). The field has no such push
in its run. Counted per step, the hunter ran faster the shorter the steps
were: at 0.25-tick steps the start covered 29–41 units a tick instead of
21–27 and the run 18–22 instead of 14–19 (`MH3_STEP_SCALE=0.25`); at the
arm64 build's free variable step (8–30 steps a tick) the start covered 53–110 units a
tick and the route overshot into walls.
The ports push by the amount times the step's length. The go-to-spot glide
(GlideTo 0x8020A0BC, stepped by 0x8020A1BC: a straight move over 20 steps)
now lasts 20 ticks, and another console's player eases its yaw by
1 − (3/4)^s of the gap.

Measured on arm64 Linux (`--village`, the stick full forward and back for 150
ticks with 30-tick pauses, `MH3_TRACE=input`, path per segment over game
time): stock 15.28 units a tick; variable step 15.00–15.35 (median ratio
1.003), the route within 8–40 units of the stock route; half tilt (walking)
5.52–5.63 against 5.61. Moga area 1 (`--quest 1060 --area 1`): 13.68–13.83
stock, variable within 1–2 % before and after. A settle probe during the
run start flagged the hunter's position copies before (+15.07 for one
1/30 s step, +20.07 for two halves: the extra step's push of 5) and nothing
of the hunter after. Port checks at the stock step (organic, the player fuzz
with and without the stick held, and a glide started from gdb) match the
originals; at the variable step a 20-step glide lasts 20 ticks.

---

## 8. Patterns

Every variable-step bug found so far is one of the patterns below. Each names
what the original assumed (one call per 1/30 s tick), a fixed example, and
the fix. Section 9 lists every fix by commit. Search for these when a new
system is ported or a scene misbehaves at high frame rates.

| # | Pattern | What goes wrong at short steps | Fixed example | Fix |
|---|---|---|---|---|
| 1 | **Count per call**: a timer, wait, steps-in-state or countdown `x ± 1` in per-step code | runs at the frame rate: waits end early, sequences race | enemy, quest, fish and title timers (tick sites, 4ca32a3a, 6a562877); the event demo clock 0x803C477C (df2dab1b); the name-entry keyboard's 15-step wait (6548750b); pad repeats and key locks (311cc5d6); movie frames, one decoded per call (df797dba) | count `Ticks()`: a `Countdown`/`Countup` tick site, or a port with `Ticks()` |
| 2 | **Count per call inside a level-triggered condition**: a countdown or count kept while a motion frame lies in a range (`em_frame_check` modes 1, 3–7), or while a state lasts | the repeat (an effect every 17 calls, a sound every 8) runs per step | Ceadeus's swim-in spray, countdown +0x20 in [82, 484] (0x80371730, 4136a621); the roars' steam every 8 calls (13fd1d20) | as 1; the test of the count must pass once per tick crossing (`Em::Enemy::CountEvery`) |
| 3 | **Periodic test of a whole-tick clock** (`clock % N == k`, `(clock & mask) == k`, `clock == K`) outside the hooked step counters | the clock holds between ticks, so the test passes on every step of that tick | `system_w+0xC` / scene counters (step_counter_sites.inc, 63f036c8); the player's PLW+0x20 (kind 2, 1745f702); Ceadeus's foam on demo steps `% 10` (4136a621) | pass on the step whose ticks reach such a value: `StepCounter::Every`, a kind 1/2 hook, `Em::DemoEvery`/`DemoAt` |
| 4 | **Own counter `x ± 1; if (x % N == 0)`** | as 3 | ambient creature sound every 32 (0x803BBB00), condition every 90 (0x80264500) | `CountupEvery`/`CountdownEvery` tick sites |
| 5 | **Per-step constant advance** `x += k` (spin, phase, push, steering) | moves k per step: k·steps a tick | enemy spins 0x801BA674; title PRESS A pulse (Rate, 6a562877); village run push (d5532692); camera yaw 1035 a step (93233749); aim (d0863beb); flutter angles (70f680b4) | `Share(k)` (`Advance`/`Rate` sites) or k·`Scale()` |
| 6 | **Rate integration per step** `p += v`, `v += a` | the motion runs at the frame rate | shells, VecAddInPlace at 30 rate call sites, VelocityStep, EnemyAddVelocity (5f9f4734, d5bd714b, d67aa8e8); sway chain (70f680b4); hit timers (d67aa8e8) | the amount × `Scale()` (`PerStep`, return-address-scaled helpers) |
| 7 | **Ease or decay per step** `x += (goal − x) / d`, `x += gap >> n`, `x *= f` | converges in steps, not time | camera height eases (`EaseDivisor`), enemy turn damping (`Decay`, 3e130479), CameraAngleEase (93233749), another console's yaw (d5532692) | `1 − (1 − 1/d)^s`, `f^s` |
| 8 | **Fade per call** (alpha or level ± step) | fades finish early | screen fades (df2dab1b), soft-reset fade (629246e1), creature fade 1/frames a step (c8d2081c) | `Share()` of the per-tick amount, or × `Scale()` |
| 9 | **Random re-roll per call** | flicker, churn or chances `steps / tick` times as often | effect 45's random scale (629246e1); ambient hover's 1-in-4 / 1-in-32 (4ca32a3a); particle curves that pick at random each tick (9c5a6134) | roll once per whole tick the step covers (as many draws as ticks, so the stream moves as at 30 Hz); a step without one keeps the last roll, and anything new between ticks previews the next roll without drawing it |
| 10 | **Predictive window of one tick**: a test over `(now, now + per-tick rate]` | the window spans several steps, so the event fires on each | MHchar frame check 0x800E1740 (em_frame_check, Pl_frame_check): frame events four times at 120 fps (4136a621) | the window over this step's advance: rate·`Scale()`, tiled (mode 0 continues where the window before the character's last frame update ended; the checks between two updates share one) |
| 11 | **Real time per call with a floor**: subtract the clock delta but at least one frame | the floor makes the clock run fast at high fps | quest time left 0x803A9014 (80ab9120) | the step's game time |
| 12 | **Seconds → frames through the fps float**, counted per step | limits taken during short steps come out too long; counters run at the frame rate | 56 uses of 0x8065B4D0 (80ab9120) | keep the float at 30; count the frames in ticks |
| 13 | **Work in draw or HUD code** (runs once per drawn frame) | animates at the frame rate | Now Loading letters (6b5aba20, 629246e1), movie decode (df797dba) | port with `Ticks()`/fractional progress |
| 14 | **Whole-tick phase for a visual**: a sine or table lookup of a tick counter | holds and jumps at 30 Hz (judder against interpolated motion) | Ceadeus's texture offset (44e1d5aa); pulses and swing props (63f036c8); camera paths (camera_script.cpp); monster cutscene poses from DemoPose 0x8014620C / DemoPoseAxes 0x80146308 (this pass) | evaluate at `counter + Fraction()` |
| 15 | **Partial ticks of a hold**: a hit stop or hold counter ending mid-step | the rest of the step's motion is lost or doubled | MHchar pause/hold (2e01d7ff) | spend exact partial ticks |
| 16 | **Over-scaling** (a wrong fix): scaling something that is not a rate | an offset recomputed each step, or a count of events, follows the step's length | the height offset tick site 0x801389C0 (9e9d645c); spin motion-loop counts as Countup sites (d5bd714b) | only per-tick amounts scale; offsets and event counts stay whole |
| 17 | **Long steps skipping exact values**: `== K` after a count that now moves by several ticks | the event is skipped | `CountupEvery` lands on the multiple it passed; `Countdown` stops at its limit; `DemoAt` takes the crossing | test the crossing, not the value |

Things that must **not** be scaled: event-driven adds (awareness from a hit,
0x8011F39C callers), push-outs and offsets (the ~200 whole VecAddInPlace
callers), one-off counts (loops, hits), and anything reset every step.

---

## 9. Fix log

Every variable-step change by commit, with the pattern of section 8 it
fixed.

| Commit | What | Pattern |
|---|---|---|
| 2aa1a181 | frame pacer on the host clock; ports of the fps-float timers (`fps_timers.cpp`), the reset fade and the movie frame draw | 12 |
| 63f036c8 | the variable step (GameStep, `smBaseUpdateRate`); tick sites; step-counter hooks; ports of the player timers, stamina, player turning, swing props, root motion, camera script and paths, pulses, rumble, map sound, effects; `Share()` | 1, 3, 5, 6, 7, 14 |
| 2e01d7ff | MHchar hit stop and hold spend exact partial ticks | 15 |
| 557d0dfe | particles move by the step's game time | 6 |
| 6b5aba20 | Now Loading letters once per tick (DrawHud ran them per frame) | 13 |
| 0b20b408 | variable step on by default | — |
| 80ab9120 | fps float stays 30; seconds-based counters (quest clock steps, quest counters, result screen, skill timers) count ticks; QuestClockTick subtracts game time | 1, 11, 12 |
| 6a562877 | title idle timeout (900 steps), waits and PRESS A pulse (`Rate` kind) | 1, 5 |
| 9c5a6134 | particle curves that jump at a tick (random picks, flipbooks) held for the tick | 9 |
| df797dba | intro movie decodes the step's ticks of frames | 1, 13 |
| 4ca32a3a | survey: enemy countdowns and ages, quest object countdowns, area ambience cycles, village NPC wait, camera shake, fish swim; ambient hover port | 1, 5, 9 |
| 3e130479 | enemy turn damping (`Decay` kind) and turn add | 6, 7 |
| df2dab1b | event demo clock counts ticks (the Alatreon intro raced); screen fades by the step | 1, 8 |
| 1745f702 | player actions, enemy timers and periodic checks (`StepCounter::Every`), object ages, screen fades, texture scrolls; PLW+0x20 tests (kind 2); RSO overlay scan; PlChargeStep no longer charges faster | 1, 3, 5 |
| d5bd714b | VelocityStep and the falls × s; spin motion-loop counts no longer add ticks per loop; spin steering `Share(192)` | 5, 6, 16 |
| 5f9f4734 | 211 tick sites from a survey; `CountupEvery`/`CountdownEvery`; shells, VecAddInPlace by return address, VecMagWrapper budgets, enemy turning and blend, awareness gain, player tint | 1, 4, 5, 6 |
| 699d916e | three enemy gauges from the per-step caller; small-monster timers and state steps | 1, 5 |
| 53782903 | sandship anchored slide (`PerStep`) | 6 |
| 2a630232 | desert object group shuttle port | 1, 6 |
| d67aa8e8 | hit timers, flying-enemy glide, EnemyAddVelocity, flight AI vertical speed and countdown | 1, 6 |
| c8d2081c | volcano creature fade (1/frames a step), ambient creature glow | 5, 8 |
| 70f680b4 | ambient flutterer, sway chain solver; hover rests at a bound until the next tick | 5, 6, 7 |
| 311cc5d6 | pad repeats, game-key locks, WPAD info wait; input lock and Classic chord window | 1 |
| 93233749 | field camera turn, tilt, eases, settle and reset timers; climb view countdowns | 1, 5, 7 |
| d0863beb | gunner aim (unverified in play) | 5 |
| 6548750b | name-entry keyboard's open and close delays | 1 |
| d5532692 | village run push × s, GlideTo in ticks, remote player's yaw ease | 5, 7 |
| 9e9d645c | removed the tick site that scaled the enemy height offset (Jhen Mohran flicker) | 16 |
| 44e1d5aa | Ceadeus's texture offset at counter + `Fraction()` | 14 |
| 629246e1 | soft-reset fade by `Share()`; effect 45's random roll once per tick; loading letters' fractional hop | 8, 9, 13 |
| 4136a621 | MHchar frame check over the step's advance; Ceadeus intro spray countdown in ticks and its demo-step `% 10` once per tick | 2, 3, 10 |
| 12e4997d | DemoPose / DemoPoseAxes read the cutscene keyframes at the steps plus `Fraction()` | 14 |
| d0cbd38e | Alatreon's intro action and meteor breath: breath effect every 8 ticks (3.5 times as many at 120 fps before), the intro's shells once per demo step (`alatreon_actions.cpp`, `enemy_work.h`) | 2, 3 |
| f86c5c51 | port checks: the A: fuzz runs enemy actions on live monsters from made-up states (how the intros below, which only first-encounter demos reach, were checked) | — |
| aa2f0f1f | kinds 1/2 spits' hiss every 8 ticks, em015 discharges' sparks every 3, the Ludroth spit's channel blend 0.1 a tick (`monster_attacks.cpp`) | 2 |
| 13fd1d20 | eleven roars' steam every 8 ticks (`monster_roars.cpp`) | 2, 3 |
| fe4912f0 | ten monster intros: cutscene poses by ticks (+ `Fraction()`), demo-step effects once per tick, roar steam every 8 ticks (`monster_intros.cpp`) | 1, 3, 14 |
| 2bc9015e | thirteen waits and repeats: countdowns by ticks, sparks/steam/bursts every N ticks, em020's pitch turn by `Share()` (`monster_waits.cpp`) | 1, 2, 5, 17 |
| 9693efd7 | Deviljho's tail spin (0x80133FB0, `enemy_turn.cpp`), found with a write watch on its yaw in the arena pit ([monster control](monster-control.md)) | 4 |
| 3f2b2153, 310dc43f, 5b6ab8dd, e940cf13, d8a40aed | the watch pass of the monster sweep (count, yaw, x of the forced actions): Deviljho's charge limit, Rathian's wait, Rathian's and Rathalos' dive and flying charge, Qurupeco's move and backward hop: counts by ticks (`monster_waits.cpp`, `monster_attacks.cpp`) | 1 |
| a49d44d6 | EnemyTurnWhile: the motion's advance per step, not per tick (`enemy_turn.cpp`) | 4 |
| eef463d2 … d9c6b992, 5efed2fa | the watch pass over every large and small monster's forced actions (count, yaw, pitch, x): about 70 waits, sleeps, walks, swims and timed motions of 20 kinds counted by ticks (`monster_waits.cpp`, the `Timed` table), Gobul's spin by ticks and `Share()` (`monster_attacks.cpp`), the pitch and roll levelling of every monster (0x80137E34) by `Share()` (`enemy_turn.cpp`) | 1, 4 |
| d5f96bfd | texture flipbooks (ResAnm keys a frame or less apart) hold the earlier key between frames at the variable step: Flooded Forest's underwater surface no longer slides through in-between rows several times too fast; found with `MH3_TRACE=srt` on the Pi 5 (the water's own scrolls were right) (`res_anm_keys.cpp`) | 3 |
| aa4237b1 | the hunter's swim and drawn walk (Walk101 0x80252C78) moved a step's distance each step and the swim pitch (0x80252B50) turned 0x400 a step: in the water the hunter swam and dived about four times as fast at 120 steps a second ("swimming takes over the controls"); both by the step's share, found with a write watch on the hunter's x/z in Flooded Forest (`field_player_actions.cpp`) | 4, 5 |
| 5262ca1a | the MHchar frame check's mode 0 window ahead (rate·`Scale()` past the frame) guessed the next step's advance from this one: at varying sub-tick steps consecutive windows overlapped or left gaps, so a walking hunter's footstep dust came two or three times in one tick and some footsteps had none (Deserted Island area 1, Pi 5 `MH3_TRACE=particles`); the windows now tile, each continuing where the channel's last one ended (`mhchar_motion.cpp`) | 10 |
| e0f6defb | fish and effect objects (4.5): the fish's appear, bait and flee timers, speeds and scale changes and the effect objects live in Deserted Island area 10 (hunter and weapon effects, glows, debris, an enemy-attached model) counted their lives, waits, colour key frames and spark repeats per step: 64 tick sites; the fish's turn helper (0x8034027C) by `Share()` (`fish_effects.cpp`). Pi 5 area 10 at about 11 steps a tick: fish appear in 9.9 ticks (10.0 fixed), swim legs of 30.1 ticks (31.0) at 1.50 units a tick (1.50) | 1, 2, 5, 6, 8 |
| b9cb201e | the fish swim state (0x803408B4, 4.5): its turn rate eased toward its goal by an eighth and its wobble flipped and halved once a step, so a swimming fish turned faster and a turning one slower at short steps; native port (with FishFindBait 0x8034048C, `fish_effects.cpp`): eases and wobble once a tick, the turn countdown by ticks, the speed and its fall by the step | 1, 6, 7 |
| b9778960 | effect objects of the Alatreon quest, the arena and the village (4.5): hunter, monster, Alatreon and village effects counted their ages (colour keys, texture matrix curves), lives, waits between parts and colour phases per step, grew their pieces and spun per step: 36 tick sites; `MH3_TRACE=effect-objects` lists the live callbacks and kinds and their mean lives in ticks | 1, 4, 5, 6 |
| 6f41ab3d | effects spawned on every step without a tick in the Alatreon quest (4.5): the 12 step-counter tests of quest_field's pl_effect_sub were kind 0, since llvm-objdump prints their hinted branches as `bc 13, 2` (kind 20 effects: 409 ended in 150 s at the variable step, now 71, at most one per 32 ticks); the quest object's countdown (+0x10, 0x808F7A7C) tested `% 10` for kind 123 is a CountdownEvery site (77 ended, now 9, as at the fixed step); the kind 7 fade 0x80308D64 ported (EffectFadeUpdate) | 3, 4 |
| 1c3f9e27 | the hunter's weapon trail (0x80115AE4 HunterTrailUpdate, effect 0x801156C8): its six matrices of joint 7 were shifted on every step, so at 400 fps the trail spanned 5 steps (12 ms) instead of 5 ticks (167 ms); at a step without a tick the history is shifted back first, so only the head follows the joint; Pi 5 frame dumps of scripted sword-and-shield swings in the Alatreon quest: the variable step (~300 fps) draws arcs of a whole swing, as long as the fixed step's | 2 |
| 3e4f522f | the camera's step counts (4.3): BindCamera (0x802B8ED0, a monster's hold of the hunter) ran its count and path once a step, 13 times as fast at 400 fps (Deviljho's grab in the arena pit now holds 181 ticks at count 180); the collision camera's hold and release counts, the scope zoom, the aim view's blend and side offset, and the zone cameras' angle and point blends counted steps (tick sites, `PerStep`, AimCameraPort); CameraResetCurve reads the blends' steps left at the step's time (`field_camera.cpp`); `MH3_TRACE=bind-camera,camera-blends` | 1, 5, 7, 14 |
| 0e713dd9 | camera turns stuttered on the Vita: with the frame limit, frames are flipped a VBlank apart, but each step was the game thread's time between frame starts (12-44 ms, set by slot waits and the just-in-time start), so in a 10 s stick turn 158 of 555 flips showed the turn more than 10% off its speed (76 over 25%); the game's own yaw was smooth per game time. A shown frame's step is now the display's (`DisplayStep`, `frame_pacer.cpp`): 38 off, all at frames the display really repeated, and with the repeats paid back fading out 24 (7 over 25%) (`MH3_TRACE=camera-path,flips`) | — |
| f215518f | the hold camera between ticks: BindCamera (0x802B8ED0) placed the eye and target once a tick, so a monster's hold moved the camera at 30 Hz; past the last tick it now runs once more for the next tick's place (its state put back) and the eye, target and field of view sit the step's fraction of the way there (`BindPlace`, `field_camera.cpp`): Deviljho's grab moved on 6863 of 6870 steps; port check matches | 14 |
| 70d846b8 | online and town per-step work (8.3, step probes on an online Tundra quest between two Pi 5 instances): village and city NPCs eased their yaw (NpcYawTo callers, 1 - (1 - 1/d)^s), ran timed turns and counted script entries once a step (`npc_turn.cpp`, tick sites 0x801FFE40/74); the quest's field actor waited and turned once a step (`field_actor.cpp`, tick site 0x803A451C); a kind-8 monster's height rise at the quest's start (EnemyHeightAdd × s); shared objects counted steps to their 900-step resync message (tick site 0x803440A8) | 1, 5, 7 |
| 70d846b8 | the online quest load's member sync every 32 steps (0x8028CB4C, `CountupEvery`), the 30-step input lock at an online quest's start (0x8028D064) and a field object's random 30..329-step wait (0x8032C5A8), named by a write watch on the joiner | 1, 4 |
| c8dd3078 | item releases and monster senses timers (a paintball thrown at a frozen Deviljho in the arena pit): the throw (0x8024902C) and the whetstone (0x8024F264) check their release frame from two callers on the same frame, and the per-step mode-0 window left the second call empty when a step boundary fell between them, so the item never left the hand; windows now last until the character's next frame update (MHcharRootMotionPort). The paint timer (s16 +0x810, 18000 from EnemyPaint 0x80133678, 0x80133704) and 18 other timers of the senses step 0x801298E0, plus the grill cook time 0x8024AE74, counted once a step and are tick sites now | 1, 10 |

---

## 10. Candidates from the pattern sweep

Found by scanning Ghidra decompiles of the 588 DOL callers of
`em_frame_check` (0x8012F980), `Pl_frame_check` (0x8026A378), the MHchar
frame check and the demo clock (0x80146048, 0x803C48A4) for patterns 2 and 3,
and the translated code of every enemy and player action (functions calling
the frame checks, MotionDone or SetMotion) for read-modify-write `± 1` sites
that are not tick sites: 283 sites in 211 functions outside the state bytes
+5/+6, 191 of them on the enemy action count +0x20. The RSO overlays
(quest_field, enemy_field) were not scanned. None of these were run at a
variable step except where noted; a monster's attack is needed to reach most.
Global step-counter tests in them (`system_w+0xC & 3` etc., 92 seen) are
already hooked.

**Status.** The 121 enemy functions of this list that take ±1 from
the action count (+0x20) were gone through by kind: about 100 are ported
(`monster_waits.cpp`, `monster_attacks.cpp`, `monster_intros.cpp`; each
checked with the A: fuzz on a live monster of its kind, 256 states). Left as
they are: counts of events (motion loops ended: 0x8015E5CC, 0x801ACBF8,
0x80188A94; a fragment, 0x801C8278), counts nothing reads (0x803542A4,
0x80351FAC), counts of a state nothing enters beside pitch clamps
(0x8017A420, 0x8036FE4C) and 0x801CFE68 (no per-call count). 0x801BA438,
Uragaan's bouncing rock (kind 40: velocity plus gravity once a call over a
count), integrates by the step's length.

Ranked by what shows. The first five classes of the sweep (cutscene poses
driven by a count per call, demo-clock tests spawning effects, range counters
spawning effects, range counters on sounds, countdown waits per call) are
fixed, apart from the two Kelbi hops below: `monster_intros.cpp`, `monster_roars.cpp`, `monster_attacks.cpp`,
`monster_waits.cpp` (section 9). Four of the listed functions needed no
change: 0x80187A48 and 0x80186B98 test only the global step counter
(`system_w+0xC & 3`), which the hooks already answer in ticks; 0x80159B1C
lowers its u8 +0x1E7 once, when the state ends (an event count, pattern 16);
0x80351FAC (em024) adds 1 to +0x20 every call but nothing in that action
reads it.

| Class (pattern) | Functions | Effect at short steps |
|---|---|---|
| Countdown waits per call (1) | 0x801B795C, 0x801B867C (em034 Kelbi hops: a 10-call hold after landing) | ported (`monster_waits.cpp`, counted by ticks); checked with the A: fuzz on the Kelbi of `--quest 1028 --area 1 --quest-small Kelbi` (256 states each). The organic callers come through 0x801B94B4, a dispatcher not of Kelbi variant 1 (kind 34's handler 3 is 0x801B6560) |
| Player counts in a reached range (1, 2) | 0x8010E30C (u8 +7 ×3), 0x801E1BCC, 0x8020FE7C (s16 0x806AD070+4), 0x80250E9C, 0x8025779C, 0x80257BE0 (u8 +6), 0x80262AC8, 0x803A2730 | short delays (1–2 ticks) end within a tick |
| Other enemy-action ± 1 sites (1) | the rest of the 283 (list: rerun the scan, `scan_rmw`-style: `x = Read(r + off); x ± 1; Write(r + off, x)` in functions calling 0x8012F980/0x8012F9A0/0x8012F61C/0x8026A378) | to be classified: many are event counts (hits, loops) that must stay whole (pattern 16) |

`runtime/src/enemy_work.h` has the helpers these ports need
(`Em::Enemy::CountTicks`, `CountEvery`, `Em::DemoEvery`, `Em::DemoAt`), as
`alatreon_actions.cpp` uses them.

Also found:

- Barroth (arena 9002, Pi): the emitter `p03_em008_0` was created every 3.3
  ticks in a burst at the fixed step and about every 0.6 ticks at the
  variable step (~115-170 steps a second), with and without these ports. It
  comes through the enemy effect queue (callers 0x80304978, 0x800F8E74), so
  the action that queues it is not known; another count or test per call.
  In the arena pit with Barroth (`--quest 9004 --quest-monster
  Barroth`, `MH3_TRACE=actions,particles`): every lookup of the emitter came
  at ticks 27-57, before Barroth's first action (tick 74), one a frame: the
  quest start's effect set-up, not an emission of an action.
- `--spawn exit:N` on arm64 Linux stopped the game once in the area change
  (quest 1003, area 6 to 5): "draw of a dead model" then a missing
  translated target 0 in the model scene draw. A renderer issue of the area
  change, not of these ports.

- `MH3_TRACE=particles` at the variable step sometimes stops the game in
  quest 19001 on the Pi: twice a few seconds in (tick 75–108) with the build
  before these ports, once after about 40 s of play with them (stats and
  trace end, the process lives on until killed); other such runs went
  through. Runs without the trace, at the fixed step, and in the arena did
  not stop. Not traced further; the Pi was shared with other runs.
- The effect counts of the arena (Rathian 9004, Uragaan 9006, 100 s, no
  input) matched between the fixed and the variable step within the scenes'
  phase differences.
- Quest 19001 with the ports: `p40_em024_1` (an effect set through the effect
  table entry 0x80112DBC, table word 0x80026450, from the enemy effect
  dispatcher 0x800F927C) came 9 times in the intro at the fixed step (1, 2,
  3, 2, 1 on ticks 10 apart) and 30 at the variable step (5, 4, 12, 6, 3 on
  the same ticks). Its caller was not found (the five traced return
  addresses end in the dispatcher); the other Alatreon effects match per
  attack sequence.
