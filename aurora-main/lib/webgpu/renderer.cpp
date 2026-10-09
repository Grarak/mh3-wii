// The renderer interface (gl/gl.hpp) on WebGPU, for the web build
// (docs/optimizations/platforms/web-build.md). The GLES backend's immediate state model maps onto
// WebGPU as follows:
//
// - State calls only record state. A draw opens the target's render pass on
//   first use (a clear before it becomes the pass's load operation), finds or
//   creates the pipeline for its program, vertex layout, raster state and
//   target formats, and binds two groups: group 0 holds every uniform block as
//   a dynamic offset into one uniform buffer, group 1 the program's textures.
// - Per-draw uniforms, vertices and indices are copied into staging memory;
//   a submission writes each staging range to its GPU buffer once, ahead of
//   its command buffer. Persistent uniform blocks live past the staging range
//   of the same uniform buffer.
// - Programs come from the WGSL shader bank (shader_bank.hpp), converted at
//   build time from the GLSL ES the GLES backend compiles; their vertex
//   stages already write WebGPU clip space, and the GL row order is kept
//   (texture row 0 is the top row on both), so viewports and scissors carry
//   over unchanged. The GL winding seen through the negated y is clockwise.
// - WebGPU objects live on the browser's main thread (gpu.hpp). Calls of the
//   context owner on another thread (the game thread before the render loop
//   starts) run there synchronously.
#include "../gl/gl.hpp"
#include <aurora/fast_map.h>
#include <aurora/fatal.hpp>
#include <aurora/native_frame_gate.hpp>
#include <aurora/gpu_completion.hpp>
#include "../internal.hpp"
#include "../gfx/native_mesh_io.hpp"
#include "../gfx/native_mesh_layout.hpp"
#include "../gfx/native_texture_set.hpp"
#include "../gfx/native_texture_decode.hpp"
#include "gpu.hpp"
#include "shader_bank.hpp"

#include <emscripten/em_asm.h>
#include <emscripten/em_macros.h>
#include <pthread.h>
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace aurora::gfx {
uint32_t current_frame() noexcept;
}

namespace aurora::gl {
namespace {
using namespace webgpu;
Module Log("aurora::gl");

[[noreturn]] void fail(const char* message) {
  std::fprintf(stderr, "Aurora WebGPU renderer: %s\n", message);
  std::fflush(stderr);
  std::abort();
}

// An entry point's failure stops the program. Without exceptions (release
// builds) it stops at its AURORA_THROW instead.
template <typename F>
decltype(auto) guarded(F&& operation) noexcept {
#if defined(__cpp_exceptions)
  try {
    return operation();
  } catch (const std::exception& error) {
    fail(error.what());
  } catch (...) {
    fail("Unknown renderer failure");
  }
#else
  return operation();
#endif
}

// ---- Memory layout ----

// Every group 0 binding is a 16 KiB window at a dynamic offset (the skinned
// palette, the largest block, is 12,240 bytes).
constexpr uint32_t UniformAlignment = 256;
constexpr uint32_t BindingBytes = 16384;
constexpr uint32_t UniformBindings = 8;
constexpr uint32_t StagingUniformBytes = 8u << 20;
constexpr uint32_t PersistentUniformBytes = 2u << 20;
constexpr uint32_t UniformBufferBytes = StagingUniformBytes + PersistentUniformBytes + BindingBytes;
constexpr uint32_t StagingVertexBytes = 16u << 20;
constexpr uint32_t StagingIndexBytes = 4u << 20;

// Group 0 bindings: the GL uniform binding points 0..5 and the loose
// uniforms of the vertex (6) and fragment (7) stage (web-shader-bank.py).
constexpr uint32_t LooseVertexBinding = 6, LooseFragmentBinding = 7;

// Attribute defaults for shader inputs a draw leaves unbound: (0, 0, 0, 1) as
// floats at 0 and as unsigned integers at 16, read with a zero stride.
constexpr uint32_t DefaultsBytes = 32;

// ---- Objects ----

enum class Upload : uint8_t { Direct, Rgb565, U8Rrrr, U8One, U8R000, U8R111, U8U8Grrr };

struct TextureEntry {
  wgpu::Texture texture;
  wgpu::TextureView view, target;
  uint32_t width = 0, height = 0, mips = 1;
  Format format = Format::RGBA8;
  wgpu::TextureFormat gpuFormat = wgpu::TextureFormat::RGBA8Unorm;
  Upload upload = Upload::Direct;
  uint32_t bytesPerPixel = 4;
  bool filterable = true;

  // The submission serial that last referenced the texture.
  uint64_t used = 0;
};

struct Slot {
  std::string name;
  int unit = 0; // fixed programs: the GL unit; shaded: the texture role
  char kind = 'f';
  bool sampled = false;
};

struct ProgramEntry {
  std::string label;
  wgpu::ShaderModule vertex, fragment;
  wgpu::BindGroupLayout textureLayout;
  wgpu::PipelineLayout layout;
  std::vector<Slot> slots;
  std::vector<uint32_t> blocks; // group 0 bindings of uniform blocks, both stages
  std::vector<std::string> loose[2];
  std::vector<std::array<float, 4>> looseValues[2];
  bool shaded = false;
  bool geometry = false; // shaded program over AuroraNativeMaterialVertex input
};

struct VertexLayout {
  uint64_t stride = 0;
  std::vector<wgpu::VertexAttribute> attributes, defaults;
};

struct PipelineKey {
  GLuint program = 0;
  uint32_t layout = 0;
  wgpu::TextureFormat color = wgpu::TextureFormat::Undefined, depth = wgpu::TextureFormat::Undefined;
  RasterState raster;
  bool operator==(const PipelineKey& rhs) const {
    const auto& a = raster;
    const auto& b = rhs.raster;
    return program == rhs.program && layout == rhs.layout && color == rhs.color && depth == rhs.depth && a.blend == b.blend && a.colorEquation == b.colorEquation &&
           a.alphaEquation == b.alphaEquation && a.srcColor == b.srcColor && a.dstColor == b.dstColor && a.srcAlpha == b.srcAlpha && a.dstAlpha == b.dstAlpha &&
           a.depthTest == b.depthTest && a.depthWrite == b.depthWrite && a.depthFunc == b.depthFunc && a.cull == b.cull && a.cullFace == b.cullFace && a.colorMask == b.colorMask;
  }

  friend uint32_t FastHashValue(const PipelineKey& k) {
    const auto& r = k.raster;
    const uint32_t mask = r.colorMask[0] | r.colorMask[1] << 1 | r.colorMask[2] << 2 | r.colorMask[3] << 3;
    return FastHash(k.program, k.layout, k.color, k.depth, r.blend, r.colorEquation, r.alphaEquation, r.srcColor, r.dstColor, r.srcAlpha, r.dstAlpha, r.depthTest, r.depthWrite,
                    r.depthFunc, r.cull, r.cullFace, mask);
  }
};

struct TextureGroupKey {
  const void* layout = nullptr;
  std::array<GLuint, 4> textures{};
  std::array<GLuint, 4> samplers{};
  bool operator==(const TextureGroupKey&) const = default;
  friend uint32_t FastHashValue(const TextureGroupKey& k) {
    return FastHash(k.layout, k.textures[0], k.textures[1], k.textures[2], k.textures[3], k.samplers[0], k.samplers[1], k.samplers[2], k.samplers[3]);
  }
};

struct SamplerEntry {
  SamplerDesc desc;
  wgpu::Sampler filtering, nearest;
};

struct UniformRange {
  GLuint buffer = 0;
  uint32_t offset = 0, size = 0;
};

struct State {
  RasterState raster;
  std::array<int32_t, 4> viewport{0, 0, 0, 0};
  std::array<float, 2> depthRange{0.f, 1.f};
  std::array<int32_t, 4> scissor{0, 0, 0, 0};
  bool scissorEnabled = false;
  std::array<float, 4> blendColor{0.f, 0.f, 0.f, 0.f};
  GLuint color = 0, depth = 0, program = 0;
  std::array<GLuint, 16> textures{}, samplers{};
  std::array<UniformRange, UniformBindings> uniforms{};
};

// The bound render pass and the dynamic state already set in it.
struct Pass {
  wgpu::RenderPassEncoder encoder;
  GLuint color = 0, depth = 0;
  uint32_t width = 0, height = 0;
  const wgpu::RenderPipeline* pipeline = nullptr;
  std::array<float, 6> viewport{-1, -1, -1, -1, -1, -1};
  std::array<uint32_t, 4> scissor{~0u, ~0u, ~0u, ~0u};
  std::array<float, 4> blend{-1, -1, -1, -1};
  const wgpu::BindGroup* textures = nullptr;
};

// The pass's first operation, when it is a whole-target clear.
struct PendingClear {
  bool color = false, depth = false;
  std::array<float, 4> colorValue{};
  float depthValue = 1.f;
};

constexpr std::array<FormatInfo, 19> FormatInfos{{
    {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false, false, true},
    {GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, false, false, true},
    {GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2, false, false, true},
    {GL_R16I, GL_RED_INTEGER, GL_SHORT, 2, true, false, false},
    {GL_R32F, GL_RED, GL_FLOAT, 4, false, false, false},
    {GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 4, true, false, false},
    {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 4, false, true, false},
    // As on GLES, offline formats the backend cannot sample load as RGBA8.
    {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false, false, true},
    {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false, false, true},
    {GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, false, false, true},
    {GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, false, false, true},
    {GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, false, false, true},
    {GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, false, false, true},
    {GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2, false, false, true},
    {GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2, false, false, true},
    {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false, false, true},
    {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false, false, true},
    {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false, false, true},
    {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false, false, true},
}};

// ---- WGSL of the backend's own programs ----

// A full-target triangle at depth `clear.depth.x` writing `clear.color`.
constexpr const char* ClearWgsl = R"(
struct Clear { color: vec4f, depth: vec4f }
@group(0) @binding(0) var<uniform> clear: Clear;
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
  let p = array(vec2f(-1.0, 1.0), vec2f(-1.0, -3.0), vec2f(3.0, 1.0));
  return vec4f(p[i], clear.depth.x, 1.0);
}
@fragment fn fs() -> @location(0) vec4f { return clear.color; }
)";

// The letterboxed blit of the present source into the canvas.
constexpr const char* PresentWgsl = R"(
@group(1) @binding(0) var source: texture_2d<f32>;
@group(1) @binding(1) var source_sampler: sampler;
struct Out { @builtin(position) position: vec4f, @location(0) uv: vec2f }
@vertex fn vs(@builtin(vertex_index) i: u32) -> Out {
  let p = array(vec2f(-1.0, 1.0), vec2f(-1.0, -3.0), vec2f(3.0, 1.0));
  let uv = array(vec2f(0.0, 0.0), vec2f(0.0, 2.0), vec2f(2.0, 0.0));
  return Out(vec4f(p[i], 0.0, 1.0), uv[i]);
}
@fragment fn fs(in: Out) -> @location(0) vec4f { return vec4f(textureSample(source, source_sampler, in.uv).rgb, 1.0); }
)";

// AuroraNativeVertex triangles (the launcher's UI): GL clip space with y up,
// which the GLES program negates for its flipped rows.
constexpr const char* NativeGeometryWgsl = R"(
@group(1) @binding(0) var native_texture: texture_2d<f32>;
@group(1) @binding(1) var native_sampler: sampler;
struct Out { @builtin(position) position: vec4f, @location(0) uv: vec2f, @location(1) color: vec4f }
@vertex fn vs(@location(0) position: vec4f, @location(1) uv: vec2f, @location(2) color: vec4f) -> Out {
  return Out(vec4f(position.x, position.y, 0.5 * (position.z + position.w), position.w), uv, color);
}
@fragment fn colored(in: Out) -> @location(0) vec4f { return in.color; }
@fragment fn textured(in: Out) -> @location(0) vec4f { return textureSample(native_texture, native_sampler, in.uv) * in.color; }
)";

// Reserved program names of the backend's own programs.
constexpr GLuint ClearProgram = 1, PresentProgram = 2, NativeColoredProgram = 3, NativeTexturedProgram = 4;

class Renderer {
public:
  Renderer() {
    create_buffers();
    create_layouts();
    create_utility_programs();
    encoder = g_device.CreateCommandEncoder();
  }

  // ---- Names ----

  GLuint name() {
    if (!nextName)
      AURORA_THROW(std::runtime_error("Renderer name space exhausted"));

    return nextName++;
  }

