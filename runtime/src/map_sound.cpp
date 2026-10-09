// Map ambient sounds, RMHP08. C reconstruction of the per-step update of the
// map's sound sources (0x800DFE30, called by the sound task 0x800D83B8),
// decompiled with Ghidra (scripts/dol2elf.py -> ELF import).
//
// Each source in the map's sound table repeats every `interval` steps: a
// countdown reaches 0, the sound is requested on the shell (se) object and the
// countdown restarts. A looping source placed on an area also follows the
// player each step (clamped into its box). The countdown is in steps, so here
// it counts the whole ticks of game time (GameStep::Ticks) instead: sounds
// repeat at the same rate at any step length, while the following still runs
// every step. At the stock 1/30 s step both are the original.
#include "game_step.h"
#include "port_check.h"
#include "guest_call.h"

#include <algorithm>

namespace {

constexpr uint32_t kSeWork = 0x80796E38;      // se_w*: r13-17576
constexpr uint32_t kSeShell = 0x292E0;        // se_w: shell se object (_se_w*)
constexpr uint32_t kSeLoopHandles = 0x292E4;  // se_w: looping source handles [n]
constexpr uint32_t kAreaNow = 0x2927E;        // this: u8 current area
constexpr uint32_t kAreaPrev = 0x2927F;       // this: u8 area last step
constexpr uint32_t kMoveWorkStride = 0xB20;   // player work

// Stereo pair of the village ambience (sda2 VEC3 components).
constexpr uint32_t kAmbienceLeftX = 0x807988DC, kAmbienceY = 0x807988E0, kAmbienceLeftZ = 0x807988E4;
constexpr uint32_t kAmbienceRightX = 0x807988E8, kAmbienceRightZ = 0x807988EC;

// Map sound table: u32 valid, u32 count, then sources.
enum : uint32_t {
    kSources = 8,
    kSourceSize = 72,
    kState = 0,        // u8: 0 unused, 1 armed, 2 done (one-shot played)
    kFollows = 1,      // u8: 1 = clamped into its box around the listener
    kSuppressed = 2,   // u8
    kPosition = 4,     // VEC3
    kBoxMin = 28,      // VEC3
    kBoxMax = 40,      // VEC3
    kInterval = 60,    // s32 steps (0 = one-shot)
    kSoundId = 64,     // u32
    kLoopHandle = 68,  // u8 index into se_w's handles, 255 none
    kArea = 69,        // u8
    kCountdown = 70,   // s16 steps
};

// Shell se object.
enum : uint32_t {
    kShellId = 12,
    kShellListener = 24,  // VEC3
    kShellListenerDir = 36,
    kShellSlots = 60,     // [32] of 80 bytes
};
// Shell sound slot.
enum : uint32_t { kSlotPosition = 4, kSlotPrevPosition = 16, kSlotVoice = 60 };

enum : uint32_t {
    Vec3Ctor = 0x80043F0C,
    Vec3Copy = 0x80041EA4,             // (dst, src)
    Vec3Set = 0x80041EF0,              // (out; f1 x, f2 y, f3 z) -> out
    SoundOffline = 0x800D84A0,         // () -> 1 while no player/map sounds
    ClearMapSounds = 0x800DFA34,       // (0)
    ChangeArea = 0x800DFB58,
    GetMoveWorkAdrs = 0x800CFAF4,      // (type) -> work table
    MyPlayerIndex = 0x800CF3E8,        // () -> s8
    MapSoundTable = 0x800F2964,        // () -> table or 0
    ShellFind = 0x800F0C78,            // (id) -> -1 if gone
    GetNowAreaNo = 0x802AFCE8,
    GetNowMapNo = 0x802AFCD8,
    MapKind = 0x802B06CC,              // (map) -> u8
    AmbienceMuted = 0x803AAFEC,        // () -> u8
    ShellSeReq = 0x800DE360,           // (shell, VEC3*, u8 bank, u32 no)
    LoopHandle = 0x800DFDB4,           // (shell, index) -> slot or 0
    VoicePlaying = 0x800E8090,         // (voice) -> 0 when stopped
    ReleaseLoopHandle = 0x800DFD8C,    // (index)
    OnlineMode = 0x800CF26C,           // () -> u8
    ClampToBox = 0x8028F99C,           // (min, max, point, out)
    ArenaQuestA = 0x802FF298,
    ArenaMonsterAlive = 0x801E9BD8,    // (i)
    ArenaQuestB = 0x8021F29C,
    ShellAllocSlot = 0x800D8EBC,       // (shell, id) -> slot or 0
    AllocLoopHandle = 0x800DFC70,      // () -> u8 index, 255 none
    ShellSlotUpdate = 0x800D9348,      // (shell, slot)
    ShellSlotFinish = 0x800D8468,      // (slot)
};

void CopyWords(uint32_t dst, uint32_t src, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) Memory::Write32(dst + i * 4, Memory::Read32(src + i * 4));
}

} // namespace

