// Tick sites: guest loads of step counters and countdowns (recomp.yml
// translation.tick_sites_path; tick_sites.cpp). {pc, kind[, limit]}, sorted
// by pc. The value loaded at pc is followed by the site's +1 / -1: MH3_TickLoad
// adjusts it so that the store advances it by the ticks of the step.
// Float sites (lfs): EaseDivisor loads the d of `x += (goal - x) / d`, PerStep an
// amount applied once per step. Integer Advance sites load a value the guest
// moves on by a constant (limit) each step: it is moved on by limit * s.
// Rate sites load such an amount kept in memory: it becomes k * s. Decay sites
// (lfs) load the f of `x *= f` each step: it becomes f^s.
{0x800E1F08u, TickSite::PerStep},           // MHchar channel blend (800E1EB4): cross-fade -= rate per step
{0x800FF9D8u, TickSite::Countup},           // effect spawner (800FF938): per-slot steps since the last spawn (+0xE8)
{0x801298C8u, TickSite::Countup, 26999},    // enemy: steps (s16 +0x452), capped at 27000
{0x80129A3Cu, TickSite::Countdown},         // enemy (80129A3C): countdowns (s16 +0x200, +0x202), clamped at 0
{0x80129A5Cu, TickSite::Countdown},
{0x801307C4u, TickSite::Countup, 32766},    // enemy: steps (s16 +0x440), capped at 0x7FFF
{0x8013896Cu, TickSite::PerStep},           // enemy update (801382A0): fast turn speed (+0x1AC) slows by 10 a step
{0x801389ACu, TickSite::Decay},             // enemy update: slow turn speed (+0x1AC) *= 0.9 a step
{0x801389C0u, TickSite::PerStep},           // enemy update: yaw (+0x1A4) += turn speed a step
{0x80138D14u, TickSite::Countdown},         // enemy (80138BC4): countdown (s16 +0x764)
{0x80138D40u, TickSite::Countdown},
{0x801556C8u, TickSite::Countup},           // enemy (801556C8): cycle steps (u16 +0x33C), wraps at 50
{0x801557A8u, TickSite::Countdown},         // enemy action (8015574C): countdown (+0x20), ends the action below 0
{0x801AC6D8u, TickSite::Countdown},         // enemy action (801AC674): countdown (+0x20), ends the action at 0
{0x80201884u, TickSite::Advance, 512},      // village NPC (802014C8): yaw (LB_NPC+0x2C) turning, +512 per step
{0x80203744u, TickSite::Countdown},         // village NPC (802036C8): wait (+0xC) before the next idle move
{0x8020A84Cu, TickSite::Countup},           // village player action step counter (PLW+0x28)
{0x8020A858u, TickSite::Countup, 149},      // village player idle steps (PLW+0x36), capped at 150
{0x8020B114u, TickSite::CountdownPastZero}, // village player action countdown (PLW+0x28), fires at <= 0
{0x8020B250u, TickSite::CountdownPastZero},
{0x8020B7DCu, TickSite::CountdownPastZero},
{0x8020B960u, TickSite::CountdownPastZero},
{0x8020C660u, TickSite::CountdownPastZero},
{0x8020C694u, TickSite::CountdownPastZero},
{0x8020C6F8u, TickSite::CountdownPastZero},
{0x8020C74Cu, TickSite::CountdownPastZero},
{0x8020C7F8u, TickSite::CountdownPastZero},
{0x8020C944u, TickSite::CountdownPastZero},
{0x8020C9DCu, TickSite::CountdownPastZero},
{0x8020CB14u, TickSite::CountdownPastZero},
{0x8020CD20u, TickSite::Countup},
{0x8020CEA8u, TickSite::CountdownPastZero},
{0x8020D51Cu, TickSite::CountdownPastZero},
{0x8020D7FCu, TickSite::Countdown},         // village player action countdown (PLW+0x28), while > 0
{0x8020DB2Cu, TickSite::Countdown},
{0x8020E47Cu, TickSite::CountdownPastZero},
{0x802214D4u, TickSite::Countdown},         // village object (80221410) countdown (s16 +0x28), while != 0
{0x80246C64u, TickSite::Countup},           // player action step counter (PLW+0x28) in action handlers
{0x80247844u, TickSite::Countdown},         // player action countdown (PLW+0x28), while > 0
{0x802498B4u, TickSite::Countup},
{0x802499E4u, TickSite::CountdownPastZero}, // player action countdown (PLW+0x28), fires at <= 0
{0x8024A884u, TickSite::CountdownPastZero},
{0x8024ACD0u, TickSite::CountdownPastZero},
{0x8024B9F0u, TickSite::Countup},
{0x8024C224u, TickSite::Countdown, 1},
{0x8024C314u, TickSite::Countdown},
{0x80250070u, TickSite::Countdown},
{0x80250114u, TickSite::Countdown},
{0x802501DCu, TickSite::CountdownPastZero},
{0x802506CCu, TickSite::CountdownPastZero},
{0x80250D68u, TickSite::Countdown},
{0x80251590u, TickSite::Countdown},
{0x8025182Cu, TickSite::Countdown},
{0x80251A08u, TickSite::CountdownPastZero},
{0x80253200u, TickSite::Countdown},
{0x80253628u, TickSite::CountdownPastZero},
{0x80253D78u, TickSite::Countdown},
{0x8025484Cu, TickSite::Countdown},
{0x80254A4Cu, TickSite::Countup},
{0x80254F3Cu, TickSite::Countup},
{0x80256C68u, TickSite::Countdown},
{0x80257E98u, TickSite::CountdownPastZero},
{0x80257FE8u, TickSite::CountdownPastZero},
{0x802582F8u, TickSite::Countdown},
{0x802584E0u, TickSite::CountdownPastZero},
{0x802584ECu, TickSite::CountdownPastZero},
{0x80258E9Cu, TickSite::CountdownPastZero},
{0x80259480u, TickSite::CountdownPastZero},
{0x802596A8u, TickSite::Countdown},
{0x80259884u, TickSite::Countdown},
{0x802598F0u, TickSite::Countup},
{0x80259ABCu, TickSite::Countup},
{0x80259CB8u, TickSite::Countup},
{0x8025AC3Cu, TickSite::CountdownPastZero},
{0x8025B380u, TickSite::Countup},
{0x8025B4C4u, TickSite::Countup},
{0x8025BD14u, TickSite::Countdown},
{0x8025C19Cu, TickSite::Countdown},
{0x8025CEF8u, TickSite::Countup},
{0x8025D0BCu, TickSite::Countup},
{0x8025D6B0u, TickSite::Countup},
{0x8025DBF8u, TickSite::CountdownPastZero},
{0x8025DD58u, TickSite::CountdownPastZero},
{0x8026ABC4u, TickSite::Countup, 99},       // player button hold steps (8026AB00), capped at 100
{0x80281750u, TickSite::CountdownPastZero},
{0x80281ECCu, TickSite::CountdownPastZero},
{0x802823CCu, TickSite::CountdownPastZero},
{0x80282880u, TickSite::CountdownPastZero},
{0x802857D8u, TickSite::Countup},
{0x80286B70u, TickSite::Countup},
{0x80288808u, TickSite::CountdownPastZero},
{0x802B638Cu, TickSite::EaseDivisor},       // field camera (802B5CBC): height eases, target (f31) and eye (f30)
{0x802B644Cu, TickSite::EaseDivisor},
{0x802B6450u, TickSite::EaseDivisor},
{0x802B65BCu, TickSite::Countdown, 1},      // field camera: ease timer (+0xA8) while swimming, down to 1
{0x802B65D0u, TickSite::EaseDivisor},
{0x802B65D4u, TickSite::EaseDivisor},
{0x802B65DCu, TickSite::EaseDivisor},
{0x802B65FCu, TickSite::Countdown},         // field camera: ease timer (+0xA8)
{0x802B6610u, TickSite::EaseDivisor},
{0x802BACE8u, TickSite::Countdown},         // camera shake (802BAC0C): steps left (s16 +0x44)
{0x802BCDC8u, TickSite::CountdownPastZero}, // camera script wait (op 0x25): frames left, done below 0
{0x802BD878u, TickSite::Countdown},         // camera path hold (802BD78C): steps before playing
{0x802BD8B0u, TickSite::Countup},           // camera path frame (802BD78C)
{0x802E4C90u, TickSite::Countup},           // 802E4C88: steps in the area (806C04E8)
{0x802E4CA4u, TickSite::Countup},           // 802E4C88: steps (806C02D8)
{0x802E4CB0u, TickSite::Countup},           // 802E4C88: steps (806C046C)
{0x802E4D60u, TickSite::Countup},           // area ambience (802E4CC0): cycle steps (u8 +0xC), 40..190
{0x802E4DB4u, TickSite::Countup},           // area ambience: cycle steps (u8 +0xD)
{0x802E4E24u, TickSite::Countup},           // area ambience: cycle steps (u8 +0xE), 30..50
{0x802EF2ACu, TickSite::Countdown},         // player-side timer (802EF294): +0x5C, set from seconds * fps by 802EF3A4
{0x8031F728u, TickSite::Advance, 512},      // marker effect (8031F574): bob phase (u16), +512 per step
{0x8031F734u, TickSite::Advance, 255},      // marker effect: spin angle (+0x28), +255 per step
{0x803327A4u, TickSite::CountdownPastZero}, // actor action countdown (+0x28), fires at <= 0
{0x80334330u, TickSite::CountdownPastZero},
{0x80340924u, TickSite::Countdown},         // fish (803408B4): turn steps left (+0xC)
{0x80340A00u, TickSite::PerStep},           // fish: swim speed (+0x20) moved per step
{0x80340B00u, TickSite::PerStep},           // fish: swim speed change per step
{0x80344EBCu, TickSite::Countup, 32766},    // 80344EBC: object ages (+0x34, +0x1C, +0x1E), capped at 0x7FFF
{0x80344ED0u, TickSite::Countup, 32766},
{0x80344EE4u, TickSite::Countup, 32766},
{0x8034E5ECu, TickSite::Countup},           // actor action step counter (+0x28)
{0x8034EC68u, TickSite::Countup},
{0x8035B21Cu, TickSite::Countdown},         // 8035AF48: countdown (+8), blinks below 30
{0x80383AB0u, TickSite::Countdown},         // 0x806C4898 countdown (80383A50), set from seconds * fps by 80383C0C / 80384388
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
{0x803BBA14u, TickSite::Countdown},         // quest object (803BB984): countdown (s16 +0xE), clamped at 0
{0x803BBF60u, TickSite::Countdown},         // quest object (803BBDA4): countdown (s16 +0xE), clamped at 0
{0x803BC010u, TickSite::CountdownPastZero}, // quest object: countdown (+0x10), fires below 0
{0x803C37D4u, TickSite::Advance, 255},      // online marker effect (803C3574): spin angle (+0x28)
{0x803C37E0u, TickSite::Advance, 512},      // online marker effect: bob phase (u16)
{0x803C3A28u, TickSite::Advance, 255},
{0x803C3A34u, TickSite::Advance, 512},
{0x8097DDE0u, TickSite::Advance, 1024},     // title character view (8097DCFC): turn (+0x1142) while the button is held
{0x8097DDF8u, TickSite::Advance, -1024},
{0x8097EC5Cu, TickSite::Countdown},         // title (demo_title_move 8097EBE4): wait (+4) before the state runs
{0x8097ED38u, TickSite::Countdown},         // title: state timer (+6) counting down
{0x8097ED70u, TickSite::Countdown},
{0x8097EF14u, TickSite::Countup, 899},      // title screen: idle steps (+6) until the attract movie at 900
{0x8097EF54u, TickSite::Rate},              // title: pulse phase (+0x104C) += rate (+0x1050, 512)
{0x8097EFECu, TickSite::Countup},           // title: steps (+6) of the fade to the attract movie, 20
{0x8097F144u, TickSite::Countdown},
{0x8097F694u, TickSite::Rate},
{0x8097FA50u, TickSite::Countdown},
{0x8097FBFCu, TickSite::Rate},
{0x8097FEA8u, TickSite::Countdown},
{0x809808F8u, TickSite::Rate},
{0x80980FA8u, TickSite::Countup},           // title: steps (+0x1140) until the menu music starts
{0x80981144u, TickSite::Countdown},
{0x80981218u, TickSite::Rate},
{0x809814D0u, TickSite::Countdown},
{0x80981654u, TickSite::Rate},
{0x80981ECCu, TickSite::Rate},
{0x809828A0u, TickSite::Rate},
{0x80982B78u, TickSite::Countdown},
{0x80982DCCu, TickSite::Rate},
{0x80982DECu, TickSite::Countup},           // title menu: blink steps (+6), mod 30
{0x809831C8u, TickSite::Rate},
{0x809833DCu, TickSite::Countdown},
{0x80983918u, TickSite::Countdown},
{0x80984200u, TickSite::Countdown},
{0x80993528u, TickSite::Countdown},         // title task (Demo_task 80993470): logo timer (+0xC)
{0x80993560u, TickSite::Countdown},
