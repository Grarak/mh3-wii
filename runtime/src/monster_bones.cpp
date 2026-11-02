// The monsters' CalcWorld callback, RMHP08, C reconstructions from the
// translated code. G3dCalcWorld calls the object at each node the callback's
// link names (u16 +6, ICalcWorldCallback) and once after the last, for every
// monster model, so these run a few dozen times per monster per frame:
//
//   0x80139538 BoneCallbackA(cb, ChrAnmResult*, mdl*, link): before a node's
//              world matrix. The node em +0x110 gets a zero translation; the
//              table's entries for the node run their handler (kinds 0, 3
//              and 4 vtable +0x1C, others +0x20), then the link moves to the
//              table's next node unless cb +8 bit 1 is set
//   0x801398BC BoneCallbackB(cb, CalcWorldCallbackArgs*, mdl*, link): after
//              it. Kinds below 4 vtable +0x24, others +0x28; the link moves on
//   0x80139BD0 BoneCallbackC(cb, world matrices, mdl*, link): after all of
//              them. Every node with user data "T-X", "T-Y" or "DBL" (the
//              keys at r13 - 0x7418, -0x7410, -0x740C, -0x7408: its kind, two
//              node names and a weight in percent) is a twist bone: it turns
//              by the weight's share of the difference between the two nodes'
//              angles (DBL: blends their rotations). Then the table's
//              overrides, vtable +0x2C
//   0x80139684 BoneEntryA(cb, result, mdl*, link, node, entry): vtable +0x1C
//   0x801399B8 BoneEntryB(cb, args, mdl*, link, node, entry): vtable +0x24
//   0x8013A724 BoneTable(em): the species' table (EnemyData +0x94)
//   0x8013A7D4 BoneNext(link, em): the link's next node
//
// The table: entries of 0x18 bytes until -1: +0 node, u8 +4 kind, +8 value,
// +0xC source node, u8 +0x10 part, +0x14 the part's angle curves. A part's
// state is at em + 0x84 * part: u8 +0x488 state, a matrix at +0x4B4.
//
// The originals walk the node's user data through a dozen one-line accessors
// and save nine paired-single registers to do it, on a pass most nodes leave
// at the first test. The bone-moving branch of BoneEntryA is rare and stays
// translated.
#include "abi_bridge.h"
#include "g3d_native.h"
#include "guest_call.h"
#include "memory.h"
#include "nw4r_math.h"
#include "port_check.h"

#include <cstdint>

extern "C" void func_80139538(CpuContext* ctx);
extern "C" void func_80139684(CpuContext* ctx);
extern "C" void func_801398BC(CpuContext* ctx);
extern "C" void func_801399B8(CpuContext* ctx);
extern "C" void func_80139BD0(CpuContext* ctx);
extern "C" void func_8013A724(CpuContext* ctx);
extern "C" void func_8013A7D4(CpuContext* ctx);

