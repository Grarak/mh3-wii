// Setting a large monster up, RMHP08. C reconstructions from the disassembly:
//
//   0x801255C0 EnemySetUp(em, how): every field of the work for a new life;
//              how 1 a fresh monster, 2 and 4 its later forms. A fresh one
//              or one of how 4 takes its place from the quest's record
//              (0x80142248) and its first action from handler 0xC. Handlers 0
//              and 1 run around the fields; the effects of a first setup
//              (u8 +9) are made at the end
//   0x80141FD0 MonsterFromEntry(entry, kind, slot, id, area, form, flags, at,
//              angles, stay): a small monster's table entry (0x806A7960)
//              made a large one, or held as an entry when it is not in this
//              area or not loaded
//   0x80142CBC MonsterAdd(f1 size, kind, r4, id, level): a free work set up
//              and stepped once
#include "abi_bridge.h"
#include "guest_call.h"
#include "guest_fields.h"
#include "port_check.h"

#include <cstdint>

namespace {

using namespace GuestFields;

enum : uint32_t {
    kChar = 0x24,
    kPosition = 0x188,
    kPrevious = 0x194,
    kAngles = 0x1BC,
    kFlags = 0x1C8,
    kFade = 0x1D4,
    kArea = 0x1E1,
    kPlace = 0x1E2,
    kHealth = 0x7A0,
    kMaxHealth = 0x7A4,
    kHealthShare = 0x7A8,
};

enum : uint32_t {
    EnemyData = 0x80126098,
    Handler = 0x80126520,
    SetAction = 0x80128624,
    Vec3Set = 0x8005000C,
    Vec3Make = 0x80041EF0,
    Vec3Copy = 0x80041EA4,
    SetEffect = 0x8010C640,
    Rand = 0x800CEF20,
};

constexpr float kNone = -1.0f;

void Zero8(uint32_t em, std::initializer_list<uint32_t> fields) {
    for (const uint32_t field : fields)
        W8(em + field, 0);
}

void Zero16(uint32_t em, std::initializer_list<uint32_t> fields) {
    for (const uint32_t field : fields)
        W16(em + field, 0);
}

// The first part of a setup: what a new life starts from.
void StartLife(GuestCall& game, uint32_t em, uint32_t how) {
    W8(em + 1, 1);

    if (how == 1) {
        game.Call<0x803B9A24>({em});
        W8(em + 0x9F6, 0xFF);
        game.Call<0x80004350>({em + 0xB04, 0, 0x10});
        W8(em + 0x11, 0);
    } else
        game.Call<0x80144FFC>({em});

    game.Call<0x80135B4C>({em});
    game.Call<0x80137C84>({em});
    W8(em + 0x11, 0);
    W8(em + 0x17, 0);
    W16(em + 0x18, 0);

    if ((R32(em + kFlags) & 1) || R8(em + 0xC) == 0)
        W32(em + kFlags, R32(em + kFlags) | 8);

    WF(em + 0x1D8, 1.0f);
    WF(em + kFade, 1.0f);
    W16(em + 0x1DC, 0);
    game.Call<0x801308A8>({em});
    game.Call<0x80132108>({em});
    WF(em + 0x7C0, 1.0f);
    Zero8(em, {kPlace, 0x1E4});
    game.Call<0x8012F61C>({em, 1, 0, 0});
    game.Call<0x8012B3E4>({em, 0xFF, 0, 0});
    W8(em + 899, 0);
    Zero8(em, {0x38A, 0x38B});
    W8(em + 0x38C, 3);
    Zero8(em, {0x38D, 0x8B1, 0x8B2, 0x8B3});
    game.Call<0x8012825C>({em});
    W8(em + 0x1E0, game.Call<0x802AFCD8>());
    Zero8(em, {0x1E8, 0x1E9, 0x1EA, 0x1EB});
    game.Call<0x801282D0>({em});
    W8(em + 0xAEE, 0);
    W16(em + 0x468, 0);

    if (R8(em + 9) == 0) {
        game.Call<0x80125244>({em});
        W8(em + 0x1E, 0);
    }

    W8(em + 0x1F8, 0);

    if (const uint32_t handler = game.Call<Handler>({em, 0}))
        game(handler, {em});

    game.Call<0x80133C18>({em});
}

// The place and first action of a fresh monster, the waiting action otherwise.
void FirstAction(GuestCall& game, GuestFrame& frame, uint32_t em, uint32_t how) {
    if (how != 1 && how != 4) {
        game.Call<SetAction>({em, 0, 0, 1});
        return;
    }

    const uint32_t record = frame[56], mode = frame[9], sub = frame[8];

    if (game.Call<0x80142248>({R16(em + 0x1A), record}) == 1) {
        W8(em + kArea, R8(record + 1));
        Copy3(em + kPosition, record + 8);
        game.Call<0x800FC138>({em + kAngles, record + 0x14});
    }

    W8(em + 0x48E, R8(em + kArea));
    W8(em + 0x512, R8(em + kArea));
    W8(mode, 0);
    W8(sub, 0);

    if (const uint32_t handler = game.Call<Handler>({em, 0xC})) {
        game(handler, {em, mode, sub});
        game.Call<0x80133C24>({em});
    }

    game.Call<SetAction>({em, R8(mode), R8(sub), 1});
}

void ClearAi(GuestCall& game, uint32_t em, uint32_t how) {
    W8(em + 0x9F7, 0xFF);
    W8(em + 0x9F8, 0xFF);
    W8(em + 0x1F9, 0);
    W8(em + 0x1FA, 1);
    W8(em + 0x1FB, 0);
    game.Call<0x801299E8>({em});
    W16(em + 0x200, 0);
    W8(em + 0x1FD, 0);
    W16(em + 0x202, 900);
    W8(em + 0x79A, 0);
    game.Call<0x8012CF00>({em, 0xFFFF});
    Zero8(em, {0x79B, 0x9E4});
    game.Call<0x801255B0>({em});

    if (how != 2) {
        W8(em + 0xA04, 0);
        W8(em + 0xA05, 0xFF);
        W8(em + 0xA06, 0);
    }

    Zero8(em, {0xA07, 0x38E, 0x38F, 0x438, 0x43B, 0x43C});
    W16(em + 0x440, 0);
    Zero8(em, {0x43D, 0x43E, 0x439, 0x43A});
    W16(em + 0x442, 0);
    Zero8(em, {0x46A, 0x46F});

    for (uint32_t i = 0; i < 4; ++i) {
        W32(em + 0x390 + i * 4, 0);
        W32(em + 0x428 + i * 4, 0);
        WF(em + 0x454 + i * 4, kNone);
    }

    for (uint32_t at = 0x3A0; at <= 0x420; at += 4)
        W32(em + at, 0);

    game.Call<0x8011EEE8>({em});
    W8(em + 0x801, 0);
    W32(em + 0x888, 0xFFFFFFFFu);

    for (uint32_t i = 0; i < 3; ++i) {
        W32(em + 0x88C + i * 4, 0xFFFFFFFFu);
        W16(em + 0x898 + i * 2, 0);
    }
}

// A weighted draw from the species' list for its level; 1000 without one.
uint32_t DrawnValue8A2(GuestCall& game, uint32_t em, uint32_t list) {
    const uint32_t level = R8(em + 0x7C8);
    uint32_t entry = R32(list + 4);

    for (uint32_t bound; (bound = R8(entry)) < level; entry += 8)
        if (bound == 0xFF)
            return 1000;

    const uint32_t weights = R32(entry + 4);
    int32_t total = 0;

    for (uint32_t w = weights; S16(w) != -1; w += 4)
        total += S16(w);

    if (int16_t(total) == 0)
        return 1000;

    const int32_t draw = int32_t(game.Call<Rand>({1}) & 0xFFFF);
    int32_t left = int16_t(draw - int16_t(draw / int16_t(total)) * int16_t(total));

    for (uint32_t w = weights; S16(w) != -1; w += 4) {
        left -= S16(w);

        if (int16_t(left) < 0)
            return R16(w + 2);
    }

    return 1000;
}

void ClearDamage(GuestCall& game, uint32_t em, uint32_t data) {
    const uint32_t first = R32(data + 0x6C);
    W8(em + 0x89E, first ? R8(first) : 0);
    Zero16(em, {0x836, 0x810, 0x94A, 0x94C, 0x812, 0x814});
    Zero8(em, {0x834, 0x835});
    game.Call<0x80130B8C>({em});
    const uint32_t values = game.Call<0x801264F8>({em});

    if (game.Call<0x803B4F2C>({0}) == 1 || game.Call<0x803B5094>({0}) == 1)
        W16(em + 0x8A2, 500);
    else
        W16(em + 0x8A2, values ? DrawnValue8A2(game, em, values) : 1000);

    game.Call<0x801297F4>({em});
    Zero16(em, {0x818, 0x8C2, 0x8C4, 0x8C6, 0x81A, 0x81C, 0x81E});
    Zero8(em, {0x820, 0x821, 0x8D3});

    for (const uint32_t f : {0x80132544u, 0x80132B94u, 0x801328C0u, 0x801332C0u, 0x80132DB8u, 0x801330CCu})
        game(f, {em});

    Zero8(em, {0x8AB, 0x8AC, 0x8AD, 0x8AE, 0x8AF, 0x8B0});
    game.Call<0x801322E8>({em});
    game.Call<0x80132364>({em});
    Zero16(em, {0x950, 0x952});
    W8(em + 0x918, 0);
    W16(em + 0x916, 0);
    Zero8(em, {0x919, 0x91A, 0x91B});
}

void ClearAttachments(GuestCall& game, GuestFrame& frame, uint32_t em) {
    for (uint32_t i = 0; i < 2; ++i) {
        const uint32_t at = em + 0xA08 + i * 0x5C;
        game.Call<0x80129788>({em, i});
        game.Call<0x8029F0A0>({at, 1, em});
        game.Call<0x8029F0AC>({at, 1, em});
    }

    W8(em + 0xAE9, 0);
    W16(em + 0xAEA, 0);
    game.Call<0x8029F1D4>({em + 0xAC0, em + 0xAE8});
    game.Call<0x802ABD8C>({em});
    game.Call<0x80136E7C>({em});
    const uint32_t node = frame[16], word = frame[12];
    W32(word, game.Call<0x80097E48>({em + 0x138, frame.Arg(13) - 0x7438}));
    game.Call<0x8005D130>({node, word});
    W32(em + 0x204, R32(node) ? game.Call<0x8005D0B4>({node}) : 0xFFFFFFFFu);
    game.Call<0x80133BC0>({em});
    W8(em + 0x761, 0);
    game.Call<0x8012FCC4>({em});
    W8(em + 0x766, 0);
    W16(em + 0x768, 0);

    for (uint32_t k = 0; k < 12; k += 4) {
        WF(em + 0x76C + k, 0.0f);
        WF(em + 0x778 + k, 0.0f);
    }

    WF(em + kHealthShare, 1.0f);
    WF(em + 0x7B0, kNone);

    for (uint32_t i = 0; i < 2; ++i) {
        uint32_t list = game.Call<0x8013A894>({em, i});

        if (!list)
            continue;

        game.Call<0x80135BC0>({em + 0x488 + i * 0x84, R8(list)});

        for (uint32_t k = 0; k < 3; ++k, list += 4)
            if (R32(list + 0x14) != 0xFFFFFFFFu)
                game.Call<0x800E31C8>({em + kChar, R32(list + 0x14), R8(em + 2) + 8});
    }
}

void StartHealth(GuestCall& game, uint32_t em, uint32_t how) {
    if (const uint32_t handler = game.Call<Handler>({em, 1}))
        game(handler, {em, how});

    if (how != 1 && R32(em + kHealth) != 0xFFFFFFFFu)
        return;

    if (RF(em + kHealthShare) > 1.0f)
        WF(em + kHealthShare, 1.0f);

    const int32_t health = int32_t(float(int32_t(R32(em + kMaxHealth))) * RF(em + kHealthShare));
    W32(em + 0x7AC, uint32_t(health));
    W32(em + kHealth, uint32_t(health));

    if (how == 1)
        W8(em + 0x14, 0);
}

void SetGround(GuestCall& game, uint32_t em) {
    game.Call<0x80138EC8>({em});
    game.Call<0x8013046C>({em});
    WF(em + 0x1AC, 0.0f);
    Copy3(em + 0x1B0, em + kPosition);
    Copy3(em + kPrevious, em + kPosition);
    const uint32_t y = R32(em + kPosition + 4);
    W32(em + 0x20C, y);
    W32(em + 0x210, y);
    W32(em + 0x214, y);
    W32(em + 0x208, 0xFFFFFFFFu);
    W32(em + 0x218, 0);
    Zero8(em, {0x23C, 0x23D});
    WF(em + 0x240, 0.0f);
}

// The first setup's shadow, effects and sound owner.
void FirstEffects(GuestCall& game, GuestFrame& frame, uint32_t em, uint32_t data) {
    game.Call<0x800FAD54>({em});
    game.Call<0x80133008>({em});
    const uint32_t info = R32(data + 0x9C), shadow = R32(info + 0x14), at = frame[32];

    if (shadow) {
        game.Call<Vec3Make>({frame[20]}, {RF(shadow + 4), RF(shadow + 8), RF(shadow + 0xC)});
        game.Call<Vec3Copy>({at, frame[20]});
        game.Call<SetEffect>({em, R32(shadow), at, 3}, {RF(shadow + 0x10)});
    } else {
        game.Call<Vec3Make>({at}, {0.0, 100.0, 0.0});
        game.Call<SetEffect>({em, 2, at, 3}, {2.0});
    }

    if ((R32(em + kFlags) & 1) && R32(info + 0x18) != 0)
        game.Call<0x8031E5E0>({em});

    W32(em + 0xB14, game.Call<0x800D9868>({4, em, game.Call<Handler>({em, 7})}));
    W8(em + 9, 1);
}

void SetUp(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x90);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3), how = frame.Arg(4) & 0xFF;
    const uint32_t data = game.Call<EnemyData>({em});
    game.Call<0x8005D210>({frame[16], 0});
    game.Call<0x80125FB8>({frame[56], 0});
    StartLife(game, em, how);
    FirstAction(game, frame, em, how);
    W32(em + 0x1CC, R32(data + 8));
    ClearAi(game, em, how);
    ClearDamage(game, em, data);
    ClearAttachments(game, frame, em);
    StartHealth(game, em, how);
    SetGround(game, em);

    if (R8(em + 9) == 0)
        FirstEffects(game, frame, em, data);

    game.Call<0x8012A418>({em});

    if (!(R32(em + kFlags) & 1))
        game.Call<0x80136E9C>({em, 1});
}