static void MapSoundUpdatePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x70);
    GuestCall game{ctx};
    const uint32_t self = frame.Arg(3);
    const uint32_t right = frame[8], left = frame[20], pos = frame[32];
    game(Vec3Ctor, {pos});

    if (game(SoundOffline) == 1) {
        Memory::Write8(self + kAreaPrev, Memory::Read8(self + kAreaNow));
        const uint8_t area = uint8_t(game(GetNowAreaNo));
        Memory::Write8(self + kAreaNow, area);
        if (Memory::Read32(Memory::Read32(kSeWork) + kSeShell) == 0) game(ClearMapSounds, {0});
        else if (Memory::Read8(self + kAreaPrev) != area) game(ChangeArea);
        return;
    }

    uint32_t player = game(GetMoveWorkAdrs, {2});
    player += int32_t(int8_t(game(MyPlayerIndex))) * kMoveWorkStride;
    const uint32_t table = game(MapSoundTable);
    uint32_t shell;
    if (table == 0) {
        shell = Memory::Read32(Memory::Read32(kSeWork) + kSeShell);
        if (shell == 0) game(ClearMapSounds, {0});
        if (shell == 0) return;
        game(Vec3Copy, {shell + kShellListener, self + 4});
        game(Vec3Copy, {shell + kShellListenerDir, self + 28});
    } else {
        if (Memory::Read32(table) == 0 || Memory::Read32(table + 4) == 0) return;
        shell = Memory::Read32(Memory::Read32(kSeWork) + kSeShell);
        if (shell == 0) return;
        if (int32_t(game(ShellFind, {Memory::Read32(shell + kShellId)})) == -1) return;
        game(Vec3Copy, {shell + kShellListener, self + 4});
        game(Vec3Copy, {shell + kShellListenerDir, self + 28});

        const uint8_t area = Memory::Read8(self + kAreaNow);
        if (area == 0 && uint8_t(game(MapKind, {uint8_t(game(GetNowMapNo))})) == 1 &&
            uint8_t(game(AmbienceMuted)) == 0) {
            const float y = Memory::ReadFloat32(kAmbienceY);
            game(Vec3Copy, {pos, game(Vec3Set, {left},
                                      {Memory::ReadFloat32(kAmbienceLeftX), y, Memory::ReadFloat32(kAmbienceLeftZ)})});
            game(ShellSeReq, {shell, pos, 6, 0});
            game(Vec3Copy, {pos, game(Vec3Set, {right},
                                      {Memory::ReadFloat32(kAmbienceRightX), y, Memory::ReadFloat32(kAmbienceRightZ)})});
            game(ShellSeReq, {shell, pos, 6, 1});
        }

        const uint32_t ticks = GameStep::Ticks();
        uint32_t source = table + kSources;
        for (int32_t i = 0; i < int32_t(Memory::Read32(table + 4)); ++i, source += kSourceSize) {
            const uint8_t state = Memory::Read8(source + kState);
            if (state == 0) continue;
            if (Memory::Read8(source + kArea) != area) {
                Memory::Write8(source + kState, 1);
                Memory::Write16(source + kCountdown, 0);
                continue;
            }
            if (state == 2) continue;

            const int16_t countdown = int16_t(Memory::Read16(source + kCountdown));
            if (countdown > 0) {
                // The original: countdown - 1 per step.
                Memory::Write16(source + kCountdown,
                                uint16_t(countdown - int16_t(std::min<uint32_t>(ticks, uint32_t(countdown)))));
                if (Memory::Read8(source + kSuppressed) != 0 || Memory::Read8(source + kFollows) != 1) continue;
                const uint8_t index = Memory::Read8(source + kLoopHandle);
                uint32_t slot = 0;
                if (index != 255) slot = game(LoopHandle, {shell, index});
                if (index == 255 || slot == 0 || Memory::Read32(slot + kSlotVoice) == 0 ||
                    game(VoicePlaying, {Memory::Read32(slot + kSlotVoice)}) == 0) {
                    game(ReleaseLoopHandle, {Memory::Read8(source + kLoopHandle)});
                    Memory::Write8(source + kLoopHandle, 255);
                    continue;
                }
                const uint32_t listener = uint8_t(game(OnlineMode)) == 2 && player != 0 ? player + 0x3C
                                                                                      : shell + kShellListener;
                game(ClampToBox, {source + kBoxMin, source + kBoxMax, listener, pos});
                game(Vec3Copy, {slot + kSlotPosition, pos});
                CopyWords(slot + kSlotPrevPosition, slot + kSlotPosition, 3);
                continue;
            }

            Memory::Write16(source + kCountdown, uint16_t(Memory::Read32(source + kInterval)));
            if (Memory::Read8(source + kSuppressed) != 0) continue;
            if (uint8_t(game(MapKind, {uint8_t(game(GetNowMapNo))})) == 22) { // arena
                const uint32_t id = Memory::Read32(source + kSoundId);
                if (game(ArenaQuestA) == 0 && id == 4) continue;
                if (game(ArenaMonsterAlive, {0}) == 0 && game(ArenaMonsterAlive, {1}) == 0 &&
                    game(ArenaMonsterAlive, {2}) == 0 && id == 5)
                    continue;
                if (game(ArenaQuestB) == 0 && id == 13) continue;
            }
            const uint32_t slot = game(ShellAllocSlot, {shell, Memory::Read32(source + kSoundId)});
            if (slot) {
                Memory::Write8(slot + 1, 3);
                Memory::Write8(slot + 2, 2);
                Memory::Write32(slot + 40, Memory::Read32(shell + kShellId));
                Memory::Write32(slot + 44, Memory::Read32(source + kSoundId));
                Memory::Write32(slot + 48, 0xFFFFFFFF);
                Memory::Write32(slot + 52, 0xFFFFFFFF);
                Memory::Write32(slot + 56, 0);
                if (Memory::Read8(source + kFollows) == 0) {
                    game(Vec3Copy, {slot + kSlotPosition, source + kPosition});
                    CopyWords(slot + kSlotPrevPosition, slot + kSlotPosition, 3);
                } else {
                    game(ClampToBox, {source + kBoxMin, source + kBoxMax, shell + kShellListener, pos});
                    game(Vec3Copy, {slot + kSlotPosition, pos});
                    CopyWords(slot + kSlotPrevPosition, slot + kSlotPosition, 3);
                    Memory::Write8(source + kLoopHandle, 255);
                    const uint8_t handle = uint8_t(game(AllocLoopHandle));
                    if (handle != 255) {
                        Memory::Write32(Memory::Read32(kSeWork) + kSeLoopHandles + handle * 4, slot);
                        Memory::Write8(source + kLoopHandle, handle);
                    }
                }
            }
            if (Memory::Read32(source + kInterval) == 0) Memory::Write8(source + kState, 2);
        }
    }

    for (uint32_t i = 0; i < 32; ++i) {
        const uint32_t slot = shell + kShellSlots + i * 80;
        if (Memory::Read8(slot) != 0 && uint8_t(Memory::Read8(slot + 1) - 3) < 2) {
            game(ShellSlotUpdate, {shell, slot});
            game(ShellSlotFinish, {slot});
        }
    }
}
extern "C" void func_800DFE30(CpuContext* ctx);
extern "C" void MH3_MapSoundUpdate(CpuContext* ctx) { PortCheck::Call(ctx, "MapSoundUpdate", func_800DFE30, MapSoundUpdatePort); }
REGISTER_NATIVE_FUNCTION_AS(0x800DFE30, MH3_MapSoundUpdate, "MapSoundUpdate");
