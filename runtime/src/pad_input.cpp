// Controller reading, RMHP08. C reconstructions of the per-step pad update
// that PollControllers (0x80041F6C, called by the game frame) runs for each
// of the four channels, decompiled with Ghidra:
//   0x80042088  PadReadWii (channel): WPAD probe and read, the Wii Remote,
//               Nunchuk and Classic Controller button maps, presses and
//               releases, held-button repeat, the accelerometer history,
//               then PadGameKeys
//   0x800429A4  PadReadGc (channel): the GameCube pad's buttons and repeat
//   0x80044368  PadGameKeys (channel): the game's own key set (Psw+0x2C0)
//               from whichever controller is attached, its repeat and the
//               stick as a d-pad in menus
//
// Three things in these count steps, and so ran 15-20 times too often at the
// VM's 500-700 steps a second:
//   - repeat: a held button set repeats 13 steps after it last changed, then
//     every 3 steps (each of the five sets: Wii Remote, Classic, Nunchuk,
//     GameCube pad, game keys, and the stick directions);
//   - key locks: a game key that changes stays as it was for a few steps
//     (u8 countdowns at Psw+0x336, reloaded from a per-mode table);
//   - the extension info countdown (system_w+0x7E0, from 120).
// Here they count whole ticks of game time (GameStep::Ticks()), so a held
// cursor repeats at the Wii's rate at any step length. At the stock 1/30 s
// step every count moves by 1: the original.
//
// The accelerometer history (16 samples, index Psw+0x2A4) still takes one
// sample a step: the host bridge has no Wii Remote motion, so the history
// holds the stationary remote either way.
#include "game_step.h"
#include "guest_call.h"
#include "port_check.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>

