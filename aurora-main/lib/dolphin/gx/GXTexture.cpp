#include <aurora/legacy_gx.h>
#include "gx.hpp"
#include "__gx.h"

#include "../../gfx/texture.hpp"
#include "../../gfx/texture_replacement.hpp"
#include "dolphin/gx/GXAurora.h"

#include <absl/container/flat_hash_map.h>

#include <algorithm>
#include <cstddef>
#include <utility>

#include "tracy/Tracy.hpp"

namespace {
constexpr u8 GXTexMode0Ids[8] = {0x80, 0x81, 0x82, 0x83, 0xA0, 0xA1, 0xA2, 0xA3};
constexpr u8 GXTexMode1Ids[8] = {0x84, 0x85, 0x86, 0x87, 0xA4, 0xA5, 0xA6, 0xA7};
constexpr u8 GXTexImage0Ids[8] = {0x88, 0x89, 0x8A, 0x8B, 0xA8, 0xA9, 0xAA, 0xAB};
constexpr u8 GXTexImage1Ids[8] = {0x8C, 0x8D, 0x8E, 0x8F, 0xAC, 0xAD, 0xAE, 0xAF};
constexpr u8 GXTexImage2Ids[8] = {0x90, 0x91, 0x92, 0x93, 0xB0, 0xB1, 0xB2, 0xB3};
constexpr u8 GXTexImage3Ids[8] = {0x94, 0x95, 0x96, 0x97, 0xB4, 0xB5, 0xB6, 0xB7};
constexpr u8 GXTexTlutIds[8] = {0x98, 0x99, 0x9A, 0x9B, 0xB8, 0xB9, 0xBA, 0xBB};
constexpr u8 GX2HWFiltConv[6] = {0x00, 0x04, 0x01, 0x05, 0x02, 0x06};

u32 sNextTexObjId = 1;
u32 sNextTlutObjId = 1;

u32 next_tex_obj_id() {
  if (sNextTexObjId == 0) {
    FATAL("texObj ID overflow");
  }
  return sNextTexObjId++;
}

u32 next_tlut_obj_id() {
  if (sNextTlutObjId == 0) {
    FATAL("tlutObj ID overflow");
  }
  return sNextTlutObjId++;
}

  // Give each palette buffer a stable ID so its GPU cache can be reused.
struct TlutIdentity {
  const void* data = nullptr;
  u32 format = 0;
  u32 entries = 0;

  bool operator==(const TlutIdentity& rhs) const noexcept {
    return data == rhs.data && format == rhs.format && entries == rhs.entries;
  }

  template <typename H>
  friend H AbslHashValue(H h, const TlutIdentity& key) {
    return H::combine(std::move(h), key.data, key.format, key.entries);
  }
};

struct TlutIdentityRecord {
  u32 objId = 0;
  u32 dataVersion = 0;
  u64 digest = 0;
};

  // Limit cached palette IDs; evicted palettes are safely uploaded again.
constexpr size_t MaxTlutIdentities = 4096;
absl::flat_hash_map<TlutIdentity, TlutIdentityRecord> sTlutIdentities;

