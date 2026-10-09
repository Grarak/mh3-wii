// Land sweep collision, RMHP08, decompiled with Ghidra (Mh3Decompile and the
// Mh3Listing disassembly): a sphere swept from one point to another against
// the area's collision triangles. Hunters and monsters sweep their moves
// every step; on Jhen Mohran's deck these translated functions were among
// the game thread's hottest (devkit, 2026-10-06).
//
// The area's ground (0x806AE508 + area * 0x88, ground_hit.cpp) has two grids
// of candidate lists, one per layer of triangles: layer 0 (+0x08 flag, origin
// +0x0C, cell size +0x24, cell counts +0x30/+0x34/+0x38 for z/y/x, lists
// +0x3C) and layer 1 (+0x44 flag, origin +0x48, cell size +0x60, counts
// +0x6C/+0x70/+0x74, lists +0x78). A list is a run of triangle pointers ending
// in 0xFFFFFFFF.
//
//   0x80293AEC (from, to, f1 radius, out, u8 layers, u16 flag mask, u8 area
//     or 0xFF, u16 skip mask) -> r3: the sweep from `from` to `to`. Its
//     record (to +0, from +0xC, radius +0x18, |to - from| +0x1C, two u16 0)
//     goes to the per-triangle test 0x8029459C for each
//     candidate in the grid cells the sweep's box (the segment lengthened by
//     the radius at both ends) covers that 0x802962C4 lets through. Without
//     `out` the first hit returns 1; with it, the hits are listed at
//     0x806AED00 and 0x80294F60(record, out) (translated) gives the result.
//     -1 when the area has no ground. Clears the hit list first (count
//     0x80797018, 0x806AE4B8 10 floats, 0x806AE0E8 10 bytes).
//   0x802962C4 (triangle, tested list, tested count, u16 flag mask, u16 skip
//     mask, taken count, u8 area) -> 1 when the triangle counts: its flags
//     (0 taken as 0x20 and stored) meet the flag mask, its +4 bits miss the
//     skip mask, its +7 bits miss the area's mask (0x806BD45C + area), and it
//     is in neither the tested list nor the first taken entries at 0x806AED00.
//   0x8029459C LandSweepTriangle (record, triangle, collect) -> 1 when the
//     sweep touches the triangle: `from` in front of its plane and `to`
//     within the radius of it, the sphere's leading point (the centre
//     moved by the radius against the normal) crossing the plane inside the
//     triangle. Collecting, recorded as a hit (centre `from`, flags as below,
//     the distance travelled to the contact at 0x806AE4B8). The path where
//     the crossing falls outside (the edges as capsules, 0x802908A4) stays
//     translated: the port runs the original then, which recomputes from
//     the start (nothing before it writes).
//   0x80292524 LandSweepTriangleCandidateFilter (sweep record, triangle,
//     point) -> 1 when the sphere touches the triangle, recorded as hit
//     count 0x80797018 (at most 9): the triangle (0x806AE0F8, 0x3C each, the
//     normal's components under 0.001 zeroed), the sphere's centre
//     (0x806AE350), flags (0x806AE4EC: 1 the centre not in front of the
//     plane, 2 the plane crossed during the move), whether the contact came
//     from an edge (0x806AE4F8), the contact (0x806AE3C8) and, from an
//     edge, the point on the plane (0x806AE440).
//
// Approximations (user, 2026-10-06: "uses neon with approximations that are
// good enough, we don't need to be exact"): NEON vector math (a scalar
// fallback elsewhere) instead of the paired-single helpers' rounding; the
// edge distances compared squared (no square root); the normalizing and
// dividing done with reciprocal (square root) estimates and one Newton step;
// the edge tests' sign without normalizing the cross product.
// MH3_PORT_CHECK=LandSweepQuery,LandSweepTriangle,LandSweepTriangleCounts,
// LandSweepTriangleCandidateFilter,tolerance=1e-3 checks them against the
// originals.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#if defined(__ARM_NEON) && !defined(MH3_WRITE_WATCH)
#include <arm_neon.h>
#define MH3_SWEEP_NEON 1
#endif

