// Cheats in the save data's functions, RMHP08: items, money, the hunter rank,
// the shops, the smithy and the boxes. The Gecko codes patched instructions
// of these functions (US address, then EU); here each is a wrapper around the
// translated function that changes its arguments, results or the fields it
// writes, or a port of a small one with the code's change in it.
#include "cheats.h"

#include "abi_bridge.h"
#include "guest_call.h"
#include "ppc_runtime.h"

#include <algorithm>
#include <cstdint>

namespace {

using Cheats::Id;
using Cheats::On;
using Cheats::Value;

// The save data (userdata) as these functions find it (r13 - 0x45A0).
constexpr uint32_t kUserDataPointer = 0x80796D40;

uint32_t UserData() {
    return Memory::Read32(kUserDataPointer);
}

// Save data fields.
enum : uint32_t {
    kMoney = 0x18,          // s32
    kItemBox = 0x180,       // (u16 item, s16 count) pairs
    kTalismans = 0xE00,     // 12 bytes each, byte 0 the kind (0: free)
    kHunterPoints = 0x3DE0, // u32
    kHunterRank = 0x3DE4,   // u16
    kResources = 0x3F04,    // s32
    kStarQuests = 0x47DA,   // u16 bits
    kClearedQuests = 0x512C, // u32 bits
};

constexpr int32_t kMaxMoney = 9999999;
constexpr uint32_t kTalismanSlots = 800; // 8 pages of 100

constexpr uint32_t GetItemData = 0x8029F740; // GetItemData__FUs (item): +3 u8 stack size, +0xC u32 price

} // namespace

// 0x8004BAA0 (item, s16 change, slots, slot count, notify, max): adds to an
// item's stack among (u16 item, s16 count) slots, or starts one in a free
// slot when the change is positive; max 0 is the item's stack size. Into a
// stack it has (when the change is negative or the stack is below the
// maximum):
//   Infinite bombs in pouch (US 0x8004BB64, EU 0x8004BBC8: the new count's
//     store dropped) keeps the count;
//   Gain/use items get 10 (0x8004BB60/BBC4: the count + change replaced by
//     10), and its button-switched copy, set it to 10;
//   Sell/use item get 10 (C2 at 0x8004BB64/BBC8) sets it to 10 when it goes
//     down;
// a new stack (Buy new item get 10, C2 at 0x8004BAF0/BB54) starts at 10.
extern "C" void func_8004BAA0(CpuContext* ctx);