namespace {

enum : uint32_t {
    kSystem = 0x8065AA60,        // system_w
    kInputSource = 0x8D8,        // system_w: fn(system_w) -> 1 when input comes from elsewhere (no pads)
    kInfoWait = 0x7E0,           // system_w: s8[4], info countdown (120 when set)
    kInfoPending = 0x7E4,        // system_w: u8[4], 1 = ask WPADGetInfoAsync when the wait starts
    kPsw = 0x8065B7D0,           // Psw: pad work, 0x350 bytes a channel
    kPswSize = 0x350,
    kGcPad = 0x8065C510,         // GameCube pad work, 0x350 bytes a channel
    kInfo = 0x8065D250,          // WPADInfo[4], 0x18 bytes each
    kGcMap = 0x8065D2B0,         // u16[16]: GameCube pad bits, mapped to kGameBits
    kClassicMap = 0x8065D2D0,    // u16[16]: Classic bits
    kRemoteMap = 0x8065D2F0,     // u16[16]: Wii Remote bits
    kGameBits = 0x80581630,      // u16[16]: the game's button bits
    kKeySources = 0x80581800,    // {u16 key bit, u16 key value}[17]
    kNunchukKeys = 0x805816F0,   // {s16 which, u16 mask}[17] per device
    kClassicKeysA = 0x80581734,
    kClassicKeysB = 0x80581778,
    kRemoteKeys = 0x805817BC,
    kFloatZero = 0x80797FA8,     // f32 0.0
    kLastProbe = 0x80796D00,     // channel 0's probe result and device
    kLastDevice = 0x80796D04,
};

// Psw (per channel).
enum : uint32_t {
    kProbe = 0x30,         // s32 WPADProbe result (0: connected)
    kDevice = 0x34,        // u32: 0 Wii Remote, 1 Nunchuk, 2 Classic
    kCoreStatus = 0x38,    // WPADStatus
    kClassicStatus = 0x62, // WPADCLStatus (buttons at +0x2A)
    kNunchukStatus = 0x98, // WPADFSStatus
    kRemote = 0xCA,        // the Wii Remote's button set
    kClassic = 0xD6,       // the Classic Controller's button set
    kNunchuk = 0xFC,       // the Nunchuk's button set
    kHistoryIndex = 0x2A4, // u8, 0..15
    kNunchukHistory = 0x124,
    kRemoteHistory = 0x1E4, // VEC3[16] each
    kKeys = 0x2C0,          // the game's key set
};

// A button set: buttons, the last step's, pressed, released.
enum : uint32_t {
    kHeld = 0,
    kPrevious = 2,
    kPressed = 4,
    kReleased = 6,
};

// Classic and Nunchuk sets also have a d-pad set (the stick as directions).
enum : uint32_t {
    kDirs = 8,
    kDirsPrevious = 10,
    kDirsPressed = 12,
    kDirsReleased = 14,
};

// The repeat output and its count, by set.
constexpr uint32_t kRemoteRepeat = 8, kClassicRepeat = 0x22, kNunchukRepeat = 0x22, kGcRepeat = 0x18;

// The game key set (Psw+0x2C0).
enum : uint32_t {
    kKeyRepeat = 0x14,       // u16, its count at +0x16
    kKeyDegrees = 0x18,      // u16 stick direction
    kKeyMagnitude = 0x1A,    // u16 stick tilt
    kKeyStickDirs = 0x1C,    // u8 stick direction bits
    kKeyStick = 0x1D,        // u8 stick direction bits (this step)
    kKeyStickPrevious = 0x1E,
    kKeyStickRepeat = 0x1F,  // u8, its count (u16) at +0x20
    kKeyDevice = 0x22,       // u8: 0 none, 1 Nunchuk, 2 Classic, 3 Wii Remote
    kKeyLockReload = 0x24,   // u8[mode][16]
    kKeyLockMode = 0x74,     // u8
    kKeyStickAsDpad = 0x75,  // u8 set_menu_key_ad_flag
    kKeyLocks = 0x76,        // u8[16] countdowns
};

enum : uint32_t {
    PadCleared = 0x80042890,       // (channel): no pads (input from elsewhere)
    PadDisconnected = 0x80042920,  // (channel)
    ClassicSticks = 0x80042FDC,    // (channel, Psw): Classic stick directions
    NunchukStick = 0x800432C0,     // (channel, Psw): Nunchuk stick direction
    GcSticks = 0x80042C0C,         // (channel, GameCube work)
    GcError = 0x80042BAC,          // (channel) -> s8 PADStatus error
    GcButtons = 0x80042B98,        // (channel) -> u16 PADStatus buttons
    PadGameKeys = 0x80044368,      // (u8 channel)
    SetKeyLocks = 0x802AA6F0,      // (u8 channel, offline): lock table
    GameOption = 0x803BED2C,       // (channel, option, value) -> 1 when set
    GetOnlineMode = 0x800CF26C,
    IsNetworkBusy = 0x804360A0,
    WPADProbe = 0x804EF040,
    WPADRead = 0x804EF9D0,
    WPADClampStick = 0x804F8A00,   // (channel, status, kind)
    WPADClampTrigger = 0x804F8B60, // (channel, status, kind)
    WPADGetInfoAsync = 0x804EF3C0,
    InfoCallback = 0x80041F08,
};

uint16_t R16(uint32_t a) {
    return Memory::Read16(a);
}

void W16(uint32_t a, uint32_t v) {
    Memory::Write16(a, uint16_t(v));
}

// The game's bits for the device bits in `held`, through a 16-entry map.
uint16_t MapButtons(uint32_t held, uint32_t map) {
    uint16_t bits = 0;

    for (uint32_t i = 0; i < 16; ++i)
        if (held & R16(map + 2 * i))
            bits |= R16(kGameBits + 2 * i);
    return bits;
}

void PressesAndReleases(uint32_t set, uint32_t held, uint32_t previous, uint32_t pressed, uint32_t released) {
    W16(set + pressed, R16(set + held) & ~R16(set + previous));
    W16(set + released, R16(set + previous) & ~R16(set + held));
}

// A held set repeats 13 ticks after it last changed, then every 3 ticks (the
// original: steps). `count` (u16) holds the ticks held, between 10 and 12
// once repeating; `out` gets the set on a repeat, else 0. A step of more than
// 3 ticks repeats once.
template <typename T>
void Repeat(uint32_t count, uint32_t out, T held, T previous) {
    const auto write = [](uint32_t a, T v) {
        if constexpr (sizeof(T) == 1)
            Memory::Write8(a, v);
        else
            W16(a, v);
    };

    write(out, 0);

    if (previous != held) {
        W16(count, 0);
        return;
    }

    const uint32_t ticks = R16(count) + GameStep::Ticks();

    if (ticks > 12) {
        W16(count, std::min<uint32_t>(ticks - 3, 12));
        write(out, held);
    } else {
        W16(count, ticks);
    }
}

void HistorySample(uint32_t history, uint32_t index, uint32_t status) {
    for (uint32_t axis = 0; axis < 3; ++axis)
        Memory::WriteFloat32(history + index * 12 + axis * 4, float(int16_t(R16(status + axis * 2))));
}

} // namespace