namespace {

constexpr uint32_t kGrounds = 0x806AE508;   // 0x88 bytes per area
constexpr uint32_t kTaken = 0x806AED00;     // u32 triangles the sweep took
constexpr uint32_t kAreaMasks = 0x806BD45C; // u8 per area
constexpr uint32_t kHitCount = 0x80797018;  // s8
constexpr uint32_t kHitCount2 = 0x80797019; // u8, cleared with it
constexpr uint32_t kHitTriangles = 0x806AE0F8;
constexpr uint32_t kHitCentres = 0x806AE350;
constexpr uint32_t kHitFlags = 0x806AE4EC;
constexpr uint32_t kHitEdge = 0x806AE4F8;
constexpr uint32_t kHitContacts = 0x806AE3C8;
constexpr uint32_t kHitPlanePoints = 0x806AE440;
constexpr uint32_t kSweepFloats = 0x806AE4B8; // 10 floats cleared per sweep: hit distances
constexpr uint32_t kSweepBytes = 0x806AE0E8;  // 10 bytes cleared per sweep
constexpr uint32_t kTriangleBytes = 0x3C;
constexpr int kMaxHits = 9;
constexpr float kTiny = 0.001f;   // 0x8079C804
constexpr float kFar = 8000.0f;   // 0x8079C808

enum : uint32_t {                 // GuestCall targets
    GetNowAreaNo = 0x802AFCE8,
    SweepTriangle = 0x8029459C,   // (record, triangle, collect) -> 1 hit
    SweepResult = 0x80294F60,     // (record, out) -> r3
};

#if MH3_SWEEP_NEON
// A guest VEC3 in lanes 0-2 of a NEON register, lane 3 zero.
struct V3 {
    float32x4_t v;
};

V3 operator+(V3 a, V3 b) { return {vaddq_f32(a.v, b.v)}; }
V3 operator-(V3 a, V3 b) { return {vsubq_f32(a.v, b.v)}; }
V3 operator*(V3 a, float s) { return {vmulq_n_f32(a.v, s)}; }

float Dot(V3 a, V3 b) {
    const float32x4_t m = vmulq_f32(a.v, b.v);
#if defined(__aarch64__)
    return vaddvq_f32(m);
#else
    const float32x2_t pair = vpadd_f32(vget_low_f32(m), vget_high_f32(m));
    return vget_lane_f32(vpadd_f32(pair, pair), 0);
#endif
}

// (v1, v2, v0, v3)
float32x4_t Yzx(float32x4_t v) {
    const float32x2_t lo = vget_low_f32(v), hi = vget_high_f32(v);
    return vcombine_f32(vext_f32(lo, hi, 1), vrev64_f32(vext_f32(hi, lo, 1)));
}

V3 Cross(V3 a, V3 b) {
    const float32x4_t c = vmlsq_f32(vmulq_f32(a.v, Yzx(b.v)), Yzx(a.v), b.v); // (a x b) in yzx order
    return {Yzx(c)};
}

// 1/sqrt(x) and 1/x: the NEON estimates refined by one Newton step.
float RSqrt(float x) {
    const float32x2_t v = vdup_n_f32(x);
    float32x2_t e = vrsqrte_f32(v);
    e = vmul_f32(e, vrsqrts_f32(vmul_f32(v, e), e));
    return vget_lane_f32(e, 0);
}

float Recip(float x) {
    const float32x2_t v = vdup_n_f32(x);
    float32x2_t e = vrecpe_f32(v);
    e = vmul_f32(e, vrecps_f32(v, e));
    return vget_lane_f32(e, 0);
}

// Twelve big-endian bytes: eight, then four, byte-swapped per float.
V3 Load(uint32_t a) {
    const uint8_t* p = MemoryInline::PsqHost(a);
    uint32_t z;
    std::memcpy(&z, p + 8, 4);
    const uint8x16_t bytes = vcombine_u8(vld1_u8(p), vreinterpret_u8_u32(vset_lane_u32(z, vdup_n_u32(0), 0)));
    return {vreinterpretq_f32_u8(vrev32q_u8(bytes))};
}

void Store(uint32_t a, V3 v) {
    uint8_t* p = MemoryInline::PsqHost(a);
    const uint8x16_t bytes = vrev32q_u8(vreinterpretq_u8_f32(v.v));
    vst1_u8(p, vget_low_u8(bytes));
    vst1q_lane_u32(reinterpret_cast<uint32_t*>(p + 8), vreinterpretq_u32_u8(bytes), 2);
}
#else
struct V3 {
    float x, y, z;
};

V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float RSqrt(float x) { return 1.0f / std::sqrt(x); }
float Recip(float x) { return 1.0f / x; }

V3 Load(uint32_t a) {
    return {MemoryInline::FlatReadFloat32(a), MemoryInline::FlatReadFloat32(a + 4), MemoryInline::FlatReadFloat32(a + 8)};
}

void Store(uint32_t a, V3 v) {
    MemoryInline::FlatWriteFloat32(a, v.x);
    MemoryInline::FlatWriteFloat32(a + 4, v.y);
    MemoryInline::FlatWriteFloat32(a + 8, v.z);
}
#endif

float ReadF(uint32_t a) {
    return MemoryInline::FlatReadFloat32(a);
}

void Lanes(V3 v, float out[3]) {
#if MH3_SWEEP_NEON
    out[0] = vgetq_lane_f32(v.v, 0);
    out[1] = vgetq_lane_f32(v.v, 1);
    out[2] = vgetq_lane_f32(v.v, 2);
#else
    out[0] = v.x, out[1] = v.y, out[2] = v.z;
#endif
}

// |v|, 0 for a zero vector.
float Length(V3 v) {
    const float sq = Dot(v, v);
    return sq > 0.0f ? sq * RSqrt(sq) : 0.0f;
}

// ---- 0x802962C4 ----

bool Listed(uint32_t triangle, uint32_t list, uint32_t count) {
    for (; count; --count, list += 4)
        if (Memory::Read32(list) == triangle)
            return true;

    return false;
}

template <typename Tested>
uint32_t TriangleCounts(uint32_t triangle, Tested tested, uint32_t flagMask, uint32_t skipMask, uint32_t taken, uint32_t area) {
    uint16_t flags = MemoryInline::FlatRead16(triangle);

    if (flags == 0) {
        flags = 0x20;
        MemoryInline::FlatWrite16(triangle, flags);
    }

    if (!(flagMask & flags) || (skipMask & MemoryInline::FlatRead16(triangle + 4)))
        return 0;

    if (MemoryInline::FlatRead8(kAreaMasks + (area & 0xFF)) & MemoryInline::FlatRead8(triangle + 7))
        return 0;

    if (tested(triangle) || (taken && Listed(triangle, kTaken, taken)))
        return 0;

    return 1;
}

void TriangleCountsPort(CpuContext* cpu) {
    const uint32_t list = cpu->gpr[4], count = cpu->gpr[5];
    cpu->gpr[3] = TriangleCounts(cpu->gpr[3], [&](uint32_t t) { return count && Listed(t, list, count); }, cpu->gpr[6] & 0xFFFF, cpu->gpr[7] & 0xFFFF,
                                 cpu->gpr[8], cpu->gpr[9]);
}

// ---- 0x80292524 ----

// 0x80296DC8 PointInTriangle3D: p is on the inner side of all three edges
// ((p - b) x (a - b) along the normal, edges v0v1, v1v2, v2v0).
bool PointInTriangle(V3 p, uint32_t vertices, V3 normal) {
    const V3 v[3] = {Load(vertices), Load(vertices + 0xC), Load(vertices + 0x18)};

    for (int i = 0; i < 2; ++i)
        if (!(Dot(Cross(p - v[i + 1], v[i] - v[i + 1]), normal) >= 0.0f))
            return false;

    return !(Dot(Cross(p - v[0], v[2] - v[0]), normal) < 0.0f);
}

// 0x8028F99C (a, b, p, out) -> f1: the point of segment ab closest to p
// (t clamped to [0, 1]; a for a segment under 0.001 squared) and the
// squared distance to it.
float SegmentClosest(V3 a, V3 b, V3 p, V3& out) {
    const V3 d = b - a;
    V3 w = p - a;
    float t = 0.0f;

    if (Dot(d, d) >= kTiny) {
        t = Dot(w, d) * Recip(Dot(d, d));
        t = !(t >= 0.0f) ? 0.0f : t <= 1.0f ? t : 1.0f;
        w = w - d * t;
    }

    out = a + d * t;
    return Dot(w, w);
}

// Records hit `n`: the triangle (0x802929A4) with its normal cleaned
// (0x80296EB0), the centre, flags, edge byte and contact.
void Record(int n, uint32_t triangle, V3 centre, uint8_t flags, V3 contact) {
    const uint32_t copy = kHitTriangles + n * kTriangleBytes;

    for (uint32_t k = 0; k < kTriangleBytes; k += 4)
        Memory::Write32(copy + k, Memory::Read32(triangle + k));

    for (uint32_t k = 0x2C; k < 0x38; k += 4)
        if (std::fabs(ReadF(copy + k)) < kTiny)
            MemoryInline::FlatWriteFloat32(copy + k, 0.0f);

    Store(kHitCentres + n * 12, centre);
    MemoryInline::FlatWrite8(kHitFlags + n, flags);
    Store(kHitContacts + n * 12, contact);
}

uint32_t CandidateFilter(uint32_t sweep, uint32_t triangle, uint32_t point) {
    const int n = int8_t(MemoryInline::FlatRead8(kHitCount));

    if (n >= kMaxHits)
        return 0;

    if (MemoryInline::FlatRead16(sweep + 0x22) == 1) {
        const float y = ReadF(point + 4);

        if (y >= ReadF(triangle + 0xC) && y >= ReadF(triangle + 0x18) && y >= ReadF(triangle + 0x24))
            return 0;
    }

    const V3 centre = Load(sweep), other = Load(sweep + 0xC), v0 = Load(triangle + 8), normal = Load(triangle + 0x2C);

    if (!(Dot(other - v0, normal) >= 0.0f))
        return 0;

    const float distance = Dot(centre - v0, normal), radius = ReadF(sweep + 0x18);
    uint8_t flags = distance > 0.0f ? 0 : 1;

    if (!(distance <= radius))
        return 0;

    V3 onPlane;

    if (distance >= -radius) {
        onPlane = centre - normal * distance;
    } else {
        // Behind the plane by more than the radius: where the move crossed it.
        const V3 move = centre - other;
        const float along = Dot(move, normal);

        if (along == 0.0f)
            return 0;

        onPlane = centre + move * -(distance * Recip(along));
        flags |= 2;
    }

    if (PointInTriangle(onPlane, triangle + 8, normal)) {
        Record(n, triangle, centre, flags, onPlane);
        MemoryInline::FlatWrite8(kHitEdge + n, 0);
        MemoryInline::FlatWrite8(kHitCount, uint8_t(n + 1));
        return 1;
    }

    // Outside the triangle: the nearest point of its edges.
    MemoryInline::FlatWrite8(kHitEdge + n, 1);
    const V3 from = (flags & 2) ? onPlane : centre;
    const V3 v[4] = {v0, Load(triangle + 0x14), Load(triangle + 0x20), v0};
    float best = kFar * kFar; // squared distances: no square root
    V3 nearest = v0;

    for (int i = 0; i < 3; ++i) {
        V3 closest;
        const float d = SegmentClosest(v[i], v[i + 1], from, closest);

        if (d < best) {
            best = d;
            nearest = closest;
        }
    }

    if (radius < 0.0f || best >= radius * radius)
        return 0;

    Record(n, triangle, centre, flags, nearest);
    Store(kHitPlanePoints + n * 12, onPlane);
    MemoryInline::FlatWrite8(kHitCount, uint8_t(n + 1));
    return 1;
}

void CandidateFilterPort(CpuContext* cpu) {
    cpu->gpr[3] = CandidateFilter(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}

// ---- 0x8029459C ----

// 1 or 0 as the original returns, or -1 when the crossing falls outside the
// triangle (the original's capsule path).
int SweepTriangleFast(uint32_t record, uint32_t triangle, uint32_t collect) {
    const int n = int8_t(MemoryInline::FlatRead8(kHitCount));

    if (n >= kMaxHits)
        return 0;

    const V3 from = Load(record + 0xC), to = Load(record), v0 = Load(triangle + 8), normal = Load(triangle + 0x2C);

    if (!(Dot(from - v0, normal) >= 0.0f))
        return 0;

    const float distance = Dot(to - v0, normal), radius = ReadF(record + 0x18);
    uint8_t flags = distance > 0.0f ? 0 : 1;

    if (!(distance <= radius))
        return 0;

    if (distance < -radius)
        flags |= 2;

    // 0x80290C8C: where the leading point's move crosses the plane.
    const V3 down = normal * -radius, start = from + down, move = (to + down) - start;
    const float t = (Dot(normal, v0) - Dot(normal, start)) * Recip(Dot(normal, move));

    if (t < 0.0f || t > 1.0f)
        return -1;

    const V3 hit = start + move * t;

    if (!PointInTriangle(hit, triangle + 8, normal))
        return -1;

    if (!(collect & 0xFF))
        return 1;

    Record(n, triangle, from, flags, hit);
    MemoryInline::FlatWrite8(kHitEdge + n, 0);
    MemoryInline::FlatWriteFloat32(kSweepFloats + n * 4, Length(hit - start));
    MemoryInline::FlatWrite8(kHitCount, uint8_t(n + 1));
    return 1;
}

extern "C" void func_8029459C(CpuContext* cpu);

void SweepTrianglePort(CpuContext* cpu) {
    const int hit = SweepTriangleFast(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);

    if (hit >= 0)
        cpu->gpr[3] = uint32_t(hit);
    else
        func_8029459C(cpu);
}

// ---- 0x80293AEC ----

struct Layer {
    uint32_t flag, origin, size, countZ, countY, countX, lists;
};

constexpr Layer kLayers[2] = {{0x08, 0x0C, 0x24, 0x30, 0x34, 0x38, 0x3C}, {0x44, 0x48, 0x60, 0x6C, 0x70, 0x74, 0x78}};

// 0x802971A0 (layer 0), 0x802974F4 (layer 1): the cells the box between
// corners a and b covers, as a start and a count per axis, the counts
// capped by the grid's (the z count by the x axis's, as in the original).
// A grid with its flag set measures from the origin in float, and both
// corners from a (as the original does).
void CellRange(V3 a, V3 b, uint32_t ground, const Layer& l, int32_t start[3], uint32_t count[3]) {
    const int32_t size[3] = {int32_t(Memory::Read32(ground + l.size)), int32_t(Memory::Read32(ground + l.size + 4)), int32_t(Memory::Read32(ground + l.size + 8))};
    const V3 origin = Load(ground + l.origin);
    int32_t lo[3], hi[3];

    if (MemoryInline::FlatRead8(ground + l.flag) == 0) {
        float pa[3], pb[3], po[3];
        Lanes(a, pa);
        Lanes(b, pb);
        Lanes(origin, po);

        for (int i = 0; i < 3; ++i) {
            lo[i] = (int32_t(std::floor(pa[i])) - int32_t(po[i])) / size[i];
            hi[i] = (int32_t(std::floor(pb[i])) - int32_t(po[i])) / size[i];
        }
    } else {
        float pd[3];
        Lanes(a - origin, pd);

        for (int i = 0; i < 3; ++i)
            lo[i] = hi[i] = int32_t(pd[i] / float(uint32_t(size[i])));
    }

    const uint32_t limit[3] = {Memory::Read32(ground + l.countX), Memory::Read32(ground + l.countY), Memory::Read32(ground + l.countX)};
    const uint32_t cap[3] = {Memory::Read32(ground + l.countX), Memory::Read32(ground + l.countY), Memory::Read32(ground + l.countZ)};

    for (int i = 0; i < 3; ++i) {
        int32_t delta = hi[i] - lo[i];
        start[i] = lo[i];

        if (delta < 0) {
            start[i] += delta;
            delta = -delta;
        }

        count[i] = uint32_t(delta + 1);

        if (cap[i] < count[i])
            count[i] = limit[i];
    }
}

// 0x80296460/0x80296414: whether the cell is in the grid; 0x80296A34/
// 0x80296A0C: its list.
bool CellIn(uint32_t ground, const Layer& l, int32_t x, int32_t y, int32_t z) {
    return uint32_t(x) < Memory::Read32(ground + l.countX) && x >= 0 && uint32_t(y) < Memory::Read32(ground + l.countY) && y >= 0 &&
           uint32_t(z) < Memory::Read32(ground + l.countZ) && z >= 0;
}

uint32_t CellList(uint32_t ground, const Layer& l, int32_t x, int32_t y, int32_t z) {
    const uint32_t index = Memory::Read32(ground + l.countZ) * (y + x * Memory::Read32(ground + l.countY)) + z;
    return Memory::Read32(Memory::Read32(ground + l.lists) + index * 4);
}

void SweepQuery(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x40);
    GuestCall game{cpu};
    const uint32_t from = frame.Arg(3), to = frame.Arg(4), out = frame.Arg(5), layers = frame.Arg(6) & 0xFF;
    const uint32_t flagMask = frame.Arg(7) & 0xFFFF, skipMask = frame.Arg(9) & 0xFFFF;
    const float radius = float(cpu->fpr[1].d);
    uint32_t area = frame.Arg(8);
    const uint32_t collect = out != 0;

    if ((area & 0xFF) == 0xFF)
        area = game.Call<GetNowAreaNo>();

    const uint32_t ground = kGrounds + (area & 0xFF) * 0x88;

    if (MemoryInline::FlatRead8(ground) == 0 && MemoryInline::FlatRead8(ground + 1) == 0) {
        cpu->gpr[3] = uint32_t(-1);
        return;
    }

    const V3 a = Load(from), b = Load(to);
    V3 dir = b - a;

    if (Dot(dir, dir) > kTiny)
        dir = dir * RSqrt(Dot(dir, dir));

    const V3 boxB = b + dir * radius, boxA = a + dir * -radius;

    for (uint32_t i = 0; i < 10; ++i) {
        MemoryInline::FlatWriteFloat32(kSweepFloats + i * 4, 0.0f);
        MemoryInline::FlatWrite8(kSweepBytes + i, 0);
    }

    MemoryInline::FlatWrite8(kHitCount2, 0);
    MemoryInline::FlatWrite8(kHitCount, 0);

    // The sweep record the triangle test reads.
    const uint32_t record = frame[0x10];
    Store(record, b);
    Store(record + 0xC, a);
    MemoryInline::FlatWriteFloat32(record + 0x18, radius);
    MemoryInline::FlatWriteFloat32(record + 0x1C, Length(b - a));
    MemoryInline::FlatWrite16(record + 0x20, 0);
    MemoryInline::FlatWrite16(record + 0x22, 0);

    uint32_t tested[10] = {}, testedCount = 0, taken = 0;

    for (int which = 0; which < 2; ++which) {
        if (!(layers & (1u << which)))
            continue;

        const Layer& l = kLayers[which];
        int32_t start[3];
        uint32_t count[3];
        CellRange(boxB, boxA, ground, l, start, count);

        for (int32_t i = 0; i < int32_t(count[0]); ++i)
            for (int32_t j = 0; j < int32_t(count[1]); ++j)
                for (int32_t k = 0; k < int32_t(count[2]); ++k) {
                    const int32_t x = start[0] + i, y = start[1] + j, z = start[2] + k;

                    if (!CellIn(ground, l, x, y, z))
                        continue;

                    for (uint32_t list = CellList(ground, l, x, y, z);; list += 4) {
                        const uint32_t triangle = Memory::Read32(list);

                        if (triangle == 0xFFFFFFFF)
                            break;

                        const auto inTested = [&](uint32_t t) { return std::find(tested, tested + testedCount, t) != tested + testedCount; };

                        if (!TriangleCounts(triangle, inTested, flagMask, skipMask, taken, area))
                            continue;

                        int hit = SweepTriangleFast(record, triangle, collect);

                        if (hit < 0)
                            hit = int(game.Call<SweepTriangle>({record, triangle, collect}));

                        if (hit != 0) {
                            if (!collect) {
                                cpu->gpr[3] = 1;
                                return;
                            }

                            Memory::Write32(kTaken + taken++ * 4, triangle);
                        }

                        tested[testedCount++] = triangle;

                        if (testedCount >= 10)
                            testedCount = 0;
                    }
                }
    }

    cpu->gpr[3] = out ? game.Call<SweepResult>({record, out}) : 0;
}

} // namespace