  TextureEntry& texture(GLuint id) {
    const auto it = textures.find(id);

    if (it == textures.end())
      AURORA_THROW(std::runtime_error("Unknown texture name"));

    return it->second;
  }

  // ---- Submission ----

  void close_pass() {
    if (pass.encoder) {
      pass.encoder.End();
      pass = {};
    }
  }

  // Ends the pass; a clear still waiting to become a load operation runs first.
  void end_pass() {
    if (pending.color || pending.depth)
      begin_pass();

    close_pass();
  }

  // Submits the recorded commands after their staging data.
  void flush() {
    end_pass();

    if (uniformCursor)
      g_queue.WriteBuffer(uniformBuffer, 0, uniformStaging.data(), (uniformCursor + 3) & ~3u);

    if (vertexCursor)
      g_queue.WriteBuffer(vertexBuffer, 0, vertexStaging.data(), (vertexCursor + 3) & ~3u);

    if (indexCursor)
      g_queue.WriteBuffer(indexBuffer, 0, indexStaging.data(), (indexCursor + 3) & ~3u);

    const auto commands = encoder.Finish();
    g_queue.Submit(1, &commands);
    encoder = g_device.CreateCommandEncoder();
    uniformCursor = vertexCursor = indexCursor = 0;
    ++serial;
  }

  // Makes room for one draw's staging data before any of it is allocated.
  void reserve(uint32_t uniforms, uint32_t vertices, uint32_t indices) {
    if (uniforms > StagingUniformBytes || vertices > StagingVertexBytes || indices > StagingIndexBytes)
      AURORA_THROW(std::runtime_error("Draw data exceeds the staging capacity"));

    if (uniformCursor + uniforms > StagingUniformBytes || vertexCursor + vertices > StagingVertexBytes || indexCursor + indices > StagingIndexBytes)
      flush();
  }

  uint32_t stage_uniform(const void* data, uint32_t size) {
    const uint32_t offset = (uniformCursor + UniformAlignment - 1) & ~(UniformAlignment - 1);

    if (offset + size > StagingUniformBytes)
      AURORA_THROW(std::runtime_error("Uniform staging overflow"));

    std::memcpy(uniformStaging.data() + offset, data, size);
    uniformCursor = offset + size;
    return offset;
  }

  uint32_t stage_vertices(const void* data, uint32_t size) {
    const uint32_t offset = (vertexCursor + 15) & ~15u;
    std::memcpy(vertexStaging.data() + offset, data, size);
    vertexCursor = offset + size;
    return offset;
  }

  uint32_t stage_indices(const void* data, uint32_t size) {
    const uint32_t offset = (indexCursor + 3) & ~3u;
    std::memcpy(indexStaging.data() + offset, data, size);
    indexCursor = offset + size;
    return offset;
  }

  // ---- Resources ----

  void create_buffers() {
    const auto buffer = [](const char* label, wgpu::BufferUsage usage, uint64_t size) {
      wgpu::BufferDescriptor descriptor;
      descriptor.label = label;
      descriptor.usage = usage | wgpu::BufferUsage::CopyDst;
      descriptor.size = size;
      return g_device.CreateBuffer(&descriptor);
    };

    // Each binding window may start at the last staged offset.
    uniformBuffer = buffer("Aurora uniforms", wgpu::BufferUsage::Uniform, UniformBufferBytes);
    vertexBuffer = buffer("Aurora vertices", wgpu::BufferUsage::Vertex, StagingVertexBytes);
    indexBuffer = buffer("Aurora indices", wgpu::BufferUsage::Index, StagingIndexBytes);
    defaultsBuffer = buffer("Aurora attribute defaults", wgpu::BufferUsage::Vertex, DefaultsBytes);
    const float floats[4]{0.f, 0.f, 0.f, 1.f};
    const uint32_t uints[4]{0, 0, 0, 1};
    uint8_t defaults[DefaultsBytes];
    std::memcpy(defaults, floats, 16);
    std::memcpy(defaults + 16, uints, 16);
    g_queue.WriteBuffer(defaultsBuffer, 0, defaults, sizeof(defaults));
    uniformStaging.resize(StagingUniformBytes);
    vertexStaging.resize(StagingVertexBytes);
    indexStaging.resize(StagingIndexBytes);

    // One texture and one sampler for every binding a draw leaves empty.
    wgpu::TextureDescriptor descriptor;
    descriptor.label = "Aurora placeholder";
    descriptor.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    descriptor.size = {1, 1, 1};
    descriptor.format = wgpu::TextureFormat::RGBA8Unorm;
    placeholder = g_device.CreateTexture(&descriptor);
    placeholderView = placeholder.CreateView();
    const uint8_t white[4]{255, 255, 255, 255};
    wgpu::TexelCopyTextureInfo destination{.texture = placeholder};
    wgpu::TexelCopyBufferLayout layout{.offset = 0, .bytesPerRow = 4, .rowsPerImage = 1};
    const wgpu::Extent3D size{1, 1, 1};
    g_queue.WriteTexture(&destination, white, 4, &layout, &size);

    for (const auto kind : {wgpu::TextureSampleType::Sint, wgpu::TextureSampleType::Uint}) {
      descriptor.format = kind == wgpu::TextureSampleType::Sint ? wgpu::TextureFormat::R16Sint : wgpu::TextureFormat::R32Uint;
      auto& view = kind == wgpu::TextureSampleType::Sint ? placeholderSint : placeholderUint;
      view = g_device.CreateTexture(&descriptor).CreateView();
    }

    wgpu::SamplerDescriptor sampler;
    placeholderSampler = g_device.CreateSampler(&sampler);
  }

  void create_layouts() {
    std::array<wgpu::BindGroupLayoutEntry, UniformBindings> entries{};

    for (uint32_t i = 0; i < UniformBindings; ++i) {
      entries[i].binding = i;
      entries[i].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
      entries[i].buffer.type = wgpu::BufferBindingType::Uniform;
      entries[i].buffer.hasDynamicOffset = true;
      entries[i].buffer.minBindingSize = 0;
    }

    wgpu::BindGroupLayoutDescriptor descriptor;
    descriptor.label = "Aurora uniforms";
    descriptor.entryCount = entries.size();
    descriptor.entries = entries.data();
    uniformLayout = g_device.CreateBindGroupLayout(&descriptor);
    std::array<wgpu::BindGroupEntry, UniformBindings> bindings{};

    for (uint32_t i = 0; i < UniformBindings; ++i) {
      bindings[i].binding = i;
      bindings[i].buffer = uniformBuffer;
      bindings[i].offset = 0;
      bindings[i].size = BindingBytes;
    }

    wgpu::BindGroupDescriptor group;
    group.label = "Aurora uniforms";
    group.layout = uniformLayout;
    group.entryCount = bindings.size();
    group.entries = bindings.data();
    uniformGroup = g_device.CreateBindGroup(&group);
  }

  wgpu::ShaderModule module(const std::string& wgsl, const std::string& label) {
    wgpu::ShaderSourceWGSL source;
    source.code = wgsl.c_str();
    wgpu::ShaderModuleDescriptor descriptor;
    descriptor.nextInChain = &source;
    descriptor.label = label.c_str();
    return g_device.CreateShaderModule(&descriptor);
  }

  wgpu::BindGroupLayout texture_layout(const std::vector<Slot>& slots) {
    std::vector<wgpu::BindGroupLayoutEntry> entries;

    for (uint32_t k = 0; k < slots.size(); ++k) {
      const auto& slot = slots[k];
      wgpu::BindGroupLayoutEntry texture{};
      texture.binding = 2 * k;
      texture.visibility = wgpu::ShaderStage::Fragment;
      texture.texture.viewDimension = wgpu::TextureViewDimension::e2D;
      texture.texture.sampleType = slot.kind == 'i'  ? wgpu::TextureSampleType::Sint
                                   : slot.kind == 'u' ? wgpu::TextureSampleType::Uint
                                   : slot.sampled     ? wgpu::TextureSampleType::Float
                                                      : wgpu::TextureSampleType::UnfilterableFloat;
      entries.push_back(texture);
      wgpu::BindGroupLayoutEntry sampler{};
      sampler.binding = 2 * k + 1;
      sampler.visibility = wgpu::ShaderStage::Fragment;
      sampler.sampler.type = slot.sampled ? wgpu::SamplerBindingType::Filtering : wgpu::SamplerBindingType::NonFiltering;
      entries.push_back(sampler);
    }

    wgpu::BindGroupLayoutDescriptor descriptor;
    descriptor.entryCount = entries.size();
    descriptor.entries = entries.data();
    return g_device.CreateBindGroupLayout(&descriptor);
  }

  wgpu::PipelineLayout pipeline_layout(const wgpu::BindGroupLayout& textureLayout) {
    const wgpu::BindGroupLayout layouts[2]{uniformLayout, textureLayout};
    wgpu::PipelineLayoutDescriptor descriptor;
    descriptor.bindGroupLayoutCount = textureLayout ? 2 : 1;
    descriptor.bindGroupLayouts = layouts;
    return g_device.CreatePipelineLayout(&descriptor);
  }

  void create_utility_programs() {
    const auto add = [&](GLuint id, const char* label, const char* wgsl, std::vector<Slot> slots) {
      ProgramEntry entry;
      entry.label = label;
      entry.vertex = entry.fragment = module(wgsl, label);
      entry.slots = std::move(slots);

      if (!entry.slots.empty())
        entry.textureLayout = texture_layout(entry.slots);

      entry.layout = pipeline_layout(entry.textureLayout);
      programs.emplace(id, std::move(entry));
    };

    add(ClearProgram, "Aurora clear", ClearWgsl, {});
    add(PresentProgram, "Aurora present", PresentWgsl, {{"source", 0, 'f', true}});
    add(NativeColoredProgram, "Native colored geometry", NativeGeometryWgsl, {{"native_texture", 0, 'f', true}});
    add(NativeTexturedProgram, "Native textured geometry", NativeGeometryWgsl, {{"native_texture", 0, 'f', true}});
    nextName = NativeTexturedProgram + 1;
  }

  // ---- Samplers ----

  static wgpu::AddressMode address(GLenum mode) {
    switch (mode) {
    case GL_REPEAT:
      return wgpu::AddressMode::Repeat;
    case GL_MIRRORED_REPEAT:
      return wgpu::AddressMode::MirrorRepeat;
    default:
      return wgpu::AddressMode::ClampToEdge;
    }
  }

  SamplerEntry make_sampler(const SamplerDesc& desc) {
    const bool mipmapped = desc.minFilter != GL_NEAREST && desc.minFilter != GL_LINEAR;
    const bool minLinear = desc.minFilter == GL_LINEAR || desc.minFilter == GL_LINEAR_MIPMAP_NEAREST || desc.minFilter == GL_LINEAR_MIPMAP_LINEAR;
    const bool mipLinear = desc.minFilter == GL_NEAREST_MIPMAP_LINEAR || desc.minFilter == GL_LINEAR_MIPMAP_LINEAR;
    wgpu::SamplerDescriptor descriptor;
    descriptor.addressModeU = address(desc.wrapS);
    descriptor.addressModeV = address(desc.wrapT);
    descriptor.addressModeW = wgpu::AddressMode::ClampToEdge;
    descriptor.magFilter = desc.magFilter == GL_LINEAR ? wgpu::FilterMode::Linear : wgpu::FilterMode::Nearest;
    descriptor.minFilter = minLinear ? wgpu::FilterMode::Linear : wgpu::FilterMode::Nearest;
    descriptor.mipmapFilter = mipLinear ? wgpu::MipmapFilterMode::Linear : wgpu::MipmapFilterMode::Nearest;

    // GL samples level 0 alone without a mipmap filter.
    descriptor.lodMinClamp = mipmapped ? std::clamp(desc.minLod, 0.f, 32.f) : 0.f;
    descriptor.lodMaxClamp = mipmapped ? std::clamp(desc.maxLod, descriptor.lodMinClamp, 32.f) : 0.f;
    const bool linear = descriptor.magFilter == wgpu::FilterMode::Linear && minLinear && mipLinear;
    descriptor.maxAnisotropy = uint16_t(linear ? std::clamp<uint32_t>(desc.maxAnisotropy, 1, 16) : 1);
    SamplerEntry entry;
    entry.desc = desc;
    entry.filtering = g_device.CreateSampler(&descriptor);
    descriptor.magFilter = descriptor.minFilter = wgpu::FilterMode::Nearest;
    descriptor.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
    descriptor.maxAnisotropy = 1;
    entry.nearest = g_device.CreateSampler(&descriptor);
    return entry;
  }

