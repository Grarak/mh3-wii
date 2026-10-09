#include <aurora/native_texture.hpp>
#include <aurora/fatal.hpp>
#include <aurora/aurora.h>
#include <aurora/generated_texture.hpp>
#include <aurora/native_scene_feedback.hpp>
#include "texture_convert.hpp"
#include "native_texture_layout.hpp"
#include "native_texture_set.hpp"
#include "../gl/gl.hpp"
#include "../gxm/sampler_bias.hpp"
#include <aurora/fast_map.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace aurora {
namespace {
// Budget resident texture storage separately from display/depth/offscreen
// targets. GLES texel estimates exclude driver bookkeeping; GXM charges the
// CDRAM image/palette layout. Backend fences retain evicted in-flight storage.
constexpr size_t CacheBudget = 48u << 20;
struct Key {
  uintptr_t bytes;
  uint32_t asset, width, height, levels, format;
  uint64_t generation;
  uintptr_t palette;
  uint32_t paletteFormat, paletteEntries;
  uint64_t paletteGeneration;
  bool operator==(const Key&) const = default;
  friend uint32_t FastHashValue(const Key& k) {
    return FastHash(k.bytes, k.asset, k.width, k.height, k.levels, k.format, k.generation, k.palette, k.paletteFormat, k.paletteEntries, k.paletteGeneration);
  }
};

struct Entry {
  gl::TexturePtr texture;
  size_t bytes;
  uint64_t used;