  // Use separate high-range versions when a palette points to a new buffer.
u32 sNextRepointedTlutDataVersion = 0;

u32 next_repointed_tlut_data_version() {
  if (sNextRepointedTlutDataVersion == 0x7FFFFFFFu) {
    sNextRepointedTlutDataVersion = 0;
  }
  return 0x80000000u | ++sNextRepointedTlutDataVersion;
}

std::pair<u32, u32> resolve_tlut_identity(const void* data, GXTlutFmt format, u16 entries) {
  if (data == nullptr || entries == 0) {
    return {next_tlut_obj_id(), 1};
  }

  const auto digest =
      static_cast<u64>(aurora::xxh3_hash_s(data, static_cast<size_t>(entries) * sizeof(u16)));
  if (sTlutIdentities.size() >= MaxTlutIdentities) {
    sTlutIdentities.clear();
  }

  const TlutIdentity key{
      .data = data,
      .format = static_cast<u32>(format),
      .entries = entries,
  };
  auto [it, inserted] = sTlutIdentities.try_emplace(key);
  auto& record = it->second;
  if (inserted) {
    record.objId = next_tlut_obj_id();
    record.dataVersion = 1;
    record.digest = digest;
  } else if (record.digest != digest) {
    // Same buffer, different palette: invalidate everything keyed on this ID.
    record.digest = digest;
    if (record.dataVersion >= 0x7FFFFFFFu) {
      record.dataVersion = 1;
    } else {
      ++record.dataVersion;
    }
  }
  return {record.objId, record.dataVersion};
}

int __cntlzw(unsigned int val) {
  if (val == 0)
    return 32; // PowerPC returns 32 if the input is 0
#ifdef _MSC_VER
  unsigned long idx;
  _BitScanReverse(&idx, val);
  return 31 - (int)idx;
#else
  return __builtin_clz(val);
#endif
}

void init_texobj_common(GXTexObj_& obj, const void* data, u16 width, u16 height, u32 format, GXTexWrapMode wrapS,
                        GXTexWrapMode wrapT, GXBool mipmap) {
  obj = {};
  obj.mWidth = width;
  obj.mHeight = height;
  obj.mFormat = format;
  obj.tlut = GX_TLUT0;
  obj.flags = 2;
  obj.texObjId = next_tex_obj_id();
  obj.texDataVersion = 1;

  SET_REG_FIELD(0, obj.mode0, 2, 0, wrapS);
  SET_REG_FIELD(0, obj.mode0, 2, 2, wrapT);
  SET_REG_FIELD(0, obj.mode0, 1, 4, 1);
  if (mipmap) {
    obj.flags |= 1;
    obj.mode0 = (obj.mode0 & 0xFFFFFF1F) | 0xC0;
    const u32 maxDim = std::max(width, height);
    const u8 lmax = static_cast<u8>(16.0f * static_cast<float>(31 - __cntlzw(maxDim)));
    SET_REG_FIELD(0, obj.mode1, 8, 8, lmax);
  } else {
    obj.mode0 = (obj.mode0 & 0xFFFFFF1F) | 0x80;
  }
  SET_REG_FIELD(0, obj.image0, 10, 0, static_cast<u32>(width - 1));
  SET_REG_FIELD(0, obj.image0, 10, 10, static_cast<u32>(height - 1));
  SET_REG_FIELD(0, obj.image0, 4, 20, format & 0xF);

  obj.data = data;
}

void emit_loaded_texobj_metadata(const GXTexObj_& obj, GXTexMapID id) {
  GX_WRITE_AURORA(GX_LOAD_AURORA_TEXOBJ);
  GX_WRITE_U8(static_cast<u8>(id));
  GX_WRITE_U64(reinterpret_cast<u64>(obj.data));
  GX_WRITE_U32(obj.width());
  GX_WRITE_U32(obj.height());
  GX_WRITE_U32(obj.format());
  GX_WRITE_U32(static_cast<u32>(obj.tlut));
  GX_WRITE_U8(static_cast<u8>(obj.has_mips()));
  GX_WRITE_U32(obj.texObjId);
  GX_WRITE_U32(obj.texDataVersion);
}

void emit_loaded_tlut_metadata(const GXTlutObj_& obj, u32 idx) {
  GX_WRITE_AURORA(GX_LOAD_AURORA_TLUT);
  GX_WRITE_U8(static_cast<u8>(idx));
  GX_WRITE_U64(reinterpret_cast<u64>(obj.data));
  GX_WRITE_U32(static_cast<u32>(obj.format));
  GX_WRITE_U16(obj.numEntries);
  GX_WRITE_U32(obj.tlutObjId);
  GX_WRITE_U32(obj.tlutDataVersion);
}
} // namespace