  // ---- Render passes ----

  std::pair<uint32_t, uint32_t> target_size() {
    if (state.color)
      return {texture(state.color).width, texture(state.color).height};

    if (state.depth)
      return {texture(state.depth).width, texture(state.depth).height};

    return {0, 0};
  }

  bool begin_pass() {
    if (pass.encoder && pass.color == state.color && pass.depth == state.depth)
      return true;

    close_pass();

    if (!state.color && !state.depth)
      return false;

    wgpu::RenderPassColorAttachment color{};
    wgpu::RenderPassDepthStencilAttachment depth{};
    wgpu::RenderPassDescriptor descriptor;

    if (state.color) {
      auto& target = texture(state.color);
      target.used = serial;
      color.view = target.target;
      color.loadOp = pending.color ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
      color.storeOp = wgpu::StoreOp::Store;
      color.clearValue = {pending.colorValue[0], pending.colorValue[1], pending.colorValue[2], pending.colorValue[3]};
      descriptor.colorAttachmentCount = 1;
      descriptor.colorAttachments = &color;
    }

    if (state.depth) {
      auto& target = texture(state.depth);
      target.used = serial;
      depth.view = target.target;
      depth.depthLoadOp = pending.depth ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
      depth.depthStoreOp = wgpu::StoreOp::Store;
      depth.depthClearValue = pending.depth ? pending.depthValue : 1.f;
      descriptor.depthStencilAttachment = &depth;
    }

    pending = {};
    pass.encoder = encoder.BeginRenderPass(&descriptor);
    pass.color = state.color;
    pass.depth = state.depth;
    const auto [width, height] = target_size();
    pass.width = width;
    pass.height = height;
    return true;
  }

  void bind_target(GLuint color, GLuint depth) {
    if (state.color == color && state.depth == depth)
      return;

    if (color && texture(color).format == Format::Depth32F)
      AURORA_THROW(std::runtime_error("Color attachment has a depth format"));

    if (depth && texture(depth).format != Format::Depth32F)
      AURORA_THROW(std::runtime_error("Depth attachment has a color format"));

    // A clear recorded for the previous target is its pass's load operation.
    if (pending.color || pending.depth)
      begin_pass();

    state.color = color;
    state.depth = depth;
  }

  // ---- Dynamic state ----

  void apply_dynamic_state(bool blendConstant) {
    const auto& v = state.viewport;
    const std::array<float, 6> viewport{float(v[0]), float(v[1]), float(v[2]), float(v[3]), std::clamp(state.depthRange[0], 0.f, 1.f),
                                        std::clamp(state.depthRange[1], 0.f, 1.f)};

    if (viewport != pass.viewport) {
      pass.encoder.SetViewport(viewport[0], viewport[1], viewport[2], viewport[3], viewport[4], std::max(viewport[4], viewport[5]));
      pass.viewport = viewport;
    }

    std::array<uint32_t, 4> scissor{0, 0, pass.width, pass.height};

    if (state.scissorEnabled) {
      const int64_t x0 = std::clamp<int64_t>(state.scissor[0], 0, pass.width), y0 = std::clamp<int64_t>(state.scissor[1], 0, pass.height);
      const int64_t x1 = std::clamp<int64_t>(int64_t(state.scissor[0]) + state.scissor[2], x0, pass.width);
      const int64_t y1 = std::clamp<int64_t>(int64_t(state.scissor[1]) + state.scissor[3], y0, pass.height);
      scissor = {uint32_t(x0), uint32_t(y0), uint32_t(x1 - x0), uint32_t(y1 - y0)};
    }

    if (scissor != pass.scissor) {
      pass.encoder.SetScissorRect(scissor[0], scissor[1], scissor[2], scissor[3]);
      pass.scissor = scissor;
    }

    if (blendConstant) {
      // GL_CONSTANT_ALPHA reads the constant's alpha in every channel.
      const auto& c = state.blendColor;
      const bool alpha = state.raster.srcColor == GL_CONSTANT_ALPHA || state.raster.dstColor == GL_CONSTANT_ALPHA || state.raster.srcAlpha == GL_CONSTANT_ALPHA ||
                         state.raster.dstAlpha == GL_CONSTANT_ALPHA || state.raster.srcColor == GL_ONE_MINUS_CONSTANT_ALPHA ||
                         state.raster.dstColor == GL_ONE_MINUS_CONSTANT_ALPHA;
      const std::array<float, 4> blend = alpha ? std::array<float, 4>{c[3], c[3], c[3], c[3]} : c;

      if (blend != pass.blend) {
        const wgpu::Color color{blend[0], blend[1], blend[2], blend[3]};
        pass.encoder.SetBlendConstant(&color);
        pass.blend = blend;
      }
    }
  }

  bool viewport_empty() const { return state.viewport[2] <= 0 || state.viewport[3] <= 0; }

  // ---- Pipelines ----

  static wgpu::BlendFactor blend_factor(GLenum factor) {
    switch (factor) {
    case GL_ZERO:
      return wgpu::BlendFactor::Zero;
    case GL_ONE:
      return wgpu::BlendFactor::One;
    case GL_SRC_COLOR:
      return wgpu::BlendFactor::Src;
    case GL_ONE_MINUS_SRC_COLOR:
      return wgpu::BlendFactor::OneMinusSrc;
    case GL_SRC_ALPHA:
      return wgpu::BlendFactor::SrcAlpha;
    case GL_ONE_MINUS_SRC_ALPHA:
      return wgpu::BlendFactor::OneMinusSrcAlpha;
    case GL_DST_ALPHA:
      return wgpu::BlendFactor::DstAlpha;
    case GL_ONE_MINUS_DST_ALPHA:
      return wgpu::BlendFactor::OneMinusDstAlpha;
    case GL_DST_COLOR:
      return wgpu::BlendFactor::Dst;
    case GL_ONE_MINUS_DST_COLOR:
      return wgpu::BlendFactor::OneMinusDst;
    case GL_CONSTANT_COLOR:
    case GL_CONSTANT_ALPHA:
      return wgpu::BlendFactor::Constant;
    case GL_ONE_MINUS_CONSTANT_COLOR:
    case GL_ONE_MINUS_CONSTANT_ALPHA:
      return wgpu::BlendFactor::OneMinusConstant;
    case GL_SRC_ALPHA_SATURATE:
      return wgpu::BlendFactor::SrcAlphaSaturated;
    default:
      AURORA_THROW(std::runtime_error("Unsupported blend factor"));
    }
  }

  static wgpu::BlendOperation blend_operation(GLenum equation) {
    switch (equation) {
    case GL_FUNC_ADD:
      return wgpu::BlendOperation::Add;
    case GL_FUNC_SUBTRACT:
      return wgpu::BlendOperation::Subtract;
    case GL_FUNC_REVERSE_SUBTRACT:
      return wgpu::BlendOperation::ReverseSubtract;
    case GL_MIN:
      return wgpu::BlendOperation::Min;
    case GL_MAX:
      return wgpu::BlendOperation::Max;
    default:
      AURORA_THROW(std::runtime_error("Unsupported blend equation"));
    }
  }

  static wgpu::CompareFunction compare(GLenum function) {
    switch (function) {
    case GL_NEVER:
      return wgpu::CompareFunction::Never;
    case GL_LESS:
      return wgpu::CompareFunction::Less;
    case GL_EQUAL:
      return wgpu::CompareFunction::Equal;
    case GL_LEQUAL:
      return wgpu::CompareFunction::LessEqual;
    case GL_GREATER:
      return wgpu::CompareFunction::Greater;
    case GL_NOTEQUAL:
      return wgpu::CompareFunction::NotEqual;
    case GL_GEQUAL:
      return wgpu::CompareFunction::GreaterEqual;
    default:
      return wgpu::CompareFunction::Always;
    }
  }

  uint32_t intern_layout(const VertexLayout& layout) {
    XXH3_state_t hasher;
    XXH3_64bits_reset(&hasher);
    XXH3_64bits_update(&hasher, &layout.stride, sizeof(layout.stride));

    for (const auto* list : {&layout.attributes, &layout.defaults}) {
      const uint32_t count = list->size();
      XXH3_64bits_update(&hasher, &count, sizeof(count));

      for (const auto& a : *list) {
        const uint64_t fields[3]{uint64_t(a.format), a.offset, a.shaderLocation};
        XXH3_64bits_update(&hasher, fields, sizeof(fields));
      }
    }

    const uint64_t hash = XXH3_64bits_digest(&hasher);
    const auto [it, inserted] = layoutIds.try_emplace(hash, uint32_t(layouts.size() + 1));

    if (inserted)
      layouts.push_back(layout);

    return it->second;
  }

  const wgpu::RenderPipeline& pipeline(GLuint id, ProgramEntry& program, uint32_t layoutId, const char* vertexEntry, const char* fragmentEntry) {
    PipelineKey key;
    key.program = id;
    key.layout = layoutId;
    key.color = state.color ? texture(state.color).gpuFormat : wgpu::TextureFormat::Undefined;
    key.depth = state.depth ? wgpu::TextureFormat::Depth32Float : wgpu::TextureFormat::Undefined;
    key.raster = state.raster;

    // Canonical fields: what the pipeline does not use does not split it.
    if (!key.raster.blend)
      key.raster.colorEquation = key.raster.alphaEquation = key.raster.srcColor = key.raster.dstColor = key.raster.srcAlpha = key.raster.dstAlpha = 0;

    if (!key.raster.depthTest || !state.depth) {
      key.raster.depthFunc = 0;
      key.raster.depthTest = false;
      key.raster.depthWrite = false;
    }

    if (!key.raster.cull)
      key.raster.cullFace = 0;

    const auto [it, inserted] = pipelines.try_emplace(key);

    if (!inserted)
      return it->second;

    std::array<wgpu::VertexBufferLayout, 2> buffers{};
    size_t bufferCount = 0;

    if (layoutId) {
      const auto& layout = layouts[layoutId - 1];
      buffers[0].stepMode = wgpu::VertexStepMode::Vertex;
      buffers[0].arrayStride = layout.stride;
      buffers[0].attributeCount = layout.attributes.size();
      buffers[0].attributes = layout.attributes.data();
      buffers[1].stepMode = wgpu::VertexStepMode::Vertex;
      buffers[1].arrayStride = 0;
      buffers[1].attributeCount = layout.defaults.size();
      buffers[1].attributes = layout.defaults.data();
      bufferCount = 2;
    }

    wgpu::RenderPipelineDescriptor descriptor;
    descriptor.label = program.label.c_str();
    descriptor.layout = program.layout;
    descriptor.vertex.module = program.vertex;
    descriptor.vertex.entryPoint = vertexEntry;
    descriptor.vertex.bufferCount = bufferCount;
    descriptor.vertex.buffers = buffers.data();
    descriptor.primitive.topology = wgpu::PrimitiveTopology::TriangleList;

    // Front faces are counter-clockwise in GL's (y-negated) clip space.
    descriptor.primitive.frontFace = wgpu::FrontFace::CW;
    descriptor.primitive.cullMode = !key.raster.cull ? wgpu::CullMode::None : key.raster.cullFace == GL_FRONT ? wgpu::CullMode::Front : wgpu::CullMode::Back;
    wgpu::DepthStencilState depth;

    if (state.depth) {
      depth.format = wgpu::TextureFormat::Depth32Float;
      depth.depthWriteEnabled = key.raster.depthWrite ? wgpu::OptionalBool::True : wgpu::OptionalBool::False;
      depth.depthCompare = key.raster.depthTest ? compare(key.raster.depthFunc) : wgpu::CompareFunction::Always;
      descriptor.depthStencil = &depth;
    }

    wgpu::BlendState blend;
    wgpu::ColorTargetState target;
    wgpu::FragmentState fragment;
    fragment.module = program.fragment;
    fragment.entryPoint = fragmentEntry;

    if (state.color) {
      target.format = key.color;
      const auto& r = key.raster;

      if (r.blend) {
        blend.color = {blend_operation(r.colorEquation), blend_factor(r.srcColor), blend_factor(r.dstColor)};
        blend.alpha = {blend_operation(r.alphaEquation), blend_factor(r.srcAlpha), blend_factor(r.dstAlpha)};

        // Min and max take no factors.
        if (r.colorEquation == GL_MIN || r.colorEquation == GL_MAX)
          blend.color.srcFactor = blend.color.dstFactor = wgpu::BlendFactor::One;

        if (r.alphaEquation == GL_MIN || r.alphaEquation == GL_MAX)
          blend.alpha.srcFactor = blend.alpha.dstFactor = wgpu::BlendFactor::One;

        target.blend = &blend;
      }

      target.writeMask = (r.colorMask[0] ? wgpu::ColorWriteMask::Red : wgpu::ColorWriteMask::None) | (r.colorMask[1] ? wgpu::ColorWriteMask::Green : wgpu::ColorWriteMask::None) |
                         (r.colorMask[2] ? wgpu::ColorWriteMask::Blue : wgpu::ColorWriteMask::None) | (r.colorMask[3] ? wgpu::ColorWriteMask::Alpha : wgpu::ColorWriteMask::None);

      // Integer targets take no blending.
      if (key.color == wgpu::TextureFormat::R16Sint || key.color == wgpu::TextureFormat::R32Uint)
        target.blend = nullptr;

      fragment.targetCount = 1;
      fragment.targets = &target;
    }

    descriptor.fragment = &fragment;
    it->second = g_device.CreateRenderPipeline(&descriptor);
    return it->second;
  }