  // The source's owner range, from its latest binding (retire_native_textures).
  uint32_t owner = 0, ownerBytes = 0;
};

FastMap<Key, Entry> cache;
std::array<gl::TexturePtr, 8> bindings;
struct Binding {
  NativeTextureSource source;
  NativeTextureSampler sampling;
  bool valid = false;
};

std::array<Binding, 8> logicalBindings;
std::array<float, 8> shaderLodBias{};
std::array<NativeTexturePortableSampling, 8> portableSampling{};
size_t residentBytes = 0;
uint64_t serial = 0;

// Counts the cache's erases: a memo's entry is still cached while the stamp
// it was made at is current.
uint64_t cacheStamp = 1;
void owner() {
  if (!gl::on_render_thread())
    AURORA_THROW(std::runtime_error("Native texture changed outside graphics owner"));
}

gl::SamplerDesc sampler(const NativeTextureSampler& source) {
  if (source.wrap_s > 2 || source.wrap_t > 2 || source.min_filter > 5 || source.mag_filter > 1 || source.anisotropy > 2 || !std::isfinite(source.min_lod) || !std::isfinite(source.max_lod) ||
      !std::isfinite(source.lod_bias) || source.min_lod > source.max_lod)
    AURORA_THROW(std::runtime_error("Invalid native texture sampler"));
  constexpr GLenum wrap[]{GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT};
  constexpr GLenum filter[]{GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR};
  return {wrap[source.wrap_s], wrap[source.wrap_t], filter[source.min_filter], filter[source.mag_filter], source.min_lod, source.max_lod, 1u << source.anisotropy};
}

void make_room(size_t bytes) {
  if (bytes > CacheBudget)
    AURORA_THROW(std::runtime_error("Native texture exceeds resident cache budget"));
  while (residentBytes > CacheBudget - bytes) {
    auto victim = cache.end();

    for (auto it = cache.begin(); it != cache.end(); ++it)
      if (it->second.texture.use_count() == 1 && (victim == cache.end() || it->second.used < victim->second.used))
        victim = it;
    if (victim == cache.end())
      AURORA_THROW(std::runtime_error("Bound native textures exceed resident cache budget"));

    residentBytes -= victim->second.bytes;
    cache.erase(victim);
    ++cacheStamp;
  }

  // The budget exceeds GXM's CDRAM texture arena, which has no spill: also
  // drop the least recently used unbound textures until the backing fits
  // (the opening movie after character creation filled it, 2026-10-04).
  // Their storage frees at once unless the GPU still reads it.
  while (!gl::render_target_allocation_available(bytes)) {
    auto victim = cache.end();

    for (auto it = cache.begin(); it != cache.end(); ++it)
      if (it->second.texture.use_count() == 1 && (victim == cache.end() || it->second.used < victim->second.used))
        victim = it;
    if (victim == cache.end())
      break; // the allocation reports what is resident
    residentBytes -= victim->second.bytes;
    cache.erase(victim);
    ++cacheStamp;
  }
}

size_t rgba_allocation(const NativeTextureSource& source) {
  size_t bytes = 0;

  for (unsigned mip = 0; mip < source.levels; ++mip) {
    const size_t w = std::max(source.width >> mip, 1u), h = std::max(source.height >> mip, 1u);
    bytes += mip + 1 < source.levels ? size_t(std::max(std::bit_ceil(source.width) >> mip, 8u)) * std::max(std::bit_ceil(source.height) >> mip, 1u) * 4 : ((w + 7) & ~size_t(7)) * h * 4;
  }

  return (bytes + 63) & ~size_t(63);
}

size_t offline_allocation(const gl::NativeTextureInfo& info) {
#ifdef AURORA_GXM
  // NativeAssets reads the complete prepared image/palette layout into CDRAM.
  return (size_t(info.bytes) + 63) & ~size_t(63);
#else
  // GLES expands Morton palette/BC storage to RGBA. Linear prepared formats
  // retain their native channel widths; archive padding is not GPU storage.
  const size_t pixel = gfx::native_swizzled(info.format) ? 4 : gfx::native_pixel_bytes(info.format);
  size_t bytes = 0;

  for (unsigned mip = 0; mip < info.mips; ++mip)
    bytes += size_t(std::max(info.width >> mip, 1u)) * std::max(info.height >> mip, 1u) * pixel;

  return (bytes + 63) & ~size_t(63);
#endif
}

// NativeTextureFormatYuv420 -> linear RGBA8. The THP player's four TEV stages
// (C0 = (-90, 0, -114, 135), K0 = (0, 0, 226, 88), K1 = (179, 0, 0, 182),
// K2 = (255, 0, 255)) compute, with GX's konst weights (k + (k >> 7)) / 256:
//   R = Y + 2 (180 V / 256 - 90)
//   G = Y + 135 - (88 U + 183 V) / 256     (alpha path, selected by K2)
//   B = Y + 2 (227 U / 256 - 114)
// each clamped to 0..255. A 2x2 block of Y shares one U and V sample.
// Converts rows first..first+rows-1 of the frame into out (rows x width x 4).
// The scalar loop took about 50 ms a 640x480 frame on the Vita's A9, which held
// movies to some 19 frames a second; NEON converts 16 texels a step.
void convert_yuv420(const uint8_t* planes, uint32_t width, uint32_t height, uint32_t first, uint32_t rows, uint8_t* out) {
  const uint32_t chromaWidth = width / 2, lumaTiles = width / 8, chromaTiles = chromaWidth / 8;
  const uint8_t* luma = planes;
  const uint8_t* blue = planes + size_t(width) * height;
  const uint8_t* red = blue + size_t(chromaWidth) * (height / 2);
#if defined(__ARM_NEON)
  const uint8x8_t k180 = vdup_n_u8(180), k88 = vdup_n_u8(88), k183 = vdup_n_u8(183), k227 = vdup_n_u8(227);
  const int16x8_t k135 = vdupq_n_s16(135), k180s = vdupq_n_s16(180), k228 = vdupq_n_s16(228);
  uint8x16x4_t pixels;
  pixels.val[3] = vdupq_n_u8(255);

  for (uint32_t y = first; y < first + rows; ++y) {
    // I8 tiles are 8x4 texels, 32 bytes, row-major across the plane.
    const uint8_t* lumaRow = luma + size_t(y >> 2) * lumaTiles * 32 + (y & 3) * 8;
    const uint32_t cy = y >> 1;
    const size_t chromaRow = size_t(cy >> 2) * chromaTiles * 32 + (cy & 3) * 8;
    uint8_t* row = out + size_t(y - first) * width * 4;

    // Sixteen luma texels (two tiles) use one chroma tile row of eight.
    for (uint32_t tile = 0; tile < lumaTiles; tile += 2) {
      const uint8x8_t u = vld1_u8(blue + chromaRow + (tile >> 1) * 32), v = vld1_u8(red + chromaRow + (tile >> 1) * 32);

      // (x * k + 128) >> 8, as the scalar tables round.
      const int16x8_t r = vsubq_s16(vreinterpretq_s16_u16(vshll_n_u8(vrshrn_n_u16(vmull_u8(v, k180), 8), 1)), k180s);
      const int16x8_t g = vsubq_s16(vsubq_s16(k135, vreinterpretq_s16_u16(vmovl_u8(vrshrn_n_u16(vmull_u8(u, k88), 8)))),
                                    vreinterpretq_s16_u16(vmovl_u8(vrshrn_n_u16(vmull_u8(v, k183), 8))));
      const int16x8_t b = vsubq_s16(vreinterpretq_s16_u16(vshll_n_u8(vrshrn_n_u16(vmull_u8(u, k227), 8), 1)), k228);

      // Each chroma sample covers two horizontal texels.
      const int16x8x2_t rr = vzipq_s16(r, r), gg = vzipq_s16(g, g), bb = vzipq_s16(b, b);
      const int16x8_t l0 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(lumaRow + tile * 32)));
      const int16x8_t l1 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(lumaRow + (tile + 1) * 32)));
      pixels.val[0] = vcombine_u8(vqmovun_s16(vaddq_s16(l0, rr.val[0])), vqmovun_s16(vaddq_s16(l1, rr.val[1])));
      pixels.val[1] = vcombine_u8(vqmovun_s16(vaddq_s16(l0, gg.val[0])), vqmovun_s16(vaddq_s16(l1, gg.val[1])));
      pixels.val[2] = vcombine_u8(vqmovun_s16(vaddq_s16(l0, bb.val[0])), vqmovun_s16(vaddq_s16(l1, bb.val[1])));
      vst4q_u8(row + tile * 32, pixels);
    }
  }
