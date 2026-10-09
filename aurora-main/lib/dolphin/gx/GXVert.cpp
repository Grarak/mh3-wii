#include <aurora/legacy_gx.h>
#include "gx.hpp"
#include "__gx.h"
#include "../../gx/fifo.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

// Track vertex count between GXBegin/GXEnd for mismatch detection
static u16 sBeginNVerts = 0;
static u32 sBeginFifoSize = 0;
static GXPrimitive sBeginPrimitive = GX_TRIANGLES;
static GXVtxFmt sBeginVtxFmt = GX_VTXFMT0;
static bool sInBegin = false;
static GXAttr sWriteAttr = GX_VA_NULL;
static u32 sWriteAttrComps = 0;

namespace {

static u8 normal_frac_bits(GXCompType type) {
  switch (type) {
  case GX_U8:
    return 7;
  case GX_S8:
    return 6;
  case GX_U16:
    return 15;
  case GX_S16:
    return 14;
  default:
    return 0;
  }
}

struct ActiveAttrFmt {
  GXCompCnt cnt = GX_POS_XYZ;
  GXCompType type = GX_F32;
  u8 frac = 0;
};

enum class ScalarKind {
  Float,
  Signed,
  Unsigned,
};

struct Scalar {
  ScalarKind kind = ScalarKind::Float;
  float f = 0.0f;
  int32_t s = 0;
  uint32_t u = 0;
};

static Scalar scalar_f32(float value) {
  return Scalar{.kind = ScalarKind::Float, .f = value, .s = static_cast<int32_t>(value), .u = static_cast<uint32_t>(value)};
}

static Scalar scalar_u32(uint32_t value) {
  return Scalar{.kind = ScalarKind::Unsigned, .f = static_cast<float>(value), .s = static_cast<int32_t>(value), .u = value};
}

static Scalar scalar_s32(int32_t value) {
  return Scalar{.kind = ScalarKind::Signed, .f = static_cast<float>(value), .s = value, .u = static_cast<uint32_t>(value)};
}

static bool is_matrix_index_attr(GXAttr attr) {
  return attr >= GX_VA_PNMTXIDX && attr <= GX_VA_TEX7MTXIDX;
}

static bool is_color_attr(GXAttr attr) {
  return attr == GX_VA_CLR0 || attr == GX_VA_CLR1;
}

static bool is_tex_attr(GXAttr attr) {
  return attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7;
}

static GXAttrType active_attr_type(GXAttr attr) {
  switch (attr) {
  case GX_VA_PNMTXIDX:
  case GX_VA_TEX0MTXIDX:
  case GX_VA_TEX1MTXIDX:
  case GX_VA_TEX2MTXIDX:
  case GX_VA_TEX3MTXIDX:
  case GX_VA_TEX4MTXIDX:
  case GX_VA_TEX5MTXIDX:
  case GX_VA_TEX6MTXIDX:
  case GX_VA_TEX7MTXIDX:
    return GET_REG_FIELD(__gx->vcdLo, 1, underlying(attr)) ? GX_DIRECT : GX_NONE;
  case GX_VA_POS:
    return static_cast<GXAttrType>(GET_REG_FIELD(__gx->vcdLo, 2, 9));
  case GX_VA_NRM:
    return (__gx->hasNrms || __gx->hasBiNrms) ? static_cast<GXAttrType>(GET_REG_FIELD(__gx->vcdLo, 2, 11)) : GX_NONE;
  case GX_VA_CLR0:
    return static_cast<GXAttrType>(GET_REG_FIELD(__gx->vcdLo, 2, 13));
  case GX_VA_CLR1:
    return static_cast<GXAttrType>(GET_REG_FIELD(__gx->vcdLo, 2, 15));
  case GX_VA_TEX0:
    return static_cast<GXAttrType>(GET_REG_FIELD(__gx->vcdHi, 2, 0));
  case GX_VA_TEX1:
    return static_cast<GXAttrType>(GET_REG_FIELD(__gx->vcdHi, 2, 2));
  case GX_VA_TEX2:
    return static_cast<GXAttrType>(GET_REG_FIELD(__gx->vcdHi, 2, 4));
  case GX_VA_TEX3:
    return static_cast<GXAttrType>(GET_REG_FIELD(__gx->vcdHi, 2, 6));
  case GX_VA_TEX4:
    return static_cast<GXAttrType>(GET_REG_FIELD(__gx->vcdHi, 2, 8));
  case GX_VA_TEX5:
    return static_cast<GXAttrType>(GET_REG_FIELD(__gx->vcdHi, 2, 10));
  case GX_VA_TEX6:
    return static_cast<GXAttrType>(GET_REG_FIELD(__gx->vcdHi, 2, 12));
  case GX_VA_TEX7:
    return static_cast<GXAttrType>(GET_REG_FIELD(__gx->vcdHi, 2, 14));
  default:
    return GX_NONE;
  }
}

static ActiveAttrFmt active_attr_fmt(GXVtxFmt fmt, GXAttr attr) {
  ActiveAttrFmt out{};
  const u32 va = __gx->vatA[fmt];
  const u32 vb = __gx->vatB[fmt];
  const u32 vc = __gx->vatC[fmt];

  switch (attr) {
  case GX_VA_POS:
    out.cnt = GET_REG_FIELD(va, 1, 0) ? GX_POS_XYZ : GX_POS_XY;
    out.type = static_cast<GXCompType>(GET_REG_FIELD(va, 3, 1));
    out.frac = static_cast<u8>(GET_REG_FIELD(va, 5, 4));
    break;
  case GX_VA_NRM:
    out.type = static_cast<GXCompType>(GET_REG_FIELD(va, 3, 10));
    if (GET_REG_FIELD(va, 1, 31)) {
      out.cnt = GX_NRM_NBT3;
    } else {
      out.cnt = GET_REG_FIELD(va, 1, 9) ? GX_NRM_NBT : GX_NRM_XYZ;
    }
    out.frac = normal_frac_bits(out.type);
    break;
  case GX_VA_CLR0:
    out.cnt = GET_REG_FIELD(va, 1, 13) ? GX_CLR_RGBA : GX_CLR_RGB;
    out.type = static_cast<GXCompType>(GET_REG_FIELD(va, 3, 14));
    out.frac = 0;
    break;
  case GX_VA_CLR1:
    out.cnt = GET_REG_FIELD(va, 1, 17) ? GX_CLR_RGBA : GX_CLR_RGB;
    out.type = static_cast<GXCompType>(GET_REG_FIELD(va, 3, 18));
    out.frac = 0;
    break;
  case GX_VA_TEX0:
    out.cnt = GET_REG_FIELD(va, 1, 21) ? GX_TEX_ST : GX_TEX_S;
    out.type = static_cast<GXCompType>(GET_REG_FIELD(va, 3, 22));
    out.frac = static_cast<u8>(GET_REG_FIELD(va, 5, 25));
    break;
  case GX_VA_TEX1:
    out.cnt = GET_REG_FIELD(vb, 1, 0) ? GX_TEX_ST : GX_TEX_S;
    out.type = static_cast<GXCompType>(GET_REG_FIELD(vb, 3, 1));
    out.frac = static_cast<u8>(GET_REG_FIELD(vb, 5, 4));
    break;
  case GX_VA_TEX2:
    out.cnt = GET_REG_FIELD(vb, 1, 9) ? GX_TEX_ST : GX_TEX_S;
    out.type = static_cast<GXCompType>(GET_REG_FIELD(vb, 3, 10));
    out.frac = static_cast<u8>(GET_REG_FIELD(vb, 5, 13));
    break;
  case GX_VA_TEX3:
    out.cnt = GET_REG_FIELD(vb, 1, 18) ? GX_TEX_ST : GX_TEX_S;
    out.type = static_cast<GXCompType>(GET_REG_FIELD(vb, 3, 19));
    out.frac = static_cast<u8>(GET_REG_FIELD(vb, 5, 22));
    break;
  case GX_VA_TEX4:
    out.cnt = GET_REG_FIELD(vb, 1, 27) ? GX_TEX_ST : GX_TEX_S;
    out.type = static_cast<GXCompType>(GET_REG_FIELD(vb, 3, 28));
    out.frac = static_cast<u8>(GET_REG_FIELD(vc, 5, 0));
    break;
  case GX_VA_TEX5:
    out.cnt = GET_REG_FIELD(vc, 1, 5) ? GX_TEX_ST : GX_TEX_S;
    out.type = static_cast<GXCompType>(GET_REG_FIELD(vc, 3, 6));
    out.frac = static_cast<u8>(GET_REG_FIELD(vc, 5, 9));
    break;
  case GX_VA_TEX6:
    out.cnt = GET_REG_FIELD(vc, 1, 14) ? GX_TEX_ST : GX_TEX_S;
    out.type = static_cast<GXCompType>(GET_REG_FIELD(vc, 3, 15));
    out.frac = static_cast<u8>(GET_REG_FIELD(vc, 5, 18));
    break;
  case GX_VA_TEX7:
    out.cnt = GET_REG_FIELD(vc, 1, 23) ? GX_TEX_ST : GX_TEX_S;
    out.type = static_cast<GXCompType>(GET_REG_FIELD(vc, 3, 24));
    out.frac = static_cast<u8>(GET_REG_FIELD(vc, 5, 27));
    break;
  default:
    break;
  }
  return out;
}

static u32 expected_component_count(GXAttr attr, const ActiveAttrFmt& fmt, GXAttrType type) {
  if (is_matrix_index_attr(attr) || is_color_attr(attr)) {
    return 1;
  }
  if (type == GX_INDEX8 || type == GX_INDEX16) {
    return (attr == GX_VA_NRM && fmt.cnt == GX_NRM_NBT3) ? 3u : 1u;
  }
  if (attr == GX_VA_POS) {
    return fmt.cnt == GX_POS_XY ? 2u : 3u;
  }
  if (attr == GX_VA_NRM) {
    return (fmt.cnt == GX_NRM_NBT || fmt.cnt == GX_NRM_NBT3) ? 9u : 3u;
  }
  if (is_tex_attr(attr)) {
    return fmt.cnt == GX_TEX_S ? 1u : 2u;
  }
  return 1;
}

