#include <aurora/native_texture.hpp>
#include <aurora/aurora.h>
#include <aurora/generated_texture.hpp>
#include <aurora/native_scene_feedback.hpp>
#include "texture_convert.hpp"
#include "../gxm/sampler_bias.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <tuple>
#include <stdexcept>

namespace aurora {
namespace {
// Leave space in the CDRAM arena for display surfaces, depth and offscreen
// targets. Backend fences retain evicted in-flight storage until it is safe.
constexpr size_t CacheBudget=48u<<20;
using Key=std::tuple<uintptr_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,
                     uint64_t,uintptr_t,uint32_t,uint32_t,uint64_t>;
struct Entry { gl::TexturePtr texture; size_t bytes; uint64_t used; };
std::map<Key,Entry> cache;
std::array<gl::TexturePtr,8> bindings;
struct Binding { NativeTextureSource source; NativeTextureSampler sampling; bool valid=false; };
std::array<Binding,8> logicalBindings;
std::array<float,8> shaderLodBias{};
size_t residentBytes=0;
uint64_t serial=0;
void owner() {
  if(!gl::on_render_thread()) throw std::runtime_error("Native texture changed outside graphics owner");
}
gl::SamplerDesc sampler(const NativeTextureSampler& source) {
  if(source.wrap_s>2 || source.wrap_t>2 || source.min_filter>5 || source.mag_filter>1 ||
     source.anisotropy>2 || !std::isfinite(source.min_lod) || !std::isfinite(source.max_lod) ||
     !std::isfinite(source.lod_bias) || source.min_lod>source.max_lod)
    throw std::runtime_error("Invalid native texture sampler");
  constexpr GLenum wrap[]{GL_CLAMP_TO_EDGE,GL_REPEAT,GL_MIRRORED_REPEAT};
  constexpr GLenum filter[]{GL_NEAREST,GL_LINEAR,GL_NEAREST_MIPMAP_NEAREST,
    GL_LINEAR_MIPMAP_NEAREST,GL_NEAREST_MIPMAP_LINEAR,GL_LINEAR_MIPMAP_LINEAR};
  return {wrap[source.wrap_s],wrap[source.wrap_t],filter[source.min_filter],filter[source.mag_filter],
    source.min_lod,source.max_lod,1u<<source.anisotropy};
}
void make_room(size_t bytes) {
  if(bytes>CacheBudget) throw std::runtime_error("Native texture exceeds resident cache budget");
  while(residentBytes>CacheBudget-bytes) {
    auto victim=cache.end();
    for(auto it=cache.begin();it!=cache.end();++it)
      if(it->second.texture.use_count()==1 && (victim==cache.end() || it->second.used<victim->second.used)) victim=it;
    if(victim==cache.end()) throw std::runtime_error("Bound native textures exceed resident cache budget");
    residentBytes-=victim->second.bytes;cache.erase(victim);
  }
}
size_t rgba_allocation(const NativeTextureSource& source) {
  size_t bytes=0;
  for(unsigned mip=0;mip<source.levels;++mip) {
    const size_t w=std::max(source.width>>mip,1u),h=std::max(source.height>>mip,1u);
    bytes+=mip+1<source.levels ? size_t(std::max(std::bit_ceil(source.width)>>mip,8u))*
      std::max(std::bit_ceil(source.height)>>mip,1u)*4 : ((w+7)&~size_t(7))*h*4;
  }
  return (bytes+63)&~size_t(63);
}
}

size_t native_texture_source_size(uint32_t format,uint32_t width,uint32_t height,uint32_t levels) {
  if(!width || !height || width>4096 || height>4096 || !levels ||
     levels>std::bit_width(std::max(width,height))) throw std::runtime_error("Invalid native texture dimensions");
  uint32_t tileWidth=4,tileHeight=4,tileBytes=32;
  switch(format) {
  case 0:case 8: tileWidth=tileHeight=8;break;
  case 1:case 2:case 9: tileWidth=8;break;
  case 3:case 4:case 5:case 10: break;
  case 6: tileBytes=64;break;
  case 14: tileWidth=tileHeight=8;break;
  default: throw std::runtime_error("Unsupported native texture source format");
  }
  size_t bytes=0;
  for(unsigned mip=0;mip<levels;++mip) {
    bytes+=size_t((width+tileWidth-1)/tileWidth)*((height+tileHeight-1)/tileHeight)*tileBytes;
    width=std::max(width>>1,1u);height=std::max(height>>1,1u);
  }
  return bytes;
}
void bind_native_texture(uint32_t unit,const NativeTextureSource& source,const NativeTextureSampler& sampling) {
  owner();
  if(unit>=bindings.size()) throw std::runtime_error("Invalid native texture unit");
  const auto descriptor=sampler(sampling);
  const size_t sourceBytes=native_texture_source_size(source.format,source.width,source.height,source.levels);
  // GPU producers publish their own backing revisions. A guest texture slot
  // naming such a target is not a mutable CPU image and needs no byte revision.
  // Resolve this before the frame gate so skipped bindings remain validated.
  const auto identity=source.bytes.data();
  const uint64_t feedbackRevision=identity ? scene_feedback_texture_version(identity,source.width,source.height) : 0;
  const uint32_t generatedRevision=identity && !feedbackRevision ? generated_texture_version(identity,source.width,source.height) : 0;
  if(source.mutable_source && !source.generation && !feedbackRevision && !generatedRevision)
    throw std::runtime_error("Mutable native texture has no producer revision");
  logicalBindings[unit]={source,sampling,true};
  shaderLodBias[unit]=sampling.lod_bias;
  if(!aurora_native_frame_should_render()) return;
  const auto publish=[&](const gl::TexturePtr& texture) {
    auto nativeDescriptor=descriptor;
#ifdef AURORA_GXM
    if(texture->format!=gl::Format::Depth32F && gxm::sampler_bias_encoding(sampling.lod_bias)>=0) {
      nativeDescriptor.lodBias=sampling.lod_bias;
      shaderLodBias[unit]=0;
    }
#endif
    bindings[unit]=texture;gl::bind_texture(unit,texture->gl(),gl::sampler(nativeDescriptor));
  };
  if(feedbackRevision) {
    if(!bind_scene_feedback_texture(unit,identity,source.width,source.height,sampling))
      throw std::runtime_error("Published scene feedback disappeared before binding");
    // GPU targets own their storage. Do not inspect or convert guest pixels.
    bindings[unit].reset();return;
  }
  if(generatedRevision) {publish(gfx::find_generated_texture(identity,generatedRevision)->texture);return;}
  const bool indexed=source.format==8 || source.format==9 || source.format==10;
  // A native asset already includes its immutable palette. Live palette or
  // image revisions must use their own producer payload instead.
  bool offline=source.asset && !source.mutable_source && source.original_image;
#ifdef AURORA_GXM
  gl::NativeTextureInfo offlineInfo{};
  if(offline) {
    offlineInfo=gl::native_texture_info(source.asset);
    if(offlineInfo.width!=source.width||offlineInfo.height!=source.height||offlineInfo.mips!=source.levels||
        (offlineInfo.sourceProvenance&&offlineInfo.sourceFormat!=source.format))
      throw std::runtime_error("Native asset metadata disagrees with published texture");
    if(indexed) {
      const int64_t delta=int64_t(reinterpret_cast<uintptr_t>(source.palette.data()))-
          int64_t(reinterpret_cast<uintptr_t>(source.bytes.data()));
      offline=source.original_palette&&offlineInfo.sourceProvenance&&
          delta==offlineInfo.sourcePaletteDelta&&source.palette_entries==offlineInfo.sourcePaletteEntries&&
          source.palette_format==offlineInfo.sourcePaletteFormat;
    }
  }
#else
  // Live host rendering retains its authored producer bytes. The standalone
  // GLES pack probe exercises offline payload layouts separately.
  offline=false;
#endif
  const Key key{offline?0:reinterpret_cast<uintptr_t>(source.bytes.data()),offline?source.asset:0,
    source.width,source.height,source.levels,source.format,offline?0:source.generation,
    offline?0:reinterpret_cast<uintptr_t>(source.palette.data()),offline?0:source.palette_format,
    offline?0:source.palette_entries,offline?0:source.palette_generation};
  if(auto found=cache.find(key);found!=cache.end()) {
    found->second.used=++serial;publish(found->second.texture);return;
  }
  // Drop the binding being replaced before evicting cold cache entries. Other
  // units keep their texture owners, and GPU consumers retain backing fences.
  bindings[unit].reset();
  for(auto it=cache.begin();it!=cache.end();) {
    if(!offline && std::get<0>(it->first)==std::get<0>(key) &&
       std::get<6>(it->first)!=source.generation && it->second.texture.use_count()==1) {
      residentBytes-=it->second.bytes;it=cache.erase(it);
    } else ++it;
  }
  gl::TexturePtr texture;
  size_t allocated=rgba_allocation(source);
#ifdef AURORA_GXM
  if(offline) {
    allocated=(size_t(offlineInfo.bytes)+63)&~size_t(63);
    make_room(allocated);texture=gl::native_texture_asset(source.asset);
  } else
#endif
  {
    if(source.bytes.size()<sourceBytes) throw std::runtime_error("Native texture source is truncated");
    make_room(allocated);
    gfx::ConvertedTexture converted;
    if(indexed) {
      if(source.palette_format>2 || !source.palette_entries || source.palette_entries>16384 ||
         source.palette.size()<size_t(source.palette_entries)*2)
        throw std::runtime_error("Native indexed texture has no valid palette");
      converted=gfx::convert_texture_palette(source.format,source.width,source.height,source.levels,
        {source.bytes.data(),sourceBytes},static_cast<GXTlutFmt>(source.palette_format),source.palette_entries,
        {source.palette.data(),size_t(source.palette_entries)*2});
    } else converted=gfx::convert_texture(source.format,source.width,source.height,source.levels,
                                          {source.bytes.data(),sourceBytes});
    if(converted.data.empty() || converted.format!=gl::Format::RGBA8)
      throw std::runtime_error("Native texture conversion produced no RGBA image");
    texture=gl::create_texture(source.width,source.height,source.levels,converted.format,"Native producer texture");
    size_t offset=0;
    for(unsigned mip=0;mip<source.levels;++mip) {
      const unsigned w=std::max(source.width>>mip,1u),h=std::max(source.height>>mip,1u);
      gl::upload_texture(*texture,mip,0,0,w,h,converted.data.data()+offset,w*4);
      offset+=size_t(w)*h*4;
    }
  }
  cache.emplace(key,Entry{texture,allocated,++serial});residentBytes+=allocated;
  publish(texture);
}
void clear_native_textures() {
  owner();logicalBindings={};bindings={};shaderLodBias={};cache.clear();residentBytes=0;serial=0;
}
float native_texture_shader_lod_bias(uint32_t unit) {
  owner();
  if(unit>=logicalBindings.size() || !logicalBindings[unit].valid || !aurora_native_frame_should_render())
    throw std::runtime_error("Native texture shader bias requested without an admitted binding");
  return shaderLodBias[unit];
}
void restore_native_textures() {
  owner();
  for(unsigned unit=0;unit<logicalBindings.size();++unit) {
    const auto binding=logicalBindings[unit];
    if(binding.valid) bind_native_texture(unit,binding.source,binding.sampling);
  }
}
}