#else
  struct Tables {
    int16_t red[256], green_u[256], green_v[256], blue[256];
    uint8_t clamp[1024];
  };

  static const Tables tables = [] {
    Tables t{};

    for (int i = 0; i < 256; ++i) {
      t.red[i] = int16_t(2 * ((i * 180 + 128) >> 8) - 180);
      t.green_u[i] = int16_t((i * 88 + 128) >> 8);
      t.green_v[i] = int16_t((i * 183 + 128) >> 8);
      t.blue[i] = int16_t(2 * ((i * 227 + 128) >> 8) - 228);
    }

    for (int i = 0; i < 1024; ++i)
      t.clamp[i] = uint8_t(std::clamp(i - 384, 0, 255));

    return t;
  }();
  const uint8_t* clamp = tables.clamp + 384;

  for (uint32_t y = first; y < first + rows; ++y) {
    // I8 tiles are 8x4 texels, 32 bytes, row-major across the plane.
    const uint8_t* lumaRow = luma + size_t(y >> 2) * lumaTiles * 32 + (y & 3) * 8;
    const uint32_t cy = y >> 1;
    const size_t chromaRow = size_t(cy >> 2) * chromaTiles * 32 + (cy & 3) * 8;
    uint8_t* row = out + size_t(y - first) * width * 4;

    for (uint32_t tile = 0; tile < lumaTiles; ++tile) {
      const uint8_t* l = lumaRow + tile * 32;

      // Eight luma texels use four chroma texels: half of a chroma tile row.
      const size_t c = chromaRow + (tile >> 1) * 32 + (tile & 1) * 4;

      for (uint32_t i = 0; i < 4; ++i) {
        const int u = blue[c + i], v = red[c + i];
        const int r = tables.red[v], g = 135 - tables.green_u[u] - tables.green_v[v], b = tables.blue[u];

        for (uint32_t k = 0; k < 2; ++k) {
          const int luminance = l[2 * i + k];
          uint8_t* p = row + (tile * 8 + 2 * i + k) * 4;
          p[0] = clamp[luminance + r];
          p[1] = clamp[luminance + g];
          p[2] = clamp[luminance + b];
          p[3] = 255;
        }
      }
    }
  }