  // ---- Bindings ----

  // Group 1 for the program's slots: slot k reads the texture at units[k].
  const wgpu::BindGroup* texture_group(const ProgramEntry& program, const std::array<int, 4>& units) {
    if (program.slots.empty())
      return nullptr;

    if (program.slots.size() > 4)
      AURORA_THROW(std::runtime_error("A program samples more than four textures"));

    TextureGroupKey key;
    key.layout = program.textureLayout.Get();

    for (uint32_t k = 0; k < program.slots.size(); ++k) {
      if (units[k] < 0 || units[k] >= int(state.textures.size()))
        continue;

      key.textures[k] = state.textures[units[k]];
      key.samplers[k] = state.samplers[units[k]];

      // WebGPU cannot sample a texture its pass renders to. On GL that read
      // is undefined; a program's unused roles may still name the target.
      if (key.textures[k] && (key.textures[k] == state.color || key.textures[k] == state.depth))
        key.textures[k] = 0;
    }

    for (uint32_t k = 0; k < program.slots.size(); ++k)
      if (key.textures[k])
        texture(key.textures[k]).used = serial;

    const auto [it, inserted] = textureGroups.try_emplace(key);

    if (!inserted)
      return &it->second;

    std::vector<wgpu::BindGroupEntry> entries;

    for (uint32_t k = 0; k < program.slots.size(); ++k) {
      const auto& slot = program.slots[k];
      wgpu::BindGroupEntry view{};
      view.binding = 2 * k;
      view.textureView = slot.kind == 'i' ? placeholderSint : slot.kind == 'u' ? placeholderUint : placeholderView;

      if (key.textures[k]) {
        const auto& source = texture(key.textures[k]);

        // A float slot that filters cannot take an unfilterable texture.
        if (!slot.sampled || source.filterable || slot.kind != 'f')
          view.textureView = source.view;
        else
          Log.error("{}: unfilterable texture bound to a filtering slot", program.label);
      }

      entries.push_back(view);
      wgpu::BindGroupEntry sampler{};
      sampler.binding = 2 * k + 1;
      sampler.sampler = placeholderSampler;
      const auto found = key.samplers[k] ? samplers.find(key.samplers[k]) : samplers.end();

      if (found != samplers.end())
        sampler.sampler = slot.sampled ? found->second.filtering : found->second.nearest;

      entries.push_back(sampler);
    }

    wgpu::BindGroupDescriptor descriptor;
    descriptor.layout = program.textureLayout;
    descriptor.entryCount = entries.size();
    descriptor.entries = entries.data();
    it->second = g_device.CreateBindGroup(&descriptor);
    return &it->second;
  }

  // Stages a uniform block's bytes once per submission and data pointer.
  struct Streamed {
    const void* data = nullptr;
    uint32_t size = 0;
    uint64_t serial = ~0ull;
    uint32_t offset = 0;
  };

  uint32_t stream(Streamed& cache, const void* data, uint32_t size) {
    if (cache.data != data || cache.size != size || cache.serial != serial) {
      cache.offset = stage_uniform(data, size);
      cache.data = data;
      cache.size = size;
      cache.serial = serial;
    }

    return cache.offset;
  }

  uint32_t block_offset(const UniformBlock& block, Streamed& cache) {
    if (block.buffer == PersistentBufferName)
      return StagingUniformBytes + block.offset;

    return stream(cache, block.data, block.size);
  }

  // Loose uniforms and GL uniform ranges of a fixed program.
  void fixed_offsets(ProgramEntry& program, std::array<uint32_t, UniformBindings>& offsets) {
    for (const uint32_t binding : program.blocks) {
      const auto& range = state.uniforms[binding];
      const auto found = buffers.find(range.buffer);

      if (found == buffers.end() || !range.size || uint64_t(range.offset) + range.size > found->second.size())
        AURORA_THROW(std::runtime_error(program.label + ": uniform block bound to an invalid range"));

      offsets[binding] = stage_uniform(found->second.data() + range.offset, range.size);
    }

    for (unsigned stage = 0; stage < 2; ++stage)
      if (!program.looseValues[stage].empty())
        offsets[stage ? LooseFragmentBinding : LooseVertexBinding] = stage_uniform(program.looseValues[stage].data(), uint32_t(program.looseValues[stage].size() * 16));
  }

  uint32_t fixed_bytes(const ProgramEntry& program) {
    uint32_t bytes = 0;

    for (const uint32_t binding : program.blocks)
      bytes += state.uniforms[binding].size + UniformAlignment;

    for (unsigned stage = 0; stage < 2; ++stage)
      bytes += uint32_t(program.looseValues[stage].size() * 16) + UniformAlignment;

    return bytes;
  }

  // Records one draw with its pipeline and groups; the caller binds buffers.
  bool prepare(GLuint id, ProgramEntry& program, uint32_t layoutId, const std::array<uint32_t, UniformBindings>& offsets, const std::array<int, 4>& units,
               const char* vertexEntry = "main", const char* fragmentEntry = "main") {
    if (!begin_pass() || viewport_empty())
      return false;

    if (state.raster.cull && state.raster.cullFace == GL_FRONT_AND_BACK)
      return false;

    const auto& selected = pipeline(id, program, layoutId, vertexEntry, fragmentEntry);
    const auto* group = texture_group(program, units);

    if (pass.pipeline != &selected) {
      pass.encoder.SetPipeline(selected);
      pass.pipeline = &selected;
      pass.textures = nullptr;
    }

    pass.encoder.SetBindGroup(0, uniformGroup, offsets.size(), offsets.data());

    if (group && group != pass.textures) {
      pass.encoder.SetBindGroup(1, *group);
      pass.textures = group;
    }

    apply_dynamic_state(state.raster.blend);
    return true;
  }

  // ---- Clears ----

  void clear(const float* color, float depth, bool depthOnly) {
    if (depthOnly && (!state.depth || !state.raster.depthWrite))
      return;

    if (!depthOnly && !state.color)
      return;

    const auto [width, height] = target_size();
    const bool whole = !state.scissorEnabled ||
                       (state.scissor[0] <= 0 && state.scissor[1] <= 0 && int64_t(state.scissor[0]) + state.scissor[2] >= int64_t(width) &&
                        int64_t(state.scissor[1]) + state.scissor[3] >= int64_t(height));
    const bool fullMask = state.raster.colorMask == std::array<bool, 4>{true, true, true, true};
    const bool open = pass.encoder && pass.color == state.color && pass.depth == state.depth;

    // The pass's first operation: its load operation clears.
    if (!open && whole && (depthOnly || fullMask)) {
      if (depthOnly) {
        pending.depth = true;
        pending.depthValue = std::clamp(depth, 0.f, 1.f);
      } else {
        pending.color = true;
        pending.colorValue = {color[0], color[1], color[2], color[3]};
      }

      return;
    }

    reserve(32 + UniformAlignment, 0, 0);
    const float values[8]{color[0], color[1], color[2], color[3], std::clamp(depth, 0.f, 1.f), 0.f, 0.f, 0.f};
    std::array<uint32_t, UniformBindings> offsets{};
    offsets[0] = stage_uniform(values, sizeof(values));
    const auto saved = state;
    state.raster = {};
    state.raster.colorMask = depthOnly ? std::array<bool, 4>{false, false, false, false} : saved.raster.colorMask;
    state.raster.depthTest = depthOnly;
    state.raster.depthWrite = depthOnly;
    state.raster.depthFunc = GL_ALWAYS;
    state.viewport = {0, 0, int32_t(width), int32_t(height)};
    state.depthRange = {0.f, 1.f};

    if (prepare(ClearProgram, programs.at(ClearProgram), 0, offsets, {-1, -1, -1, -1}, "vs", "fs"))
      pass.encoder.Draw(3);

    state = saved;
  }

  // ---- Members ----

  wgpu::CommandEncoder encoder;
  Pass pass;
  PendingClear pending;
  State state;
  uint64_t serial = 1;

  wgpu::Buffer uniformBuffer, vertexBuffer, indexBuffer, defaultsBuffer;
  std::vector<uint8_t> uniformStaging, vertexStaging, indexStaging;
  uint32_t uniformCursor = 0, vertexCursor = 0, indexCursor = 0;
  wgpu::BindGroupLayout uniformLayout;
  wgpu::BindGroup uniformGroup;
  wgpu::Texture placeholder;
  wgpu::TextureView placeholderView, placeholderSint, placeholderUint;
  wgpu::Sampler placeholderSampler;

  FastMap<GLuint, TextureEntry> textures;
  FastMap<GLuint, ProgramEntry> programs;
  FastMap<GLuint, SamplerEntry> samplers;
  FastMap<GLuint, std::vector<uint8_t>> buffers;
  FastMap<GLuint, wgpu::Buffer> meshes;
  FastMap<PipelineKey, wgpu::RenderPipeline> pipelines;
  FastMap<TextureGroupKey, wgpu::BindGroup> textureGroups;
  FastMap<uint64_t, uint32_t> layoutIds;
  std::vector<VertexLayout> layouts;
  std::map<std::pair<GLuint, GLuint>, GLuint> framebuffers;
  Streamed scene, material, animated, view;
  uint64_t sceneRevision = ~0ull, viewRevision = ~0ull;

  // Persistent uniform blocks, after the staging range of the uniform buffer.
  uint32_t persistentCursor = 0;
  size_t persistentBytes = 0;
  std::multimap<uint32_t, uint32_t> persistentFree; // size -> offset
  struct Retired {
    uint32_t frame, offset, size;
  };

  std::vector<Retired> persistentRetired;
  wgpu::RenderPipeline presentPipeline;
  GLuint presentSampler = 0;
  uint32_t nativeGeometryLayout = 0;

  // The frame slot of the next present (frame memory is copied at each draw).
  uint32_t frameSlot = 0;
  GLuint nextName = 1;

  static constexpr GLuint PersistentBufferName = 0xffffffffu;
};

// A frame the page asked for (aurora_webgpu_capture_frame): the present
// source read back asynchronously and handed to Module.onFrameCapture.
std::atomic<bool> g_captureRequested{false};

void capture(Renderer& r, const TextureEntry& source) {
  const uint32_t rowBytes = (source.width * 4 + 255) & ~255u;
  wgpu::BufferDescriptor descriptor;
  descriptor.label = "Aurora frame capture";
  descriptor.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
  descriptor.size = uint64_t(rowBytes) * source.height;
  auto buffer = g_device.CreateBuffer(&descriptor);
  wgpu::TexelCopyTextureInfo from{.texture = source.texture};
  wgpu::TexelCopyBufferInfo to{.layout = {.offset = 0, .bytesPerRow = rowBytes, .rowsPerImage = source.height}, .buffer = buffer};
  const wgpu::Extent3D size{source.width, source.height, 1};
  r.encoder.CopyTextureToBuffer(&from, &to, &size);

  // Its own submission: queue callbacks fail while a canvas texture is in it.
  r.flush();
  buffer.MapAsync(wgpu::MapMode::Read, 0, descriptor.size, wgpu::CallbackMode::AllowSpontaneous,
                  [buffer, width = source.width, height = source.height, rowBytes](wgpu::MapAsyncStatus status, wgpu::StringView) {
                    if (status != wgpu::MapAsyncStatus::Success)
                      return;

                    const auto* rows = static_cast<const uint8_t*>(buffer.GetConstMappedRange(0, uint64_t(rowBytes) * height));
                    std::vector<uint8_t> pixels(size_t(width) * height * 4);

                    for (uint32_t row = 0; row < height; ++row)
                      std::memcpy(pixels.data() + size_t(row) * width * 4, rows + size_t(row) * rowBytes, size_t(width) * 4);

                    buffer.Unmap();
                    EM_ASM({
                      if (Module.onFrameCapture)
                        Module.onFrameCapture($0, $1, HEAPU8.slice($2, $2 + $0 * $1 * 4));
                    }, width, height, pixels.data());
                  });
}