namespace {

constexpr uint32_t kEnd = 0xFFFFFFFFu, kEntryBytes = 0x18, kPartBytes = 0x84, kMtx = 0x30;
constexpr uint32_t kEntryA = 0x1C, kEntryOtherA = 0x20, kEntryB = 0x24, kEntryOtherB = 0x28, kDone = 0x2C;
constexpr uint32_t kEmptyA = 0x801398B8u, kEmptyB = 0x80139B04u, kEmptyDone = 0x8013A6B4u;
constexpr uint32_t kNoKind = 0xFF;

// BoneCallbackC's frame: the matrices, then the word the callbacks take.
constexpr uint32_t kMdlWord = 0x08, kParent = 0x20, kInverse = 0x50, kLocal = 0x80, kOwn = 0xB0;
constexpr uint32_t kFromA = 0xE0, kFromB = 0x110, kResult = 0x140, kNodeWorld = 0x170, kBase = 0x1A0, kBaseInverse = 0x1D0;
constexpr uint32_t kFrame = 0x210;

uint32_t R8(uint32_t a) {
    return MemoryInline::FlatRead8(a);
}

uint32_t R16(uint32_t a) {
    return MemoryInline::FlatRead16(a);
}

uint32_t R32(uint32_t a) {
    return MemoryInline::FlatRead32(a);
}

uint32_t Table(GuestCall& game, uint32_t em) {
    const uint32_t data = game.Call<0x80126098>({em}); // EnemyData
    return data ? R32(data + 0x94) : 0;
}

uint32_t StringLength(uint32_t s) {
    uint32_t n = 0;

    while (R8(s + n))
        ++n;

    return n;
}

bool SameString(uint32_t a, uint32_t b) {
    for (;; ++a, ++b) {
        const uint32_t c = R8(a);

        if (c != R8(b))
            return false;

        if (!c)
            return true;
    }
}

// ResDic::Get (0x800922B4, 0x800921A0): the data of `name` in the dictionary
// at dic, a Patricia trie of 16-byte nodes from +8 (u16 +0 bit, +4 left, +6
// right, +8 the name's offset, +0xC the data's), 0 for none.
uint32_t DicFind(uint32_t dic, uint32_t name) {
    const uint32_t nodes = dic + 8;
    const uint32_t length = StringLength(name);
    uint32_t parentBit = R16(nodes);
    uint32_t at = nodes + R16(dic + 0xC) * 16;

    for (uint32_t bit = R16(at); bit < parentBit; bit = R16(at)) {
        const uint32_t byte = bit >> 3;
        const bool right = byte < length && ((int32_t(int8_t(R8(name + byte))) >> (bit & 7)) & 1) != 0;
        parentBit = bit;
        at = nodes + R16(at + (right ? 6 : 4)) * 16;
    }

    const uint32_t string = R32(at + 8);
    return string && SameString(name, dic + string) ? dic + R32(at + 0xC) : 0;
}

// ResUserData's value (0x8013A408, 0x8013A27C): the entry's +4 from the entry.
uint32_t UserValue(uint32_t entry) {
    const uint32_t offset = R32(entry + 4);
    return offset ? entry + offset : 0;
}

// The list at 0x805A1B18: "T-X", "T-Y", "DBL".
uint32_t TwistKind(uint32_t name) {
    if (R8(name + 3) != 0)
        return kNoKind;

    const uint32_t a = R8(name), b = R8(name + 1), c = R8(name + 2);

    if (a == 'T' && b == '-' && c == 'X')
        return 0;

    if (a == 'T' && b == '-' && c == 'Y')
        return 1;

    return a == 'D' && b == 'B' && c == 'L' ? 2 : kNoKind;
}

uint32_t NodeId(uint32_t node) {
    return R32(node + 0x10);
}

uint32_t ParentOf(uint32_t node) {
    const uint32_t offset = R32(node + 0x5C);
    return offset ? node + offset : 0;
}

// 0x80139B08: out = the inverse of the node's parent's world * its own, the
// matrix relative to the parent.
void LocalMatrix(GuestCall& game, GuestFrame& frame, uint32_t out, uint32_t node, uint32_t world) {
    G3dNative::MtxCopy(world + NodeId(node) * kMtx, frame[kNodeWorld]);
    G3dNative::MtxCopy(world + NodeId(ParentOf(node)) * kMtx, frame[kBase]);
    game.Call<0x80088428>({frame[kBaseInverse], frame[kBase]}); // MtxInverseWrapper
    G3dNative::MtxConcat(frame[kBaseInverse], frame[kNodeWorld], out);
}

float Degrees(uint32_t angle) {
    const float turned = 360.0f * float(angle);
    return turned / 65536.0f;
}

// The rotation (0x80051758): its 3x3 written, the translation left.
void SetRotation(uint32_t to, uint32_t from) {
    for (uint32_t row = 0; row < kMtx; row += 0x10)
        for (uint32_t k = 0; k < 12; k += 4)
            MemoryInline::FlatWrite32(to + row + k, R32(from + row + k));
}

void TwistNode(GuestCall& game, GuestFrame& frame, uint32_t kind, uint32_t node, uint32_t a, uint32_t b, float weight, uint32_t world) {
    const uint32_t own = world + NodeId(node) * kMtx;
    G3dNative::MtxCopy(own, frame[kOwn]);

    if (kind == 2) {
        G3dNative::MtxCopy(world + NodeId(a) * kMtx, frame[kFromA]);
        G3dNative::MtxCopy(world + NodeId(b) * kMtx, frame[kFromB]);
        game.Call<0x800518F8>({frame[kResult], frame[kFromA], frame[kFromB], 0}, {weight, 1.0f - weight});
        SetRotation(own, frame[kResult]);
        return;
    }

    G3dNative::MtxCopy(world + NodeId(ParentOf(node)) * kMtx, frame[kParent]);
    game.Call<0x80088428>({frame[kInverse], frame[kParent]}); // MtxInverseWrapper
    G3dNative::MtxConcat(frame[kInverse], frame[kOwn], frame[kLocal]);
    LocalMatrix(game, frame, frame[kFromA], a, world);
    LocalMatrix(game, frame, frame[kFromB], b, world);

    // 0x80050E48, 0x80050EC4: the matrix's angle about X, Y.
    const float first = Degrees(kind == 0 ? game.Call<0x80050E48>({frame[kFromA]}) : game.Call<0x80050EC4>({frame[kFromA]}));
    const float second = Degrees(kind == 0 ? game.Call<0x80050E48>({frame[kFromB]}) : game.Call<0x80050EC4>({frame[kFromB]}));
    const float share = weight * (second - first);
    const float scaled = 65536.0f * share;
    const float units = scaled / 360.0f;
    const uint32_t angle = uint32_t(int32_t(0.5f + units)) & 0xFFFF;

    if (kind == 0)
        Nw4rMath::RotMatrixX(angle, frame[kLocal]);
    else
        Nw4rMath::RotMatrixY(angle, frame[kLocal]);

    G3dNative::MtxConcat(frame[kParent], frame[kLocal], frame[kResult]);
    SetRotation(own, frame[kResult]);
}

// 0x8013A7D4: the link's node moved to the table's lowest above it.
void AdvanceNode(uint32_t link, uint32_t table) {
    const uint32_t node = R16(link + 6);
    uint32_t next = node;

    for (uint32_t entry = table;; entry += kEntryBytes) {
        const uint32_t index = R32(entry);

        if (index == kEnd)
            break;

        if (index > node && (index < next || next == node))
            next = index;
    }

    if (next > node && next != kEnd)
        MemoryInline::FlatWrite16(link + 6, uint16_t(next));
}

void CallEntry(GuestCall& game, uint32_t cb, uint32_t slot, uint32_t empty, uint32_t mdlWord, uint32_t subject, uint32_t link, uint32_t node, uint32_t entry) {
    const uint32_t fn = R32(R32(cb) + slot);

    if (fn != empty)
        game(fn, {cb, subject, mdlWord, link, node, entry});
}

void BoneAPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const uint32_t cb = frame.Arg(3), result = frame.Arg(4), mdl = frame.Arg(5), link = frame.Arg(6);
    const uint32_t em = R32(cb + 4), node = R16(link + 6);