  // Cache decoded vertex settings and refresh them when their GX registers change.
struct AttrCacheEntry {
  ActiveAttrFmt fmt{};
  GXAttrType type = GX_NONE;
  u32 expected = 1;
  GXAttr next = GX_VA_NULL;
};

struct VtxStateCache {
  bool valid = false;
  GXVtxFmt vtxFmt = GX_VTXFMT0;
  u32 vcdLo = 0;
  u32 vcdHi = 0;
  u32 vatA = 0;
  u32 vatB = 0;
  u32 vatC = 0;
  u8 hasNrms = 0;
  u8 hasBiNrms = 0;
  GXAttr first = GX_VA_NULL;
  std::array<AttrCacheEntry, GX_VA_MAX_ATTR> attrs{};
};

static VtxStateCache sVtxCache;

static void rebuild_vtx_cache(GXVtxFmt vtxFmt) {
  sVtxCache.valid = true;
  sVtxCache.vtxFmt = vtxFmt;
  sVtxCache.vcdLo = __gx->vcdLo;
  sVtxCache.vcdHi = __gx->vcdHi;
  sVtxCache.vatA = __gx->vatA[vtxFmt];
  sVtxCache.vatB = __gx->vatB[vtxFmt];
  sVtxCache.vatC = __gx->vatC[vtxFmt];
  sVtxCache.hasNrms = __gx->hasNrms;
  sVtxCache.hasBiNrms = __gx->hasBiNrms;

  for (int i = 0; i < GX_VA_MAX_ATTR; ++i) {
    const auto attr = static_cast<GXAttr>(i);
    AttrCacheEntry& entry = sVtxCache.attrs[i];
    entry.type = active_attr_type(attr);
    entry.fmt = active_attr_fmt(vtxFmt, attr);
    entry.expected = expected_component_count(attr, entry.fmt, entry.type);
    entry.next = GX_VA_NULL;
  }

  GXAttr running = GX_VA_NULL;
  for (int i = GX_VA_TEX7; i >= GX_VA_PNMTXIDX; --i) {
    sVtxCache.attrs[i].next = running;
    if (sVtxCache.attrs[i].type != GX_NONE) {
      running = static_cast<GXAttr>(i);
    }
  }
  sVtxCache.first = running;
}

static inline void ensure_vtx_cache(GXVtxFmt vtxFmt) {
  if (sVtxCache.valid && sVtxCache.vtxFmt == vtxFmt && sVtxCache.vcdLo == __gx->vcdLo &&
      sVtxCache.vcdHi == __gx->vcdHi && sVtxCache.vatA == __gx->vatA[vtxFmt] &&
      sVtxCache.vatB == __gx->vatB[vtxFmt] && sVtxCache.vatC == __gx->vatC[vtxFmt] &&
      sVtxCache.hasNrms == __gx->hasNrms && sVtxCache.hasBiNrms == __gx->hasBiNrms) {
    return;
  }
  rebuild_vtx_cache(vtxFmt);
}

static inline const AttrCacheEntry& attr_cache(GXAttr attr) {
  static const AttrCacheEntry kDisabled{};
  const auto idx = static_cast<u32>(attr);
  return idx < GX_VA_MAX_ATTR ? sVtxCache.attrs[idx] : kDisabled;
}

static void reset_attr_writer() {
  sWriteAttr = sVtxCache.first;
  sWriteAttrComps = 0;
}

static GXAttr select_implicit_attr(GXAttr first, GXAttr last) {
  if (sInBegin && sWriteAttr >= first && sWriteAttr <= last && attr_cache(sWriteAttr).type != GX_NONE) {
    return sWriteAttr;
  }
  if (sInBegin) {
    for (int i = static_cast<int>(sWriteAttr); i <= static_cast<int>(last); ++i) {
      const auto attr = static_cast<GXAttr>(i);
      if (attr >= first && attr_cache(attr).type != GX_NONE) {
        sWriteAttr = attr;
        sWriteAttrComps = 0;
        return attr;
      }
    }
  }
  for (int i = static_cast<int>(first); i <= static_cast<int>(last); ++i) {
    const auto attr = static_cast<GXAttr>(i);
    if (attr_cache(attr).type != GX_NONE) {
      sWriteAttr = attr;
      sWriteAttrComps = 0;
      return attr;
    }
  }
  return first;
}

static void align_explicit_attr(GXAttr attr) {
  if (!sInBegin || sWriteAttr == attr) {
    return;
  }
  sWriteAttr = attr;
  sWriteAttrComps = 0;
}

static void advance_attr(GXAttr attr, u32 compsWritten) {
  if (!sInBegin) {
    return;
  }
  const AttrCacheEntry& entry = attr_cache(attr);
  sWriteAttrComps += compsWritten;
  if (sWriteAttrComps < entry.expected) {
    return;
  }
  sWriteAttr = entry.next;
  sWriteAttrComps = 0;
  if (sWriteAttr == GX_VA_NULL) {
    reset_attr_writer();
  }
}

static int64_t encode_float_component(float value, bool isSigned, u8 bits, u8 frac) {
  if (!std::isfinite(value)) {
    value = 0.0f;
  }
  const double scaled = static_cast<double>(value) * static_cast<double>(uint64_t{1} << frac);
  const double rounded = std::round(scaled);
  if (isSigned) {
    const int64_t minVal = -(int64_t{1} << (bits - 1));
    const int64_t maxVal = (int64_t{1} << (bits - 1)) - 1;
    return std::clamp(static_cast<int64_t>(rounded), minVal, maxVal);
  }
  const int64_t maxVal = (int64_t{1} << bits) - 1;
  return std::clamp(static_cast<int64_t>(rounded), int64_t{0}, maxVal);
}

static int64_t scalar_to_signed_raw(const Scalar& value, u8 bits, u8 frac) {
  if (value.kind == ScalarKind::Float) {
    return encode_float_component(value.f, true, bits, frac);
  }
  const int64_t raw = value.kind == ScalarKind::Signed ? value.s : static_cast<int64_t>(value.u);
  const int64_t minVal = -(int64_t{1} << (bits - 1));
  const int64_t maxVal = (int64_t{1} << (bits - 1)) - 1;
  return std::clamp(raw, minVal, maxVal);
}

static uint64_t scalar_to_unsigned_raw(const Scalar& value, u8 bits, u8 frac) {
  if (value.kind == ScalarKind::Float) {
    return static_cast<uint64_t>(encode_float_component(value.f, false, bits, frac));
  }
  const uint64_t raw = value.kind == ScalarKind::Signed ? static_cast<uint64_t>(std::max(value.s, int32_t{0})) : value.u;
  const uint64_t maxVal = (uint64_t{1} << bits) - 1;
  return std::min(raw, maxVal);
}

static float scalar_to_float(const Scalar& value) {
  if (value.kind == ScalarKind::Float) {
    return value.f;
  }
  if (value.kind == ScalarKind::Signed) {
    return static_cast<float>(value.s);
  }
  return static_cast<float>(value.u);
}

static void write_numeric_component(const ActiveAttrFmt& fmt, const Scalar& value) {
  switch (fmt.type) {
  case GX_U8:
    GX_WRITE_U8(static_cast<u8>(scalar_to_unsigned_raw(value, 8, fmt.frac)));
    break;
  case GX_S8:
    GX_WRITE_U8(static_cast<u8>(static_cast<int8_t>(scalar_to_signed_raw(value, 8, fmt.frac))));
    break;
  case GX_U16:
    GX_WRITE_U16(static_cast<u16>(scalar_to_unsigned_raw(value, 16, fmt.frac)));
    break;
  case GX_S16:
    GX_WRITE_U16(static_cast<u16>(static_cast<int16_t>(scalar_to_signed_raw(value, 16, fmt.frac))));
    break;
  case GX_F32:
  default:
    GX_WRITE_F32(scalar_to_float(value));
    break;
  }
}

template <size_t N>
static void write_numeric_attr(GXAttr attr, const std::array<Scalar, N>& values) {
  ensure_vtx_cache(sBeginVtxFmt);
  align_explicit_attr(attr);
  const AttrCacheEntry& entry = attr_cache(attr);
  if (entry.type == GX_NONE) {
    return;
  }
  if (entry.type == GX_INDEX8 || entry.type == GX_INDEX16) {
    return;
  }
  const u32 expected = entry.expected;
  const u32 remaining = expected > sWriteAttrComps ? expected - sWriteAttrComps : expected;
  const u32 count = std::min<u32>(static_cast<u32>(values.size()), remaining);
  for (u32 i = 0; i < count; ++i) {
    write_numeric_component(entry.fmt, values[i]);
  }
  advance_attr(attr, count);
}

static u8 clamp_color_float(float value) {
  if (!std::isfinite(value)) {
    return 0;
  }
  return static_cast<u8>(std::clamp(std::round(value * 255.0f), 0.0f, 255.0f));
}

static u8 quantize_unorm(u8 value, u8 bits) {
  const u32 maxValue = (1u << bits) - 1u;
  return static_cast<u8>((static_cast<u32>(value) * maxValue + 127u) / 255u);
}

static void write_color_attr(u8 r, u8 g, u8 b, u8 a) {
  ensure_vtx_cache(sBeginVtxFmt);
  const GXAttr attr = select_implicit_attr(GX_VA_CLR0, GX_VA_CLR1);
  const AttrCacheEntry& entry = attr_cache(attr);
  if (entry.type == GX_NONE) {
    return;
  }
  if (entry.type == GX_INDEX8 || entry.type == GX_INDEX16) {
    return;
  }

  const ActiveAttrFmt& fmt = entry.fmt;
  switch (fmt.type) {
  case GX_RGB565: {
    const u16 packed = static_cast<u16>((quantize_unorm(r, 5) << 11) | (quantize_unorm(g, 6) << 5) |
                                        quantize_unorm(b, 5));
    GX_WRITE_U16(packed);
    break;
  }
  case GX_RGB8:
    GX_WRITE_U8(r);
    GX_WRITE_U8(g);
    GX_WRITE_U8(b);
    break;
  case GX_RGBX8:
    GX_WRITE_U8(r);
    GX_WRITE_U8(g);
    GX_WRITE_U8(b);
    GX_WRITE_U8(0xFF);
    break;
  case GX_RGBA4: {
    const u16 packed = static_cast<u16>((quantize_unorm(r, 4) << 12) | (quantize_unorm(g, 4) << 8) |
                                        (quantize_unorm(b, 4) << 4) | quantize_unorm(a, 4));
    GX_WRITE_U16(packed);
    break;
  }
  case GX_RGBA6: {
    const u32 packed = (quantize_unorm(r, 6) << 18) | (quantize_unorm(g, 6) << 12) |
                       (quantize_unorm(b, 6) << 6) | quantize_unorm(a, 6);
    GX_WRITE_U8(static_cast<u8>((packed >> 16) & 0xFF));
    GX_WRITE_U8(static_cast<u8>((packed >> 8) & 0xFF));
    GX_WRITE_U8(static_cast<u8>(packed & 0xFF));
    break;
  }
  case GX_RGBA8:
  default:
    GX_WRITE_U8(r);
    GX_WRITE_U8(g);
    GX_WRITE_U8(b);
    GX_WRITE_U8(a);
    break;
  }
  advance_attr(attr, 1);
}

template <size_t N>
static void write_tex_attr(const std::array<Scalar, N>& values) {
  ensure_vtx_cache(sBeginVtxFmt);
  const GXAttr attr = select_implicit_attr(GX_VA_TEX0, GX_VA_TEX7);
  write_numeric_attr(attr, values);
}

static void write_index_attr(GXAttr attr, u16 index, bool index16) {
  ensure_vtx_cache(sBeginVtxFmt);
  if (attr == GX_VA_TEX0) {
    attr = select_implicit_attr(GX_VA_TEX0, GX_VA_TEX7);
  } else if (attr == GX_VA_CLR0) {
    attr = select_implicit_attr(GX_VA_CLR0, GX_VA_CLR1);
  } else {
    align_explicit_attr(attr);
  }

  const GXAttrType type = attr_cache(attr).type;
  if (type == GX_INDEX16 || (type != GX_INDEX8 && index16)) {
    GX_WRITE_U16(index);
  } else {
    GX_WRITE_U8(static_cast<u8>(index));
  }
  advance_attr(attr, 1);
}

} // namespace