extern "C" void func_802962C4(CpuContext* cpu);
extern "C" void MH3_LandSweepTriangleCounts(CpuContext* cpu) {
    if (PortCheck::Enabled("LandSweepTriangleCounts"))
        PortCheck::Run(cpu, "LandSweepTriangleCounts", func_802962C4, TriangleCountsPort);
    else
        TriangleCountsPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x802962C4, MH3_LandSweepTriangleCounts, "LandSweepTriangleCounts");

extern "C" void func_80292524(CpuContext* cpu);
extern "C" void MH3_LandSweepTriangleCandidateFilter(CpuContext* cpu) {
    if (PortCheck::Enabled("LandSweepTriangleCandidateFilter"))
        PortCheck::Run(cpu, "LandSweepTriangleCandidateFilter", func_80292524, CandidateFilterPort);
    else
        CandidateFilterPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80292524, MH3_LandSweepTriangleCandidateFilter, "LandSweepTriangleCandidateFilter");

extern "C" void MH3_LandSweepTriangle(CpuContext* cpu) {
    if (PortCheck::Enabled("LandSweepTriangle"))
        PortCheck::Run(cpu, "LandSweepTriangle", func_8029459C, SweepTrianglePort);
    else
        SweepTrianglePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8029459C, MH3_LandSweepTriangle, "LandSweepTriangle");

extern "C" void func_80293AEC(CpuContext* cpu);
extern "C" void MH3_LandSweepQuery(CpuContext* cpu) {
    if (PortCheck::Enabled("LandSweepQuery"))
        PortCheck::Run(cpu, "LandSweepQuery", func_80293AEC, SweepQuery);
    else
        SweepQuery(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80293AEC, MH3_LandSweepQuery, "LandSweepQuery");