extern "C" {
void GXInitTexObj(GXTexObj* obj_, const void* data, u16 width, u16 height, GXTexFmt format, GXTexWrapMode wrapS,
                  GXTexWrapMode wrapT, GXBool mipmap) {
  aurora_legacy_gx_panic("GXInitTexObj");
}

void GXInitTexObjCI(GXTexObj* obj_, const void* data, u16 width, u16 height, GXCITexFmt format, GXTexWrapMode wrapS,
                    GXTexWrapMode wrapT, GXBool mipmap, u32 tlut) {
  aurora_legacy_gx_panic("GXInitTexObjCI");
}

void GXInitTexObjLOD(GXTexObj* obj_, GXTexFilter minFilt, GXTexFilter magFilt, float minLod, float maxLod,
                     float lodBias, GXBool biasClamp, GXBool doEdgeLod, GXAnisotropy maxAniso) {
  aurora_legacy_gx_panic("GXInitTexObjLOD");
}

void GXInitTexObjData(GXTexObj* obj_, const void* data) {
  aurora_legacy_gx_panic("GXInitTexObjData");
}

void GXInitTexObjWrapMode(GXTexObj* obj_, GXTexWrapMode wrapS, GXTexWrapMode wrapT) {
  aurora_legacy_gx_panic("GXInitTexObjWrapMode");
}

void GXInitTexObjTlut(GXTexObj* obj_, u32 tlut) {
  aurora_legacy_gx_panic("GXInitTexObjTlut");
}

void GXInitTexObjFilter(GXTexObj* obj_, GXTexFilter minFilt, GXTexFilter magFilt) {
  aurora_legacy_gx_panic("GXInitTexObjFilter");
}

void GXInitTexObjMaxLOD(GXTexObj* obj_, float maxLod) {
  aurora_legacy_gx_panic("GXInitTexObjMaxLOD");
}

void GXInitTexObjMinLOD(GXTexObj* obj_, float minLod) {
  aurora_legacy_gx_panic("GXInitTexObjMinLOD");
}

void GXInitTexObjLODBias(GXTexObj* obj_, float lodBias) {
  aurora_legacy_gx_panic("GXInitTexObjLODBias");
}

void GXInitTexObjBiasClamp(GXTexObj* obj_, GXBool biasClamp) {
  aurora_legacy_gx_panic("GXInitTexObjBiasClamp");
}

void GXInitTexObjEdgeLOD(GXTexObj* obj_, GXBool doEdgeLod) {
  aurora_legacy_gx_panic("GXInitTexObjEdgeLOD");
}

void GXInitTexObjMaxAniso(GXTexObj* obj_, GXAnisotropy maxAniso) {
  aurora_legacy_gx_panic("GXInitTexObjMaxAniso");
}

void GXInitTexObjUserData(GXTexObj* obj_, void* userData) {
  aurora_legacy_gx_panic("GXInitTexObjUserData");
}

void* GXGetTexObjUserData(const GXTexObj* obj_) {
  aurora_legacy_gx_panic("GXGetTexObjUserData");
}

void GXLoadTexObj(GXTexObj* obj_, GXTexMapID id) {
  aurora_legacy_gx_panic("GXLoadTexObj");
}

u32 GXGetTexBufferSize(u16 width, u16 height, u32 fmt, GXBool mips, u8 maxLod) {
  aurora_legacy_gx_panic("GXGetTexBufferSize");
}

void GXInitTlutObj(GXTlutObj* obj_, const void* data, GXTlutFmt format, u16 entries) {
  aurora_legacy_gx_panic("GXInitTlutObj");
}

void GXInitTlutObjData(GXTlutObj* obj_, const void* data) {
  aurora_legacy_gx_panic("GXInitTlutObjData");
}

void GXLoadTlut(const GXTlutObj* obj_, u32 idx) {
  aurora_legacy_gx_panic("GXLoadTlut");
}

// TODO GXInitTexCacheRegion
// TODO GXInitTexPreLoadRegion
// TODO GXInitTlutRegion
// TODO GXInvalidateTexRegion

void GXInvalidateTexAll() {
  aurora_legacy_gx_panic("GXInvalidateTexAll");
}

// TODO GXPreLoadEntireTexture
// TODO GXSetTexRegionCallback
// TODO GXSetTlutRegionCallback
// TODO GXLoadTexObjPreLoaded
void GXSetTexCoordScaleManually(GXTexCoordID coord, GXBool enable, u16 ss, u16 ts) {
  aurora_legacy_gx_panic("GXSetTexCoordScaleManually");
}

void GXSetTexCoordCylWrap(GXTexCoordID coord, GXBool s_enable, GXBool t_enable) {
  aurora_legacy_gx_panic("GXSetTexCoordCylWrap");
}

void GXSetTexCoordBias(GXTexCoordID coord, GXBool s_enable, GXBool t_enable) {
  aurora_legacy_gx_panic("GXSetTexCoordBias");
}

void __GXFlushTextureState() {
  aurora_legacy_gx_panic("__GXFlushTextureState");
}
}