static void PadReadWiiPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x60);
    GuestCall game{ctx};
    const uint32_t channel = frame.Arg(3);
    const uint32_t pad = kPsw + channel * kPswSize;
    const uint32_t classic = pad + kClassic, remote = pad + kRemote, nunchuk = pad + kNunchuk;
    W16(classic + kPrevious, R16(classic + kHeld));
    W16(classic + kDirsPrevious, R16(classic + kDirs));

    if (game(Memory::Read32(kSystem + kInputSource), {kSystem}) == 1) {
        game.Call<PadCleared>({channel});
        return;
    }

    Memory::Write32(pad + kProbe, game.Call<WPADProbe>({channel, pad + kDevice}));

    if (channel == 0) {
        Memory::Write32(kLastProbe, Memory::Read32(pad + kProbe));
        Memory::Write32(kLastDevice, Memory::Read32(pad + kDevice));
    }

    const auto probe = [&] {
        return int32_t(Memory::Read32(pad + kProbe));
    };

    if (probe() == -2 || game.Call<IsNetworkBusy>() == 1) {
        game.Call<PadDisconnected>({channel});
        return;
    }

    const auto status = [&] {
        switch (Memory::Read32(pad + kDevice)) {
        case 1:
            return pad + kNunchukStatus;
        case 2:
            return pad + kClassicStatus;
        default:
            return pad + kCoreStatus;
        }
    };

    if (probe() == 0) {
        game.Call<WPADRead>({channel, status()});
        game.Call<WPADClampStick>({channel, status(), 2});
        game.Call<WPADClampTrigger>({channel, status(), 1});
    }

    // The Classic Controller.
    if (probe() == 0 && Memory::Read32(pad + kDevice) == 2) {
        W16(classic + kHeld, MapButtons(R16(pad + kClassicStatus + 0x2A), kClassicMap));
        game.Call<ClassicSticks>({channel, pad, 12});
    } else {
        W16(classic + kHeld, 0);
        W16(classic + kDirs, 0);
        W16(classic + 0x12, 0);
        W16(classic + 0x10, 0);
        W16(classic + 0x16, 0);
        W16(classic + 0x14, 0);
        game.Call<ClassicSticks>({channel, pad, 0});
    }

    PressesAndReleases(classic, kHeld, kPrevious, kPressed, kReleased);
    PressesAndReleases(classic, kDirs, kDirsPrevious, kDirsPressed, kDirsReleased);
    Repeat<uint16_t>(classic + kClassicRepeat + 2, classic + kClassicRepeat, R16(classic + kHeld), R16(classic + kPrevious));

    // The Wii Remote (also the Nunchuk's base).
    W16(remote + kPrevious, R16(remote + kHeld));
    W16(nunchuk + kPrevious, R16(nunchuk + kHeld));
    W16(nunchuk + kDirsPrevious, R16(nunchuk + kDirs));

    if (probe() == 0) {
        const uint32_t info = kSystem + kInfoWait + channel, pending = kSystem + kInfoPending + channel;

        if (Memory::Read8(pending) == 1 && int8_t(Memory::Read8(info)) == 120) {
            game.Call<WPADGetInfoAsync>({channel, kInfo + channel * 0x18, InfoCallback});
            Memory::Write8(pending, 0);
        }

        const int8_t wait = int8_t(Memory::Read8(info));

        if (wait > 0)
            Memory::Write8(info, uint8_t(wait - int8_t(std::min<uint32_t>(GameStep::Ticks(), uint32_t(wait)))));

        W16(remote + kHeld, MapButtons(R16(status()), kRemoteMap));
    } else {
        W16(remote + kHeld, 0);
    }

    PressesAndReleases(remote, kHeld, kPrevious, kPressed, kReleased);
    Repeat<uint16_t>(remote + kRemoteRepeat + 2, remote + kRemoteRepeat, R16(remote + kHeld), R16(remote + kPrevious));

    // The Nunchuk: the Wii Remote's buttons plus its stick, and the
    // accelerometer history of both.
    const uint32_t index = (Memory::Read8(pad + kHistoryIndex) + 1) & 15;
    Memory::Write8(pad + kHistoryIndex, uint8_t(index));

    if (probe() == 0 && Memory::Read32(pad + kDevice) == 1) {
        W16(nunchuk + kHeld, R16(remote + kHeld));
        W16(nunchuk + kPrevious, R16(remote + kPrevious));
        W16(nunchuk + kPressed, R16(remote + kPressed));
        W16(nunchuk + kReleased, R16(remote + kReleased));
        W16(nunchuk + kNunchukRepeat, R16(remote + kRemoteRepeat));
        W16(nunchuk + kNunchukRepeat + 2, R16(remote + kRemoteRepeat + 2));
        game.Call<NunchukStick>({channel, pad});
        HistorySample(pad + kRemoteHistory, index, pad + kNunchukStatus + 2);
        HistorySample(pad + kNunchukHistory, index, pad + kNunchukStatus + 0x2A);
    } else {
        W16(nunchuk + kHeld, 0);
        W16(nunchuk + kDirs, 0);
        W16(nunchuk + 0x12, 0);
        W16(nunchuk + 0x10, 0);
        W16(nunchuk + 0x16, 0);
        W16(nunchuk + 0x14, 0);
        const uint32_t zero = Memory::Read32(kFloatZero);

        for (uint32_t axis = 0; axis < 3; ++axis) {
            Memory::Write32(pad + kRemoteHistory + index * 12 + axis * 4, zero);
            Memory::Write32(pad + kNunchukHistory + index * 12 + axis * 4, zero);
        }

        game.Call<NunchukStick>({channel, pad});
    }

    PressesAndReleases(nunchuk, kHeld, kPrevious, kPressed, kReleased);
    PressesAndReleases(nunchuk, kDirs, kDirsPrevious, kDirsPressed, kDirsReleased);
    Repeat<uint16_t>(nunchuk + kNunchukRepeat + 2, nunchuk + kNunchukRepeat, R16(nunchuk + kHeld), R16(nunchuk + kPrevious));

    game.Call<PadGameKeys>({channel & 0xFF});
}