// The context owner is a thread; the renderer itself lives on the main thread.
struct RenderContext {
  std::atomic<pthread_t> owner{};
  std::atomic<bool> owned{false};
  std::unique_ptr<Renderer> renderer;
};

RenderContext* g_context = nullptr;

// Nonzero while the main thread runs a call for the owner on another thread.
int g_proxied = 0;
Caps g_caps;
gfx::NativeTextureSet g_nativeTextures;
size_t g_nativeMeshLiveBytes = 0;

Renderer& renderer() {
  if (!g_context || !g_context->renderer || !on_gpu_thread())
    AURORA_THROW(std::runtime_error("Renderer used off the browser's main thread"));

  return *g_context->renderer;
}

// Runs an entry point on the main thread: inline there, for the owner on
// another thread by a synchronous proxy, during which the main thread acts as
// the owner.
template <typename F>
decltype(auto) run(F&& operation) noexcept {
  using Result = decltype(operation());

  if (on_gpu_thread())
    return guarded(operation);

  if constexpr (std::is_void_v<Result>) {
    on_gpu([&] {
      ++g_proxied;
      guarded(operation);
      --g_proxied;
    });
  } else {
    Result result{};
    on_gpu([&] {
      ++g_proxied;
      result = guarded(operation);
      --g_proxied;
    });
    return result;
  }
}

wgpu::TextureFormat gpu_format(Format format, Upload& upload, uint32_t& bytes, bool& filterable) {
  upload = Upload::Direct;
  filterable = true;
  bytes = format_info(format).bytesPerPixel;

  switch (format) {
  case Format::R8:
    return wgpu::TextureFormat::R8Unorm;
  case Format::RG8:
    return wgpu::TextureFormat::RG8Unorm;
  case Format::R16I:
    filterable = false;
    return wgpu::TextureFormat::R16Sint;
  case Format::R32F:
    filterable = false;
    return wgpu::TextureFormat::R32Float;
  case Format::R32UI:
    filterable = false;
    return wgpu::TextureFormat::R32Uint;
  case Format::Depth32F:
    filterable = false;
    return wgpu::TextureFormat::Depth32Float;

  // WebGPU has no texture swizzle or 5-6-5 format: uploads expand to RGBA8.
  case Format::RGB565:
    upload = Upload::Rgb565;
    return wgpu::TextureFormat::RGBA8Unorm;
  case Format::U8_RRRR:
    upload = Upload::U8Rrrr;
    return wgpu::TextureFormat::RGBA8Unorm;
  case Format::U8_1RRR:
    upload = Upload::U8One;
    return wgpu::TextureFormat::RGBA8Unorm;
  case Format::U8_R000:
    upload = Upload::U8R000;
    return wgpu::TextureFormat::RGBA8Unorm;
  case Format::U8_R111:
    upload = Upload::U8R111;
    return wgpu::TextureFormat::RGBA8Unorm;
  case Format::U8U8_GRRR:
    upload = Upload::U8U8Grrr;
    return wgpu::TextureFormat::RGBA8Unorm;
  default:
    return wgpu::TextureFormat::RGBA8Unorm;
  }
}

void upload_now(Renderer& r, const Texture& handle, uint32_t mip, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data, uint32_t bytesPerRow) {
  auto& entry = r.texture(handle.gl());

  if (!width || !height || mip >= entry.mips)
    return;

  // Draws recorded before the upload read the old contents, as on GL.
  if (entry.used == r.serial)
    r.flush();

  const auto* source = static_cast<const uint8_t*>(data);
  std::vector<uint8_t> expanded;
  uint32_t rowBytes = bytesPerRow;

  if (entry.upload != Upload::Direct) {
    expanded.resize(size_t(width) * height * 4);
    rowBytes = width * 4;

    for (uint32_t row = 0; row < height; ++row) {
      const uint8_t* in = source + size_t(row) * bytesPerRow;
      uint8_t* out = expanded.data() + size_t(row) * rowBytes;

      for (uint32_t column = 0; column < width; ++column, out += 4) {
        switch (entry.upload) {
        case Upload::Rgb565: {
          uint16_t v;
          std::memcpy(&v, in + column * 2, 2);
          out[0] = uint8_t((v >> 11) * 255 / 31);
          out[1] = uint8_t(((v >> 5) & 63) * 255 / 63);
          out[2] = uint8_t((v & 31) * 255 / 31);
          out[3] = 255;
          break;
        }
        case Upload::U8Rrrr:
          out[0] = out[1] = out[2] = out[3] = in[column];
          break;
        case Upload::U8One:
          out[0] = out[1] = out[2] = in[column];
          out[3] = 255;
          break;
        case Upload::U8R000:
          out[0] = out[1] = out[2] = 0;
          out[3] = in[column];
          break;
        case Upload::U8R111:
          out[0] = out[1] = out[2] = 255;
          out[3] = in[column];
          break;
        case Upload::U8U8Grrr:
          out[0] = out[1] = out[2] = in[column * 2];
          out[3] = in[column * 2 + 1];
          break;
        default:
          break;
        }
      }
    }

    source = expanded.data();
  }

  wgpu::TexelCopyTextureInfo destination{.texture = entry.texture, .mipLevel = mip, .origin = {x, y, 0}};
  wgpu::TexelCopyBufferLayout layout{.offset = 0, .bytesPerRow = rowBytes, .rowsPerImage = height};
  const wgpu::Extent3D size{width, height, 1};
  g_queue.WriteTexture(&destination, source, size_t(rowBytes) * height, &layout, &size);
}
} // namespace

// ---- Context ----

NativeMeshBuffer::NativeMeshBuffer(const char* path, uint64_t offset, uint32_t bytes) {
  create(path, offset, nullptr, bytes);
}

NativeMeshBuffer::NativeMeshBuffer(const void* payload, uint32_t bytes) {
  create(nullptr, 0, payload, bytes);
}

void NativeMeshBuffer::create(const char* path, uint64_t offset, const void* payload, uint32_t bytes) {
  if (!on_render_thread() || !bytes)
    AURORA_THROW(std::runtime_error("Native mesh load outside context owner or empty payload"));

  // The shared compact pack reader requires a 16-byte base.
  const auto release = [](uint8_t* pointer) { ::operator delete[](pointer, std::align_val_t(16)); };
  std::unique_ptr<uint8_t, decltype(release)> data(static_cast<uint8_t*>(::operator new[]((bytes + 3) & ~3u, std::align_val_t(16))), release);
  gfx::fill_mesh_payload(path, offset, payload, data.get(), bytes);
  std::memset(data.get() + bytes, 0, ((bytes + 3) & ~3u) - bytes);
  run([&] {
    auto& r = renderer();
    wgpu::BufferDescriptor descriptor;
    descriptor.label = "Aurora model pack";
    descriptor.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
    descriptor.size = (bytes + 3) & ~3u;
    auto buffer = g_device.CreateBuffer(&descriptor);
    g_queue.WriteBuffer(buffer, 0, data.get(), descriptor.size);
    m_buffer = r.name();
    r.meshes.emplace(m_buffer, std::move(buffer));
  });
  m_data = data.release();
  m_bytes = bytes;
  g_nativeMeshLiveBytes += bytes;
}

NativeMeshBuffer::~NativeMeshBuffer() {
  // The command encoder keeps a buffer it recorded alive until it is done.
  if (m_buffer && initialized())
    post([buffer = m_buffer, bytes = m_bytes] {
      renderer().meshes.erase(buffer);
      g_nativeMeshLiveBytes -= bytes;
    });
  ::operator delete[](const_cast<uint8_t*>(m_data), std::align_val_t(16));
}

size_t native_mesh_allocation_bytes(uint32_t bytes) {
  if (!on_render_thread())
    AURORA_THROW(std::runtime_error("Model allocation query outside context owner"));

  return bytes;
}

size_t native_mesh_resident_bytes() {
  if (!on_render_thread())
    AURORA_THROW(std::runtime_error("Model backing query outside context owner"));

  return g_nativeMeshLiveBytes;
}

bool initialize(SDL_Window*) noexcept {
  if (g_context)
    return true;

  if (!webgpu::initialize())
    return false;

  auto context = std::make_unique<RenderContext>();
  g_context = context.get();
  bool ok = false;
  on_gpu([&] {
    guarded([&] { g_context->renderer = std::make_unique<Renderer>(); });
    g_caps.vendor = "WebGPU";
    g_caps.renderer = "emdawnwebgpu";
    g_caps.version = "WebGPU";
    g_caps.maxTextureSize = int32_t(std::min<uint32_t>(g_limits.maxTextureDimension2D, 8192));
    g_caps.uniformOffsetAlignment = int32_t(std::max<uint32_t>(g_limits.minUniformBufferOffsetAlignment, UniformAlignment));
    g_caps.maxUniformBlockSize = int32_t(BindingBytes);
    g_caps.maxAnisotropy = 16;
    ok = true;
  });

  if (!ok) {
    g_context = nullptr;
    return false;
  }

  context->owner.store(pthread_self(), std::memory_order_release);
  context->owned.store(true, std::memory_order_release);
  context.release();
  Log.info("WebGPU renderer: uniform alignment {}, max texture {}", g_caps.uniformOffsetAlignment, g_caps.maxTextureSize);
  return true;
}

void shutdown() noexcept {
  if (!g_context)
    return;

  if (!on_render_thread())
    fail("WebGPU shutdown called outside the context owner");

  run([] {
    auto& r = renderer();
    r.flush();
    g_context->renderer.reset();
  });
  webgpu::shutdown();
  delete g_context;
  g_context = nullptr;
}

bool initialized() noexcept {
  return g_context != nullptr;
}

const Caps& caps() noexcept {
  return g_caps;
}

bool on_render_thread() noexcept {
  if (!g_context || !g_context->owned.load(std::memory_order_acquire))
    return false;

  return pthread_equal(g_context->owner.load(std::memory_order_acquire), pthread_self()) || (on_gpu_thread() && g_proxied);
}

void release_context() noexcept {
  if (!on_render_thread())
    fail("WebGPU context released outside its owner");

  run([] { renderer().flush(); });
  g_context->owned.store(false, std::memory_order_release);
}

void acquire_context() noexcept {
  if (!g_context || g_context->owned.load(std::memory_order_acquire))
    fail("WebGPU context acquired while another thread owns it");

  g_context->owner.store(pthread_self(), std::memory_order_release);
  g_context->owned.store(true, std::memory_order_release);
}

void post(Task task) noexcept {
  if (!g_context)
    return;

  if (!on_render_thread())
    fail("WebGPU task posted outside the context owner");

  run([&] { std::move(task)(); });
}

void call(Task task) noexcept {
  post(std::move(task));
}

// The browser paces requestAnimationFrame to the frames the GPU completes.
// Queue completion cannot stand in: with a canvas texture in flight, headless
// Chromium rejects onSubmittedWorkDone (docs/optimizations/platforms/web-build.md).
bool gpu_frame_pending() noexcept {
  return false;
}

uint32_t native_frame_slot() noexcept {
  return run([] { return renderer().frameSlot; });
}

void discard_native_frame() noexcept {
  run([] { renderer().flush(); });
}

void finish() noexcept {
  // The main thread cannot wait for the GPU; submitting is all it can do.
  if (initialized())
    run([] { renderer().flush(); });
}

// ---- Formats and textures ----

const FormatInfo& format_info(Format format) noexcept {
  return FormatInfos[static_cast<size_t>(format)];
}

