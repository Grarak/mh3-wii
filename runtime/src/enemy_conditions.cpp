// Small per-monster lookups, RMHP08. C reconstructions from the disassembly:
//
//   0x8012FA28 EnemyPartEnabled(em, id): 0 when a condition of the monster
//              data's list (+0x90, 16-byte entries {s32 id (-1 ends), u16
//              kind, u16 want, s32 a, s32 b}) for `id` fails, else 1. Kinds:
//              0 u8 +0x762 set, 1 0x801357AC(em, a) == 1, 3 u8 +0x1E2 == 2
//              or in water (0x80130168), 4 u8 +0xA == a, 5 all of a's bits
//              in u8 +0xA, 6 the part's damage level (0x8011EA40) >= b, 7
//              u8 +0x761 & a; `want` 1 needs it true, 0 false, others
//              ignore it
//   0x8012F9C4 EnemyNodePart(em, id): the part (u8 +4) of the monster data's
//              list (+0x8C, 8-byte entries {s32 id (-1 ends), u8 part}) entry
//              for `id`, -1 for none
//   0x8012FC10 EnemyShowNode(em, id): the MHchar node shown when the monster,
//              or the node's part (its area, u8 +0x48E of the part at
//              em + 0x84 * part), is in the hunter's area and its conditions
//              pass
//   0x80126098 EnemyData(em): the species' data, EnemyActionTable of its kind
//              (+3) and variant (+0xA)
//   0x8012D090 EnemyDying(em): 1 in mode 11, or mode 12 by 0x8012D270
//   0x8012D204 EnemyFlag1F5(em): u8 +0x1F5
//   0x8012D0D8 EnemyInArea(em): 1 in the hunter's area (+0x1E1)
//   0x80140C64 EnemyActionTable(u8 group, u8 variant): the action table
//              word of the group (the table at *(0x8065B2F8)); group 0x20
//              picks entries 1-6 by variant, every other group entry 0
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
#ifdef MH3_DEVELOPMENT
#include "dev_options.h"

#include <cstdio>
#include <set>
#endif

#include <cstdint>

namespace {

uint32_t Read32(uint32_t a) {
    return MemoryInline::FlatRead32(a);
}

uint32_t PartEnabled(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3), id = frame.Arg(4);
    uint32_t entry = Read32(game.Call<0x80126098>({em}) + 0x90); // get_enemy_data

    for (; entry && Read32(entry) != 0xFFFFFFFFu; entry += 16) {
        if (Read32(entry) != id)
            continue;

        const uint32_t a = Read32(entry + 8);
        bool holds = false;

        switch (MemoryInline::FlatRead16(entry + 4)) {
        case 0:
            holds = MemoryInline::FlatRead8(em + 0x762) != 0;
            break;
        case 1:
            holds = game.Call<0x801357AC>({em, a & 0xFFFF}) == 1;
            break;
        case 3:
            holds = MemoryInline::FlatRead8(em + 0x1E2) == 2 || game.Call<0x80130168>({em}) == 1; // em_water_check
            break;
        case 4:
            holds = MemoryInline::FlatRead8(em + 0xA) == a;
            break;
        case 5:
            holds = (a & MemoryInline::FlatRead8(em + 0xA)) == a;
            break;
        case 6:
            holds = (game.Call<0x8011EA40>({em, a & 0xFF}) & 0xFF) >= Read32(entry + 12); // em_parts_damage_level_get
            break;
        case 7:
            holds = (MemoryInline::FlatRead8(em + 0x761) & a) != 0;
            break;
        default:
            break;
        }

        const uint32_t want = MemoryInline::FlatRead16(entry + 6);

        if ((want == 1 && !holds) || (want == 0 && holds))
            return 0;
    }

    return 1;
}

void PartEnabledPort(CpuContext* ctx) {
    const uint32_t result = PartEnabled(ctx); // the frame restored the GPRs
    ctx->gpr[3] = result;
}

int32_t NodePart(GuestCall& game, uint32_t em, uint32_t id) {
    const uint32_t list = Read32(game.Call<0x80126098>({em}) + 0x8C); // EnemyData

    for (uint32_t entry = list; entry && Read32(entry) != 0xFFFFFFFFu; entry += 8)
        if (Read32(entry) == id)
            return MemoryInline::FlatRead8(entry + 4);

    return -1;
}

void NodePartPort(CpuContext* ctx) {
    int32_t part;

    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        part = NodePart(game, frame.Arg(3), frame.Arg(4));
    }

    ctx->gpr[3] = uint32_t(part);
}

void ShowNodePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3), id = frame.Arg(4);
    const int32_t part = NodePart(game, em, id);
    const uint32_t area = game.Call<0x802AFCE8>() & 0xFF;
    const uint32_t where = part == -1 ? MemoryInline::FlatRead8(em + 0x1E1) : MemoryInline::FlatRead8(em + 0x84 * uint32_t(part) + 0x48E);
    const uint32_t shown = area == where ? game.Call<0x8012FA28>({em, id}) : 0;
    game.Call<0x800E3154>({em + 0x24, id, shown}); // MHchar::setVisibility
}

