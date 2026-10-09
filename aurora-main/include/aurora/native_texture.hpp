#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace aurora {
// Immutable view of one explicitly published producer revision. Data is used
// only when creating a new resident texture; no content hashing or protection.
struct NativeTextureSource {
  std::span<const uint8_t> bytes, palette;
  uint32_t width=0,height=0,levels=1,format=0,asset=0;
  uint32_t palette_format=0,palette_entries=0;
  // CPU image/palette revisions. GPU producer registries certify their own
  // published backing revisions without reading guest image bytes.
  uint64_t generation=0,palette_generation=0;
  bool mutable_source=false;
  // Certified by explicit resource-load producers, never inferred from bytes.
  // Later image or palette writes invalidate the corresponding baseline.
  bool original_image=false,original_palette=false;
};
struct NativeTextureSampler {
  uint32_t wrap_s=0,wrap_t=0,min_filter=0,mag_filter=0,anisotropy=0;
  float min_lod=0,max_lod=0;
  float lod_bias=0;
};
size_t native_texture_source_size(uint32_t format,uint32_t width,uint32_t height,uint32_t levels);
void bind_native_texture(uint32_t unit,const NativeTextureSource& source,const NativeTextureSampler& sampler);
// Bias left for the shader after native sampler bias. Read on the graphics
// owner after frame admission and binding restoration.
float native_texture_shader_lod_bias(uint32_t unit);
// Authoritative GXM route for the same admitted backing, also available to host
// development capture. It never replaces the GLES shader's authored bias.
struct NativeTexturePortableSampling {
  uint32_t asset=0,width=0,height=0,mips=0,format=0;
  uint32_t min_filter=0,mag_filter=0,anisotropy=0;
  uint64_t generation=0,palette_generation=0;
  float authored_bias=0,shader_bias=0,sampler_bias=0;
};
NativeTexturePortableSampling native_texture_portable_sampling(uint32_t unit);
// Reapply logical bindings after presentation changed the backend's units.
void restore_native_textures();
void clear_native_textures();
}