Texture::Texture(uint32_t width_, uint32_t height_, uint32_t mips_, Format format_, std::string_view label) noexcept : width(width_), height(height_), mips(mips_), format(format_), m_name(new Name) {
  auto create = [name = m_name, width_, height_, mips_, format_, label = std::string(label)] {
    auto& r = renderer();
    TextureEntry entry;
    entry.width = width_;
    entry.height = height_;
    entry.mips = mips_;
    entry.format = format_;
    entry.gpuFormat = gpu_format(format_, entry.upload, entry.bytesPerPixel, entry.filterable);
    wgpu::TextureDescriptor descriptor;
    descriptor.label = label.c_str();
    descriptor.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::RenderAttachment;
    descriptor.size = {width_, height_, 1};
    descriptor.format = entry.gpuFormat;
    descriptor.mipLevelCount = mips_;
    entry.texture = g_device.CreateTexture(&descriptor);
    wgpu::TextureViewDescriptor view;
    view.aspect = format_ == Format::Depth32F ? wgpu::TextureAspect::DepthOnly : wgpu::TextureAspect::All;
    entry.view = entry.texture.CreateView(&view);
    view.mipLevelCount = 1;
    entry.target = entry.texture.CreateView(&view);
    name->value = r.name();
    r.textures.emplace(name->value, std::move(entry));
  };

  if (on_render_thread())
    run(create);
  else
    post(std::move(create));
}

Texture::~Texture() {
  if (!initialized()) {
    delete m_name;
    return;
  }

  post([name = m_name] {
    auto& r = renderer();
    const GLuint id = name->value;

    for (auto it = r.textureGroups.begin(); it != r.textureGroups.end();) {
      const auto& t = it->first.textures;

      if (std::find(t.begin(), t.end(), id) != t.end())
        r.textureGroups.erase(it++);
      else
        ++it;
    }

    for (auto& bound : r.state.textures)
      if (bound == id)
        bound = 0;

    if (r.state.color == id || r.state.depth == id) {
      r.pending = {};
      r.close_pass();
      r.state.color = r.state.color == id ? 0 : r.state.color;
      r.state.depth = r.state.depth == id ? 0 : r.state.depth;
    }

    r.textures.erase(id);
    delete name;
  });
}

TexturePtr create_texture(uint32_t width, uint32_t height, uint32_t mips, Format format, std::string_view label) noexcept {
  return std::make_shared<Texture>(std::max(width, 1u), std::max(height, 1u), std::max(mips, 1u), format, label);
}

// Single-sampled: this WebGPU renderer has no multisampled targets.
TexturePtr create_render_target(uint32_t width, uint32_t height, Format format, uint32_t, std::string_view label) noexcept {
  return create_texture(width, height, 1, format, label);
}

bool render_target_reusable(const Texture& texture) noexcept {
  return run([&] {
    auto& r = renderer();
    const auto id = texture.gl();

    if (std::find(r.state.textures.begin(), r.state.textures.end(), id) != r.state.textures.end())
      return false;

    return r.state.color != id && r.state.depth != id;
  });
}

bool render_target_allocation_available(size_t bytes) noexcept {
  return bytes != 0;
}

// Without exceptions (release builds) a failure stops at its AURORA_THROW.
void open_native_texture_pack(const char* path) noexcept {
  guarded([&] { g_nativeTextures.add(path); });
}

void set_native_texture_resolver(void (*resolver)(uint32_t asset)) noexcept { g_nativeTextures.set_resolver(resolver); }
NativeTextureInfo native_texture_info(uint32_t asset) noexcept {
  return guarded([&] {
    const auto entry = g_nativeTextures.find(asset);
    const auto& r = *entry.record;
    return NativeTextureInfo{gfx::native_texture_format(r.format),
                             r.width,
                             r.height,
                             r.mips,
                             r.bytes,
                             bool(r.flags & 1),
                             r.image_bytes,
                             r.palette_offset,
                             r.palette_bytes,
                             r.source_palette_delta,
                             r.source_palette_entries,
                             r.source_palette_format,
                             r.source_format,
                             entry.version == 2};
  });
}

TexturePtr native_texture_asset(uint32_t asset) noexcept {
  if (!on_render_thread())
    fail("Native texture asset load outside the graphics owner");

  return guarded([&] {
    const auto info = native_texture_info(asset);
    const auto entry = g_nativeTextures.find(asset);
    const auto& record = *entry.record;
    std::vector<uint8_t> payload(record.bytes);
    g_nativeTextures.read(entry, payload.data());

    // As on GLES: block-compressed and paletted packs decode to RGBA8.
    const auto layout = gfx::native_texture_layout(info.width, info.height, info.mips, info.format);
    const bool expand = gfx::native_swizzled(info.format);
    auto texture = create_texture(info.width, info.height, info.mips, expand ? Format::RGBA8 : info.format, "Offline native texture");

    for (unsigned mip = 0; mip < info.mips; mip++) {
      const auto& level = layout.levels[mip];

      if (expand) {
        auto rgba = gfx::native_texture_rgba_level(record, payload, mip);
        upload_texture(*texture, mip, 0, 0, level.width, level.height, rgba.data(), level.width * 4);
      } else
        upload_texture(*texture, mip, 0, 0, level.width, level.height, payload.data() + level.offset, level.pitch);
    }

    return texture;
  });
}

void write_texture(const TexturePtr& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t width, uint32_t height, std::vector<uint8_t>&& data, uint32_t bytesPerRow) noexcept {
  if (!texture || !width || !height)
    return;

  post([texture, mip, x, y, width, height, data = std::move(data), bytesPerRow] { upload_texture(*texture, mip, x, y, width, height, data.data(), bytesPerRow); });
}

void write_texture(const TexturePtr& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data, uint32_t bytesPerRow) noexcept {
  const auto* bytes = static_cast<const uint8_t*>(data);
  write_texture(texture, mip, x, y, width, height, std::vector<uint8_t>(bytes, bytes + static_cast<size_t>(bytesPerRow) * height), bytesPerRow);
}

void upload_texture(const Texture& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data, uint32_t bytesPerRow) noexcept {
  run([&] { upload_now(renderer(), texture, mip, x, y, width, height, data, bytesPerRow); });
}

// ---- Samplers and framebuffers ----

GLuint sampler(const SamplerDesc& desc) noexcept {
  return run([&] {
    if (desc.lodBias != 0.f)
      AURORA_THROW(std::runtime_error("WebGPU sampler bias must be passed through the material shader"));

    auto& r = renderer();

    for (const auto& [id, existing] : r.samplers)
      if (existing.desc == desc)
        return id;

    const auto id = r.name();
    r.samplers.emplace(id, r.make_sampler(desc));
    return id;
  });
}

GLuint framebuffer(GLuint color, GLuint depth) noexcept {
  return run([&] {
    auto& r = renderer();
    auto [it, inserted] = r.framebuffers.try_emplace({color, depth}, 0);

    if (inserted)
      it->second = r.name();

    return it->second;
  });
}

void bind_framebuffer(GLuint color, GLuint depth) noexcept {
  run([&] { renderer().bind_target(color, depth); });
}

// ---- Programs ----

Program::Program(ProgramDesc desc) noexcept : m_desc(std::move(desc)) {
  m_vertexHash = XXH3_64bits(m_desc.vertex.data(), m_desc.vertex.size());
  m_fragmentHash = XXH3_64bits(m_desc.fragment.data(), m_desc.fragment.size());
  m_usesDiscard = m_desc.fragment.find("discard;") != std::string::npos;
  m_replacesDepth = m_desc.fragment.find("gl_FragDepth =") != std::string::npos;

  // The bank holds the stages by these hashes; the sources are not needed.
  m_desc.vertex = {};
  m_desc.fragment = {};
  m_desc.portableVertex = {};
  m_desc.portableFragment = {};
}

Program::~Program() {
  if (m_program && initialized())
    post([program = m_program] {
      auto& r = renderer();

      for (auto it = r.pipelines.begin(); it != r.pipelines.end();) {
        if (it->first.program == program)
          r.pipelines.erase(it++);
        else
          ++it;
      }

      if (r.state.program == program)
        r.state.program = 0;

      r.pass.pipeline = nullptr;
      r.programs.erase(program);
    });
}

GLuint Program::get() noexcept {
  if (m_program || m_failed)
    return m_program;

  run([&] {
    shader_bank::Module vertex, fragment;

    if (!shader_bank::find(m_vertexHash, false, vertex) || !shader_bank::find(m_fragmentHash, true, fragment)) {
      Log.error("WebGPU shader bank has no program {:016x}_v/{:016x}_f ({}); rebuild the bank", m_vertexHash, m_fragmentHash, m_desc.label);
      m_failed = true;
      return;
    }

    auto& r = renderer();
    ProgramEntry entry;
    entry.label = m_desc.label;
    entry.shaded = std::any_of(m_desc.blocks.begin(), m_desc.blocks.end(), [](const auto& block) { return block.first == "Draw"; });
    entry.vertex = r.module(vertex.wgsl, m_desc.label + " vertex");
    entry.fragment = r.module(fragment.wgsl, m_desc.label + " fragment");

    for (const auto* stage : {&vertex, &fragment})
      for (const auto& [name, binding] : stage->blocks) {
        const auto declared = std::find_if(m_desc.blocks.begin(), m_desc.blocks.end(), [&](const auto& block) { return block.first == name; });

        if (declared == m_desc.blocks.end() || uint32_t(declared->second) != binding)
          AURORA_THROW(std::runtime_error(m_desc.label + ": uniform block " + name + " is not at its WebGPU binding"));

        if (std::find(entry.blocks.begin(), entry.blocks.end(), binding) == entry.blocks.end())
          entry.blocks.push_back(binding);
      }

    for (unsigned stage = 0; stage < 2; ++stage) {
      entry.loose[stage] = (stage ? fragment : vertex).loose;
      entry.looseValues[stage].assign(entry.loose[stage].size(), {0.f, 0.f, 0.f, 0.f});
    }

    for (const auto& texture : fragment.textures) {
      Slot slot{texture.name, -1, texture.kind, texture.sampled};

      if (entry.shaded) {
        // u_texture0..2: the role, bound per draw to a GX map unit.
        slot.unit = texture.name.back() - '0';
      } else {
        const auto found = std::find_if(m_desc.samplers.begin(), m_desc.samplers.end(), [&](const auto& sampler) { return sampler.first == texture.name; });

        if (found == m_desc.samplers.end())
          AURORA_THROW(std::runtime_error(m_desc.label + ": sampler " + texture.name + " has no unit"));

        slot.unit = found->second;
      }

      entry.slots.push_back(slot);
    }

    entry.geometry = entry.shaded && vertex.wgsl.find("a_view_position") != std::string::npos;

    if (!entry.slots.empty())
      entry.textureLayout = r.texture_layout(entry.slots);

    entry.layout = r.pipeline_layout(entry.textureLayout);
    m_program = r.name();
    r.programs.emplace(m_program, std::move(entry));
  });

  return m_program;
}

// ---- State ----

void apply_raster_state(const RasterState& value) noexcept {
  run([&] { renderer().state.raster = value; });
}

void set_viewport(int32_t x, int32_t y, int32_t width, int32_t height, float minDepth, float maxDepth) noexcept {
  run([&] {
    auto& s = renderer().state;
    s.viewport = {x, y, width, height};
    s.depthRange = {minDepth, maxDepth};
  });
}

void set_scissor(int32_t x, int32_t y, int32_t width, int32_t height) noexcept {
  run([&] { renderer().state.scissor = {x, y, width, height}; });
}

void set_scissor_enabled(bool enabled) noexcept {
  run([&] { renderer().state.scissorEnabled = enabled; });
}

void set_blend_color(float r, float g, float b, float a) noexcept {
  run([&] { renderer().state.blendColor = {r, g, b, a}; });
}

void use_program(GLuint program) noexcept {
  run([&] { renderer().state.program = program; });
}

void bind_texture(uint32_t unit, GLuint texture, GLuint samplerObject) noexcept {
  run([&] {
    auto& s = renderer().state;

    if (unit >= s.textures.size())
      AURORA_THROW(std::runtime_error("Texture unit out of range"));

    s.textures[unit] = texture;
    s.samplers[unit] = samplerObject;
  });
}

void bind_uniform_range(uint32_t binding, GLuint buffer, uint32_t offset, uint32_t size) noexcept {
  run([&] {
    if (binding >= UniformBindings)
      AURORA_THROW(std::runtime_error("Uniform binding out of range"));

    renderer().state.uniforms[binding] = {buffer, offset, size};
  });
}

void set_color_mask(bool r, bool g, bool b, bool a) noexcept {
  run([&] { renderer().state.raster.colorMask = {r, g, b, a}; });
}

void set_depth_write(bool enabled) noexcept {
  run([&] { renderer().state.raster.depthWrite = enabled; });
}