#endif
}
} // namespace

size_t native_texture_source_size(uint32_t format, uint32_t width, uint32_t height, uint32_t levels) {
  if (!width || !height || width > 4096 || height > 4096 || !levels || levels > std::bit_width(std::max(width, height)))
    AURORA_THROW(std::runtime_error("Invalid native texture dimensions"));

  if (format == NativeTextureFormatYuv420) {
    // Whole 8x4 I8 tiles in all three planes.
    if (width % 16 || height % 8 || levels != 1)
      AURORA_THROW(std::runtime_error("Invalid native YUV texture dimensions"));

    return size_t(width) * height * 3 / 2;
  }

  // Tiles are 4 or 8 texels a side: shifts (the Cortex-A9 has no divide).
  uint32_t tileWidthShift = 2, tileHeightShift = 2, tileBytes = 32;

  switch (format) {
  case 0:
  case 8:
    tileWidthShift = tileHeightShift = 3;
    break;
  case 1:
  case 2:
  case 9:
    tileWidthShift = 3;
    break;
  case 3:
  case 4:
  case 5:
  case 10:
    break;
  case 6:
    tileBytes = 64;
    break;
  case 14:
    tileWidthShift = tileHeightShift = 3;
    break;
  default:
    AURORA_THROW(std::runtime_error("Unsupported native texture source format"));
  }

  size_t bytes = 0;

  for (unsigned mip = 0; mip < levels; ++mip) {
    bytes += size_t((width + (1u << tileWidthShift) - 1) >> tileWidthShift) * ((height + (1u << tileHeightShift) - 1) >> tileHeightShift) * tileBytes;
    width = std::max(width >> 1, 1u);
    height = std::max(height >> 1, 1u);
  }

  return bytes;
}

