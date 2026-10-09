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
//
// With NEON, a candidate's point-in-triangle test runs its four signed
// areas (the triangle's and the point's against each edge) in the lanes of
// one register, on the corners loaded and byte-swapped in one go, and the
// candidate loop reads the triangles through their host pointers: the same
// products, differences and comparisons as the original, without the scalar
// compare and status-register move per area.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>
#if defined(__ARM_NEON) && !defined(MH3_WRITE_WATCH)
#include <arm_neon.h>
#define MH3_GROUND_NEON 1
#endif

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
        if (MemoryInline::FlatRead32(kTaken + 4 * i) == triangle)
            return true;
    return false;
}

// Whether the triangle's normal points up (b > 0), on the float's bits: an
// integer compare instead of a float one and its status-register move.
bool FacesUp(uint32_t triangle) {
    return MemoryInline::FlatRead32(triangle + kNormal + 4) - 1 < 0x7F800000u;
}

// Starts loading the next candidate (its header, corners and normal span two
// cache lines) while this one is tested.
void PrefetchNext(uint32_t list) {
    const uint32_t next = MemoryInline::FlatRead32(list + 4);

    if (next != 0xFFFFFFFF) {
        __builtin_prefetch(GuestFlat::Host(next));
        __builtin_prefetch(GuestFlat::Host(next + 0x3B));
    }
}

// The flags, with 0 stored as 0x20 the first time.
uint16_t Flags(uint32_t triangle) {
    const uint16_t flags = MemoryInline::FlatRead16(triangle + kFlags);

    if (flags != 0)
        return flags;

    Memory::Write16(triangle + kFlags, 0x20);
    return 0x20;
}

#if MH3_GROUND_NEON
// The point as x and z lanes.
struct Probe {
    float32x2_t x, z;
};

Probe ProbeOf(Point p) {
    return {vdup_n_f32(p.x), vdup_n_f32(p.z)};
}

// Lanes (a, a, b, c), (b, b, c, a) and (c, p, p, p) of the corners a, b, c
// (lanes 0-2 of v) and the point: the four signed areas' operands.
struct Operands {
    float32x4_t a, b, c;
};

Operands OperandsOf(float32x4_t v, float32x2_t p) {
    const float32x2_t lo = vget_low_f32(v), hi = vget_high_f32(v);
    return {vcombine_f32(vdup_lane_f32(lo, 0), vext_f32(lo, hi, 1)), vcombine_f32(vdup_lane_f32(lo, 1), vtrn_f32(hi, lo).val[0]),
            vcombine_f32(vtrn_f32(hi, p).val[0], p)};
}

// Inside on the triangle's corners: SignedArea of (a, b, c), (a, b, p),
// (b, c, p) and (c, a, p) in lanes 0-3, then Classify's sign tests.
bool Contains(uint32_t triangle, Probe p) {
    // x0 x1 x2 and z0 z1 z2 in lanes 0-2 (lane 3: the normal's a and c).
    const uint32x4x3_t corners = vld3q_u32(reinterpret_cast<const uint32_t*>(GuestFlat::Host(triangle + kVertices)));
    const float32x4_t xs = vreinterpretq_f32_u8(vrev32q_u8(vreinterpretq_u8_u32(corners.val[0])));
    const float32x4_t zs = vreinterpretq_f32_u8(vrev32q_u8(vreinterpretq_u8_u32(corners.val[2])));
    const Operands x = OperandsOf(xs, p.x), z = OperandsOf(zs, p.z);
    float32x4_t ps = vmulq_f32(vsubq_f32(x.b, x.a), vsubq_f32(z.c, z.a));
    const float32x4_t qs = vmulq_f32(vsubq_f32(x.c, x.a), vsubq_f32(z.b, z.a));

    // Products one unit in the last place apart (or equal) count as equal.
    const uint32x4_t apart = vsubq_u32(vreinterpretq_u32_f32(ps), vreinterpretq_u32_f32(qs));
    ps = vbslq_f32(vcleq_u32(vaddq_u32(apart, vdupq_n_u32(1)), vdupq_n_u32(2)), qs, ps);
    const float32x4_t areas = vsubq_f32(ps, qs);

    // Bytes 0-3: area > 0 per lane; 4-7: area < 0.
    const uint16x4_t above = vmovn_u32(vcgtq_f32(areas, vdupq_n_f32(0.0f)));
    const uint16x4_t below = vmovn_u32(vcltq_f32(areas, vdupq_n_f32(0.0f)));
    const uint64_t signs = vget_lane_u64(vreinterpret_u64_u8(vmovn_u16(vcombine_u16(above, below))), 0);

    if (signs & 0xFF)
        return (signs & 0xFFFFFF0000000000ull) == 0;

    if (signs & 0xFF00000000ull)
        return (signs & 0xFFFFFF00ull) == 0;

    return false;
}
#else
using Probe = Point;

