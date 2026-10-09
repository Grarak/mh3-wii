// Ground collision, RMHP08, decompiled with Ghidra: the ground height under a
// point from the area's collision triangles. Every hunter, monster and small
// creature asks every step, so in the field the translated originals cost
// about 3% of the devkit's game thread (Tundra area 2 while running), mostly
// in calls to three small triangle tests per candidate triangle.
//
// The area's ground (0x806AE508 + area * 0x88, the first byte nonzero when
// loaded) has an XZ bounding box and a grid of candidate lists per layer:
// GroundHitGridCandidates (0x802968D4, kept translated) finds the point's
// cell, and each layer's list for it is a run of triangle pointers ending in
// 0xFFFFFFFF. A triangle counts when its flags meet the query's mask, the
// area's mask (0x806BD45C + area) misses its +7 bits, it was not taken
// already, and the point lies inside it in XZ (GroundHitPointInTriangle,
// 0x80296A5C, with 0x80296B28 and 0x80296BA4). Its height at the point comes
// from its plane: -(d + a x + c z) / b. The triangles taken (at most five)
// are listed at 0x806AED00, where the "taken already" test looks.
//
//   GetGroundHit2 (0x802916C8: pos, mask, area or 0xFF, u8* hit) -> f1: one
//     triangle's height, or of several the highest at most 50 above the
//     point, else the lowest; the point's own height when none (*hit 0).
//   hit_ground_comon (0x80291D34: pos, area, LandData* out, f32* height,
//     mask, u16 skip mask, f32* special4, f32* special2000, HIT_TENJO*
//     special10 on the stack) -> r3 1 found, 0 none, -1 no ground: triangles
//     with flag 4, 0x2000 or 0x10 go to their own outputs when given; the
//     others are recorded (flags, bytes, normal) and 0x80297878 (kept
//     translated) picks the result.
//
// MH3_PORT_CHECK=GroundHitPointInTriangle,GetGroundHit2,HitGroundComon
// checks each call against the original.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <bit>
#include <cstdint>