static void PadReadGcPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t channel = frame.Arg(3);
    const uint32_t gc = kGcPad + channel * kPswSize;
    W16(gc + 0x1E, R16(gc + 0x1C));
    W16(gc + kPrevious, R16(gc + kHeld));
    W16(gc + kDirsPrevious, R16(gc + kDirs));
    W16(gc + 0x1C, uint32_t(int32_t(int8_t(game.Call<GcError>({channel})))));

    if (R16(gc + 0x1C) == 0) {
        W16(gc + kHeld, MapButtons(game.Call<GcButtons>({channel}) & 0xFFFF, kGcMap));
        game.Call<GcSticks>({channel, gc, 12});
    } else {
        W16(gc + kHeld, 0);
        W16(gc + kDirs, 0);
        W16(gc + 0x12, 0);
        W16(gc + 0x10, 0);
        W16(gc + 0x16, 0);
        W16(gc + 0x14, 0);
    }

    PressesAndReleases(gc, kHeld, kPrevious, kPressed, kReleased);
    PressesAndReleases(gc, kDirs, kDirsPrevious, kDirsPressed, kDirsReleased);
    Repeat<uint16_t>(gc + kGcRepeat + 2, gc + kGcRepeat, R16(gc + kHeld), R16(gc + kPrevious));
}