// The state is the backend's own; there is no driver state to forget.
void invalidate_state() noexcept {}
void bind_draw_vao(GLuint) noexcept {}
OffscreenState save_offscreen_state() noexcept {
  return run([] {
    const auto& s = renderer().state;
    OffscreenState out;
    out.raster = s.raster;
    out.rasterValid = true;
    out.viewport = s.viewport;
    out.depthRange = s.depthRange;
    out.scissor = s.scissor;
    out.scissorEnabled = s.scissorEnabled;
    out.blendColor = s.blendColor;
    out.color = s.color;
    out.depth = s.depth;
    out.program = s.program;
    out.textures = s.textures;
    out.samplers = s.samplers;

    for (unsigned i = 0; i < out.uniforms.size(); ++i)
      out.uniforms[i] = {s.uniforms[i].buffer, s.uniforms[i].offset, s.uniforms[i].size};

    out.uniformValid = true;
    return out;
  });
}

void restore_offscreen_state(const OffscreenState& saved) noexcept {
  run([&] {
    auto& r = renderer();
    r.bind_target(saved.color, saved.depth);
    auto& s = r.state;
    s.raster = saved.raster;
    s.viewport = saved.viewport;
    s.depthRange = saved.depthRange;
    s.scissor = saved.scissor;
    s.scissorEnabled = saved.scissorEnabled > 0;
    s.blendColor = saved.blendColor;
    s.program = saved.program;
    s.textures = saved.textures;
    s.samplers = saved.samplers;

    for (unsigned i = 0; i < saved.uniforms.size(); ++i)
      s.uniforms[i] = {saved.uniforms[i].buffer, saved.uniforms[i].offset, saved.uniforms[i].size};
  });
}

// ---- Buffers ----

GLuint create_buffer() noexcept {
  return run([] {
    auto& r = renderer();
    const auto id = r.name();
    r.buffers[id] = {};
    return id;
  });
}

void destroy_buffer(GLuint buffer) noexcept {
  run([&] { renderer().buffers.erase(buffer); });
}

void write_buffer(GLuint buffer, bool, const void* bytes, size_t size) noexcept {
  run([&] {
    const auto* data = static_cast<const uint8_t*>(bytes);
    renderer().buffers[buffer].assign(data, data + size);
  });
}

void copy_texture_region(GLuint source, GLuint dest, int32_t srcX, int32_t srcY, int32_t dstX, int32_t dstY, int32_t width, int32_t height) noexcept {
  run([&] {
    if (width <= 0 || height <= 0)
      return;

    auto& r = renderer();
    auto& src = r.texture(source);
    auto& dst = r.texture(dest);

    if (src.gpuFormat != dst.gpuFormat || src.format == Format::Depth32F)
      AURORA_THROW(std::runtime_error("WebGPU texture copies need one color format"));

    r.end_pass();
    src.used = dst.used = r.serial;
    wgpu::TexelCopyTextureInfo from{.texture = src.texture, .origin = {uint32_t(srcX), uint32_t(srcY), 0}};
    wgpu::TexelCopyTextureInfo to{.texture = dst.texture, .origin = {uint32_t(dstX), uint32_t(dstY), 0}};
    const wgpu::Extent3D size{uint32_t(width), uint32_t(height), 1};
    r.encoder.CopyTextureToTexture(&from, &to, &size);
  });
}

// ---- Clears ----

void clear_color(const float* color) noexcept {
  if (aurora::native_frame_drawing_allowed())
    run([&] { renderer().clear(color, 0.f, false); });
}

void clear_depth(float depth) noexcept {
  if (aurora::native_frame_drawing_allowed())
    run([&] {
      const float ignored[4]{};
      renderer().clear(ignored, depth, true);
    });
}

void set_depth_liveness(GLuint, bool, bool) noexcept {}
void set_present_timing(const PresentTiming&) noexcept {}
void clear_depth_full_target(float depth) noexcept {
  if (!aurora::native_frame_drawing_allowed())
    return;

  run([&] {
    auto& r = renderer();
    const bool depthWrite = r.state.raster.depthWrite, scissor = r.state.scissorEnabled;
    r.state.raster.depthWrite = true;
    r.state.scissorEnabled = false;
    const float ignored[4]{};
    r.clear(ignored, depth, true);
    r.state.raster.depthWrite = depthWrite;
    r.state.scissorEnabled = scissor;
  });
}

// ---- Draws ----

void draw_fullscreen(GLuint program) noexcept {
  if (!aurora::native_frame_drawing_allowed() || !program)
    return;

  run([&] {
    auto& r = renderer();
    auto& entry = r.programs.at(program);
    r.reserve(r.fixed_bytes(entry), 0, 0);
    std::array<uint32_t, UniformBindings> offsets{};
    r.fixed_offsets(entry, offsets);
    std::array<int, 4> units{-1, -1, -1, -1};

    for (uint32_t k = 0; k < entry.slots.size() && k < units.size(); ++k)
      units[k] = entry.slots[k].unit;

    r.state.program = program;

    if (r.prepare(program, entry, 0, offsets, units))
      r.pass.encoder.Draw(3);
  });
}

// The GX command executor's indexed draws belong to the removed FIFO path.
void draw_indexed(uint32_t, uint32_t, uint32_t) noexcept {
  Log.error("Indexed GX draws are not supported by the WebGPU renderer");
}

void draw_native_geometry(const AuroraNativeGeometry& geometry, bool textured) noexcept {
  if (!aurora::native_frame_drawing_allowed() || !geometry.index_count)
    return;

  run([&] {
    auto& r = renderer();
    const uint32_t vertexBytes = geometry.vertex_count * uint32_t(sizeof(AuroraNativeVertex)), indexBytes = geometry.index_count * 2u;
    r.reserve(0, vertexBytes + 16, indexBytes + 4);
    static_assert(sizeof(AuroraNativeVertex) == 28);
    auto& layout = r.nativeGeometryLayout;

    if (!layout) {
      VertexLayout description;
      description.stride = sizeof(AuroraNativeVertex);
      description.attributes = {{nullptr, wgpu::VertexFormat::Float32x4, 0, 0}, {nullptr, wgpu::VertexFormat::Float32x2, 16, 1}, {nullptr, wgpu::VertexFormat::Unorm8x4, 24, 2}};
      layout = r.intern_layout(description);
    }

    const auto id = textured ? NativeTexturedProgram : NativeColoredProgram;
    const uint32_t vertexOffset = r.stage_vertices(geometry.vertices, vertexBytes);
    const uint32_t indexOffset = r.stage_indices(geometry.indices, indexBytes);

    if (!r.prepare(id, r.programs.at(id), layout, {}, {0, -1, -1, -1}, "vs", textured ? "textured" : "colored"))
      return;

    r.pass.encoder.SetVertexBuffer(0, r.vertexBuffer, vertexOffset, vertexBytes);
    r.pass.encoder.SetVertexBuffer(1, r.defaultsBuffer, 0, DefaultsBytes);
    r.pass.encoder.SetIndexBuffer(r.indexBuffer, wgpu::IndexFormat::Uint16, indexOffset, indexBytes);
    r.pass.encoder.DrawIndexed(geometry.index_count);
  });
}

// GX-generated programs over native vertices belong to the removed FIFO path.
void draw_native_material_geometry(const AuroraNativeMaterialGeometry&, GLuint, const AuroraNativeUniforms&) noexcept {
  Log.error("GX material programs are not supported by the WebGPU renderer");
}

void draw_native_mesh(const NativeMeshBuffer&, const MeshLayout&, uint32_t, uint32_t, uint32_t, uint32_t, GLuint, const AuroraNativeUniforms&) {
  Log.error("GX mesh programs are not supported by the WebGPU renderer");
}

namespace {
// Group 0 offsets of a shaded draw: Draw, Scene, Palette, Shade, Material, Animated, View.
std::array<uint32_t, UniformBindings> shaded_offsets(Renderer& r, const ShadedBlocks& blocks) {
  if (blocks.shadeOffset % UniformAlignment || (blocks.paletteBytes && blocks.paletteOffset % UniformAlignment))
    AURORA_THROW(std::runtime_error("Shade or palette block offset violates the uniform alignment"));

  std::array<uint32_t, UniformBindings> offsets{};
  const uint32_t draw = r.stage_uniform(blocks.draw, blocks.drawBytes);
  offsets[0] = draw;
  offsets[3] = blocks.shadeBytes ? draw + blocks.shadeOffset : draw;
  offsets[2] = blocks.paletteBytes ? draw + blocks.paletteOffset : draw;

  // The scene block changes with its revision; stage it once per submission.
  if (blocks.sceneRevision != r.sceneRevision)
    r.scene = {};

  r.sceneRevision = blocks.sceneRevision;
  offsets[1] = r.stream(r.scene, blocks.scene, blocks.sceneBytes);

  if (blocks.material.valid())
    offsets[4] = r.block_offset(blocks.material, r.material);

  if (blocks.animated.valid())
    offsets[5] = r.block_offset(blocks.animated, r.animated);

  // The view block, like the scene's, is staged once per revision.
  if (blocks.view) {
    if (blocks.viewRevision != r.viewRevision)
      r.view = {};

    r.viewRevision = blocks.viewRevision;
    offsets[6] = r.stream(r.view, blocks.view, blocks.viewBytes);
  }

  return offsets;
}

uint32_t shaded_bytes(const ShadedBlocks& blocks) {
  return blocks.drawBytes + blocks.sceneBytes + blocks.viewBytes + (blocks.material.data ? blocks.material.size : 0) + (blocks.animated.data ? blocks.animated.size : 0) +
         5 * UniformAlignment;
}

wgpu::VertexFormat mesh_format(MeshAttributeFormat format, unsigned components, unsigned& componentBytes) {
  using F = wgpu::VertexFormat;

  switch (format) {
  case MeshAttributeFormat::F32:
    componentBytes = 4;
    return components == 1 ? F::Float32 : components == 2 ? F::Float32x2 : components == 3 ? F::Float32x3 : F::Float32x4;
  case MeshAttributeFormat::F16:
    componentBytes = 2;
    return components == 1 ? F::Float16 : components == 2 ? F::Float16x2 : F::Float16x4;
  case MeshAttributeFormat::U8N:
    componentBytes = 1;
    return components == 1 ? F::Unorm8 : components == 2 ? F::Unorm8x2 : F::Unorm8x4;
  case MeshAttributeFormat::U8:
    componentBytes = 1;
    return components == 1 ? F::Uint8 : components == 2 ? F::Uint8x2 : F::Uint8x4;
  case MeshAttributeFormat::S16N:
    componentBytes = 2;
    return components == 1 ? F::Snorm16 : components == 2 ? F::Snorm16x2 : F::Snorm16x4;
  }

  AURORA_THROW(std::runtime_error("Unknown mesh attribute format"));
}

uint32_t format_bytes(wgpu::VertexFormat format) {
  using F = wgpu::VertexFormat;

  switch (format) {
  case F::Uint8:
  case F::Unorm8:
    return 1;
  case F::Uint8x2:
  case F::Unorm8x2:
  case F::Float16:
  case F::Snorm16:
    return 2;
  case F::Uint8x4:
  case F::Unorm8x4:
  case F::Float16x2:
  case F::Snorm16x2:
  case F::Float32:
    return 4;
  case F::Float16x4:
  case F::Snorm16x4:
  case F::Float32x2:
    return 8;
  case F::Float32x3:
    return 12;
  default:
    return 16;
  }
}

// Shader inputs 6..10 (matrix selectors and skin rows) read raw bytes as
// unsigned integers; the others are floats.
bool uint_input(unsigned location) {
  return location >= 6 && location <= 10;
}
} // namespace