namespace {

constexpr uint32_t kGrounds = 0x806AE508;      // the areas' grounds, 0x88 bytes each
constexpr uint32_t kTaken = 0x806AED00;        // the triangles a query took (u32[5])
constexpr uint32_t kAreaMasks = 0x806BD45C;    // u8 per area (0x802B0B50)
constexpr uint32_t kGroundBytes = 0x88;
constexpr int kMaxTaken = 5;
constexpr float kAboveReach = 50.0f; // 0x8079C7F4

enum : uint32_t { // ground
    kMinX = 0x48,
    kMinZ = 0x50,
    kMaxX = 0x54,
    kMaxZ = 0x5C,
    kCellStride = 0x6C,
    kLayers = 0x70,
    kLists = 0x78,
};

enum : uint32_t { // triangle
    kFlags = 0x00,    // u16; 0 is taken as 0x20 (and stored)
    kKind = 0x02,     // u8
    kByte3 = 0x03,    // u8
    kSkipBits = 0x04, // u16
    kByte6 = 0x06,    // u8
    kAreaBits = 0x07, // u8
    kVertices = 0x08, // three x, y, z
    kNormal = 0x2C,   // a, b, c, then d at 0x38
    kPlaneD = 0x38,
};

enum : uint32_t { // LandData, a result record (20 bytes)
    kRecordFlags = 0x00,
    kRecordKind = 0x02,
    kRecordByte3 = 0x03,
    kRecordSkipBits = 0x04,
    kRecordByte6 = 0x06,
    kRecordAreaBits = 0x07,
    kRecordNormal = 0x08,
    kRecordBytes = 20,
};

enum : uint16_t {
    kFlagSpecial4 = 0x4,
    kFlagSpecial10 = 0x10,
    kFlagSpecial2000 = 0x2000,
};

enum : uint32_t {
    GetNowAreaNumber = 0x802AFCE8,
    GetOnlineMode = 0x800CF26C,
    GridCandidates = 0x802968D4, // (pos, ground, u32* cell x, u32* cell z)
    PickResult = 0x80297878,     // (out, pos, records, heights, f32* height, count)
};

struct Point {
    float x, z;
};

// TriangleSignedArea2D (0x80296B28): twice the signed area of a, b, c, with
// products one unit in the last place apart taken as equal (a zero area).
float SignedArea(Point a, Point b, Point c) {
    float p = (b.x - a.x) * (c.z - a.z);
    const float q = (c.x - a.x) * (b.z - a.z);
    const uint32_t pBits = std::bit_cast<uint32_t>(p), qBits = std::bit_cast<uint32_t>(q);

    if (pBits == qBits + 1)
        p = q;

    if (pBits == qBits - 1)
        p = q;

    return p - q;
}

// TriangleWindingClassifyPoint (0x80296BA4): for a triangle of signed area
// `area`, 2 when p is inside a counter-clockwise one, 1 inside a clockwise
// one, 0 outside or degenerate.
int Classify(float area, Point a, Point b, Point c, Point p) {
    if (area == 0.0f)
        return 0;

    if (area < 0.0f) {
        if (SignedArea(a, b, p) > 0.0f || SignedArea(b, c, p) > 0.0f)
            return 0;

        return SignedArea(c, a, p) > 0.0f ? 0 : 1;
    }

    if (SignedArea(a, b, p) < 0.0f || SignedArea(b, c, p) < 0.0f)
        return 0;

    return SignedArea(c, a, p) < 0.0f ? 0 : 2;
}

// GroundHitPointInTriangle (0x80296A5C), on a triangle's three XZ corners.
bool Inside(const Point (&v)[3], Point p) {
    const float area = SignedArea(v[0], v[1], v[2]);
    const int side = Classify(area, v[0], v[1], v[2], p);

    if (side == 0)
        return false;

    return area > 0.0f ? side == 2 : side == 1;
}

Point Corner(uint32_t at) {
    return {Memory::ReadFloat32(at), Memory::ReadFloat32(at + 8)};
}

// The port's own entry: r3 three corners 12 bytes apart (x and the next
// float), r4 the point.
void PointInTrianglePort(CpuContext* cpu) {
    const uint32_t corners = cpu->gpr[3], point = cpu->gpr[4];
    const Point v[3] = {{Memory::ReadFloat32(corners), Memory::ReadFloat32(corners + 4)},
                        {Memory::ReadFloat32(corners + 12), Memory::ReadFloat32(corners + 16)},
                        {Memory::ReadFloat32(corners + 24), Memory::ReadFloat32(corners + 28)}};
    cpu->gpr[3] = Inside(v, {Memory::ReadFloat32(point), Memory::ReadFloat32(point + 4)});
}

bool InBounds(uint32_t pos, uint32_t ground) {
    const float x = Memory::ReadFloat32(pos), z = Memory::ReadFloat32(pos + 8);
    return x < Memory::ReadFloat32(ground + kMaxX) && Memory::ReadFloat32(ground + kMinX) < x && z < Memory::ReadFloat32(ground + kMaxZ) &&
           Memory::ReadFloat32(ground + kMinZ) < z;
}

// 0x80296A0C: the candidate list of a cell in one layer.
uint32_t CandidateList(uint32_t ground, uint32_t cellX, uint32_t layer, uint32_t cellZ) {
    const uint32_t index = Memory::Read32(ground + kCellStride) * (layer + cellX * Memory::Read32(ground + kLayers)) + cellZ;
    return Memory::Read32(Memory::Read32(ground + kLists) + index * 4);
}

// 0x8029628C: whether the triangle is among the first `count` taken.
bool Taken(uint32_t triangle, int count) {
    for (int i = 0; i < count; ++i)
        if (Memory::Read32(kTaken + 4 * i) == triangle)
            return true;
    return false;
}

// The flags, with 0 stored as 0x20 the first time.
uint16_t Flags(uint32_t triangle) {
    if (Memory::Read16(triangle + kFlags) == 0)
        Memory::Write16(triangle + kFlags, 0x20);

    return Memory::Read16(triangle + kFlags);
}

bool Contains(uint32_t triangle, Point p) {
    const Point v[3] = {Corner(triangle + kVertices), Corner(triangle + kVertices + 12), Corner(triangle + kVertices + 24)};
    return Inside(v, p);
}

// The triangle's plane height at the point, in the original's order.
float Height(uint32_t triangle, uint32_t pos) {
    const float ax = Memory::ReadFloat32(triangle + kNormal) * Memory::ReadFloat32(pos);
    const float cz = Memory::ReadFloat32(triangle + kNormal + 8) * Memory::ReadFloat32(pos + 8);
    const float sum = ax + cz;
    const float plane = Memory::ReadFloat32(triangle + kPlaneD) + sum;
    return -plane / Memory::ReadFloat32(triangle + kNormal + 4);
}

uint8_t AreaNumber(GuestCall& call, uint32_t area) {
    return uint8_t(area) == 0xFF ? uint8_t(call(GetNowAreaNumber)) : uint8_t(area);
}

void GetGroundHit2(CpuContext* cpu) {
    const uint32_t pos = cpu->gpr[3], mask = cpu->gpr[4], hit = cpu->gpr[6];
    float result;
    {
        GuestFrame frame(cpu, 0x20);
        GuestCall call{cpu};
        const uint8_t area = AreaNumber(call, frame.Arg(5));
        const uint32_t ground = kGrounds + area * kGroundBytes;
        Memory::Write8(hit, 1);

        if (Memory::Read8(ground) == 0 || !InBounds(pos, ground)) {
            Memory::Write8(hit, 0);
            cpu->fpr[1].d = Memory::ReadFloat32(pos + 4);
            return;
        }

        float heights[kMaxTaken];
        heights[0] = Memory::ReadFloat32(pos + 4);
        call(GridCandidates, {pos, ground, frame[8], frame[12]});
        const uint32_t cellX = Memory::Read32(frame[8]), cellZ = Memory::Read32(frame[12]);
        const Point p{Memory::ReadFloat32(pos), Memory::ReadFloat32(pos + 8)};
        const uint8_t areaMask = Memory::Read8(kAreaMasks + area);
        int count = 0;

        for (uint32_t layer = 0; layer < Memory::Read32(ground + kLayers) && count < kMaxTaken; ++layer) {
            for (uint32_t list = CandidateList(ground, cellX, layer, cellZ);; list += 4) {
                const uint32_t triangle = Memory::Read32(list);

                if (triangle == 0xFFFFFFFF)
                    break;

                if (areaMask & Memory::Read8(triangle + kAreaBits))
                    continue;

                if (!(mask & Flags(triangle)) || Memory::ReadFloat32(triangle + kNormal + 4) <= 0.0f || Taken(triangle, count) || !Contains(triangle, p))
                    continue;

                Memory::Write32(kTaken + 4 * count, triangle);
                heights[count++] = Height(triangle, pos);

                if (count == kMaxTaken)
                    break;
            }
        }

        result = heights[0];

        if (count == 0)
            Memory::Write8(hit, 0);

        if (count >= 2) {
            // The highest of those at most 50 above the point, else the lowest.
            const float reach = kAboveReach + Memory::ReadFloat32(pos + 4);
            float reached[kMaxTaken];
            int n = 0;

            for (int i = 0; i < count; ++i)
                if (heights[i] <= reach)
                    reached[n++] = heights[i];

            if (n) {
                result = reached[0];

                for (int i = 1; i < n; ++i)
                    if (result < reached[i])
                        result = reached[i];
            } else {
                for (int i = 1; i < count; ++i)
                    if (result > heights[i])
                        result = heights[i];
            }
        }
    }

    cpu->fpr[1].d = result;
}

void HitGroundComon(CpuContext* cpu) {
    const uint32_t pos = cpu->gpr[3], out = cpu->gpr[5], heightOut = cpu->gpr[6], mask = cpu->gpr[7];
    const uint16_t skipMask = uint16_t(cpu->gpr[8]);
    const uint32_t special4 = cpu->gpr[9], special2000 = cpu->gpr[10], special10 = Memory::Read32(cpu->gpr[1] + 8);
    uint32_t found;
    {
        // 8 cell x, 12 cell z, 16 heights (5), 36 records (5 of 20 bytes).
        GuestFrame frame(cpu, 0x90);
        GuestCall call{cpu};
        uint8_t area = uint8_t(frame.Arg(4));

        if (area == 0xFF || uint8_t(call(GetOnlineMode)) == 2)
            area = uint8_t(call(GetNowAreaNumber));

        const uint32_t ground = kGrounds + area * kGroundBytes;

        if (Memory::Read8(ground) == 0) {
            found = uint32_t(-1);
        } else if (!InBounds(pos, ground)) {
            Memory::WriteFloat32(heightOut, Memory::ReadFloat32(pos + 4));
            Memory::Write16(out + kRecordFlags, 0);
            Memory::Write8(out + kRecordKind, 0);
            Memory::Write8(out + kRecordByte3, 0);
            Memory::Write16(out + kRecordSkipBits, 0);
            Memory::Write8(out + kRecordByte6, 0);
            Memory::Write8(out + kRecordAreaBits, 0);
            Memory::WriteFloat32(out + kRecordNormal, 0.0f);
            Memory::WriteFloat32(out + kRecordNormal + 4, 1.0f);
            Memory::WriteFloat32(out + kRecordNormal + 8, 0.0f);
            found = 0;
        } else {
            const uint32_t heights = frame[16], records = frame[36];
            call(GridCandidates, {pos, ground, frame[8], frame[12]});
            const uint32_t cellX = Memory::Read32(frame[8]), cellZ = Memory::Read32(frame[12]);
            const Point p{Memory::ReadFloat32(pos), Memory::ReadFloat32(pos + 8)};
            const uint8_t areaMask = Memory::Read8(kAreaMasks + area);
            int count = 0;

            for (uint32_t layer = 0; layer < Memory::Read32(ground + kLayers) && count < kMaxTaken; ++layer) {
                for (uint32_t list = CandidateList(ground, cellX, layer, cellZ);; list += 4) {
                    const uint32_t triangle = Memory::Read32(list);

                    if (triangle == 0xFFFFFFFF)
                        break;

                    const uint16_t flags = Flags(triangle);

                    if (!(mask & flags) || (areaMask & Memory::Read8(triangle + kAreaBits)) || (skipMask & Memory::Read16(triangle + kSkipBits)) ||
                        Taken(triangle, count) || !Contains(triangle, p))
                        continue;

                    const float height = Height(triangle, pos);

                    if ((flags & kFlagSpecial4) && special4) {
                        Memory::WriteFloat32(special4, height);
                    } else if ((flags & kFlagSpecial2000) && special2000) {
                        Memory::WriteFloat32(special2000, height);
                    } else if ((flags & kFlagSpecial10) && special10) {
                        Memory::Write8(special10, 1);
                        Memory::Write8(special10 + 1, Memory::Read8(triangle + kKind));
                        Memory::WriteFloat32(special10 + 4, height);
                    } else {
                        const uint32_t record = records + kRecordBytes * count;
                        Memory::WriteFloat32(heights + 4 * count, height);
                        Memory::Write16(record + kRecordFlags, flags);
                        Memory::Write8(record + kRecordKind, Memory::Read8(triangle + kKind));
                        Memory::Write8(record + kRecordByte3, Memory::Read8(triangle + kByte3));
                        Memory::Write16(record + kRecordSkipBits, Memory::Read16(triangle + kSkipBits));
                        Memory::Write8(record + kRecordByte6, Memory::Read8(triangle + kByte6));
                        Memory::Write8(record + kRecordAreaBits, Memory::Read8(triangle + kAreaBits) & 0xF);

                        for (uint32_t i = 0; i < 12; i += 4)
                            Memory::Write32(record + kRecordNormal + i, Memory::Read32(triangle + kNormal + i));

                        Memory::Write32(kTaken + 4 * count, triangle);

                        if (++count == kMaxTaken)
                            break;
                    }
                }
            }

            if (count == 0) {
                found = 0;
            } else {
                call(PickResult, {out, pos, records, heights, heightOut, uint32_t(count)});

                // -0 (and 0) as +0.
                if (Memory::ReadFloat32(heightOut) == 0.0f)
                    Memory::WriteFloat32(heightOut, 0.0f);

                found = 1;
            }
        }
    }

    cpu->gpr[3] = found;
}

} // namespace