    if (node == R32(em + 0x110)) {
        for (uint32_t k = 0; k < 12; k += 4)
            MemoryInline::FlatWrite32(frame[0x10 + k], 0);

        G3dNative::ChrAnmResultSetTranslation(result, frame[0x10]);
    }

    const uint32_t table = Table(game, em);

    if (!table)
        return;

    for (uint32_t entry = table;; entry += kEntryBytes) {
        const uint32_t index = R32(entry);

        if (index == kEnd)
            break;

        if (index != node)
            continue;

        const uint32_t kind = R8(entry + 4);
        const bool first = kind == 0 || kind == 3 || kind == 4;
        MemoryInline::FlatWrite32(frame[kMdlWord], R32(mdl));
        CallEntry(game, cb, first ? kEntryA : kEntryOtherA, first ? 0 : kEmptyA, frame[kMdlWord], result, link, node, entry);
    }

    if ((R32(cb + 8) & 2) == 0)
        AdvanceNode(link, table);
}

void BoneBPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t cb = frame.Arg(3), args = frame.Arg(4), mdl = frame.Arg(5), link = frame.Arg(6);
    const uint32_t node = R16(link + 6);
    const uint32_t table = Table(game, R32(cb + 4));

    if (!table)
        return;

    for (uint32_t entry = table;; entry += kEntryBytes) {
        const uint32_t index = R32(entry);

        if (index == kEnd)
            break;

        if (index != node)
            continue;

        const bool first = R8(entry + 4) < 4;
        MemoryInline::FlatWrite32(frame[kMdlWord], R32(mdl));
        CallEntry(game, cb, first ? kEntryB : kEntryOtherB, first ? 0 : kEmptyB, frame[kMdlWord], args, link, node, entry);
    }

    AdvanceNode(link, table);
}

void BoneCPort(CpuContext* ctx) {
    GuestFrame frame(ctx, kFrame);
    GuestCall game{ctx};
    const uint32_t cb = frame.Arg(3), world = frame.Arg(4), mdlRef = frame.Arg(5), link = frame.Arg(6), r13 = frame.Arg(13);
    const uint32_t em = R32(cb + 4);
    const uint32_t table = Table(game, em);
    const uint32_t count = game.Call<0x80097FE4>({mdlRef}); // MdlNodeCount
    const uint32_t mdl = R32(mdlRef);
    const uint32_t nodes = mdl + R32(mdl + 0x14);

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t node = G3dNative::ResDicAtOffset14(mdlRef, i);
        const uint32_t offset = R32(node + 0x6C);

        if (!offset || !R32(node + offset + 8))
            continue;

        const uint32_t user = node + offset + 4;
        const uint32_t kindEntry = DicFind(user, r13 - 0x7418);

        if (!kindEntry)
            continue;

        const uint32_t kind = TwistKind(UserValue(kindEntry));

        if (kind == kNoKind)
            break;

        const uint32_t a = DicFind(user, r13 - 0x7410);

        if (!a)
            continue;

        const uint32_t b = DicFind(user, r13 - 0x740C);

        if (!b)
            continue;

        const uint32_t percent = DicFind(user, r13 - 0x7408);

        if (!percent)
            continue;

        const float weight = float(int32_t(R32(UserValue(percent)))) / 100.0f;
        TwistNode(game, frame, kind, node, DicFind(nodes, UserValue(a)), DicFind(nodes, UserValue(b)), weight, world);
    }

    for (uint32_t entry = table; entry && R32(entry) != kEnd; entry += kEntryBytes) {
        const uint32_t kind = R8(entry + 4), part = R8(entry + 0x10), source = R32(entry + 0xC);

        if ((kind != 0 && kind != 3) || R32(entry + 8) - 1 > 1 || part >= 2 || R8(em + part * kPartBytes + 0x488) > 1 || source == kEnd)
            continue;

        const uint32_t to = NodeId(G3dNative::ResDicAtOffset14(mdlRef, R32(entry)));
        const uint32_t from = NodeId(G3dNative::ResDicAtOffset14(mdlRef, source));
        G3dNative::MtxCopy(world + from * kMtx, world + to * kMtx);
    }

    const uint32_t done = R32(R32(cb) + kDone);

    if (done != kEmptyDone) {
        MemoryInline::FlatWrite32(frame[kMdlWord], mdl);
        game(done, {cb, world, frame[kMdlWord], link});
    }
}