Probe ProbeOf(Point p) {
    return p;
}

bool Contains(uint32_t triangle, Point p) {
    const Point v[3] = {Corner(triangle + kVertices), Corner(triangle + kVertices + 12), Corner(triangle + kVertices + 24)};
    return Inside(v, p);
}
#endif

#if MH3_GROUND_NEON
// The candidate lists as their triangles' XZ boxes in native memory, four to
// a group: a query compares the point with four boxes at once and reads only
// the triangles whose box holds it (in the Tundra 2-3 of about 21 candidates
// a query; each triangle spans two or three cache lines, which on the A9
// mostly miss). A box is widened so that outside it Contains is false:
//
//   the area tests' rounding (the differences, the products, the one unit
//   apart taken as equal) stays under 2^-20 of the products, which are at
//   most an edge times the point's distance to a corner; outside the
//   triangle by `dist`, some edge has the point outside it by at least
//   dist * sin(smallest angle / 2) (the triangle grown by d along its edges
//   reaches d / sin(angle / 2) past its corners). So a margin of the longest
//   edge * 2^-16 / sin(smallest angle) puts that edge's area well past the
//   rounding, with the right sign (and the triangle's own area too).
//
// Triangles thinner than a sine of 1/1000, with corners beyond 1e20 or not
// finite, and with flags 0 (Flags stores 0x20 for those when tested) get an
// unbounded box, and points beyond 1e30 or NaN skip the boxes: those take
// every candidate as before. Lists are cached per area; an area's ground
// header (0x88 bytes) changing (another map, the ground reloaded) drops all.
class Boxes {
  public:
    struct alignas(16) Group {
        float minX[4], maxX[4], minZ[4], maxZ[4];
        uint32_t triangles[4];
    };

    // Takes the cache for one query on the area's ground; false when another
    // thread has it or the area has no snapshot slot.
    bool Claim(uint8_t area, uint32_t ground) {
        if (area >= kAreas || busy_.test_and_set(std::memory_order_acquire))
            return false;

        const uint8_t* header = GuestFlat::Host(ground);

        if (!known_[area] || std::memcmp(headers_[area], header, kGroundBytes) != 0) {
            Clear();
            std::memcpy(headers_[area], header, kGroundBytes);
            known_[area] = true;
        }

        area_ = area;
        return true;
    }

    void Release() {
        busy_.clear(std::memory_order_release);
    }

    // The list's groups (`count` of them), built on first use; null for a
    // list too long to cache.
    const Group* Find(uint32_t list, uint32_t& count) {
        uint32_t i = Hash(list);

        for (;; i = (i + 1) & (kSlots - 1)) {
            const Slot& slot = slots_[i];

            if (slot.list == list && slot.area == area_) {
                count = slot.count;
                return groups_.data() + slot.first;
            }

            if (slot.list == 0)
                break;
        }

        uint32_t n = 0;

        while (n < kMaxListed && MemoryInline::FlatRead32(list + 4 * n) != 0xFFFFFFFF)
            ++n;

        if (n == kMaxListed)
            return nullptr;

        count = (n + 3) / 4;

        if (used_ == kMaxSlots || groups_.size() + count > kMaxGroups) {
            Clear();
            i = Hash(list);
        }

        if (groups_.capacity() == 0)
            groups_.reserve(kMaxGroups);

        const uint32_t first = uint32_t(groups_.size());
        groups_.resize(first + count);

        for (uint32_t k = 0; k < 4 * count; ++k)
            SetBox(groups_[first + k / 4], k % 4, k < n ? MemoryInline::FlatRead32(list + 4 * k) : 0);

        while (slots_[i].list != 0)
            i = (i + 1) & (kSlots - 1);

        slots_[i] = {list, uint16_t(first), uint8_t(count), area_};
        ++used_;
        return groups_.data() + first;
    }