namespace {
bool SameSource(const NativeTextureSource& a, const NativeTextureSource& b) {
  return a.bytes.data() == b.bytes.data() && a.bytes.size() == b.bytes.size() && a.palette.data() == b.palette.data() && a.palette.size() == b.palette.size() &&
         a.width == b.width && a.height == b.height && a.levels == b.levels && a.format == b.format && a.asset == b.asset && a.palette_format == b.palette_format &&
         a.palette_entries == b.palette_entries && a.generation == b.generation && a.palette_generation == b.palette_generation && a.mutable_source == b.mutable_source &&
         a.original_image == b.original_image && a.original_palette == b.original_palette && a.identity == b.identity && a.owner == b.owner && a.owner_bytes == b.owner_bytes;
}

bool SameSampling(const NativeTextureSampler& a, const NativeTextureSampler& b) {
  return a.wrap_s == b.wrap_s && a.wrap_t == b.wrap_t && a.min_filter == b.min_filter && a.mag_filter == b.mag_filter && a.anisotropy == b.anisotropy && a.min_lod == b.min_lod &&
         a.max_lod == b.max_lod && a.lod_bias == b.lod_bias;
}

// bind_native_texture; a bind that publishes a cached entry fills `memo`.
void bind(uint32_t unit, const NativeTextureSource& source, const NativeTextureSampler& sampling, NativeTextureMemo* memo) {
  owner();

  // Only a publication refills it (a skipped frame leaves it empty).
  if (memo)
    memo->entry = nullptr;

  if (unit >= bindings.size())
    AURORA_THROW(std::runtime_error("Invalid native texture unit"));

  const auto descriptor = sampler(sampling);
  const size_t sourceBytes = native_texture_source_size(source.format, source.width, source.height, source.levels);

  // GPU producers publish their own backing revisions. A guest texture slot
  // naming such a target is not a mutable CPU image and needs no byte revision.
  // Resolve this before the frame gate so skipped bindings remain validated.
  const auto identity = static_cast<const uint8_t*>(source.identity ? source.identity : source.bytes.data());
  const uint64_t feedbackRevision = identity ? scene_feedback_texture_version(identity, source.width, source.height) : 0;
  const uint32_t generatedRevision = identity && !feedbackRevision ? generated_texture_version(identity, source.width, source.height) : 0;

  if (source.mutable_source && !source.generation && !feedbackRevision && !generatedRevision)
    AURORA_THROW(std::runtime_error("Mutable native texture has no producer revision"));

  logicalBindings[unit] = {source, sampling, true};
  shaderLodBias[unit] = sampling.lod_bias;

  if (!aurora_native_frame_should_render())
    return;

  const auto publish = [&](const gl::TexturePtr& texture) {
    auto nativeDescriptor = descriptor;

    // With one stored mip, equal point/linear minification and magnification
    // filters, and no anisotropy, a bias cannot change the sampled texels.
    // Use the admitted backing's mip count, including generated textures.
    // A single mip alone is insufficient: bias can select a different mag
    // filter, and anisotropic footprints need their authored bias retained.
    const bool ineffectiveBias =
        texture->mips == 1 && sampling.anisotropy == 0 && (sampling.min_filter == 0 || sampling.min_filter == 2 || sampling.min_filter == 4 ? sampling.mag_filter == 0 : sampling.mag_filter == 1);
    const bool samplerBias = !ineffectiveBias && texture->format != gl::Format::Depth32F && gxm::sampler_bias_encoding(sampling.lod_bias) >= 0;
    portableSampling[unit] = {source.asset,
                              texture->width,
                              texture->height,
                              texture->mips,
                              uint32_t(texture->format),
                              sampling.min_filter,
                              sampling.mag_filter,
                              sampling.anisotropy,
                              source.generation,
                              source.palette_generation,
                              sampling.lod_bias,
                              ineffectiveBias || samplerBias ? 0 : sampling.lod_bias,
                              samplerBias ? sampling.lod_bias : 0};
    if (ineffectiveBias)
      shaderLodBias[unit] = 0;
#ifdef AURORA_GXM
    nativeDescriptor.lodBias = portableSampling[unit].sampler_bias;
    shaderLodBias[unit] = portableSampling[unit].shader_bias;
#endif
    bindings[unit] = texture;
    const GLuint samplerId = gl::sampler(nativeDescriptor);
    gl::bind_texture(unit, texture->gl(), samplerId);
    return samplerId;
  };

  const auto remember = [&](const Entry& entry, GLuint samplerId) {
    if (!memo)
      return;

    *memo = {source, sampling, &entry, cacheStamp, samplerId, shaderLodBias[unit], portableSampling[unit]};
  };

  if (feedbackRevision) {
    if (!bind_scene_feedback_texture(unit, identity, source.width, source.height, sampling))
      AURORA_THROW(std::runtime_error("Published scene feedback disappeared before binding"));
    // GPU targets own their storage. Do not inspect or convert guest pixels.
    portableSampling[unit] = {source.asset,
                              source.width,
                              source.height,
                              1,
                              uint32_t(gl::Format::RGBA8),
                              sampling.min_filter,
                              sampling.mag_filter,
                              sampling.anisotropy,
                              source.generation,
                              source.palette_generation,
                              0,
                              0,
                              0};
    bindings[unit].reset();
    return;
  }

  if (generatedRevision) {
    publish(gfx::find_generated_texture(identity, generatedRevision)->texture);
    return;
  }

  const bool indexed = source.format == 8 || source.format == 9 || source.format == 10;

  // A native asset already includes its immutable palette. Live palette or
  // image revisions must use their own producer payload instead.
  bool offline = source.asset && !source.mutable_source && source.original_image;
  gl::NativeTextureInfo offlineInfo{};

  if (offline) {
    offlineInfo = gl::native_texture_info(source.asset);

    if (offlineInfo.width != source.width || offlineInfo.height != source.height || offlineInfo.mips != source.levels || (offlineInfo.sourceProvenance && offlineInfo.sourceFormat != source.format))
      AURORA_THROW(std::runtime_error("Native asset metadata disagrees with published texture: asset " + std::to_string(source.asset) + " is " +
                                      std::to_string(offlineInfo.width) + "x" + std::to_string(offlineInfo.height) + " (" + std::to_string(offlineInfo.mips) +
                                      " levels, format " + std::to_string(offlineInfo.sourceFormat) + "), published " + std::to_string(source.width) + "x" +
                                      std::to_string(source.height) + " (" + std::to_string(source.levels) + " levels, format " + std::to_string(source.format) + ")"));

    if (indexed) {
      const int64_t delta = int64_t(reinterpret_cast<uintptr_t>(source.palette.data())) - int64_t(reinterpret_cast<uintptr_t>(source.bytes.data()));
      offline = source.original_palette && offlineInfo.sourceProvenance && delta == offlineInfo.sourcePaletteDelta && source.palette_entries == offlineInfo.sourcePaletteEntries &&
                source.palette_format == offlineInfo.sourcePaletteFormat;
    }
  }

  // A converted image is the bytes at an address under one producer
  // generation. Bytes without a generation (resources never initialized,
  // such as the HUD's) keep their asset ID, so an image that a later load
  // puts at the same address with the same shape is not taken for them.
  const Key key{offline ? 0 : reinterpret_cast<uintptr_t>(source.identity ? source.identity : source.bytes.data()),
                source.asset,
                source.width,
                source.height,
                source.levels,
                source.format,
                offline ? 0 : source.generation,
                offline ? 0 : reinterpret_cast<uintptr_t>(source.palette.data()),
                offline ? 0 : source.palette_format,
                offline ? 0 : source.palette_entries,
                offline ? 0 : source.palette_generation};
  if (auto found = cache.find(key); found != cache.end()) {
    found->second.used = ++serial;
    found->second.owner = source.owner;
    found->second.ownerBytes = source.owner_bytes;
    remember(found->second, publish(found->second.texture));
    return;
  }

  // Drop the binding being replaced before evicting cold cache entries. Other
  // units keep their texture owners, and GPU consumers retain backing fences.
  bindings[unit].reset();

  for (auto it = cache.begin(); it != cache.end();) {
    if (!offline && it->first.bytes == key.bytes && (it->first.generation != source.generation || it->first.asset != key.asset) && it->second.texture.use_count() == 1) {
      residentBytes -= it->second.bytes;
      it = cache.erase(it);
      ++cacheStamp;
    } else
      ++it;
  }

  gl::TexturePtr texture;
  size_t allocated = rgba_allocation(source);

  if (offline) {
    allocated = offline_allocation(offlineInfo);
    make_room(allocated);
    texture = gl::native_texture_asset(source.asset);
  } else {
    if (source.bytes.size() < sourceBytes)
      AURORA_THROW(std::runtime_error("Native texture source is truncated"));

    make_room(allocated);

    if (source.format == NativeTextureFormatYuv420) {
      // One movie frame per generation, converted and uploaded in bands of
      // eight rows that stay in the L1 cache (20 KiB at 640 texels).
      constexpr uint32_t Band = 8;
      const std::unique_ptr<uint8_t[]> rgba(new uint8_t[size_t(source.width) * Band * 4]);
      texture = gl::create_texture(source.width, source.height, 1, gl::Format::RGBA8, "Native movie frame");

      for (uint32_t y = 0; y < source.height; y += Band) {
        convert_yuv420(source.bytes.data(), source.width, source.height, y, Band, rgba.get());
        gl::upload_texture(*texture, 0, 0, y, source.width, Band, rgba.get(), source.width * 4);
      }

      cache.emplace(key, Entry{texture, allocated, ++serial, source.owner, source.owner_bytes});
      residentBytes += allocated;
      publish(texture);
      return;
    }

    gfx::ConvertedTexture converted;

    if (indexed) {
      if (source.palette_format > 2 || !source.palette_entries || source.palette_entries > 16384 || source.palette.size() < size_t(source.palette_entries) * 2)
        AURORA_THROW(std::runtime_error("Native indexed texture has no valid palette"));

      converted = gfx::convert_texture_palette(source.format, source.width, source.height, source.levels, {source.bytes.data(), sourceBytes}, static_cast<GXTlutFmt>(source.palette_format),
                                               source.palette_entries, {source.palette.data(), size_t(source.palette_entries) * 2});
    } else
      converted = gfx::convert_texture(source.format, source.width, source.height, source.levels, {source.bytes.data(), sourceBytes});

    if (converted.data.empty() || converted.format != gl::Format::RGBA8)
      AURORA_THROW(std::runtime_error("Native texture conversion produced no RGBA image"));

    texture = gl::create_texture(source.width, source.height, source.levels, converted.format, "Native producer texture");
    size_t offset = 0;

    for (unsigned mip = 0; mip < source.levels; ++mip) {
      const unsigned w = std::max(source.width >> mip, 1u), h = std::max(source.height >> mip, 1u);
      gl::upload_texture(*texture, mip, 0, 0, w, h, converted.data.data() + offset, w * 4);
      offset += size_t(w) * h * 4;
    }
  }

  const auto [entry, inserted] = cache.try_emplace(key, Entry{texture, allocated, ++serial, source.owner, source.owner_bytes});
  residentBytes += allocated;
  remember(entry->second, publish(texture));
}
} // namespace