void FromEntry(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const uint32_t entry = frame.Arg(3), slot = frame.Arg(5) & 0xFF, id = frame.Arg(6) & 0xFFFF, area = frame.Arg(7) & 0xFF;
    const uint32_t form = frame.Arg(8) & 0xFF, flags = frame.Arg(9), at = frame.Arg(10);
    const uint32_t caller = frame.Arg(1), angles = R32(caller + 8), stay = R8(caller + 0xF);

    if (area != 0xFF)
        W8(entry + 10, area);

    W8(entry + 3, frame.Arg(4));

    if (slot != 0xFF)
        W8(entry + 9, slot);

    W8(entry + 8, 0);

    if (R32(entry + 0x30) == 0 && (R8(entry + 10) != (game.Call<0x802AFCE8>() & 0xFF) || (game.Call<0x80141538>({R8(entry + 2)}) & 0xFF) == 0xFF || stay != 0)) {
        W16(entry + 0x18, id);
        W8(entry + 0xE, 2);

        if (const uint32_t handler = game.Call<0x8012665C>({entry, 0x1A}))
            game(handler, {entry, at, angles, flags & 0xFF});

        return;
    }

    if (const uint32_t em = game.Call<0x80143010>({entry})) {
        W16(em + 0x1C, id);
        W8(em + 0xE, 2);

        if (flags & 2)
            game.Call<Vec3Copy>({em + kPosition, at});

        if (flags & 4)
            game.Call<0x800FC138>({em + kAngles, angles});

        W8(em + 0xA04, form != 0xFF);
        W8(em + 0xA05, form);
        W8(em + 0xA06, form != 0xFF);
        game.Call<0x80137980>({em});

        if (!(flags & 0x10) && (R32(em + kFlags) & 1))
            game.Call<0x802E8920>({em, R8(em + kArea)});
    }

    game.Call<0x80143AA4>({entry});
}

