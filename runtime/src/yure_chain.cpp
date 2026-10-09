// Sway chain solver ("yure"), RMHP08. C reconstruction of 0x802AC4E8 (work,
// kind, chain, part), called by yure_move 0x802AC2D8 for each sway chain of an
// enemy (kind 0, chains at +0x590) or of a player's equipment (kind 1, chains
// at +0x674, the MHchar parts at *(+0x13C)); decompiled from the disassembly.
//
// A chain (0x90 bytes) hangs a few nodes off a joint:
//   +1 mode, +2 parameter set, +3 state (0 set up, 1 running), +4 collision
//   plane, +5 anchor joint (row of the joint table), +8 node count, +0xC root
//   joint, +0x10 node joints; per node +0x18 position (the root's first),
//   +0x24 swung position, +0x3C velocity, +0x54 rest direction, +0x78/+0x80
//   angles, +0x88 length; +0x6C the anchor position a step ago.
// A parameter set (0x24 bytes in the table at r27) holds per-node lengths
// (+0), the reach (+4), the spring (+8, +0x1C when shaken), the damping
// (+0xC), the plane margin (+0x10), the rest pull (+0x14), gravity (+0x18 or
// +0x1C) and an angle offset (+0x20).
//
// State 0 (the first step) places the nodes; that runs the original. State 1
// swings them each step: mode 1 a single node on a spring toward its rest
// point, mode 2 a node dragged by the anchor, else (0, 3) a chain where each
// node is pulled back toward its length and rest direction and falls with
// gravity, then kept above the collision plane; the node angles follow.
//
// Each step moved the nodes by their velocity, added the forces to it and
// damped it by (1 - damping), so at short steps the chains swung faster and
// settled sooner. The port moves them by velocity * scale, adds the forces
// times the scale and damps by (1 - damping) ^ scale. The anchor's drag (its
// motion since the last step) is already the step's share. At the stock step
// it is the original.
#include "abi_bridge.h"
#include "game_step.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cmath>