extern "C" {

void GXBegin(GXPrimitive primitive, GXVtxFmt vtxFmt, u16 nVerts) {
  aurora_legacy_gx_panic("GXBegin");
}

void GXEnd() {
  aurora_legacy_gx_panic("GXEnd");
}

void GXPosition3f32(f32 x, f32 y, f32 z) {
  aurora_legacy_gx_panic("GXPosition3f32");
}

void GXPosition3u16(u16 x, u16 y, u16 z) {
  aurora_legacy_gx_panic("GXPosition3u16");
}

void GXPosition3s16(s16 x, s16 y, s16 z) {
  aurora_legacy_gx_panic("GXPosition3s16");
}

void GXPosition3u8(u8 x, u8 y, u8 z) {
  aurora_legacy_gx_panic("GXPosition3u8");
}

void GXPosition3s8(s8 x, s8 y, s8 z) {
  aurora_legacy_gx_panic("GXPosition3s8");
}

void GXPosition2f32(f32 x, f32 y) {
  aurora_legacy_gx_panic("GXPosition2f32");
}

void GXPosition2u16(u16 x, u16 y) {
  aurora_legacy_gx_panic("GXPosition2u16");
}

void GXPosition2s16(s16 x, s16 y) {
  aurora_legacy_gx_panic("GXPosition2s16");
}

void GXPosition2u8(u8 x, u8 y) {
  aurora_legacy_gx_panic("GXPosition2u8");
}

void GXPosition2s8(s8 x, s8 y) {
  aurora_legacy_gx_panic("GXPosition2s8");
}

void GXPosition1x16(u16 idx) {
  aurora_legacy_gx_panic("GXPosition1x16");
}

void GXPosition1x8(u8 idx) {
  aurora_legacy_gx_panic("GXPosition1x8");
}

void GXNormal3f32(f32 x, f32 y, f32 z) {
  aurora_legacy_gx_panic("GXNormal3f32");
}

void GXNormal3u16(u16 x, u16 y, u16 z) {
  aurora_legacy_gx_panic("GXNormal3u16");
}

void GXNormal3s16(s16 x, s16 y, s16 z) {
  aurora_legacy_gx_panic("GXNormal3s16");
}

void GXNormal3u8(u8 x, u8 y, u8 z) {
  aurora_legacy_gx_panic("GXNormal3u8");
}

void GXNormal3s8(s8 x, s8 y, s8 z) {
  aurora_legacy_gx_panic("GXNormal3s8");
}

void GXNormal1x16(u16 index) {
  aurora_legacy_gx_panic("GXNormal1x16");
}

void GXNormal1x8(u8 index) {
  aurora_legacy_gx_panic("GXNormal1x8");
}

void GXColor4f32(f32 r, f32 g, f32 b, f32 a) {
  aurora_legacy_gx_panic("GXColor4f32");
}

void GXColor4u8(u8 r, u8 g, u8 b, u8 a) {
  aurora_legacy_gx_panic("GXColor4u8");
}

void GXColor3u8(u8 r, u8 g, u8 b) {
  aurora_legacy_gx_panic("GXColor3u8");
}

void GXColor1u32(u32 clr) {
  aurora_legacy_gx_panic("GXColor1u32");
}

void GXColor1u16(u16 clr) {
  aurora_legacy_gx_panic("GXColor1u16");
}

void GXColor1x16(u16 index) {
  aurora_legacy_gx_panic("GXColor1x16");
}

void GXColor1x8(u8 index) {
  aurora_legacy_gx_panic("GXColor1x8");
}

void GXTexCoord2f32(f32 s, f32 t) {
  aurora_legacy_gx_panic("GXTexCoord2f32");
}

void GXTexCoord2u16(u16 s, u16 t) {
  aurora_legacy_gx_panic("GXTexCoord2u16");
}

void GXTexCoord2s16(s16 s, s16 t) {
  aurora_legacy_gx_panic("GXTexCoord2s16");
}

void GXTexCoord2u8(u8 s, u8 t) {
  aurora_legacy_gx_panic("GXTexCoord2u8");
}

void GXTexCoord2s8(s8 s, s8 t) {
  aurora_legacy_gx_panic("GXTexCoord2s8");
}

void GXTexCoord1f32(f32 s) {
  aurora_legacy_gx_panic("GXTexCoord1f32");
}

void GXTexCoord1u16(u16 s) {
  aurora_legacy_gx_panic("GXTexCoord1u16");
}

void GXTexCoord1s16(s16 s) {
  aurora_legacy_gx_panic("GXTexCoord1s16");
}

void GXTexCoord1u8(u8 s) {
  aurora_legacy_gx_panic("GXTexCoord1u8");
}

void GXTexCoord1s8(s8 s) {
  aurora_legacy_gx_panic("GXTexCoord1s8");
}

void GXTexCoord1x16(u16 index) {
  aurora_legacy_gx_panic("GXTexCoord1x16");
}

void GXTexCoord1x8(u8 index) {
  aurora_legacy_gx_panic("GXTexCoord1x8");
}

void GXMatrixIndex1u8(GXAttr attr, u8 index) {
  aurora_legacy_gx_panic("GXMatrixIndex1u8");
}

void GXCmd1u8(const u8 x) {
  aurora_legacy_gx_panic("GXCmd1u8");
}
void GXCmd1u16(const u16 x) {
  aurora_legacy_gx_panic("GXCmd1u16");
}
void GXCmd1u32(const u32 x) {
  aurora_legacy_gx_panic("GXCmd1u32");
}
void GXCmd1u64(const u64 x) {
  aurora_legacy_gx_panic("GXCmd1u64");
}

void GXParam1u8(const u8 x) {
  aurora_legacy_gx_panic("GXParam1u8");
}
void GXParam1u16(const u16 x) {
  aurora_legacy_gx_panic("GXParam1u16");
}
void GXParam1u32(const u32 x) {
  aurora_legacy_gx_panic("GXParam1u32");
}
void GXParam1s8(const s8 x) {
  aurora_legacy_gx_panic("GXParam1s8");
}
void GXParam1s16(const s16 x) {
  aurora_legacy_gx_panic("GXParam1s16");
}
void GXParam1s32(const s32 x) {
  aurora_legacy_gx_panic("GXParam1s32");
}
void GXParam1f32(const f32 x) {
  aurora_legacy_gx_panic("GXParam1f32");
}
void GXParam3f32(const f32 x, const f32 y, const f32 z) {
  aurora_legacy_gx_panic("GXParam3f32");
}
void GXParam4f32(const f32 x, const f32 y, const f32 z, const f32 w) {
  aurora_legacy_gx_panic("GXParam4f32");
}

} // extern "C"