extern "C" void func_80296A5C(CpuContext* cpu);
extern "C" void MH3_GroundHitPointInTriangle(CpuContext* cpu) {
    if (PortCheck::Enabled("GroundHitPointInTriangle"))
        PortCheck::RunPure(cpu, "GroundHitPointInTriangle", PortCheck::Returns::Gpr3, func_80296A5C, PointInTrianglePort);
    else
        PointInTrianglePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80296A5C, MH3_GroundHitPointInTriangle, "GroundHitPointInTriangle");

extern "C" void func_802916C8(CpuContext* cpu);
extern "C" void MH3_GetGroundHit2(CpuContext* cpu) {
    if (PortCheck::Enabled("GetGroundHit2"))
        PortCheck::RunOutput(cpu, "GetGroundHit2", PortCheck::Returns::Fpr1, func_802916C8, GetGroundHit2, {{cpu->gpr[6], 1}, {kTaken, 4 * kMaxTaken}});
    else
        GetGroundHit2(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x802916C8, MH3_GetGroundHit2, "GetGroundHit2");

// An optional output for the check: absent ones become a constant neither
// side writes (the 0.0f the originals compare with).
uint32_t Given(uint32_t output) {
    return output ? output : 0x8079C7F0;
}

extern "C" void func_80291D34(CpuContext* cpu);
extern "C" void MH3_HitGroundComon(CpuContext* cpu) {
    if (PortCheck::Enabled("HitGroundComon")) {
        const uint32_t special10 = Memory::Read32(cpu->gpr[1] + 8);
        PortCheck::RunOutput(cpu, "HitGroundComon", PortCheck::Returns::Gpr3, func_80291D34, HitGroundComon,
                             {{cpu->gpr[5], kRecordBytes}, {cpu->gpr[6], 4}, {kTaken, 4 * kMaxTaken}, {Given(cpu->gpr[9]), 4},
                              {Given(cpu->gpr[10]), 4}, {Given(special10), 8}});
    } else {
        HitGroundComon(cpu);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80291D34, MH3_HitGroundComon, "HitGroundComon");