  private:
    static constexpr uint32_t kAreas = 15; // 0x802970F0 clears 15 grounds
    static constexpr uint32_t kSlots = 2048;
    static constexpr uint32_t kMaxSlots = kSlots * 3 / 4;
    static constexpr uint32_t kMaxGroups = 4096;
    static constexpr uint32_t kMaxListed = 4 * 255;

    struct Slot {
        uint32_t list;
        uint16_t first;
        uint8_t count, area;
    };

    static uint32_t Hash(uint32_t list) {
        return ((list >> 2) * 0x9E3779B1u) >> 21;
    }

    void Clear() {
        std::memset(slots_, 0, sizeof(slots_));
        used_ = 0;
        groups_.clear();
    }

    // Lane `lane` of the group: the triangle's widened box, unbounded for
    // the triangles above, empty for no triangle (0, the last group's rest).
    static void SetBox(Group& group, int lane, uint32_t triangle) {
        constexpr float kInf = std::numeric_limits<float>::infinity();
        group.triangles[lane] = triangle;

        if (triangle == 0) {
            group.minX[lane] = group.minZ[lane] = kInf;
            group.maxX[lane] = group.maxZ[lane] = -kInf;
            return;
        }

        group.minX[lane] = group.minZ[lane] = -kInf;
        group.maxX[lane] = group.maxZ[lane] = kInf;

        if (MemoryInline::FlatRead16(triangle + kFlags) == 0)
            return;

        double x[3], z[3];

        for (int i = 0; i < 3; ++i) {
            x[i] = Memory::ReadFloat32(triangle + kVertices + 12 * i);
            z[i] = Memory::ReadFloat32(triangle + kVertices + 12 * i + 8);

            if (!(std::fabs(x[i]) < 1e20 && std::fabs(z[i]) < 1e20))
                return;
        }

        double edges[3];

        for (int i = 0; i < 3; ++i)
            edges[i] = std::hypot(x[(i + 1) % 3] - x[i], z[(i + 1) % 3] - z[i]);

        std::sort(edges, edges + 3);
        const double area = std::fabs((x[1] - x[0]) * (z[2] - z[0]) - (x[2] - x[0]) * (z[1] - z[0]));

        // The smallest angle's sine: twice the area over the two longest edges.
        const double sine = edges[0] > 0.0 ? area / (edges[1] * edges[2]) : 0.0;

        if (!(sine >= 1e-3))
            return;

        const double margin = edges[2] * 0x1p-16 / sine + 0x1p-10;
        const auto below = [](double v) { return std::nextafter(float(v), -kInf); };
        const auto above = [](double v) { return std::nextafter(float(v), kInf); };
        group.minX[lane] = below(std::min({x[0], x[1], x[2]}) - margin);
        group.maxX[lane] = above(std::max({x[0], x[1], x[2]}) + margin);
        group.minZ[lane] = below(std::min({z[0], z[1], z[2]}) - margin);
        group.maxZ[lane] = above(std::max({z[0], z[1], z[2]}) + margin);
    }

    std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
    uint8_t area_ = 0;
    bool known_[kAreas]{};
    uint8_t headers_[kAreas][kGroundBytes]{};
    Slot slots_[kSlots]{};
    uint32_t used_ = 0;
    std::vector<Group> groups_;
};

Boxes boxCache;

// Starts loading a triangle's three cache lines.
void Prefetch(uint32_t triangle) {
    const uint8_t* host = GuestFlat::Host(triangle);
    __builtin_prefetch(host);
    __builtin_prefetch(host + 32);
    __builtin_prefetch(host + 0x3B);
}
#endif