uint32_t Add(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const float size = float(ctx->fpr[1].d);
    const uint32_t em = game.Call<0x80142BD8>({frame.Arg(3) & 0xFF});

    if (!em)
        return 0;

    game.Call<0x801425F4>({em, frame.Arg(4)});
    W16(em + 0x1A, frame.Arg(5));
    W8(em + 0x7C8, frame.Arg(6));
    WF(em + 0x1D0, size);
    W8(em + 0xE, 4);
    game.Call<0x80137980>({em});
    return em;
}

} // namespace

extern "C" void func_801255C0(CpuContext* ctx);
extern "C" void MH3_EnemySetUp(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemySetUp", func_801255C0, SetUp);
}
REGISTER_NATIVE_FUNCTION_AS(0x801255C0, MH3_EnemySetUp, "EnemySetUp");

extern "C" void func_80141FD0(CpuContext* ctx);
extern "C" void MH3_MonsterFromEntry(CpuContext* ctx) {
    PortCheck::Call(ctx, "MonsterFromEntry", func_80141FD0, FromEntry);
}
REGISTER_NATIVE_FUNCTION_AS(0x80141FD0, MH3_MonsterFromEntry, "MonsterFromEntry");

extern "C" void func_80142CBC(CpuContext* ctx);
extern "C" void MH3_MonsterAdd(CpuContext* ctx) {
    PortCheck::Call(ctx, "MonsterAdd", func_80142CBC, [](CpuContext* c) {
        const uint32_t em = Add(c);
        c->gpr[3] = em;
    });
}
REGISTER_NATIVE_FUNCTION_AS(0x80142CBC, MH3_MonsterAdd, "MonsterAdd");
