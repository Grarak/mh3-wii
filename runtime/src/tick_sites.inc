// Tick sites: guest loads of step counters and countdowns (recomp.yml
// translation.tick_sites_path; tick_sites.cpp). {pc, kind[, limit[, module]]},
// sorted by pc. A module ("demo", or an overlay profile such as "net_data")
// marks a site in addresses a boot module and an overlay share: only that
// module's translation hooks it (scripts/tick_sites_filter.py). The value
// loaded at pc is followed by the site's +1 / -1: MH3_TickLoad
// adjusts it so that the store advances it by the ticks of the step.
// Float sites (lfs): EaseDivisor loads the d of `x += (goal - x) / d`, PerStep an
// amount applied once per step. Integer Advance sites load a value the guest
// moves on by a constant (limit) each step: it is moved on by limit * s.
// Rate sites load such an amount kept in memory: it becomes k * s. Decay sites
// (lfs) load the f of `x *= f` each step: it becomes f^s.
{0x800DE41Cu, TickSite::Countdown},          // sound handle (800DE3D8): life (s16 +0x48), stops the sound at 0
{0x800DE530u, TickSite::Countdown},
{0x800DE5F0u, TickSite::Countdown},
{0x800DE69Cu, TickSite::Countdown},
{0x800DE744u, TickSite::Countdown},
{0x800DE818u, TickSite::Countdown},
{0x800DE8C4u, TickSite::Countdown},
{0x800DE970u, TickSite::Countdown},
{0x800DEA1Cu, TickSite::Countdown},
{0x800DEAC8u, TickSite::Countdown},
{0x800DEB80u, TickSite::Countdown},
{0x800DEC24u, TickSite::Countdown},
{0x800DECC4u, TickSite::Countdown},
{0x800DED74u, TickSite::Countdown},
{0x800DEDFCu, TickSite::Countdown},          // sound (800DE3D8): countdown (s16), clamped at 0
{0x800DEE88u, TickSite::Countdown},
{0x800DEF14u, TickSite::Countdown},
{0x800DEF60u, TickSite::Countdown},
{0x800DEFFCu, TickSite::Countdown},
{0x800DF094u, TickSite::Countdown},
{0x800DF140u, TickSite::Countdown},
{0x800DF1F4u, TickSite::Countdown},
{0x800DF2A8u, TickSite::Countdown},
{0x800DF360u, TickSite::Countdown},
{0x800DF40Cu, TickSite::Countdown},
{0x800DF4B8u, TickSite::Countdown},
{0x800DF564u, TickSite::Countdown},
{0x800DF610u, TickSite::Countdown},
{0x800DF6BCu, TickSite::Countdown},
{0x800DF778u, TickSite::Countdown},
{0x800DF834u, TickSite::Countdown},
{0x800DF90Cu, TickSite::Countdown},
{0x800EB40Cu, TickSite::Countup},            // stream volume fade (StreamUpdateVolume 800EB378): steps (+0x74) against a length (+0x70)
{0x800EB490u, TickSite::Countup},
{0x800EB514u, TickSite::Countup},            // stream volume fade: steps (+0xA0) against a length (+0xBC)
{0x800EB664u, TickSite::Countup},
{0x800EB7CCu, TickSite::Countup},
{0x800FB6D8u, TickSite::Countup},            // enemy part (800FB1C4): steps (+0x10)
{0x800FB944u, TickSite::Countup, 9},         // enemy part: steps (+0xC), up to 10
{0x800FCAF4u, TickSite::CountdownPastZero}, // effect object (800FC48C): wait (+0xC) before its burst
{0x800FCCF4u, TickSite::Countup},           // effect object (800FC48C): age (+0x10) for its colour keys
{0x800FCF68u, TickSite::CountdownPastZero}, // effect object (800FC48C): wait, then life (+0xC)
{0x800FD054u, TickSite::Countup},           // effect object (800FC48C): age (+0x10) for its texture matrix curves
{0x800FF9D8u, TickSite::Countup},           // effect spawner (800FF938): per-slot steps since the last spawn (+0xE8)
{0x80101300u, TickSite::Countup},           // effect object (80100AD0): age (+0x10) for its colour and alpha keys
{0x8010425Cu, TickSite::CountdownPastZero},  // enemy (80104220): countdown (+0xC), fires below 0
{0x80106608u, TickSite::CountdownPastZero}, // enemy effect (80106594, ending): steps left (+0x10) before the release
{0x80106B7Cu, TickSite::Countdown},         // effect object (80106984): delay (+0xC)
{0x80108448u, TickSite::Countdown},         // hunter effect (80107EC0, kind 0xC): start delay (+0x10)
{0x80108DE8u, TickSite::CountdownPastZero}, // hunter effect (80108AD8, kinds 8/0x15): wait (+0xC)
{0x8010920Cu, TickSite::Countup},           // hunter effect (kind 0xE): colour key frame (+0x10)
{0x80109254u, TickSite::Countup},           // hunter effect (kind 0x10): colour key frame (+0x10)
{0x8010930Cu, TickSite::Countup},           // hunter effect (kind 0x16): colour key frame (+0x10)
{0x80109484u, TickSite::CountdownPastZero}, // hunter effect (80109458): life (+0xC), ends below 0
{0x80109968u, TickSite::Countup},           // hunter effect (80109458): colour table step (+0x10), 3 steps
{0x80109C70u, TickSite::CountdownPastZero}, // hunter effect (80109AE8): steps to the next spark (+0xC), every 20
{0x8010A16Cu, TickSite::CountdownPastZero}, // hunter effect (80109D64, state 5): wait (+0x10)
{0x8010A310u, TickSite::CountdownPastZero}, // hunter effect (8010A238, ending): steps left (+0x10) before the release
{0x8010A52Cu, TickSite::CountdownPastZero},  // object (8010A500): countdown (+0x24)
{0x8010A538u, TickSite::CountdownPastZero},  // object: countdown (+0x1C), fires below 0
{0x8010A5E8u, TickSite::CountdownPastZero},  // object: countdown (+0x20), fires below 0
{0x8010CC48u, TickSite::PerStep},           // joint effect (8010C95C): spin (+0x28) 2048 per step
{0x8010CD28u, TickSite::Countup},           // joint effect: 32-step cycle (+0xC)
{0x8010CD50u, TickSite::Countdown},         // joint effect: steps to the next sound (s16 +0x4C), every 45
{0x8010EDF0u, TickSite::Countup},           // hunter weapon effect (8010E30C): steps since the start (+0xC), its colour for the first 4
{0x8010F760u, TickSite::CountdownPastZero}, // hunter weapon effect: steps to the next spark (s8 +0x67), every 13
{0x8010FC74u, TickSite::CountdownPastZero}, // hunter weapon effect: steps to the next spark (s8 +0x67), every 13
{0x8010FD30u, TickSite::Countup},           // hunter weapon effect: steps since the start (+0xC)
{0x8011074Cu, TickSite::CountdownPastZero}, // hunter weapon effect: steps to the next burst (+0x10)
{0x80110924u, TickSite::CountdownPastZero}, // hunter weapon effect: steps to the next burst (+0x10) in the motion window
{0x80110AC0u, TickSite::Countup},           // hunter effect (80110A7C): steps (+0xC), next stage after 60 or 20
{0x80110ACCu, TickSite::Countup},           // hunter effect (80110A7C): steps (+0x10)
{0x80110B40u, TickSite::PerStep},           // hunter effect (80110A7C, falling): fall acceleration (x the steps) per step
{0x80110B98u, TickSite::PerStep},           // hunter effect falling: fall speed (+0x6C) moved per step
{0x80110E60u, TickSite::PerStep},           // hunter effect (80110A7C, landing): fall acceleration per step
{0x80110EB8u, TickSite::PerStep},           // hunter effect landing: fall speed (+0x6C) moved per step
{0x80113A64u, TickSite::Countup},           // hunter effects (80112D80, kinds 0-2, 28, 57): age (+0x10) for colour keys
{0x80113BC8u, TickSite::CountdownPastZero}, // hunter effects (kinds 7, 59): life (+0xC)
{0x80113C48u, TickSite::CountdownPastZero}, // hunter effects (kind 8): life (+0xC)
{0x80113D04u, TickSite::CountdownPastZero}, // hunter effects (kind 8): wait (+0x10) between parts, 9 ticks
{0x80113DC4u, TickSite::CountupEvery, 5},   // hunter effects (kind 11): age (+0x10), == 5 starts a part once
{0x80113F8Cu, TickSite::Countup},           // hunter effects (kind 19): age (+0x10) for colour keys
{0x80114348u, TickSite::Countdown},         // hunter effects (kind 32): life, then fade (+0xC)
{0x801143E4u, TickSite::CountupEvery, 30},  // hunter effects (kind 32): age (+0x10), a part every 30
{0x80114420u, TickSite::CountdownPastZero}, // hunter effects (kind 47): life (+0xC)
{0x80114458u, TickSite::Countup},           // hunter effects (kind 56): age (+0x10) for colour keys
{0x80114540u, TickSite::Countup},           // hunter effects (kind 102): age (+0x10) for colour keys
{0x80114788u, TickSite::Countup},           // hunter effects (kind 136): age (+0x10)
{0x80119E74u, TickSite::CountdownPastZero}, // proximity glow (80119E50): start delay (+0xC)
{0x8011A69Cu, TickSite::Countup},           // proximity glow (8011A3E8): fade-in key frame (+0x10), up to 10
{0x8011A6E4u, TickSite::Countdown},         // proximity glow: fade-out key frame (+0x10), down to 0
{0x8011A9F0u, TickSite::Advance, 13},       // proximity glow (8011A900): alpha (u8 +0xB), +13 per step
{0x8011A9FCu, TickSite::Countdown},         // proximity glow: fade-in steps left (+0xC)
{0x8011AA30u, TickSite::Advance, -5},       // proximity glow: alpha (u8 +0xB), -5 per step
{0x8011AA3Cu, TickSite::Countdown},         // proximity glow: dimming steps left (+0xC)
{0x8011AA8Cu, TickSite::Advance, -3},       // proximity glow: alpha (u8 +0xB), -3 per step
{0x8011AA98u, TickSite::Countdown},         // proximity glow: fade-out steps left (+0xC)
{0x8011D02Cu, TickSite::CountdownPastZero},  // volcano object (8011CF28): countdown (+0xC), fires below 0
{0x8011D31Cu, TickSite::Countdown},          // volcano object: countdown (+0xC), fires at 0
{0x80124A3Cu, TickSite::Countup},            // enemy (801249F0): steps (s16 +0x914) in state 11
{0x80124B78u, TickSite::Countdown},          // enemy: countdown (+0x20), fires at 0
{0x80124BB0u, TickSite::Countdown},
{0x80124BCCu, TickSite::Countdown},
{0x801298A0u, TickSite::Countdown},          // enemy (80129870): countdown (s16 +0x44C), clamped at 0
{0x80130850u, TickSite::Countup},            // enemy (80130808): steps (s16 +0x442) against a table length
{0x80134ED0u, TickSite::PerStep},           // flying enemy (80134E8C): vertical speed (+0x314) += 0.85 a step, below 100
{0x80134FA8u, TickSite::PerStep},           // flying enemy (80134F7C): the same toward -10
{0x80135538u, TickSite::PerStep},           // enemy (80135518): vertical speed (+0x314) += acceleration (+0x320) a step
{0x8013896Cu, TickSite::PerStep},           // enemy update (801382A0): a small height offset (+0x1AC) eases to 0 by 10 a step
{0x801389ACu, TickSite::Decay},             // enemy update: a large height offset (+0x1AC) *= 0.9 a step
// Not 0x801389C0: the model offset (+0x1A0, cleared each step) takes the
// height offset whole (+0x1A4 = +0x1AC * scale), which Jhen Mohran holds at
// about 630 while it swims in the sand.
{0x80138D14u, TickSite::Countdown},         // enemy (80138BC4): countdown (s16 +0x764)
{0x80138D40u, TickSite::Countdown},
{0x8014823Cu, TickSite::Countup},            // enemy (8014823C): steps (s16 +0x354), wrapping at 50
{0x8014F6F0u, TickSite::PerStep},           // flying enemy (8014F654): y (+0x18C) += 10 a step while below the target
{0x8014F708u, TickSite::Countdown},         // flying enemy: countdown (+0x20, from 300)
{0x801A1488u, TickSite::Countdown},          // enemy action (801A13E8): countdown (+0x20), fires at 0
{0x801A1524u, TickSite::Countdown},          // enemy action (801A14B8): countdown (+0x20), fires at 0
{0x801A9810u, TickSite::PerStep},            // enemy (801A97AC): f32 ramp (+0x34C) down by a constant a step
{0x801A9874u, TickSite::PerStep},            // enemy: the ramp up
{0x801A9890u, TickSite::PerStep},            // enemy: the ramp down
{0x801A989Cu, TickSite::Countup},            // enemy (801A97AC): steps (s16 +0x350), up to 180
{0x801A98F4u, TickSite::PerStep},            // enemy: the ramp up
{0x801AC6D8u, TickSite::Countdown},         // enemy action (801AC674): countdown (+0x20), ends the action at 0
{0x801AC9E4u, TickSite::Countdown},          // enemy action (801AC930): countdown (+0x20), fires at 0
{0x801AFF6Cu, TickSite::Countdown},          // small monster (801AFE24): wait (s16 +0x22) before a new roll
{0x801BA674u, TickSite::Advance, 5461},      // enemy (801BA438): spin angle (+0x1BC), +5461 per step
{0x801BA938u, TickSite::Advance, 2731},      // enemy (801BA438): spin angle (+0x1BC), +2731 per step
{0x801E330Cu, TickSite::CountdownPastZero}, // effect (801E32B0): life (+0xC), ends below 0
{0x801E36BCu, TickSite::Countup},           // effect (801E32B0): key frame (+0x10)
{0x801E37A8u, TickSite::Countup},           // effect (801E32B0): key frame (+0x10)
{0x801E3824u, TickSite::Countup},           // effect (801E32B0): key frame (+0x10)
{0x801E3EDCu, TickSite::CountdownPastZero}, // effect (801E3E68, ending): steps left (+0x10) before the release
{0x80201884u, TickSite::Advance, 512},      // village NPC (802014C8): yaw (LB_NPC+0x2C) turning, +512 per step
{0x80203744u, TickSite::Countdown},         // village NPC (802036C8): wait (+0xC) before the next idle move
{0x802214D4u, TickSite::Countdown},         // village object (80221410) countdown (s16 +0x28), while != 0
{0x8024ACD0u, TickSite::CountdownPastZero},
{0x8024C224u, TickSite::Countdown, 1},
{0x8024C314u, TickSite::Countdown},
{0x80259EFCu, TickSite::PerStep},            // player anchored action (80259E3C, motion 422): offset z (+0x80) += this each step
{0x80264378u, TickSite::Countdown},          // player condition (80264334): countdown (s16 +0x3EE), fires at 0
{0x80264500u, TickSite::CountdownEvery, 90}, // player condition: countdown (s16 +0x404), tested % 90 == 0
{0x80264760u, TickSite::Countdown},          // player condition: countdown (s16 +0x462), fires at 0
{0x8026B018u, TickSite::Countdown},          // player input (8026AF6C): input lock (s16 +0x352), also counted down by PlayerStepTimers
{0x8026C0C4u, TickSite::Countdown},          // Classic commands (8026BFFC): chord window (u8 +0x137, from 2) for a second attack button
{0x802B4BA8u, TickSite::Countdown},          // stage (802B4B20): countdown (s16 +0x2FDC), fires at 0
{0x802B638Cu, TickSite::EaseDivisor},       // field camera (802B5CBC): height eases, target (f31) and eye (f30). These 9 entries
                                              // act on the translated original only (port checks): field_camera.cpp does the same
{0x802B644Cu, TickSite::EaseDivisor},
{0x802B6450u, TickSite::EaseDivisor},
{0x802B65BCu, TickSite::Countdown, 1},      // field camera: ease timer (+0xA8) while swimming, down to 1
{0x802B65D0u, TickSite::EaseDivisor},
{0x802B65D4u, TickSite::EaseDivisor},
{0x802B65DCu, TickSite::EaseDivisor},
{0x802B65FCu, TickSite::Countdown},         // field camera: ease timer (+0xA8)
{0x802B6610u, TickSite::EaseDivisor},
{0x802B7960u, TickSite::Countdown},         // camera climb mode (802B7880, twice a step): steps before the climb view (s16 +0x86)
{0x802B799Cu, TickSite::Countdown},         // camera climb mode: steps before leaving it
{0x802B832Cu, TickSite::Countup},           // camera collision (CameraCollide 802B8244): ticks a height correction is held, to the row's limit (s8 +0xA5 while pushed)
{0x802B8364u, TickSite::Countup},           // camera collision: the same (s8 +0xA2)
{0x802B8518u, TickSite::Countup},           // camera collision: ticks a sideways correction is held (+0xA5 while pushed)
{0x802B8578u, TickSite::Countup},           // camera collision: the same (+0xA2)
{0x802B8850u, TickSite::Countdown},         // camera collision: release blends' steps left (u8 +0x102, +0x103; the curve reads the step's time)
{0x802B99F0u, TickSite::PerStep},           // scope camera (802B988C): zoom (+0x1DC, +0x1E0) by 1.75 a step
{0x802B9F50u, TickSite::Countdown},         // camera offset blend (802B9C68): steps left (s16 +0x234)
{0x802BACE8u, TickSite::Countdown},         // zone camera angle blend (802BAC0C): steps left (s16 zone +0x44)
{0x802BAF18u, TickSite::Countdown},         // zone camera point blend (802BAE68): steps left (s16 zone +0x44)
{0x802BCDC8u, TickSite::CountdownPastZero}, // camera script wait (op 0x25): frames left, done below 0
{0x802BD878u, TickSite::Countdown},         // camera path hold (802BD78C): steps before playing
{0x802BD8B0u, TickSite::Countup},           // camera path frame (802BD78C)
{0x802E4D60u, TickSite::Countup},           // area ambience (802E4CC0): cycle steps (u8 +0xC), 40..190
{0x802E4DB4u, TickSite::Countup},           // area ambience: cycle steps (u8 +0xD)
{0x802E4E24u, TickSite::Countup},           // area ambience: cycle steps (u8 +0xE), 30..50
{0x802EF2ACu, TickSite::Countdown},         // player-side timer (802EF294): +0x5C, set from seconds * fps by 802EF3A4
{0x802F2D3Cu, TickSite::CountdownPastZero}, // Alatreon pieces (802F2D14): start delay (+0x10)
{0x802F2D50u, TickSite::CountdownPastZero}, // Alatreon pieces: wait (+0xC) between pieces, 3 ticks
{0x802F3094u, TickSite::PerStep},           // Alatreon pieces: growth per step
{0x802F30D4u, TickSite::Countup},           // Alatreon pieces: colour phase (+8)
{0x802F30F4u, TickSite::Countup},           // Alatreon pieces: colour phase (+8)
{0x802F3114u, TickSite::Countup},           // Alatreon pieces: colour phase (+8)
{0x802F3134u, TickSite::Countup},           // Alatreon pieces: colour phase (+8)
{0x802F3278u, TickSite::Countup},           // Alatreon effect (802F3254): life (+0xC)
{0x802F3420u, TickSite::CountupEvery, 32},  // Alatreon effect (802F33BC): age (+0xC) for its curves, kind 6 == 32 once
{0x802FA510u, TickSite::Countup},           // glowing parts (802FA3A0): colour pulse phase (s16 +0x30A per part), 120-step cycle
{0x802FA5B8u, TickSite::Countdown},         // glowing parts: near fade (+0xC) down to 0
{0x802FA5D0u, TickSite::Countup, 14},       // glowing parts: near fade (+0xC) up to 15
{0x8030555Cu, TickSite::Countdown},         // monster effects (803048A4, kind 44): life (+0xC)
{0x803055E4u, TickSite::Countup},           // monster effects (kinds 21-23, 45-47): age (+0xC) for colour keys
{0x80308050u, TickSite::Countup},            // effect (80307F30): age (+0x10)
{0x8030805Cu, TickSite::CountdownPastZero},  // effect: life (+0xC), ends below 0
{0x80308748u, TickSite::Advance, 5461},     // effect object (80307EA8): spin (+0x2C)
{0x8030889Cu, TickSite::Advance, 5461},     // effect object (80307EA8): spin (+0x28)
{0x803089C0u, TickSite::CountdownPastZero}, // debris (80308960): life (+0xC), ends below 0
{0x80308A00u, TickSite::PerStep},            // debris (80308960): gravity (+0x18) off the fall speed each step
{0x80308A18u, TickSite::Advance, -2185},     // debris (80308960): spin (+0x24), -2185 per step
{0x80308AC4u, TickSite::CountdownPastZero}, // effect object (80308A94): life (+0xC)
{0x80308AF0u, TickSite::Countup},           // effect object (80308A94): age (+0x10) for its texture matrix curves and colours
{0x8031E51Cu, TickSite::CountdownPastZero},  // creature (8031DF1C): countdown (s16 +0x30)
{0x8031E958u, TickSite::Countup},           // enemy-attached model (8031E858): key frame (+0xC) of its scale curve
{0x8031F728u, TickSite::Advance, 512},      // marker effect (8031F574): bob phase (u16), +512 per step
{0x8031F734u, TickSite::Advance, 255},      // marker effect: spin angle (+0x28), +255 per step
{0x803327A4u, TickSite::CountdownPastZero}, // actor action countdown (+0x28), fires at <= 0
{0x80334330u, TickSite::CountdownPastZero},
{0x803407D0u, TickSite::PerStep},           // fish (8034062C, appearing): scale x grown per step (target / 10)
{0x803407E8u, TickSite::PerStep},           // fish appearing: scale y grown per step
{0x803407FCu, TickSite::PerStep},           // fish appearing: scale z grown per step
{0x80340810u, TickSite::Countdown},         // fish appearing: steps left (+0xC), then the full scale
{0x80341064u, TickSite::PerStep},           // fish (80340D14, at a bait): speed (+0x20) to the bait moved per step
{0x803410ACu, TickSite::Countdown},         // fish at a bait: steps to the bait left (+0xC)
{0x803416CCu, TickSite::PerStep},           // fish at a bait: lunge speed (+0x20) moved per step
{0x80341704u, TickSite::PerStep},           // fish at a bait: lunge speed change (+0x2C) per step
{0x803417D4u, TickSite::Countdown},         // fish at a bait: wait steps left (+0xC)
{0x80341880u, TickSite::Countdown},         // fish hooked: steps before it turns away (s8 +0x5E)
{0x80341890u, TickSite::CountdownPastZero}, // fish hooked: turn steps left (+0x10), then it faces the bait
{0x80341950u, TickSite::PerStep},           // fish leaving a bait: speed (+0x20) moved per step
{0x80341988u, TickSite::PerStep},           // fish leaving a bait: speed change (+0x2C) per step
{0x80341AD4u, TickSite::Countdown},         // fish (803419F4, on the line): steps to the next tug (+0xC)
{0x80341CE0u, TickSite::Countdown},         // fish (80341BD0, hidden): wait steps left (+0xC)
{0x80341EC4u, TickSite::CountdownPastZero}, // fish (80341DAC, fleeing): steps left (+0xC)
{0x80341F28u, TickSite::PerStep},           // fish fleeing: speed (+0x20) moved per step
{0x80341F78u, TickSite::PerStep},           // fish fleeing: scale shrunk per step (last 10)
{0x80341FB4u, TickSite::CountdownPastZero}, // fish fled: steps left (+0xC) before it hides
{0x80346E68u, TickSite::Countdown},          // object (80346AF0): countdown (s16 +0x20), fires at 0
{0x80346F08u, TickSite::Countup},            // object: steps (s16 +0x22)
{0x80346F18u, TickSite::Countup},            // object: steps (s16 +0x22), against 600
{0x8034E5ECu, TickSite::Countup},           // actor action step counter (+0x28)
{0x8034EC68u, TickSite::Countup},
{0x8034F5B4u, TickSite::Countdown},          // small monster (8034F588): s16 timer (+0x32E) in state 1
{0x8034F5CCu, TickSite::Countdown},          // small monster: s16 timer (+0x330) in state 0
{0x8034F5F8u, TickSite::Countdown},          // small monster: s16 timer (+0x332)
{0x8034FACCu, TickSite::Countdown},          // small monster (8034FA1C): wait (+0x20), acts at 0
{0x80350CBCu, TickSite::Countdown},          // small monster (80350B34): wait (+0x20), acts at 0
{0x80350EE8u, TickSite::Countup},            // small monster (80350DD8): state steps (+0x20)
{0x80351380u, TickSite::Countup},            // small monster: state steps (+0x20), against 7
{0x803520F8u, TickSite::Countup},            // small monster (80351FAC): state steps (+0x20)
{0x80354004u, TickSite::Countup},            // small monster (80353EBC): state steps (+0x20)
{0x8035437Cu, TickSite::Countup},            // small monster (803542A4): state steps (+0x20)
{0x803549D4u, TickSite::Countup},            // small monster (8035455C): state steps (+0x20)
{0x80354B48u, TickSite::Countup},            // small monster (8035455C): state steps (+0x20)
{0x80355278u, TickSite::Countup},            // small monster (80354FFC): state steps (+0x20)
{0x80356120u, TickSite::Countup},            // small monster (80355A44): state steps (+0x20)
{0x80358EB4u, TickSite::Countdown},          // creature (80358BA4): frame hold (s8 +0x18), next frame at 0
{0x80358FA8u, TickSite::CountdownPastZero},  // creature: countdown (s16 +0x14)
{0x8035B21Cu, TickSite::Countdown},         // 8035AF48: countdown (+8), blinks below 30
{0x80367920u, TickSite::Countup},            // desert object (803677C4): steps (+0x40) per part
{0x80368ACCu, TickSite::Countup},            // desert object (80368A94): steps (+0x40) per part
{0x80369360u, TickSite::Countup},            // desert object (803692F0): steps (+0x40) per part
{0x80369BA8u, TickSite::Countup},
{0x8037F62Cu, TickSite::Countup, 449},       // volcano object (8037F5C8): steps (s16 +0x35A), up to 450
{0x80382498u, TickSite::Countup, 59},        // volcano creature (80382374): colour fade (u8 +0x2C per part), up to 60
{0x803824C0u, TickSite::Countup},            // volcano creature: steps (s16 +0x8 per part), against 120
{0x80382520u, TickSite::Countup, 179},       // volcano creature: colour fade, up to 180
{0x803825B4u, TickSite::Countup},            // volcano creature: steps, against 300
{0x80382614u, TickSite::Countup, 179},
{0x8038268Cu, TickSite::Countup},            // volcano creature: steps, against 180
{0x803826ECu, TickSite::Countup, 179},
{0x80383AB0u, TickSite::Countdown},         // 0x806C4898 countdown (80383A50), set from seconds * fps by 80383C0C / 80384388
{0x803860F0u, TickSite::CountdownPastZero},  // object (8038608C): countdown (+0x16C), fires below 0
{0x80386488u, TickSite::Countdown},          // object (8038642C): countdown (+0x16C), fires at 0
{0x80393FD4u, TickSite::Countup},           // village effects (80393B50): item age (s16) to 1350
{0x80393FF8u, TickSite::Countdown},         // village effects: wait (s16) between parts, 8 ticks
{0x80396700u, TickSite::Countup},           // village effect (80396074): age (+0x10) to its life (+0xC)
{0x80398008u, TickSite::Countup},           // quest result screen (803975D0): +0x2FF4 step counter
{0x80398080u, TickSite::Countdown},         // quest result screen: +0x2FF4 countdown, set from seconds * fps by 803973A8
{0x8039E568u, TickSite::Countup},           // quest result screen (8039E3AC): +0x2FF4 step counter
{0x8039E5E0u, TickSite::Countdown},         // quest result screen: +0x2FF4 countdown, set from seconds * fps by 8039D174
{0x803B0C00u, TickSite::Countup},           // quest clock (803B0228): steps of quest time (quest work+0x28)
{0x803B0CE4u, TickSite::Countup},
{0x803B0E84u, TickSite::Countup},           // quest (803B0E28): quest work +0x697C steps, against seconds * fps
{0x803B1098u, TickSite::Countup},           // quest (803B107C): quest work +0x697C steps
{0x803B1144u, TickSite::Countup},
{0x803B11F8u, TickSite::Countup},
{0x803B12ECu, TickSite::Countup},
{0x803B7DCCu, TickSite::Countup},           // quest (803B7D4C): +0x65D4 steps, against seconds * fps
{0x803B8734u, TickSite::Countup},           // quest (803B8718): +0x10 steps, counted while below seconds * fps
{0x803B8780u, TickSite::Countup},           // (803B8718): +0x14
{0x803B9B48u, TickSite::Countdown},          // ambient creature (803B9AE8): countdown (+0x1B8), clamped at 0
{0x803B9B74u, TickSite::Countup},            // ambient creature: steps (+0x10)
{0x803BB824u, TickSite::Advance, -1024},     // ambient creature glow state (803BB7F8): phase (+0x1A4), -1024 per step
{0x803BB838u, TickSite::Rate},               // ambient creature glow: pulse (+0x1B0) += rate (u16 +0x1B4) a step
{0x803BBA14u, TickSite::Countdown},         // ambient creature (803BB984): countdown (s16 +0xE), clamped at 0
{0x803BBAD0u, TickSite::Countdown},          // ambient creature (803BB984): idle wait (s16 +0xC), fires at 0
{0x803BBB00u, TickSite::CountupEvery, 32},   // ambient creature: steps (s16 +0x220), a sound every 32
{0x803BBC20u, TickSite::Countdown},          // ambient creature: countdown (+0x10), fires at 0
{0x803BBCD8u, TickSite::CountdownPastZero},  // ambient creature: countdown (s16 +0xE), fires below 0
{0x803BBE04u, TickSite::Countdown},         // ambient creature (803BBDA4): wait (s16 +0x220) before a 1-in-2 roll
{0x803BBF60u, TickSite::Countdown},         // ambient creature: countdown (s16 +0xE), clamped at 0
{0x803BC010u, TickSite::CountdownPastZero}, // ambient creature: countdown (+0x10), fires below 0
{0x803BC0ACu, TickSite::Countdown},         // ambient creature: idle wait (s16 +0xC) in state 10, fires at 0
{0x803BC110u, TickSite::Countdown},         // ambient creature: idle wait (s16 +0xC) in state 129
{0x803BC20Cu, TickSite::CountdownPastZero}, // ambient creature: countdown (+0x10) while leaving
{0x803BC220u, TickSite::CountdownPastZero}, // ambient creature: countdown (s16 +0xE), fires below 0
{0x803BC338u, TickSite::CountdownPastZero}, // ambient creature: countdown (+0x10), fires below 0
{0x803C37D4u, TickSite::Advance, 255},      // online marker effect (803C3574): spin angle (+0x28)
{0x803C37E0u, TickSite::Advance, 512},      // online marker effect: bob phase (u16)
{0x803C3A28u, TickSite::Advance, 255},
{0x803C3A34u, TickSite::Advance, 512},
{0x803C477Cu, TickSite::Countup},           // event demo (803C46C4): demo steps (0x806D4FA4), the demo script's clock
{0x80449B20u, TickSite::Countdown},         // name-entry keyboard open (80449B00): input wait (0x8079722C), 15 after opening
{0x8044A058u, TickSite::Countdown},         // name-entry keyboard update (8044A038): the same input wait
{0x8044C784u, TickSite::CountdownPastZero}, // name-entry keyboard (8044C72C): close delay (the same word, from 2), fires below 0
{0x808CAF3Cu, TickSite::Countup, 0, "quest_field"}, // quest object (quest_data RSO): steps (+0x4)
{0x808DEA90u, TickSite::Countup, 0, "quest_field"}, // quest object (quest_data RSO): steps (+0x10)
{0x808DF69Cu, TickSite::Countup, 0, "quest_field"}, // quest object (quest_data RSO): steps (+0x10)
{0x808E07D4u, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x10)
{0x808E1058u, TickSite::Countdown},          // quest object (quest_data RSO): countdown (+0x14)
{0x808E10A8u, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x14)
{0x808E1C64u, TickSite::Advance, -5461},     // quest object (quest_data RSO): spin (+0x28), -5461 a step
{0x808E1C94u, TickSite::Advance, -546},      // quest object: spin (+0x24)
{0x808E1CA0u, TickSite::Advance, -364},      // quest object: spin (+0x2C)
{0x808E1D2Cu, TickSite::Advance, -5461},     // quest object: spin (+0x28)
{0x808E1E6Cu, TickSite::Advance, 128},       // quest object: phase (u16 +0x2E)
{0x808E33E8u, TickSite::Countdown},          // quest object (quest_data RSO): countdown (+0x10)
{0x808E3484u, TickSite::Countdown},          // quest object (quest_data RSO): countdown (+0x10)
{0x808E3EA8u, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x10)
{0x808E46A4u, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x10)
{0x808E5F34u, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x14)
{0x808E6044u, TickSite::Advance, 1274},      // quest object: spins (+0x28..+0x30) by kind
{0x808E6050u, TickSite::Advance, 910},
{0x808E6060u, TickSite::Advance, 455},
{0x808E606Cu, TickSite::Advance, 455},
{0x808E607Cu, TickSite::Advance, 910},
{0x808E608Cu, TickSite::Advance, 546},
{0x808E6098u, TickSite::Advance, 91},
{0x808E60A4u, TickSite::Advance, -181},
{0x808E60B8u, TickSite::Advance, 2731},
{0x808E60C8u, TickSite::Advance, 3641},
{0x808E60D8u, TickSite::Advance, 4915},
{0x808E60E4u, TickSite::Advance, 4369},
{0x808E60F0u, TickSite::Advance, 1638},
{0x808E7E64u, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x10)
{0x808E8430u, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x34)
{0x808E84B0u, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x34)
{0x808E85E0u, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x34)
{0x808E8694u, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x34)
{0x808E89C4u, TickSite::Countdown},          // quest object (quest_data RSO): countdown (+0x10)
{0x808E8BC0u, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x34)
{0x808E951Cu, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x14)
{0x808E95B4u, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x14)
{0x808E9660u, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x14)
{0x808E9690u, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x10)
{0x808E9798u, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x14)
{0x808EAC4Cu, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x10)
{0x808EC5ECu, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x10)
{0x808EDCB4u, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x54)
{0x808EE318u, TickSite::Countdown},          // quest object (quest_data RSO): countdown (+0x10)
{0x808EE4C8u, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x10)
{0x808EE970u, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x58)
{0x808EE97Cu, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x5C)
{0x808EEE00u, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x10)
{0x808EEF3Cu, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x58)
{0x808EF470u, TickSite::Countdown},          // quest object (quest_data RSO): countdown (+0x10)
{0x808F0344u, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x14)
{0x808F062Cu, TickSite::Countdown},          // quest object (quest_data RSO): countdown (+0x10)
{0x808F06A8u, TickSite::Countdown},          // quest object (quest_data RSO): countdown (+0x10)
{0x808F0714u, TickSite::Countdown},          // quest object (quest_data RSO): countdown (+0x10)
{0x808F079Cu, TickSite::Countdown},          // quest object (quest_data RSO): countdown (+0x10)
{0x808F088Cu, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x14)
{0x808F0E78u, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x10)
{0x808F11C0u, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x10)
{0x808F1334u, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x10)
{0x808F181Cu, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x10)
{0x808F1F48u, TickSite::Countdown},          // quest object (quest_data RSO): countdown (+0x10)
{0x808F206Cu, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x10)
{0x808F24F0u, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x14)
{0x808F25B4u, TickSite::Countdown},          // quest object (quest_data RSO): countdown (+0x10)
{0x808F29D8u, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x14)
{0x808F2C8Cu, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x10)
{0x808F3210u, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x10)
{0x808F3608u, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x14)
{0x808F3FB0u, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x10)
{0x808F44B8u, TickSite::Countdown},          // quest object (quest_data RSO): countdown (+0x14)
{0x808F4630u, TickSite::Countdown},          // quest object (quest_data RSO): countdown (+0x14)
{0x808F4848u, TickSite::Advance, 9102},      // quest object: spin (+0x24)
{0x808F4858u, TickSite::Advance, 5461},
{0x808F4868u, TickSite::Advance, 5461},
{0x808F5198u, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x14)
{0x808F64C8u, TickSite::Countdown},          // quest object (quest_data RSO): countdown (+0x10)
{0x808F6664u, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x14)
{0x808F6B98u, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x10)
{0x808F6CFCu, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x14)
{0x808F6F3Cu, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x14)
{0x808F7460u, TickSite::Countdown},          // quest object (quest_data RSO): countdown (+0x10)
{0x808F7A7Cu, TickSite::CountdownEvery, 10}, // quest object (quest_data RSO): countdown (+0x10), kind 123 at % 10 == 0
{0x808F7C90u, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x14)
{0x808F7E80u, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x14)
{0x808F860Cu, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x10)
{0x808F89B0u, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x14)
{0x808F8F78u, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x10)
{0x808F8FBCu, TickSite::Countup, 29},        // quest object (quest_data RSO): steps (+0x14)
{0x808F90BCu, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x10)
{0x808F90E8u, TickSite::CountdownPastZero},  // quest object (quest_data RSO): countdown (+0x10)
{0x808F916Cu, TickSite::Countup},            // quest object (quest_data RSO): steps (+0x28)
{0x808F91CCu, TickSite::Countdown},          // quest object (quest_data RSO): countdown (+0x14)
{0x808FB1C0u, TickSite::Countdown},          // quest object (quest_data RSO): countdown (+0x10)
{0x809735B4u, TickSite::Countup},            // enemy (em_data RSO): steps (+0x14) while +0x43D is 1
{0x8097DDE0u, TickSite::Advance, 1024, "demo"}, // title character view (8097DCFC): turn (+0x1142) while the button is held
{0x8097DDF8u, TickSite::Advance, -1024, "demo"},
{0x8097EC5Cu, TickSite::Countdown, 0, "demo"}, // title (demo_title_move 8097EBE4): wait (+4) before the state runs
{0x8097ED38u, TickSite::Countdown, 0, "demo"}, // title: state timer (+6) counting down
{0x8097ED70u, TickSite::Countdown, 0, "demo"},
{0x8097EF14u, TickSite::Countup, 899, "demo"}, // title screen: idle steps (+6) until the attract movie at 900
{0x8097EF54u, TickSite::Rate, 0, "demo"},    // title: pulse phase (+0x104C) += rate (+0x1050, 512)
{0x8097EFECu, TickSite::Countup, 0, "demo"}, // title: steps (+6) of the fade to the attract movie, 20
{0x8097F144u, TickSite::Countdown, 0, "demo"},
{0x8097F2C0u, TickSite::Countdown, 0, "net_data"}, // net menu (net_data RSO, 8097F2AC): Felyne frame countdown (+0x12C) from 20, flips +0x130 at 0
{0x8097F694u, TickSite::Rate, 0, "demo"},    // title
{0x8097F778u, TickSite::Countdown, 0, "net_data"}, // net menu text list (8097F774): arrow bumps (u8 0x80995EBA, +1) from 4
{0x8097F794u, TickSite::Countdown, 0, "net_data"},
{0x8097F830u, TickSite::Countup, 11, "net_data"}, // net menu text list: steps a direction is held (+0x2C), scrolls each step from 12
{0x8097FA50u, TickSite::Countdown, 0, "demo"}, // title
{0x8097FBFCu, TickSite::Rate, 0, "demo"},
{0x8097FEA8u, TickSite::Countdown, 0, "demo"},
{0x80980520u, TickSite::Countdown, 0, "net_data"}, // net menu window (8097FFF8): Felyne frame countdown (+0x12C) from 20
{0x8098054Cu, TickSite::Countdown, 0, "net_data"}, // net menu window: message cycle countdown (+0x12E) from 150, moves +0x131 on
{0x809808F8u, TickSite::Rate, 0, "demo"},    // title
{0x80980EA0u, TickSite::Countup, 0, "net_data"}, // online_sub_reflect (80980DE4): steps waiting (+0x128), shown as seconds (/ 30)
{0x80980EF8u, TickSite::Countup, 0, "net_data"},
{0x80980FA8u, TickSite::Countup, 0, "demo"}, // title: steps (+0x1140) until the menu music starts
{0x80981144u, TickSite::Countdown, 0, "demo"},
{0x8098114Cu, TickSite::Countup, 0, "net_data"}, // online_sub_reflect: steps waiting (+0x128)
{0x80981218u, TickSite::Rate, 0, "demo"},    // title
{0x809814D0u, TickSite::Countdown, 0, "demo"},
{0x80981654u, TickSite::Rate, 0, "demo"},
{0x80981ECCu, TickSite::Rate, 0, "demo"},
{0x809828A0u, TickSite::Rate, 0, "demo"},
{0x80982B78u, TickSite::Countdown, 0, "demo"},
{0x80982DCCu, TickSite::Rate, 0, "demo"},
{0x80982DECu, TickSite::Countup, 0, "demo"}, // title menu: blink steps (+6), mod 30
{0x809831C8u, TickSite::Rate, 0, "demo"},
{0x809833DCu, TickSite::Countdown, 0, "demo"},
{0x80983918u, TickSite::Countdown, 0, "demo"},
{0x80984200u, TickSite::Countdown, 0, "demo"},
{0x80993528u, TickSite::Countdown, 0, "demo"}, // title task (Demo_task 80993470): logo timer (+0xC)
{0x80993560u, TickSite::Countdown, 0, "demo"},