void draw_shaded_mesh(const NativeMeshBuffer& buffer, const MeshLayout& layout, const ShadedMeshInputs& inputs, uint32_t vertexOffset, uint32_t vertexCount, uint32_t indexOffset, uint32_t indexCount,
                      GLuint program, const ShadedBlocks& blocks, const std::array<uint8_t, 3>& textureUnits) {
  if (!aurora::native_frame_drawing_allowed() || !indexCount || !program)
    return;

  gfx::validate_mesh_draw(buffer, layout, vertexOffset, vertexCount, indexOffset, indexCount);

  if (!blocks.draw || !blocks.paletteBytes || !blocks.scene)
    AURORA_THROW(std::runtime_error("Shaded mesh has no uniform blocks"));

  run([&] {
    auto& r = renderer();
    const auto mesh = r.meshes.find(buffer.get());

    if (mesh == r.meshes.end())
      AURORA_THROW(std::runtime_error("Resident mesh backing was retired"));

    if (layout.stride % 4) {
      static bool reported = false;

      if (!reported)
        Log.error("Mesh stride {} is not a multiple of four; WebGPU cannot draw it", layout.stride);

      reported = true;
      return;
    }

    VertexLayout description;
    description.stride = layout.stride;

    for (unsigned i = 0; i < inputs.location.size(); ++i) {
      const MeshAttribute* attribute = nullptr;

      if (inputs.location[i] >= 0)
        for (const auto& candidate : layout.attributes)
          if (candidate.location == uint8_t(inputs.location[i]))
            attribute = &candidate;

      if (!attribute) {
        description.defaults.push_back({nullptr, uint_input(i) ? wgpu::VertexFormat::Uint32x4 : wgpu::VertexFormat::Float32x4, uint_input(i) ? 16u : 0u, i});
        continue;
      }

      const int lane = inputs.lane[i];
      unsigned componentBytes = 0;
      const auto format = mesh_format(attribute->format, lane >= 0 ? 1 : attribute->components, componentBytes);
      const uint32_t offset = attribute->offset + (lane >= 0 ? unsigned(lane) * componentBytes : 0);

      // Three-component 8- and 16-bit attributes read a fourth component.
      if (offset + format_bytes(format) > layout.stride)
        AURORA_THROW(std::runtime_error("Mesh attribute widened past the vertex stride"));

      description.attributes.push_back({nullptr, format, offset, i});
    }

    r.reserve(shaded_bytes(blocks), 0, 0);
    const auto offsets = shaded_offsets(r, blocks);
    const uint32_t layoutId = r.intern_layout(description);
    auto& entry = r.programs.at(program);
    r.state.program = program;

    if (!r.prepare(program, entry, layoutId, offsets, {textureUnits[0], textureUnits[1], textureUnits[2], -1}))
      return;

    r.pass.encoder.SetVertexBuffer(0, mesh->second, vertexOffset, uint64_t(vertexCount) * layout.stride);
    r.pass.encoder.SetVertexBuffer(1, r.defaultsBuffer, 0, DefaultsBytes);
    r.pass.encoder.SetIndexBuffer(mesh->second, wgpu::IndexFormat::Uint16, indexOffset, uint64_t(indexCount) * 2);
    r.pass.encoder.DrawIndexed(indexCount);
  });
}

// The web build records no compact quads (runtime: MH3 effect quads).
void draw_shaded_quads(const AuroraNativeQuadVertex*, uint32_t, GLuint, const ShadedBlocks&, const std::array<uint8_t, 3>&) noexcept {}

void draw_shaded_geometry(const AuroraNativeMaterialGeometry& geometry, const std::array<uint8_t, 3>& coordinates, GLuint program, const ShadedBlocks& blocks,
                          const std::array<uint8_t, 3>& textureUnits) noexcept {
  if (!aurora::native_frame_drawing_allowed() || !geometry.index_count || !program)
    return;

  run([&] {
    if (!blocks.draw || !blocks.scene)
      AURORA_THROW(std::runtime_error("Shaded geometry has no uniform blocks"));

    auto& r = renderer();
    const uint32_t vertexBytes = geometry.vertex_count * uint32_t(sizeof(AuroraNativeMaterialVertex)), indexBytes = geometry.index_count * 2u;
    r.reserve(shaded_bytes(blocks), vertexBytes + 16, indexBytes + 4);
    VertexLayout description;
    description.stride = sizeof(AuroraNativeMaterialVertex);
    description.attributes = {{nullptr, wgpu::VertexFormat::Float32x4, offsetof(AuroraNativeMaterialVertex, position), 0},
                              {nullptr, wgpu::VertexFormat::Float32x3, offsetof(AuroraNativeMaterialVertex, view_position), 1},
                              {nullptr, wgpu::VertexFormat::Float32x3, offsetof(AuroraNativeMaterialVertex, normal), 2},
                              {nullptr, wgpu::VertexFormat::Unorm8x4, offsetof(AuroraNativeMaterialVertex, color), 3}};

    for (uint32_t role = 0; role < 3; ++role)
      description.attributes.push_back(
          {nullptr, wgpu::VertexFormat::Float32x3, offsetof(AuroraNativeMaterialVertex, texcoord) + std::min<uint32_t>(coordinates[role], 7) * 12, 4 + role});
    const auto offsets = shaded_offsets(r, blocks);
    const uint32_t layoutId = r.intern_layout(description);
    const uint32_t vertexOffset = r.stage_vertices(geometry.vertices, vertexBytes);
    const uint32_t indexOffset = r.stage_indices(geometry.indices, indexBytes);
    auto& entry = r.programs.at(program);
    r.state.program = program;

    if (!r.prepare(program, entry, layoutId, offsets, {textureUnits[0], textureUnits[1], textureUnits[2], -1}))
      return;

    r.pass.encoder.SetVertexBuffer(0, r.vertexBuffer, vertexOffset, vertexBytes);
    r.pass.encoder.SetVertexBuffer(1, r.defaultsBuffer, 0, DefaultsBytes);
    r.pass.encoder.SetIndexBuffer(r.indexBuffer, wgpu::IndexFormat::Uint16, indexOffset, indexBytes);
    r.pass.encoder.DrawIndexed(geometry.index_count);
  });
}

bool shaded_target_alpha_one() noexcept { return false; }

void uniform4(GLuint program, const char* name, float x, float y, float z, float w) noexcept {
  run([&] {
    auto& entry = renderer().programs.at(program);

    for (unsigned stage = 0; stage < 2; ++stage)
      for (size_t i = 0; i < entry.loose[stage].size(); ++i)
        if (entry.loose[stage][i] == name)
          entry.looseValues[stage][i] = {x, y, z, w};
  });
}

// ---- Uniform blocks ----

UniformBlock create_uniform_block(const void* bytes, uint32_t size) noexcept {
  return run([&]() -> UniformBlock {
    if (!bytes || !size)
      return {};

    auto& r = renderer();
    const uint32_t rounded = (size + UniformAlignment - 1) & ~(UniformAlignment - 1);
    const uint32_t frame = gfx::current_frame();

    // Ranges retired four frames ago are no longer read by a queued frame.
    for (auto it = r.persistentRetired.begin(); it != r.persistentRetired.end();) {
      if (frame - it->frame >= GpuFrameRing::BufferCount + 1) {
        r.persistentFree.emplace(it->size, it->offset);
        it = r.persistentRetired.erase(it);
      } else
        ++it;
    }

    uint32_t offset;

    if (const auto free = r.persistentFree.find(rounded); free != r.persistentFree.end()) {
      offset = free->second;
      r.persistentFree.erase(free);
    } else if (rounded <= PersistentUniformBytes - r.persistentCursor) {
      offset = r.persistentCursor;
      r.persistentCursor += rounded;
    } else
      return {};

    // Queue-ordered after every submission that read the range before.
    std::vector<uint8_t> padded((size + 3) & ~3u, 0);
    std::memcpy(padded.data(), bytes, size);
    g_queue.WriteBuffer(r.uniformBuffer, StagingUniformBytes + offset, padded.data(), padded.size());
    r.persistentBytes += rounded;
    return {nullptr, Renderer::PersistentBufferName, offset, size};
  });
}

void retire_uniform_block(const UniformBlock& block) noexcept {
  if (block.buffer != Renderer::PersistentBufferName)
    return;

  run([&] {
    auto& r = renderer();
    const uint32_t rounded = (block.size + UniformAlignment - 1) & ~(UniformAlignment - 1);
    r.persistentRetired.push_back({gfx::current_frame(), block.offset, rounded});
    r.persistentBytes -= rounded;
  });
}

size_t uniform_block_bytes() noexcept {
  return run([] { return renderer().persistentBytes; });
}

UniformRing create_uniform_ring(uint32_t bytes) noexcept {
  auto* data = static_cast<uint8_t*>(std::aligned_alloc(256, (size_t(bytes) + 255) & ~size_t(255)));
  return data ? UniformRing{data, bytes} : UniformRing{};
}

void destroy_uniform_ring(const UniformRing& ring) noexcept {
  // Host memory: blocks were staged when bound, nothing reads it later.
  std::free(ring.data);
}

GpuFence submitted_gpu_fence() noexcept { return {}; }

// ---- Presentation and readback ----

std::pair<uint32_t, uint32_t> drawable_size() noexcept {
  return canvas_size();
}

void present(const TexturePtr& source, int32_t x, int32_t y, int32_t width, int32_t height) noexcept {
  if (!aurora::native_frame_drawing_allowed())
    return;

  run([&] {
    auto& r = renderer();
    r.end_pass();

    if (source && g_captureRequested.exchange(false))
      capture(r, r.texture(source->gl()));

    const auto [canvasWidth, canvasHeight] = canvas_size();
    configure_surface(canvasWidth, canvasHeight);
    wgpu::SurfaceTexture surface;
    g_surface.GetCurrentTexture(&surface);

    if (surface.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal && surface.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
      r.flush();
      return;
    }

    const auto view = surface.texture.CreateView();
    wgpu::RenderPassColorAttachment color{};
    color.view = view;
    color.loadOp = wgpu::LoadOp::Clear;
    color.storeOp = wgpu::StoreOp::Store;
    color.clearValue = {0.0, 0.0, 0.0, 1.0};
    wgpu::RenderPassDescriptor descriptor;
    descriptor.colorAttachmentCount = 1;
    descriptor.colorAttachments = &color;
    auto passEncoder = r.encoder.BeginRenderPass(&descriptor);

    if (source && width > 0 && height > 0) {
      auto& program = r.programs.at(PresentProgram);
      auto& pipeline = r.presentPipeline;

      if (!pipeline) {
        wgpu::ColorTargetState target;
        target.format = g_surfaceFormat;
        wgpu::FragmentState fragment;
        fragment.module = program.fragment;
        fragment.entryPoint = "fs";
        fragment.targetCount = 1;
        fragment.targets = &target;
        wgpu::RenderPipelineDescriptor pipelineDescriptor;
        pipelineDescriptor.label = "Aurora present";
        pipelineDescriptor.layout = program.layout;
        pipelineDescriptor.vertex.module = program.vertex;
        pipelineDescriptor.vertex.entryPoint = "vs";
        pipelineDescriptor.fragment = &fragment;
        pipeline = g_device.CreateRenderPipeline(&pipelineDescriptor);
      }

      auto& entry = r.texture(source->gl());
      entry.used = r.serial;
      auto& linear = r.presentSampler;

      if (!linear) {
        linear = r.name();
        r.samplers.emplace(linear, r.make_sampler(LinearClamp));
      }

      wgpu::BindGroupEntry entries[2]{};
      entries[0].binding = 0;
      entries[0].textureView = entry.view;
      entries[1].binding = 1;
      entries[1].sampler = r.samplers.at(linear).filtering;
      wgpu::BindGroupDescriptor group;
      group.layout = program.textureLayout;
      group.entryCount = 2;
      group.entries = entries;
      const auto bindGroup = g_device.CreateBindGroup(&group);
      const uint32_t offsets[UniformBindings]{};

      // Texture rows are top-down, as are the canvas's: no flip.
      passEncoder.SetPipeline(pipeline);
      passEncoder.SetBindGroup(0, r.uniformGroup, UniformBindings, offsets);
      passEncoder.SetBindGroup(1, bindGroup);
      passEncoder.SetViewport(float(x), float(y), float(width), float(height), 0.f, 1.f);
      passEncoder.Draw(3);
    }

    passEncoder.End();
    r.flush();
    r.frameSlot = (r.frameSlot + 1) % GpuFrameRing::BufferCount;
  });
}

// WebGPU reads back asynchronously, and the browser's main thread cannot wait
// for it: development captures and readback checks get zeros.
void read_pixels(GLuint, uint32_t, uint32_t, uint32_t width, uint32_t height, void* out) noexcept {
  static bool reported = false;

  if (!reported)
    Log.warn("Pixel readback is not available in the web build");

  reported = true;
  std::memset(out, 0, size_t(width) * height * 4);
}
} // namespace aurora::gl

// Development, from the page (scripts/web-check.mjs): read the next presented
// frame back to Module.onFrameCapture(width, height, rgba). A headless browser
// may not show the canvas itself.
extern "C" EMSCRIPTEN_KEEPALIVE void aurora_webgpu_capture_frame() {
  aurora::gl::g_captureRequested.store(true);
}