extern "C" void MH3_CheatItemAdd(CpuContext* ctx) {
    const bool get10 = On(Id::ItemsGet10) || Cheats::Toggled(Id::ItemsGet10Toggle);
    const bool any = get10 || On(Id::InfiniteBombsInPouch) || On(Id::SellUseGet10) || On(Id::BuyNewGet10);

    if (!any) {
        func_8004BAA0(ctx);
        return;
    }

    const uint16_t item = uint16_t(ctx->gpr[3]);
    const int32_t change = int16_t(ctx->gpr[4]);
    const uint32_t slots = ctx->gpr[5];
    const int32_t count = int32_t(ctx->gpr[6]);
    uint32_t stack = 0;

    for (int32_t i = 0; i < count && stack == 0; ++i)
        if (Memory::Read16(slots + 4 * i) == item)
            stack = slots + 4 * i;

    int32_t newChange = change;

    if (stack != 0) {
        uint32_t max = uint16_t(ctx->gpr[8]);

        if (max == 0) {
            GuestFrame frame(ctx, 0x10);
            const uint32_t data = GuestCall{ctx}.Call<GetItemData>({item});
            max = data != 0 ? Memory::Read8(data + 3) : 0;
        }

        const int32_t have = int16_t(Memory::Read16(stack + 2));

        // The original's branch that adds (else the stack is full and stays at
        // its maximum).
        if (change < 1 || have < int32_t(max)) {
            int32_t total = have + change;

            if (get10)
                total = 10;

            if (On(Id::SellUseGet10)) {
                if (change < 0)
                    total = 10;
            } else if (On(Id::InfiniteBombsInPouch)) {
                total = have;
            }

            newChange = total - have;
        }
    } else if (change > 0 && On(Id::BuyNewGet10)) {
        newChange = 10;
    }

    ctx->gpr[4] = uint32_t(int32_t(int16_t(newChange)));
    func_8004BAA0(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x8004BAA0, MH3_CheatItemAdd, "CheatItemAdd");

// 0x80272E94 (player, item, change): the player uses (-1) an item of the
// pouch. Infinite items (Bully: li r5,-1 made li r5,0 before six calls, US
// 0x802490B4 ..., EU 0x80249118 ...: drinking, eating, sharpening, and so on)
// and Doudley's Inf ammo (C2 at 0x80287DF0/0x80287E54, the bowgun's shot)
// use none.
extern "C" void func_80272E94(CpuContext* ctx);

extern "C" void MH3_CheatPlayerItemUse(CpuContext* ctx) {
    static constexpr uint32_t kItemUses[]{0x80249120, 0x8024B9D8, 0x8024C858, 0x8024D6E0, 0x8024F210, 0x8024F5FC};
    const uint32_t from = ctx->lr;
    const bool item = On(Id::InfiniteItems) && std::find(std::begin(kItemUses), std::end(kItemUses), from) != std::end(kItemUses);
    const bool ammo = On(Id::DoudleyInfiniteAmmo) && from == 0x80287E5C;

    if ((item || ammo) && int32_t(ctx->gpr[5]) < 0)
        ctx->gpr[5] = 0;

    func_80272E94(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x80272E94, MH3_CheatPlayerItemUse, "CheatPlayerItemUse");

// 0x8004AE6C (save data): the box's pages, 4, or 6 with flag 0x244, and 2
// more with flag 0x245 (0x8004D38C (save data, flag): 1 when set). Item/equip
// boxes have 8 pages (US 0x8004AE20, EU 0x8004AE84) starts at 8.
extern "C" void MH3_BoxPages(CpuContext* ctx) {
    uint32_t pages = 0;

    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        const uint32_t save = frame.Arg(3);
        pages = On(Id::Boxes8Pages) ? 8 : 4;

        if (game.Call<0x8004D38C>({save, 0x244}) == 1)
            pages = 6;

        if (game.Call<0x8004D38C>({save, 0x245}) == 1)
            pages += 2;
    }

    ctx->gpr[3] = pages;
}

REGISTER_NATIVE_FUNCTION_AS(0x8004AE6C, MH3_BoxPages, "BoxPages");

// 0x8004B770 (item, slots, slot count): how many of the item the slots hold,
// at most 999 (the smithy's and the combinations' required materials). No
// need to have required materials (US 0x8004B760/B76C, EU 0x8004B7C4/B7D0:
// every slot matches, with 9999) gives 999 for any item there is data for.
extern "C" void func_8004B770(CpuContext* ctx);

extern "C" void MH3_CheatMaterialCount(CpuContext* ctx) {
    const uint16_t item = uint16_t(ctx->gpr[3]);
    const int32_t count = int32_t(ctx->gpr[5]);

    if (!On(Id::NoRequiredMaterials) || item == 0 || count < 1) {
        func_8004B770(ctx);
        return;
    }

    uint32_t data = 0;

    {
        GuestFrame frame(ctx, 0x10);
        data = GuestCall{ctx}.Call<GetItemData>({item});
    }

    ctx->gpr[3] = data != 0 ? 999 : 0;
}

REGISTER_NATIVE_FUNCTION_AS(0x8004B770, MH3_CheatMaterialCount, "CheatMaterialCount");

// 0x8004C620 (save data, talisman): stores a talisman in the box's first free
// slot (0x8004AED4: the box's slots, 100 a page; 0x8004A270 copies it in,
// 0x8004C45C sorts the slot in); 0xFFFF when the box is full, else the slot.
// Talismans sent to box start at slot (US 0x8004C5E4, EU 0x8004C648) starts
// the search there (here no further than the save data's 800 slots).
extern "C" void MH3_TalismanToBox(CpuContext* ctx) {
    uint32_t slot = 0xFFFF;

    {
        GuestFrame frame(ctx, 0x20);
        GuestCall game{ctx};
        const uint32_t save = frame.Arg(3), talisman = frame.Arg(4);
        const uint32_t slots = game.Call<0x8004AED4>({save}) & 0xFFFF;
        const uint32_t first = On(Id::TalismanStartSlot) ? uint32_t(Value(Id::TalismanStartSlot)) : 0;

        for (uint32_t i = 0; i < slots && first + i < kTalismanSlots; ++i) {
            const uint32_t at = save + kTalismans + (first + i) * 12;

            if (Memory::Read8(at) != 0)
                continue;

            game.Call<0x8004A270>({at, talisman});
            game.Call<0x8004C45C>({at});
            slot = first + i;
            break;
        }
    }

    ctx->gpr[3] = slot;
}

REGISTER_NATIVE_FUNCTION_AS(0x8004C620, MH3_TalismanToBox, "TalismanToBox");

// 0x8004CD34 (save data, equipment): 1 when the hunter can wear it. Kinds 0xC
// and 0xD need +0x20 == 11; armor (0x8027E3A8: its data) needs the classes
// of 0x8004CCFC among its +6 bits. Equip blade/gunner armor (US 0x8004CD44,
// EU 0x8004CDA8) drops the class check.
extern "C" void MH3_CanWearArmor(CpuContext* ctx) {
    uint32_t wear = 0;

    {
        GuestFrame frame(ctx, 0x20);
        GuestCall game{ctx};
        const uint32_t save = frame.Arg(3), equipment = frame.Arg(4);
        const uint32_t classes = game.Call<0x8004CCFC>({save, equipment}) & 0xFF;
        const uint32_t kind = Memory::Read8(equipment);

        if (kind - 0xC < 2) {
            wear = Memory::Read8(save + 0x20) == 11;
        } else {
            const uint32_t data = game.Call<0x8027E3A8>({equipment});
            wear = data != 0 && (On(Id::BothArmorTypes) || (classes & Memory::Read8(data + 6)) == classes);
        }
    }

    ctx->gpr[3] = wear;
}

REGISTER_NATIVE_FUNCTION_AS(0x8004CD34, MH3_CanWearArmor, "CanWearArmor");

// 0x80315AC4 (menu, &equipment, flags): why the equipment menu refuses a
// piece, 6 and 7 for armor of the other weapon class. Equip blade/gunner
// armor (US 0x80315B5C/B74, EU 0x80315BC0/BD8) refuses neither.
extern "C" void func_80315AC4(CpuContext* ctx);

extern "C" void MH3_CheatEquipRefusal(CpuContext* ctx) {
    func_80315AC4(ctx);

    if (On(Id::BothArmorTypes) && (ctx->gpr[3] == 6 || ctx->gpr[3] == 7))
        ctx->gpr[3] = 0;
}

REGISTER_NATIVE_FUNCTION_AS(0x80315AC4, MH3_CheatEquipRefusal, "CheatEquipRefusal");

// Money and resources.
namespace {

// No need for money (US 0x801E47D0 ..., EU 0x801E4834 ...: six shop and
// smithy checks read the money as 0x7FFF0000): during those functions the
// money reads 0x7FFF0000, and changes made meanwhile apply to the real amount.
int32_t g_realMoney;
int g_lifted;

class LiftMoney {
  public:
    LiftMoney() : on_(On(Id::NoNeedForMoney) && UserData() != 0) {
        if (!on_)
            return;

        if (g_lifted++ == 0) {
            g_realMoney = int32_t(Memory::Read32(UserData() + kMoney));
            Memory::Write32(UserData() + kMoney, 0x7FFF0000);
        }
    }

    ~LiftMoney() {
        if (on_ && --g_lifted == 0)
            Memory::Write32(UserData() + kMoney, uint32_t(g_realMoney));
    }

    LiftMoney(const LiftMoney&) = delete;
    LiftMoney& operator=(const LiftMoney&) = delete;

  private:
    bool on_;
};

} // namespace

extern "C" void func_801E46B4(CpuContext* ctx);

extern "C" void MH3_CheatShopListMoney(CpuContext* ctx) {
    LiftMoney lift;
    func_801E46B4(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x801E46B4, MH3_CheatShopListMoney, "CheatShopListMoney");

extern "C" void func_80210990(CpuContext* ctx);

extern "C" void MH3_CheatItemShopMoney(CpuContext* ctx) {
    LiftMoney lift;
    func_80210990(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x80210990, MH3_CheatItemShopMoney, "CheatItemShopMoney");

extern "C" void func_80213D04(CpuContext* ctx);

extern "C" void MH3_CheatSmithyMoney(CpuContext* ctx) {
    LiftMoney lift;
    func_80213D04(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x80213D04, MH3_CheatSmithyMoney, "CheatSmithyMoney");

extern "C" void func_802138D4(CpuContext* ctx);

extern "C" void MH3_CheatSmithyListMoney(CpuContext* ctx) {
    LiftMoney lift;
    func_802138D4(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x802138D4, MH3_CheatSmithyListMoney, "CheatSmithyListMoney");

extern "C" void func_802140DC(CpuContext* ctx);

extern "C" void MH3_CheatSmithyUpgradeMoney(CpuContext* ctx) {
    LiftMoney lift;
    func_802140DC(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x802140DC, MH3_CheatSmithyUpgradeMoney, "CheatSmithyUpgradeMoney");

extern "C" void func_803214C0(CpuContext* ctx);

extern "C" void MH3_CheatQuestFeeMoney(CpuContext* ctx) {
    LiftMoney lift;
    func_803214C0(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x803214C0, MH3_CheatQuestFeeMoney, "CheatQuestFeeMoney");

// 0x8004D14C (s32 change, &money): money += change, clamped to 0..9,999,999.
// Gain/use money > max money (US 0x8004D100, EU 0x8004D164: the clamp's
// branch to its store) sets it to 9,999,999 on any change; Dont lose money
// buying items (C2 at 0x8004D0F0/D154) keeps it when it would go down.
extern "C" void MH3_MoneyAdd(CpuContext* ctx) {
    int32_t change = int32_t(ctx->gpr[3]);
    const uint32_t money = ctx->gpr[4];
    const bool lifted = g_lifted > 0 && money == UserData() + kMoney;
    int32_t total = lifted ? g_realMoney : int32_t(Memory::Read32(money));

    if (On(Id::DontLoseMoney) && change < 0)
        change = 0;

    total += change;

    if (total > kMaxMoney || On(Id::MoneyChangeMax))
        total = kMaxMoney;
    else if (total < 0)
        total = 0;

    if (lifted)
        g_realMoney = total;
    else
        Memory::Write32(money, uint32_t(total));
}

REGISTER_NATIVE_FUNCTION_AS(0x8004D14C, MH3_MoneyAdd, "MoneyAdd");

// 0x8004D398 (s32 change): the resources += change, clamped likewise. Max
// res on converting (US 0x8004D358, EU 0x8004D3BC) sets them to 9,999,999.
extern "C" void MH3_ResourcesAdd(CpuContext* ctx) {
    const uint32_t resources = UserData() + kResources;
    int32_t total = int32_t(Memory::Read32(resources)) + int32_t(ctx->gpr[3]);

    if (total > kMaxMoney || On(Id::MaxResourcesOnConverting))
        total = kMaxMoney;
    else if (total < 0)
        total = 0;

    Memory::Write32(resources, uint32_t(total));
}

REGISTER_NATIVE_FUNCTION_AS(0x8004D398, MH3_ResourcesAdd, "ResourcesAdd");

// 0x802DF748 and 0x803090FC: the money (and in the second the resources)
// printed for menus. Infinite funds (C2 at US 0x802DF714/0x8030942C, EU
// 0x802DF778/0x80309490) first set the money to 999,999 and 99,999,999;
// Infinite ressources (C2 at 0x80309354/0x803093B8) the resources to
// 99,999,999.
extern "C" void func_802DF748(CpuContext* ctx);
extern "C" void func_803090FC(CpuContext* ctx);

extern "C" void MH3_CheatMoneyPanel(CpuContext* ctx) {
    if (On(Id::InfiniteFunds) && UserData() != 0)
        Memory::Write32(UserData() + kMoney, 999999);

    func_802DF748(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x802DF748, MH3_CheatMoneyPanel, "CheatMoneyPanel");

extern "C" void MH3_CheatFundsPanel(CpuContext* ctx) {
    if (UserData() != 0) {
        if (On(Id::InfiniteFunds))
            Memory::Write32(UserData() + kMoney, 99999999);

        if (On(Id::InfiniteResources))
            Memory::Write32(UserData() + kResources, 99999999);
    }

    func_803090FC(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x803090FC, MH3_CheatFundsPanel, "CheatFundsPanel");

// Quests and the hunter rank.

// 0x8004D2E0 (star quest): its bit at +0x47DA. Unlock all star quests (C2 at
// US 0x8004D29C, EU 0x8004D300) sets the bits' whole halfword first.
extern "C" void MH3_StarQuestUnlocked(CpuContext* ctx) {
    const int32_t quest = int32_t(ctx->gpr[3]);
    const uint32_t word = UserData() + kStarQuests + 2 * uint32_t(quest / 16);

    if (On(Id::AllStarQuests))
        Memory::Write16(word, 0xFFFF);

    ctx->gpr[3] = (Memory::Read16(word) >> (quest & 15)) & 1;
}

REGISTER_NATIVE_FUNCTION_AS(0x8004D2E0, MH3_StarQuestUnlocked, "StarQuestUnlocked");

// 0x8004D770 (quest): 1 when cleared, its bit (0x8004D660: the quest's index,
// 0xFFFF none) at +0x512C. All quests completed (C2 at US 0x8004D748, EU
// 0x8004D7AC) sets the bits' whole word first.
extern "C" void MH3_QuestCleared(CpuContext* ctx) {
    uint32_t cleared = 0;

    {
        GuestFrame frame(ctx, 0x10);
        const uint32_t index = GuestCall{ctx}.Call<0x8004D660>({frame.Arg(3)}) & 0xFFFF;

        if (index != 0xFFFF) {
            const uint32_t word = UserData() + kClearedQuests + 4 * (index >> 5);

            if (On(Id::AllQuestsCompleted))
                Memory::Write32(word, 0xFFFFFFFF);

            cleared = (Memory::Read32(word) >> (index & 31)) & 1;
        }
    }

    ctx->gpr[3] = cleared;
}

REGISTER_NATIVE_FUNCTION_AS(0x8004D770, MH3_QuestCleared, "QuestCleared");

// 0x8004DDD8: the rank class, 0 at HR 0, 1 below 31, else 2. Armor is
// upgradeable to high rank even at low rank (US 0x8004DD78, EU 0x8004DDDC)
// reads the rank as 999.
extern "C" void MH3_RankClass(CpuContext* ctx) {
    const uint32_t rank = On(Id::HighRankArmorUpgrades) ? 999 : Memory::Read16(UserData() + kHunterRank);
    ctx->gpr[3] = rank == 0 ? 0 : rank < 31 ? 1 : 2;
}

REGISTER_NATIVE_FUNCTION_AS(0x8004DDD8, MH3_RankClass, "RankClass");

// 0x8004DF74 (&rank, &points, &next): the hunter rank, its points and the
// points the next rank needs (0x8004DE38 (rank): the points a rank starts
// at), as the status screen and the quest counter show them; HR 0 reads 1.
//   Hunter ranking modifier (C2 at US 0x8004DF68, EU 0x8004DFCC) first sets
//     the rank and its points;
//   Always display HR infos on status (0x8004DF44/DFA8) reads HR 0 like any
//     other;
//   Join/host any quest (0x8004DF84/DFE8) reads every hunter as HR 999.
extern "C" void MH3_HunterRankStatus(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t rank = frame.Arg(3), points = frame.Arg(4), next = frame.Arg(5);
    const uint32_t save = UserData();

    if (On(Id::HunterRankModifier)) {
        const uint32_t set = uint32_t(Value(Id::HunterRankModifier));
        Memory::Write16(save + kHunterRank, uint16_t(set));
        Memory::Write32(save + kHunterPoints, game.Call<0x8004DE38>({set}));
    }

    const uint32_t have = Memory::Read32(save + kHunterPoints);

    if (Memory::Read16(save + kHunterRank) == 0 && !On(Id::AlwaysShowHrInfo)) {
        Memory::Write16(rank, 1);
        Memory::Write32(points, have);
        Memory::Write32(next, 0);
        return;
    }

    Memory::Write32(points, have);

    if (have >= game.Call<0x8004DE38>({999}) || On(Id::JoinAnyQuest)) {
        Memory::Write16(rank, 999);
        Memory::Write32(next, 0);
        return;
    }

    for (uint32_t r = 0; r < 1000; ++r) {
        const uint32_t start = game.Call<0x8004DE38>({r});

        if (have < start) {
            Memory::Write16(rank, uint16_t(r - 1));
            Memory::Write32(next, start);
            return;
        }
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x8004DF74, MH3_HunterRankStatus, "HunterRankStatus");

// 0x8004E228 (save data, s32 change): adds hunter rank points and moves the
// rank up through them, stopping at the urgent quests' ranks. No HRP
// gain/loss (US 0x8004E21C/E2B0, EU 0x8004E280/E314: the add and the rank's
// store dropped) keeps both.
extern "C" void func_8004E228(CpuContext* ctx);

extern "C" void MH3_CheatHunterPointsAdd(CpuContext* ctx) {
    if (!On(Id::NoHrpChange)) {
        func_8004E228(ctx);
        return;
    }

    const uint32_t save = ctx->gpr[3];
    const uint16_t rank = Memory::Read16(save + kHunterRank);
    ctx->gpr[4] = 0;
    func_8004E228(ctx);
    Memory::Write16(save + kHunterRank, rank);
}

REGISTER_NATIVE_FUNCTION_AS(0x8004E228, MH3_CheatHunterPointsAdd, "CheatHunterPointsAdd");

// 0x80213548 (save data, condition): whether a shop weapon's condition holds,
// one case HR 1 or more. Purchase any weapon with HR0 (US 0x8021352C, EU
// 0x80213590) passes that case at HR 0: the rank reads 1 meanwhile.
extern "C" void func_80213548(CpuContext* ctx);

extern "C" void MH3_CheatWeaponCondition(CpuContext* ctx) {
    const uint32_t rank = ctx->gpr[3] + kHunterRank;
    const bool lift = On(Id::BuyAnyWeaponAtHr0) && Memory::Read16(rank) == 0;

    if (lift)
        Memory::Write16(rank, 1);

    func_80213548(ctx);

    if (lift)
        Memory::Write16(rank, 0);
}

REGISTER_NATIVE_FUNCTION_AS(0x80213548, MH3_CheatWeaponCondition, "CheatWeaponCondition");

// Shops, the smithy and the boxes.

// 0x80210854 (item, ...): an item's store price (0x8029F740's +0xC). All
// items cost 1z in item stores (C2 at US 0x80210818, EU 0x8021087C) first
// sets the item data's price to 0.
extern "C" void func_80210854(CpuContext* ctx);

extern "C" void MH3_CheatItemPrice(CpuContext* ctx) {
    if (On(Id::ItemsCost1z)) {
        GuestFrame frame(ctx, 0x10);
        const uint32_t data = GuestCall{ctx}.Call<GetItemData>({frame.Arg(3) & 0xFFFF});

        if (data != 0)
            Memory::Write32(data + 0xC, 0);
    }

    func_80210854(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x80210854, MH3_CheatItemPrice, "CheatItemPrice");

// 0x80216ACC (materials, count, ...): draws the smithy's required materials,
// (u16 item, s16 count) pairs. Smithy forge/upgrade with 1 item (C2 at US
// 0x80216B3C, EU 0x80216BA0) sets each count to 0 first; Set smithy req
// materials to (C2 at 0x80216AE8/0x80216B4C) each item.
extern "C" void func_80216ACC(CpuContext* ctx);

extern "C" void MH3_CheatSmithyMaterials(CpuContext* ctx) {
    const uint32_t materials = ctx->gpr[3];
    const int32_t count = int8_t(ctx->gpr[4]);

    for (int32_t i = 0; i < count; ++i) {
        const uint32_t at = materials + 4 * i;

        if (On(Id::SmithyMaterial))
            Memory::Write16(at, uint16_t(Value(Id::SmithyMaterial)));

        if (On(Id::SmithyOneItem) && Memory::Read16(at) != 0)
            Memory::Write16(at + 2, 0);
    }

    func_80216ACC(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x80216ACC, MH3_CheatSmithyMaterials, "CheatSmithyMaterials");

// 0x802A7588 (..., slot, ...): draws an item list's slot (u16 item, s16
// count). Item pouch items x10 (C2 at US 0x802A7704, EU 0x802A7768) sets the
// count to 10 first.
extern "C" void func_802A7588(CpuContext* ctx);

extern "C" void MH3_CheatItemSlotDraw(CpuContext* ctx) {
    const uint32_t slot = ctx->gpr[4];

    if (On(Id::PouchItemsX10) && Memory::Read16(slot) != 0)
        Memory::Write16(slot + 2, 10);

    func_802A7588(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x802A7588, MH3_CheatItemSlotDraw, "CheatItemSlotDraw");

// 0x801EDA48 (index): an item box slot's count for the box menu, at most what
// the item may stack to. All item box items x99 (C2 at US 0x801EDA0C, EU
// 0x801EDA70) sets the count to 99 first.
extern "C" void func_801EDA48(CpuContext* ctx);

extern "C" void MH3_CheatBoxSlotCount(CpuContext* ctx) {
    const uint32_t slot = UserData() + kItemBox + 4 * ctx->gpr[3];

    if (On(Id::BoxItemsX99) && Memory::Read16(slot) != 0)
        Memory::Write16(slot + 2, 99);

    func_801EDA48(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x801EDA48, MH3_CheatBoxSlotCount, "CheatBoxSlotCount");

// All boxes have 8 pages (C2s at US 0x801F5A0C/5A24 and 0x801F7718, EU
// 0x801F5A70/5A88 and 0x801F777C): the box menus' page counts, +0x1B or
// +0x17 of 0x801F5A1C's box (by flag bit 0), and +3 of the box 0x801F6E20
// passes to 0x801F5E50 (returning to 0x801F77A0, the box in r15), become 8.
extern "C" void func_801F5A1C(CpuContext* ctx);
extern "C" void func_801F5E50(CpuContext* ctx);

extern "C" void MH3_CheatBoxPages(CpuContext* ctx) {
    if (On(Id::AllBoxes8Pages))
        Memory::Write8(ctx->gpr[3] + ((ctx->gpr[4] & 1) != 0 ? 0x1B : 0x17), 8);

    func_801F5A1C(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x801F5A1C, MH3_CheatBoxPages, "CheatBoxPages");

extern "C" void MH3_CheatBoxPageDraw(CpuContext* ctx) {
    if (On(Id::AllBoxes8Pages) && ctx->lr == 0x801F77A0) {
        Memory::Write8(ctx->gpr[15] + 3, 8);
        ctx->gpr[5] = 8;
    }

    func_801F5E50(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x801F5E50, MH3_CheatBoxPageDraw, "CheatBoxPageDraw");

// 0x80214754 (entry, source, a, b): builds a bed menu entry, a at +2. Always
// able to sleep (C2 at US 0x80214750, EU 0x802147B4) makes a 0.
extern "C" void func_80214754(CpuContext* ctx);

extern "C" void MH3_CheatBedEntry(CpuContext* ctx) {
    if (On(Id::AlwaysAbleToSleep))
        ctx->gpr[5] = 0;

    func_80214754(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x80214754, MH3_CheatBedEntry, "CheatBedEntry");

namespace Cheats {

// Language changer: the US code hooked the title menu to step the language
// (system_w+9: 1 English, 2 French, 3 German, 4 Italian, 5 Spanish) with Y;
// here it is set from the start.
void StepItems() {
    if (On(Id::LanguageChanger))
        Memory::Write8(kSystem + 9, uint8_t(Value(Id::LanguageChanger)));
}

} // namespace Cheats