// Only a live part's kinds 0 and 3 and kind 4 do anything here.
void BoneEntryAPort(CpuContext* ctx) {
    const uint32_t entry = ctx->gpr[8];
    const uint32_t kind = R8(entry + 4);

    if (kind == 4) {
        func_80139684(ctx);
        return;
    }

    if (kind != 0 && kind != 3)
        return;

    const uint32_t state = R32(ctx->gpr[3] + 4) + R8(entry + 0x10) * kPartBytes;

    if (R8(state + 0x488) >= 2 && R32(entry + 0x14) != 0)
        func_80139684(ctx);
}

void BoneEntryBPort(CpuContext* ctx) {
    const uint32_t em = R32(ctx->gpr[3] + 4), args = ctx->gpr[4], entry = ctx->gpr[8];
    const uint32_t kind = R8(entry + 4), value = R32(entry + 8);

    if (kind == 0 || kind == 3) {
        const uint32_t part = R8(entry + 0x10);

        // 0x80139AFC: the node's world matrix set from the part's.
        if (part < 2 && value == 0 && R8(em + part * kPartBytes + 0x488) >= 2)
            G3dNative::MtxCopy(em + part * kPartBytes + 0x4B4, R32(args));
    } else if (kind == 1) {
        if (value < 6) {
            GuestFrame frame(ctx, 0x50);
            G3dNative::MtxCopy(R32(args), frame[0x10]);
            Nw4rMath::RotMatrixY(R16(em + value * 2 + 0x742), frame[0x10]);
            G3dNative::MtxCopy(frame[0x10], R32(args));
        }
    } else if (kind == 2) {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        game.Call<0x802AD79C>({em + value * 0x90 + 0x590, args});
    }
}

void BoneTablePort(CpuContext* ctx) {
    uint32_t table;

    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        table = Table(game, frame.Arg(3));
    }

    ctx->gpr[3] = table;
}

void BoneNextPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};

    if (const uint32_t table = Table(game, frame.Arg(4)))
        AdvanceNode(frame.Arg(3), table);
}

} // namespace

extern "C" void MH3_BoneCallbackA(CpuContext* ctx) {
    PortCheck::Call(ctx, "BoneCallbackA", func_80139538, BoneAPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80139538, MH3_BoneCallbackA, "BoneCallbackA");

extern "C" void MH3_BoneCallbackB(CpuContext* ctx) {
    PortCheck::Call(ctx, "BoneCallbackB", func_801398BC, BoneBPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801398BC, MH3_BoneCallbackB, "BoneCallbackB");

extern "C" void MH3_BoneCallbackC(CpuContext* ctx) {
    PortCheck::Call(ctx, "BoneCallbackC", func_80139BD0, BoneCPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80139BD0, MH3_BoneCallbackC, "BoneCallbackC");

extern "C" void MH3_BoneEntryA(CpuContext* ctx) {
    PortCheck::Call(ctx, "BoneEntryA", func_80139684, BoneEntryAPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80139684, MH3_BoneEntryA, "BoneEntryA");

extern "C" void MH3_BoneEntryB(CpuContext* ctx) {
    PortCheck::Call(ctx, "BoneEntryB", func_801399B8, BoneEntryBPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801399B8, MH3_BoneEntryB, "BoneEntryB");

extern "C" void MH3_BoneTable(CpuContext* ctx) {
    PortCheck::Call(ctx, "BoneTable", func_8013A724, BoneTablePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8013A724, MH3_BoneTable, "BoneTable");

extern "C" void MH3_BoneNext(CpuContext* ctx) {
    PortCheck::Call(ctx, "BoneNext", func_8013A7D4, BoneNextPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8013A7D4, MH3_BoneNext, "BoneNext");