void bind_native_texture(uint32_t unit, const NativeTextureSource& source, const NativeTextureSampler& sampling) {
  bind(unit, source, sampling, nullptr);
}

void bind_native_texture(uint32_t unit, const NativeTextureSource& source, const NativeTextureSampler& sampling, NativeTextureMemo& memo) {
  if (!memo.entry || !SameSource(memo.source, source) || !SameSampling(memo.sampling, sampling) || !rebind_native_texture(unit, memo))
    bind(unit, source, sampling, &memo);
}

bool rebind_native_texture(uint32_t unit, NativeTextureMemo& memo) {
  if (!memo.entry || memo.stamp != cacheStamp || unit >= bindings.size())
    return false;

  owner();

  // A GPU producer's target at the image's address binds its own backing.
  const auto& source = memo.source;
  const auto identity = static_cast<const uint8_t*>(source.identity ? source.identity : source.bytes.data());

  if (identity && (scene_feedback_texture_version(identity, source.width, source.height) || generated_texture_version(identity, source.width, source.height)))
    return false;

  logicalBindings[unit] = {source, memo.sampling, true};
  shaderLodBias[unit] = memo.sampling.lod_bias;

  if (!aurora_native_frame_should_render())
    return true;

  auto& entry = *static_cast<Entry*>(const_cast<void*>(memo.entry));
  entry.used = ++serial;
  entry.owner = source.owner;
  entry.ownerBytes = source.owner_bytes;
  portableSampling[unit] = memo.portable;
  shaderLodBias[unit] = memo.shader_bias;
  bindings[unit] = entry.texture;
  gl::bind_texture(unit, entry.texture->gl(), memo.sampler);
  return true;
}