// One query's walk over its cell's lists: through the box cache when it can
// have it, else every candidate.
class Candidates {
  public:
    Candidates(uint8_t area, uint32_t ground, Point point) {
#if MH3_GROUND_NEON
        if (std::fabs(point.x) < 1e30f && std::fabs(point.z) < 1e30f && boxCache.Claim(area, ground)) {
            boxes_ = &boxCache;
            x_ = vdupq_n_f32(point.x);
            z_ = vdupq_n_f32(point.z);
        }
#endif
    }

    ~Candidates() {
#if MH3_GROUND_NEON
        if (boxes_)
            boxes_->Release();
#endif
    }

    Candidates(const Candidates&) = delete;
    Candidates& operator=(const Candidates&) = delete;

    // Runs `take` on the list's candidates in order until it returns true.
    template <typename Take>
    void Scan(uint32_t list, Take&& take) const {
#if MH3_GROUND_NEON
        uint32_t count;

        if (const Boxes::Group* group = boxes_ ? boxes_->Find(list, count) : nullptr) {
            for (const Boxes::Group* end = group + count; group != end; ++group) {
                const uint32x4_t inX = vandq_u32(vcgeq_f32(x_, vld1q_f32(group->minX)), vcleq_f32(x_, vld1q_f32(group->maxX)));
                const uint32x4_t inZ = vandq_u32(vcgeq_f32(z_, vld1q_f32(group->minZ)), vcleq_f32(z_, vld1q_f32(group->maxZ)));
                uint64_t lanes = vget_lane_u64(vreinterpret_u64_u16(vmovn_u32(vandq_u32(inX, inZ))), 0) & 0x0001000100010001ull;

                for (uint64_t ahead = lanes; ahead; ahead &= ahead - 1)
                    Prefetch(group->triangles[std::countr_zero(ahead) / 16]);

                for (; lanes; lanes &= lanes - 1)
                    if (take(group->triangles[std::countr_zero(lanes) / 16]))
                        return;
            }

            return;
        }
#endif

        for (;; list += 4) {
            const uint32_t triangle = MemoryInline::FlatRead32(list);

            if (triangle == 0xFFFFFFFF || (PrefetchNext(list), take(triangle)))
                return;
        }
    }

  private:
#if MH3_GROUND_NEON
    Boxes* boxes_ = nullptr;
    float32x4_t x_, z_;
#endif
};

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
        const Point point{Memory::ReadFloat32(pos), Memory::ReadFloat32(pos + 8)};
        const Probe p = ProbeOf(point);
        const uint8_t areaMask = Memory::Read8(kAreaMasks + area);
        const Candidates candidates(area, ground, point);
        int count = 0;

        for (uint32_t layer = 0; layer < Memory::Read32(ground + kLayers) && count < kMaxTaken; ++layer) {
            candidates.Scan(CandidateList(ground, cellX, layer, cellZ), [&](uint32_t triangle) {
                if (areaMask & MemoryInline::FlatRead8(triangle + kAreaBits))
                    return false;

                if (!(mask & Flags(triangle)) || !FacesUp(triangle) || Taken(triangle, count) || !Contains(triangle, p))
                    return false;

                Memory::Write32(kTaken + 4 * count, triangle);
                heights[count++] = Height(triangle, pos);
                return count == kMaxTaken;
            });
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
            const Point point{Memory::ReadFloat32(pos), Memory::ReadFloat32(pos + 8)};
            const Probe p = ProbeOf(point);
            const uint8_t areaMask = Memory::Read8(kAreaMasks + area);
            const Candidates candidates(area, ground, point);
            int count = 0;

            for (uint32_t layer = 0; layer < Memory::Read32(ground + kLayers) && count < kMaxTaken; ++layer) {
                candidates.Scan(CandidateList(ground, cellX, layer, cellZ), [&](uint32_t triangle) {
                    const uint16_t flags = Flags(triangle);

                    if (!(mask & flags) || (areaMask & MemoryInline::FlatRead8(triangle + kAreaBits)) ||
                        (skipMask & MemoryInline::FlatRead16(triangle + kSkipBits)) ||
                        Taken(triangle, count) || !Contains(triangle, p))
                        return false;

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
                        return ++count == kMaxTaken;
                    }

                    return false;
                });
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