static void PadGameKeysPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t channel = frame.Arg(3) & 0xFF;
    const uint32_t pad = kPsw + channel * kPswSize;
    const uint32_t keys = pad + kKeys;
    W16(keys + kPrevious, R16(keys + kHeld));
    W16(keys + kDirsPrevious, R16(keys + kDirs));
    Memory::Write8(keys + kKeyStickPrevious, Memory::Read8(keys + kKeyStick));

    for (uint32_t offset : std::initializer_list<uint32_t>{kHeld, kDirs, kDirsPressed, kDirsReleased, 0x10, 0x12, kKeyDegrees, kKeyMagnitude})
        W16(keys + offset, 0);

    Memory::Write8(keys + kKeyStickDirs, 0);
    Memory::Write8(keys + kKeyStick, 0);
    W16(keys + kKeyRepeat, 0);
    Memory::Write8(keys + kKeyStickRepeat, 0);

    uint32_t device = 0, sources = 0;

    if (Memory::Read32(pad + kProbe) == 0) {
        switch (Memory::Read32(pad + kDevice)) {
        case 1:
            device = 1;
            sources = kNunchukKeys;
            break;
        case 2:
            device = 2;
            sources = game.Call<GameOption>({channel, 7, 1}) == 1 ? kClassicKeysA : kClassicKeysB;
            break;
        default:
            device = 3;
            sources = kRemoteKeys;
            break;
        }
    }

    Memory::Write8(keys + kKeyDevice, uint8_t(device));

    if (device == 0) {
        W16(keys + kKeyRepeat + 2, 0);
        W16(keys + kKeyStickRepeat + 1, 0);
        return;
    }

    const auto locked = [&](uint32_t bit) {
        return Memory::Read8(keys + kKeyLocks + bit) != 0;
    };

    // A key whose lock is running keeps its last state.
    for (uint32_t i = 0; i < 17; ++i) {
        const uint32_t which = R16(sources + i * 4), mask = R16(sources + i * 4 + 2);
        const uint32_t bit = R16(kKeySources + i * 4), value = R16(kKeySources + i * 4 + 2);

        if (mask == 0)
            continue;

        if (locked(bit)) {
            W16(keys + kHeld, R16(keys + kHeld) | (R16(keys + kPrevious) & (1u << bit)));
            continue;
        }

        uint32_t held = 0;

        if (device == 1)
            held = R16(pad + (which == 0 ? kNunchuk : kRemote));
        else if (device == 2)
            held = R16(pad + (which == 0 ? kRemote : kClassic));
        else
            held = R16(pad + kRemote);

        if (held & mask)
            W16(keys + kHeld, R16(keys + kHeld) | value);
    }

    const uint32_t stickSet = device == 1 ? pad + kNunchuk : device == 2 ? pad + kClassic : 0;

    if (stickSet) {
        // Both sets keep the stick's d-pad, direction, tilt and bits at the
        // same offsets.
        W16(keys + kDirs, R16(stickSet + kDirs));
        W16(keys + kKeyDegrees, R16(stickSet + 0x18));
        W16(keys + kKeyMagnitude, R16(stickSet + 0x1C));
        Memory::Write8(keys + kKeyStickDirs, Memory::Read8(stickSet + 0x20));
    }

    W16(keys + kDirs, R16(keys + kDirs) & 0x3C00);
    PressesAndReleases(keys, kDirs, kDirsPrevious, kDirsPressed, kDirsReleased);
    Memory::Write8(keys + kKeyStick, Memory::Read8(keys + kKeyStick) | (Memory::Read8(keys + kKeyStickDirs) & 0xF));
    Repeat<uint8_t>(keys + kKeyStickRepeat + 1, keys + kKeyStickRepeat, Memory::Read8(keys + kKeyStick), Memory::Read8(keys + kKeyStickPrevious));

    // In menus (set_menu_key_ad_flag) the stick works the d-pad keys while no
    // d-pad key is held.
    if (Memory::Read8(keys + kKeyStickAsDpad) == 1 && (R16(keys + kHeld) & 0xF) == 0) {
        const uint32_t stick = Memory::Read8(keys + kKeyStick) & 0xF;
        uint32_t dpad = 0;

        if (stick & 1)
            dpad |= 0x2000;

        if (stick & 2)
            dpad |= 0x1000;

        if (stick & 4)
            dpad |= 0x800;

        if (stick & 8)
            dpad |= 0x400;

        for (uint32_t i = 0; i < 17; ++i) {
            const uint32_t mask = R16(sources + i * 4 + 2);
            const uint32_t bit = R16(kKeySources + i * 4), value = R16(kKeySources + i * 4 + 2);

            if (mask != 0 && !locked(bit) && (dpad & mask))
                W16(keys + kHeld, R16(keys + kHeld) | value);
        }
    }

    Repeat<uint16_t>(keys + kKeyRepeat + 2, keys + kKeyRepeat, R16(keys + kHeld), R16(keys + kPrevious));

    // A key that changed is locked for its mode's count of ticks (the
    // original: steps); a running lock counts down.
    const uint32_t mode = Memory::Read8(keys + kKeyLockMode);

    for (uint32_t bit = 0; bit < 16; ++bit) {
        const uint32_t lock = keys + kKeyLocks + bit;

        if ((R16(keys + kPrevious) & (1u << bit)) != (R16(keys + kHeld) & (1u << bit)))
            Memory::Write8(lock, Memory::Read8(keys + kKeyLockReload + mode * 16 + bit));
        else if (const uint32_t left = Memory::Read8(lock))
            Memory::Write8(lock, uint8_t(left - std::min(left, GameStep::Ticks())));
    }

    PressesAndReleases(keys, kHeld, kPrevious, kPressed, kReleased);
    game.Call<SetKeyLocks>({channel, game.Call<GetOnlineMode>() == 1 ? 0u : 1u});
}

extern "C" void func_80042088(CpuContext* ctx);
extern "C" void MH3_PadReadWii(CpuContext* ctx) {
    PortCheck::Call(ctx, "PadReadWii", func_80042088, PadReadWiiPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80042088, MH3_PadReadWii, "PadReadWii");

extern "C" void func_800429A4(CpuContext* ctx);
extern "C" void MH3_PadReadGc(CpuContext* ctx) {
    PortCheck::Call(ctx, "PadReadGc", func_800429A4, PadReadGcPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800429A4, MH3_PadReadGc, "PadReadGc");

extern "C" void func_80044368(CpuContext* ctx);
extern "C" void MH3_PadGameKeys(CpuContext* ctx) {
    PortCheck::Call(ctx, "PadGameKeys", func_80044368, PadGameKeysPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80044368, MH3_PadGameKeys, "PadGameKeys");