void clear_native_textures() {
  owner();
  logicalBindings = {};
  bindings = {};
  shaderLodBias = {};
  portableSampling = {};
  cache.clear();
  ++cacheStamp;
  residentBytes = 0;
  serial = 0;
}

// A read of the bound state for every draw: the binders check the owner.
float native_texture_shader_lod_bias(uint32_t unit) {
  if (unit >= logicalBindings.size() || !logicalBindings[unit].valid || !aurora_native_frame_should_render())
    AURORA_THROW(std::runtime_error("Native texture shader bias requested without an admitted binding"));

  return shaderLodBias[unit];
}

NativeTexturePortableSampling native_texture_portable_sampling(uint32_t unit) {
  (void)native_texture_shader_lod_bias(unit);
  return portableSampling[unit];
}

bool retire_native_textures(std::vector<NativeTextureOwnerRange>& ranges) {
  owner();

  if (ranges.empty())
    return false;

  std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.address < b.address; });
  size_t merged = 0;

  for (const auto& range : ranges) {
    if (!range.bytes)
      continue;

    if (merged && uint64_t(ranges[merged - 1].address) + ranges[merged - 1].bytes >= range.address) {
      auto& last = ranges[merged - 1];
      last.bytes = uint32_t(std::max(uint64_t(last.address) + last.bytes, uint64_t(range.address) + range.bytes) - last.address);
    } else
      ranges[merged++] = range;
  }

  ranges.resize(merged);

  // Whether the last merged range that starts before the owner's end reaches it.
  const auto freed = [&](uint32_t owner, uint32_t bytes) {
    if (!bytes)
      return false;

    const uint64_t end = uint64_t(owner) + bytes;
    const auto next = std::partition_point(ranges.begin(), ranges.end(), [&](const auto& range) { return range.address < end; });
    return next != ranges.begin() && uint64_t(std::prev(next)->address) + std::prev(next)->bytes > owner;
  };

  // A frame's start rebinds the logical bindings (restore_native_textures),
  // which must not recreate a freed image: the next draw binds its own. The
  // backend keeps its texture until that bind replaces it.
  bool unbound = false;

  for (auto& binding : logicalBindings)
    if (binding.valid && freed(binding.source.owner, binding.source.owner_bytes)) {
      binding.valid = false;
      unbound = true;
    }

  uint32_t retired = 0;
  size_t retiredBytes = 0;

  for (auto it = cache.begin(); it != cache.end();) {
    const auto& entry = it->second;

    if (freed(entry.owner, entry.ownerBytes)) {
      ++retired;
      retiredBytes += entry.bytes;
      residentBytes -= entry.bytes;
      it = cache.erase(it);
      ++cacheStamp;
    } else
      ++it;
  }

  if (retired)
    std::fprintf(stderr, "Aurora native textures: retired %u textures (%.2f MiB) whose sources were freed, %u (%.2f MiB) stay\n", retired, retiredBytes / 1048576.0,
                 unsigned(cache.size()), residentBytes / 1048576.0);

  return unbound;
}

void restore_native_textures() {
  owner();

  for (unsigned unit = 0; unit < logicalBindings.size(); ++unit) {
    const auto binding = logicalBindings[unit];

    if (binding.valid)
      bind_native_texture(unit, binding.source, binding.sampling);
  }
}
} // namespace aurora

namespace aurora::gl {
NativeTextureReadStats take_native_texture_read_stats() noexcept {
  auto& s = gfx::NativeTextureSet::read_stats();
  return {s.count.exchange(0, std::memory_order_relaxed), s.bytes.exchange(0, std::memory_order_relaxed), s.ns.exchange(0, std::memory_order_relaxed),
          s.longestNs.exchange(0, std::memory_order_relaxed)};
}
} // namespace aurora::gl