namespace {

enum : uint32_t {
    kMode = 0x01,       // u8
    kSet = 0x02,        // u8
    kState = 0x03,      // u8
    kPlane = 0x04,      // u8
    kAnchor = 0x05,     // u8
    kCount = 0x08,      // u32
    kRootJoint = 0x0C,  // u32
    kJoints = 0x10,     // u32[]
    kRoot = 0x18,       // VEC3[]: the root, then the nodes
    kNode = 0x24,       // VEC3[]
    kTarget = 0x30,     // VEC3 (mode 1)
    kVelocity = 0x3C,   // VEC3[]
    kRest = 0x54,       // VEC3[]
    kLastAnchor = 0x6C, // VEC3
    kAngleA = 0x78,     // u32[]
    kAngleB = 0x80,     // u32[]
    kLength = 0x88,     // f32[]
};
enum : uint32_t {
    kSetSize = 0x24,
    kReach = 0x04,
    kSpring = 0x08,
    kDamping = 0x0C,
    kMargin = 0x10,
    kPull = 0x14,
    kGravityA = 0x18,
    kSpringShaken = 0x1C,
    kAngleOffset = 0x20
};

constexpr uint32_t kEnemySets = 0x805CF1B0, kEnemyAnchors = 0x805CF020;
constexpr uint32_t kPart5Sets = 0x805CF3A8, kPart5Anchors = 0x805CF0D8;
constexpr uint32_t kPart4Sets = 0x805CF438, kPart4Anchors = 0x805CF120;
constexpr uint32_t kPlaneJoints = 0x805CF4EC; // u8 by plane
constexpr uint32_t kPlanes = 0x805CEE78;      // by plane: normal, point (MH vectors, 12 bytes each)
constexpr uint32_t kGravity = 0x806BAC18;     // VEC3
constexpr float kTiny = 0.001f;

// Stack frame layout (as the original's).
enum : uint32_t {
    sLengths = 16,     // f32[]: node lengths
    sPlane = 448,      // plane: normal, then its offset at +12
    sPlaneProbe = 464, // VEC3 + f32 margin at +12
    sPlaneAt = 480,
    sPlaneN = 492,
    sPlanePoint = 540,
    sDir = 552,
    sRot = 564,
    sOut = 600,
    sAccel = 624,
    sAnchor = 516,
    sJoints = 576,
    sAnchorMtx = 648,
    sInverse = 696,
};

enum : uint32_t {
    Original = 0x802AC4E8,
    Ready = 0x802248E8, // (player) the parts are loaded
    DemoBusy = 0x803C4890,
    DemoState4 = 0x803C4878,    // IsEventDemoState4
    EnemyJointMtx = 0x80135994, // get_joint_wmat_em
    EnemyJointPos = 0x8013599C, // get_joint_wpos_em
    EnemyScale = 0x801359A4,    // get_em_scale
    CharJointMtx = 0x800E0A78,
    CharJointPos = 0x800E0AF8, // MHchar::get_joint_wpos
    MtxTranslation = 0x80101470,
    MtxCopy = 0x80053340,
    MtxInverse = 0x80088428,
    MulVecMtx = 0x800514A4, // v = m * v
    RotateVec = 0x80501C48, // out = m (rotation only) * v
    ToMhVec = 0x8005001C,   // vec_to_mh_vec3
    Magnitude = 0x80050F88,
    Magnitude2 = 0x80050F40,
    Normalize = 0x800508B4,
    ClampReach = 0x80051324, // (f1 reach, centre, point): point within reach of centre
    Dot = 0x80052278,
    OntoPlane = 0x80290588,
    Atan2 = 0x80464488, // ForceSingleAngleWrapper
    NodeAngles = 0x802AD830,
    NodeHeading = 0x802AD958,
};

struct Vec {
    float x, y, z;
    Vec operator+(const Vec& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec operator-(const Vec& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec operator*(float k) const { return {x * k, y * k, z * k}; }
};
Vec Load(uint32_t a) {
    return {Memory::ReadFloat32(a), Memory::ReadFloat32(a + 4), Memory::ReadFloat32(a + 8)};
}
void Store(uint32_t a, const Vec& v) {
    Memory::WriteFloat32(a, v.x);
    Memory::WriteFloat32(a + 4, v.y);
    Memory::WriteFloat32(a + 8, v.z);
}
float F(uint32_t a) {
    return Memory::ReadFloat32(a);
}

} // namespace

extern "C" void func_802AC4E8(CpuContext* ctx);

static void YureChain(CpuContext* ctx) {
    {
        // Setting up (state 0) runs the original.
        const uint32_t w = ctx->gpr[3], index = ctx->gpr[5];
        const uint32_t chain = w + index * 0x90 + ((ctx->gpr[4] & 0xFF) ? 0x674 : 0x590);
        if (Memory::Read8(chain + kState) != 1) {
            func_802AC4E8(ctx);
            return;
        }
    }
    GuestFrame frame{ctx, 1072};
    GuestCall call{ctx};
    const uint32_t work = frame.Arg(3), part = frame.Arg(6);
    const bool player = (frame.Arg(4) & 0xFF) != 0;
    const uint32_t chain = work + frame.Arg(5) * 0x90 + (player ? 0x674 : 0x590);
    uint32_t sets = frame.Arg(27), anchors = frame.Arg(28);
    const uint32_t enemy = player ? 0 : work;
    if (!player) {
        sets = kEnemySets;
        anchors = kEnemyAnchors;
    } else {
        auto stop = [&] {
            Memory::Write8(chain + kState, 0);
        };
        if (!(call.Call<Ready>({work}) & 0xFF))
            return stop();
        if ((call.Call<DemoBusy>() & 0xFF) || call.Call<DemoState4>())
            return stop();
        const uint32_t parts = Memory::Read32(work + 0x13C);
        for (uint32_t k = 0; k < 7; ++k)
            if (!Memory::Read8(parts + k * 0x164 + 0x38) || !Memory::Read8(parts + k + 0x9C0))
                return stop();
        if (part == 5) {
            sets = kPart5Sets;
            anchors = kPart5Anchors;
        } else if (part == 4) {
            sets = kPart4Sets;
            anchors = kPart4Anchors;
        }
    }
    const uint32_t count = Memory::Read32(chain + kCount);
    if (!count)
        return;

    auto set = [&] {
        return sets + Memory::Read8(chain + kSet) * kSetSize;
    };
    auto anchorJoint = [&] {
        return uint32_t(Memory::Read8(anchors + Memory::Read8(chain + kAnchor) * 14 + 3));
    };
    auto charParts = [&] {
        return Memory::Read32(work + 0x13C);
    };

    float scale, spring, damping, pull, gravity;
    if (!player) {
        call.Call<EnemyJointMtx>({enemy, anchorJoint(), frame[sAnchorMtx]});
        call.Call<MtxTranslation>({frame[sAnchorMtx], frame[sAnchor]});
        for (uint32_t i = 0; i < count; ++i) {
            call.Call<EnemyJointPos>({enemy, Memory::Read32(chain + kJoints + 4 * i), frame[sJoints] + 12 * i});
            Memory::WriteFloat32(frame[sLengths] + 4 * i, F(chain + kLength + 4 * i) + F(set() + 4 * i));
        }
        call.Call<MtxCopy>({frame[sInverse], frame[sAnchorMtx]});
        call.Call<EnemyScale>({enemy});
        scale = call.F1();
        const uint32_t t = set();
        const bool shaken = Memory::Read8(enemy + 0x1E2) == 2;
        damping = F(t + kDamping);
        pull = F(t + kPull);
        if (Memory::Read8(chain + kMode) == 2) {
            spring = F(t + (shaken ? kSpringShaken : kSpring));
            gravity = 0.0f;
        } else {
            spring = F(t + kSpring);
            gravity = F(t + (shaken ? kGravityA : kSpringShaken));
        }
    } else {
        call.Call<CharJointMtx>({charParts() + 4, anchorJoint(), frame[sAnchorMtx]});
        call.Call<CharJointPos>({charParts() + 4, Memory::Read32(chain + kRootJoint), frame[sAnchor]});
        for (uint32_t i = 0; i < count; ++i) {
            call.Call<CharJointPos>({charParts() + part * 0x164 + 4, Memory::Read32(chain + kJoints + 4 * i), frame[sJoints] + 12 * i});
            Memory::WriteFloat32(frame[sLengths] + 4 * i, F(chain + kLength + 4 * i) + F(set() + 4 * i));
        }
        scale = 1.0f;
        const uint32_t t = set();
        spring = F(t + kSpring);
        if (Memory::Read8(work + 9) == 3) {
            damping = 2.0f * F(t + kDamping);
            pull = 0.5f * F(t + kPull);
            gravity = F(t + kSpringShaken);
        } else {
            damping = F(t + kDamping);
            pull = F(t + kPull);
            gravity = F(t + kGravityA);
        }
    }

    // The step: velocity * s moves, forces * s, damping ^ s.
    const double stepScale = GameStep::Scale();
    const float s = float(stepScale);
    const float keep = 1.0f - damping;
    const float damp = stepScale == 1.0 ? keep : float(std::pow(double(keep), stepScale));
    auto magnitude = [&](uint32_t v, uint32_t fn) {
        call(fn, {v});
        return call.F1();
    };

    const uint32_t root = chain + kRoot;
    Store(root, Load(frame[sJoints]));
    const uint8_t mode = Memory::Read8(chain + kMode);
    if (mode == 1) {
        const uint32_t node = chain + kNode, velocity = chain + kVelocity, target = chain + kTarget;
        Store(node, Load(node) + Load(velocity) * s);
        Store(velocity, Load(velocity) + (Load(chain + kLastAnchor) - Load(root)) * (F(set() + kGravityA) * s));
        Store(frame[sRot], Load(chain + kRest));
        call.Call<MulVecMtx>({frame[sRot], frame[sAnchorMtx]});
        const float length = F(frame[sLengths]);
        Store(target, Load(root) + Load(frame[sRot]) * (length * scale));
        call.Call<ClampReach>({target, node}, {scale * F(set() + kReach)});
        Store(frame[sDir], Load(node) - Load(root));
        if (magnitude(frame[sDir], Magnitude2) > kTiny)
            call.Call<Normalize>({frame[sDir], frame[sDir]});
        Store(node, Load(root) + Load(frame[sDir]) * length);
        Store(velocity, Load(velocity) + (Load(target) - Load(node)) * (spring * s));
        Store(velocity, Load(velocity) * damp);
        Store(frame[sDir], Load(root) - Load(node));
        call.Call<MtxInverse>({frame[sInverse], frame[sAnchorMtx]});
        call.Call<MulVecMtx>({frame[sDir], frame[sInverse]});
        call.Call<Atan2>({}, {F(frame[sDir] + 8), F(frame[sDir] + 4)});
        const float turn = 0.5f + (65536.0f * call.F1()) / 6.2831855f;
        uint32_t angle = uint32_t(int32_t(turn)) & 0xFFFF;
        angle = (angle + Memory::Read32(set() + kAngleOffset) - 0x8000) & 0xFFFF;
        Memory::Write32(chain + kAngleA, angle);
        Memory::Write32(chain + kAngleB, 0);
        Store(chain + kLastAnchor, Load(frame[sAnchor]));
        return;
    }
    if (mode == 2) {
        const uint32_t node = chain + kNode, velocity = chain + kVelocity;
        Store(root, Load(frame[sAnchor]));
        Store(node, Load(node) + Load(velocity) * s);
        call.Call<ClampReach>({root, node}, {scale * F(set() + kReach)});
        const Vec back = (Load(root) - Load(node)) * (spring * s);
        Store(velocity, Load(velocity) * damp + back);
        Store(frame[sDir], (Load(chain + kLastAnchor) - Load(root)) * 0.1f);
        if (magnitude(frame[sDir], Magnitude) > 50.0f) {
            call.Call<Normalize>({frame[sDir], frame[sDir]});
            Store(frame[sDir], Load(frame[sDir]) * 50.0f);
        }
        Store(velocity, Load(velocity) + Load(frame[sDir]));
        Store(chain + kLastAnchor, Load(root));
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t prev = root + 12 * i, node = chain + kNode + 12 * i, velocity = chain + kVelocity + 12 * i;
        const uint32_t accel = frame[sAccel] + 12 * i;
        const float length = F(frame[sLengths] + 4 * i);
        Store(node, Load(node) + Load(velocity) * s);
        call.Call<RotateVec>({frame[sRot], frame[sAnchorMtx], chain + kRest + 12 * i});
        Store(accel, Load(frame[sRot]) * pull);
        Store(accel, Load(accel) + Load(kGravity) * gravity);
        call.Call<ClampReach>({prev, node}, {scale * (10.0f + length)});
        Store(frame[sDir], Load(prev) - Load(node));
        const float stretch = magnitude(frame[sDir], Magnitude);
        if (stretch > kTiny)
            call.Call<Normalize>({frame[sDir], frame[sDir]});
        Store(accel, Load(accel) + Load(frame[sDir]) * (spring * (stretch - scale * length)));
        Store(velocity, Load(velocity) * damp + Load(accel) * s);
        if (i == 0 && player)
            Store(prev, Load(prev) + (Load(frame[sAnchor]) - Load(chain + kLastAnchor)));
        Store(frame[sDir], Load(node) - Load(prev));
        if (magnitude(frame[sDir], Magnitude) > kTiny)
            call.Call<Normalize>({frame[sDir], frame[sDir]});
        const uint32_t out = frame[sOut] + 12 * i;
        Store(out, Load(frame[sDir]) * (scale * length));
        Store(node, Load(prev) + Load(out));
    }

    if (const uint32_t plane = Memory::Read8(chain + kPlane)) {
        if (player) {
            call.Call<CharJointMtx>({charParts() + 4, Memory::Read8(kPlaneJoints + plane), frame[sInverse]});
            call.Call<CharJointPos>({charParts() + 4, Memory::Read32(chain + kRootJoint), frame[sPlanePoint]});
        } else {
            call.Call<EnemyJointMtx>({enemy, anchorJoint(), frame[sInverse]});
            call.Call<EnemyJointPos>({enemy, Memory::Read8(kPlaneJoints + plane), frame[sPlanePoint]});
        }
        call.Call<ToMhVec>({frame[sPlaneN], kPlanes + plane * 24});
        call.Call<RotateVec>({frame[sRot], frame[sInverse], frame[sPlaneN]});
        call.Call<Normalize>({frame[sPlane], frame[sRot]});
        call.Call<ToMhVec>({frame[sPlaneAt], kPlanes + plane * 24 + 12});
        call.Call<MulVecMtx>({frame[sPlaneAt], frame[sInverse]});
        Store(frame[sPlanePoint], Load(frame[sPlanePoint]) + Load(frame[sPlaneAt]));
        call.Call<Dot>({frame[sPlanePoint], frame[sPlane]});
        Memory::WriteFloat32(frame[sPlane] + 12, call.F1());
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t node = chain + kNode + 12 * i;
            Store(frame[sPlaneProbe], Load(node));
            const float margin = scale * F(set() + kMargin);
            Memory::WriteFloat32(frame[sPlaneProbe] + 12, margin);
            call.Call<OntoPlane>({frame[sPlaneProbe], frame[sPlane], frame[sPlanePoint]});
            Store(frame[sDir], Load(frame[sPlaneProbe]) - Load(frame[sPlanePoint]));
            call.Call<Dot>({frame[sPlane], frame[sDir]});
            const float height = call.F1(), least = F(frame[sPlaneProbe] + 12);
            if (height < least) {
                Store(frame[sDir], Load(frame[sPlane]) * (least - height));
                Store(node, Load(node) + Load(frame[sDir]));
            }
        }
    }
    Store(chain + kLastAnchor, Load(frame[sAnchor]));
    if (player)
        return;
    for (uint32_t i = 0; i < count; ++i) {
        if (Memory::Read8(chain + kMode) == 3) {
            call.Call<NodeHeading>({chain, frame[sAnchorMtx], chain + kAngleB + 4 * i, i});
            Memory::Write32(chain + kAngleB + 4 * i, Memory::Read32(chain + kAngleB + 4 * i) + Memory::Read32(set() + kAngleOffset));
        } else {
            call.Call<NodeAngles>({chain, frame[sAnchorMtx], chain + kAngleA + 4 * i, chain + kAngleB + 4 * i, i});
        }
    }
}

extern "C" void MH3_YureChain(CpuContext* ctx) {
    PortCheck::Call(ctx, "YureChain", func_802AC4E8, YureChain);
}
REGISTER_NATIVE_FUNCTION_AS(0x802AC4E8, MH3_YureChain, "YureChain");