uint32_t ActionTable(uint32_t group, uint32_t variant) {
    uint32_t index = 0;

    if ((group & 0xFF) == 0x20) {
        const uint32_t v = variant & 0xFF;
        index = v >= 1 && v <= 6 ? v : 0;
    }

    return Read32(Read32(Read32(0x8065B2F8u) + 4 * (group & 0xFF)) + 4 * index);
}

void ActionTablePort(CpuContext* p) {
    p->gpr[3] = ActionTable(p->gpr[3], p->gpr[4]);
}

uint32_t Dying(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t em = frame.Arg(3);
    return MemoryInline::FlatRead8(em + 0x1E5) == 11 || game.Call<0x8012D270>({em, 12, 255}) == 1;
}

uint32_t InArea(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    return (game.Call<0x802AFCE8>() & 0xFF) == MemoryInline::FlatRead8(frame.Arg(3) + 0x1E1);
}

} // namespace

extern "C" void func_8012FA28(CpuContext* ctx);
extern "C" void MH3_EnemyPartEnabled(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyPartEnabled", func_8012FA28, PartEnabledPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8012FA28, MH3_EnemyPartEnabled, "EnemyPartEnabled");

extern "C" void func_8012F9C4(CpuContext* ctx);
extern "C" void MH3_EnemyNodePart(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyNodePart", func_8012F9C4, NodePartPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8012F9C4, MH3_EnemyNodePart, "EnemyNodePart");

extern "C" void func_8012FC10(CpuContext* ctx);
extern "C" void MH3_EnemyShowNode(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyShowNode", func_8012FC10, ShowNodePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8012FC10, MH3_EnemyShowNode, "EnemyShowNode");

extern "C" void func_80140C64(CpuContext* ctx);
static uint32_t ActionTableChecked([[maybe_unused]] uint32_t r1, uint32_t r3, uint32_t r4) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("EnemyActionTable")) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        PortCheck::RunPure(c, "EnemyActionTable", PortCheck::Returns::Gpr3, func_80140C64, ActionTablePort);
        return c->gpr[3];
    }
#endif
    return ActionTable(r3, r4);
}

extern "C" void MH3_EnemyActionTable(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    // MH3_TRACE=actions: each (group, variant) the game looks up, once, with
    // its action table word (the monsters' attack and action tables).
    static const TraceFlag traced("actions");

    if (traced) {
        static std::set<uint32_t> seen;

        if (seen.insert((ctx->gpr[3] & 0xFF) << 8 | (ctx->gpr[4] & 0xFF)).second) {
            const uint32_t word = ActionTable(ctx->gpr[3], ctx->gpr[4]);
            std::fprintf(stderr, "MH3 action table: group %u variant %u -> %08x (lr %08x)\n", unsigned(ctx->gpr[3] & 0xFF), unsigned(ctx->gpr[4] & 0xFF), word, unsigned(ctx->lr));
        }
    }
#endif
    ctx->gpr[3] = ActionTableChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80140C64, MH3_EnemyActionTable, "EnemyActionTable");

extern "C" void MH3_EnemyData(CpuContext* ctx) {
    const uint32_t em = ctx->gpr[3];
    ctx->gpr[3] = ActionTable(MemoryInline::FlatRead8(em + 3), MemoryInline::FlatRead8(em + 0xA));
}
REGISTER_NATIVE_FUNCTION_AS(0x80126098, MH3_EnemyData, "EnemyData");

extern "C" void MH3_EnemyFlag1F5(CpuContext* ctx) {
    ctx->gpr[3] = MemoryInline::FlatRead8(ctx->gpr[3] + 0x1F5);
}
REGISTER_NATIVE_FUNCTION_AS(0x8012D204, MH3_EnemyFlag1F5, "EnemyFlag1F5");

extern "C" void func_8012D090(CpuContext* ctx);
extern "C" void MH3_EnemyDying(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyDying", func_8012D090, [](CpuContext* c) {
        const uint32_t dying = Dying(c);
        c->gpr[3] = dying;
    });
}
REGISTER_NATIVE_FUNCTION_AS(0x8012D090, MH3_EnemyDying, "EnemyDying");

extern "C" void func_8012D0D8(CpuContext* ctx);
extern "C" void MH3_EnemyInArea(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyInArea", func_8012D0D8, [](CpuContext* c) {
        const uint32_t in = InArea(c);
        c->gpr[3] = in;
    });
}
REGISTER_NATIVE_FUNCTION_AS(0x8012D0D8, MH3_EnemyInArea, "EnemyInArea");
