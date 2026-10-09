#include "../gl/gl.hpp"
#include <aurora/fatal.hpp>
#include "../gfx/port_capture.hpp"
#include "../gfx/native_mesh_io.hpp"
#include "../gfx/native_mesh_layout.hpp"
#include "../gfx/target_alpha.hpp"
#include "alpha_compare.hpp"
#include "../gfx/native_texture_set.hpp"
#include "device.hpp"
#ifdef MH3_DEVELOPMENT
#include "runtime_shader.hpp"
#endif
#include "razor.hpp"
#include <aurora/native_frame_gate.hpp>
#include "draw_memory.hpp"
#include "raster.hpp"
#include "texture.hpp"
#include "sampler_bias.hpp"
#include <aurora/thread_role.hpp>
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <set>
#include <stdexcept>

#include <thread>
#include <vector>

namespace aurora::gfx {
uint32_t current_frame() noexcept;
}

namespace aurora::gl {
namespace {
using namespace gxm;
[[noreturn]] void fail(const char* message) {
  std::printf("Aurora GXM: %s\n", message);
  std::fprintf(stderr, "Aurora GXM: %s\n", message);
  std::fflush(stdout);
  sceKernelExitProcess(1);
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
    fail("Unknown native renderer failure");
  }
#else
  return operation();
#endif
}

struct TextureEntry {
  std::unique_ptr<TextureStorage> storage;
  unsigned pins = 0;
  gfx::TargetAlpha alpha;
};

struct NativeProgram {
  std::string label;
  bool usesDiscard = false, replacesDepth = false;
  std::shared_ptr<PatchedProgram> program;
  std::vector<std::pair<std::string, int>> samplers;
  std::map<std::string, std::array<float, 4>> uniforms;
  bool buffer0 = false;
  bool nativeMaterialVertices = false;
  MeshLayout nativeMeshLayout;

  // draw_shaded_*: hand-written Cg patched once per distinct vertex binding.
  bool shaded = false;
  bool preservesTargetAlphaOne = false;
  uint64_t vertexHash = 0, fragmentHash = 0;
  std::vector<std::pair<std::vector<VertexInput>, std::shared_ptr<PatchedProgram>>> bindings;
};

bool same_inputs(std::span<const VertexInput> a, std::span<const VertexInput> b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const VertexInput& x, const VertexInput& y) {
    return x.name == y.name && x.format == y.format && x.components == y.components && x.offset == y.offset && x.stride == y.stride && x.stream == y.stream;
  });
}

struct State {
  RasterState raster;
  std::array<int32_t, 4> viewport{0, 0, Device::Width, Device::Height};
  std::array<float, 2> depthRange{0.f, 1.f};
  std::array<int32_t, 4> scissor{};
  std::array<float, 4> blendColor{};
  bool scissorEnabled = false, clipDirty = true, display = true;
  GLuint color = 0, depth = 0, program = 0;
  struct TextureBinding {
    GLuint texture = 0, sampler = 0;
  };

  std::array<TextureBinding, 16> textures{};
  uint32_t uniformOffset = 0, uniformBytes = 0;
  bool uniformValid = false;
};

uint32_t address_mode(GLenum mode) {
  switch (mode) {
  case GL_REPEAT:
    return 0;
  case GL_MIRRORED_REPEAT:
    return 1;
  case GL_CLAMP_TO_EDGE:
    return 2;
  default:
    AURORA_THROW(std::runtime_error("Unsupported native texture address mode"));
  }
}

SceGxmTexture sampling_view(const TextureStorage& texture, const SamplerDesc& desc) {
  SceGxmTexture result = texture.sampled();
  set_texture_address_modes(result, address_mode(desc.wrapS), address_mode(desc.wrapT));
  const bool minLinear = desc.minFilter == GL_LINEAR || desc.minFilter == GL_LINEAR_MIPMAP_NEAREST || desc.minFilter == GL_LINEAR_MIPMAP_LINEAR;
  const bool mipmapped = desc.minFilter != GL_NEAREST && desc.minFilter != GL_LINEAR;
  const bool mipLinear = desc.minFilter == GL_NEAREST_MIPMAP_LINEAR || desc.minFilter == GL_LINEAR_MIPMAP_LINEAR;

  if (texture.format == Format::Depth32F) {
    // LINEAR_STRIDED shares min/mip/LOD bits with its explicit stride.
    if (minLinear || desc.magFilter != GL_NEAREST || mipmapped || desc.lodBias != 0.f)
      AURORA_THROW(std::runtime_error("Strided depth sampling requires nearest filtering"));

    return result;
  }

  const int bias = sampler_bias_encoding(desc.lodBias);

  if (bias < 0)
    AURORA_THROW(std::runtime_error("Native GXM sampler bias is not exactly representable"));

  check(sceGxmTextureSetLodBias(&result, uint32_t(bias)), "set sampler LOD bias");
  check(sceGxmTextureSetMinFilter(&result, minLinear ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT), "set min filter");
  check(sceGxmTextureSetMagFilter(&result, desc.magFilter == GL_LINEAR ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT), "set mag filter");
  check(sceGxmTextureSetMipFilter(&result, mipLinear ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED : SCE_GXM_TEXTURE_MIP_FILTER_DISABLED), "set mip filter");
  const float last = float(texture.mips - 1);
  const float minLod = mipmapped ? std::clamp(desc.minLod, 0.f, last) : 0.f;
  const float maxLod = mipmapped ? std::clamp(desc.maxLod, minLod, last) : 0.f;

  if (std::floor(minLod) != minLod || std::floor(maxLod) != maxLod)
    AURORA_THROW(std::runtime_error("Fractional sampler LOD bounds require an offline shader clamp"));

  uint32_t words[4];
  std::memcpy(words, &result, sizeof(words));

  // These fields are measured on hardware in sgx_texture_hw.md. Changing
  // the clamp retains the actual chain's storage layout and backing address.
  words[0] = (words[0] & ~0x001e0000u) | (uint32_t(maxLod) << 17);
  words[2] = (words[2] & ~3u) | (uint32_t(minLod) >> 2);
  words[3] = (words[3] & ~(3u << 26)) | ((uint32_t(minLod) & 3u) << 26);
  std::memcpy(&result, words, sizeof(words));
  return result;
}

class Renderer {
public:
  Renderer()
  : pool(device), draws(device), clip(device), clearProgram(device, 1, 1), presentProgram(device, 1, 2), utility(4096, MappedMemory::Kind::Draw, "Aurora utility draws") {
    const float ids[4]{0, 1, 2, 3};
    const uint16_t indices[6]{0, 1, 2, 2, 1, 3};
    std::memcpy(utility.data(), ids, sizeof(ids));
    const float defaults[4]{0, 0, 0, 1};
    std::memcpy(static_cast<char*>(utility.data()) + 64, indices, sizeof(indices));
    std::memcpy(static_cast<char*>(utility.data()) + DefaultAttributesOffset, defaults, sizeof(defaults));
  }

  ~Renderer() {
    device.finish();
    programs.clear();
    textures.clear();

    if (uniformChunk)
      device.arena().free(uniformChunk, UniformChunkBytes);
  }

  GLuint name() {
    if (!nextName)
      AURORA_THROW(std::runtime_error("Native GPU name space exhausted"));

    return nextName++;
  }

  TextureEntry& entry(GLuint id) {
    const auto it = textures.find(id);

    if (it == textures.end())
      AURORA_THROW(std::runtime_error("Unknown native texture name"));

    return it->second;
  }

  TextureStorage& texture(GLuint id) { return *entry(id).storage; }
  void pin(GLuint id, bool add) {
    if (!id)
      return;

    auto& value = entry(id);

    if (add)
      ++value.pins;
    else {
      if (!value.pins)
        AURORA_THROW(std::runtime_error("Native texture pin accounting underflow"));

      --value.pins;
    }

    value.storage->pin(value.pins != 0);
  }

  void bind_target(GLuint color, GLuint depth) {
    if (state.color == color && state.depth == depth && state.display == (!color && !depth))
      return;

    device.end();
    pin(state.color, false);
    pin(state.depth, false);
    state.color = color;
    state.depth = depth;
    state.display = !color && !depth;
    pin(color, true);
    pin(depth, true);

    if (color && texture(color).format == Format::Depth32F)
      AURORA_THROW(std::runtime_error("Native color attachment has a depth format"));

    if (depth && texture(depth).format != Format::Depth32F)
      AURORA_THROW(std::runtime_error("Native depth attachment has a color format"));

    if (color && depth && (texture(color).width != texture(depth).width || texture(color).height != texture(depth).height))
      AURORA_THROW(std::runtime_error("Native framebuffer attachment dimensions differ"));

    state.clipDirty = true;
  }

  std::pair<uint32_t, uint32_t> target_size() {
    if (state.display)
      return {Device::Width, Device::Height};

    const auto& target = texture(state.color ? state.color : state.depth);
    return {target.width, target.height};
  }

  void scene(const float* clearDepth = nullptr) {
    const auto [width, height] = target_size();

    if (!device.scene_active()) {
      if (state.display)
        device.begin_display();
      else
        device.begin(width, height, state.color ? texture(state.color).color_surface() : nullptr, state.depth ? texture(state.depth).depth_surface() : nullptr,
                     nullptr, state.depth ? depth_scene(clearDepth) : Device::DepthScene{});
      razor_scene_begin(gfx::current_frame(), device.current_fence().value, width, height, state.color, state.depth, state.display);

      // Stored DF32M contains coverage from the previous scene. Recreate the
      // current coverage even when the logical scissor has not changed.
      state.clipDirty = state.depth || state.scissorEnabled;

      if (state.color)
        texture(state.color).note_gpu_use(true);

      if (state.depth)
        texture(state.depth).note_gpu_use(true);
    }

    if (state.clipDirty) {
      clip.apply(width, height, state.scissorEnabled, state.scissor[0], state.scissor[1], state.scissor[2], state.scissor[3]);
      state.clipDirty = false;
    }
  }

  SceGxmOutputRegisterFormat output_format() { return state.color ? texture(state.color).output_format() : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4; }
  bool shaded_alpha_one() {
    if (!state.color)
      return false;

    const auto& target = entry(state.color);
    return gfx::target_alpha_one_eligible(target.storage->format == Format::RGBA8, state.display, target.alpha,
                                         state.raster.blend, state.raster.colorMask);
  }

  void invalidate_alpha() {
    for (auto& [id, target] : textures)
      target.alpha.invalidate();
  }

  void viewport() {
    const auto& v = state.viewport;
    apply_viewport(device.context(), v[0], v[1], v[2], v[3], state.depthRange[0], state.depthRange[1]);
  }

  void texture_binding(uint32_t unit, GLuint id, GLuint samplerId) {
    if (unit >= state.textures.size())
      AURORA_THROW(std::runtime_error("Native texture unit out of range"));

    auto& bound = state.textures[unit];

    if (bound.texture != id) {
      pin(bound.texture, false);
      pin(id, true);
      bound.texture = id;

      if (id)
        pool.touch(texture(id));
    }

    bound.sampler = samplerId;
  }

  void reset_texture_bindings() {
    for (auto& bound : state.textures) {
      pin(bound.texture, false);
      bound = {};
    }
  }

  void prepare(GLuint id) {
    auto found = programs.find(id);

    if (found == programs.end())
      AURORA_THROW(std::runtime_error("Unknown native program name"));

    auto& native = found->second;
    scene();
    viewport();
    apply_raster(state.raster, device.context());
    auto blend = blend_info(state.raster, state.blendColor);

    if (!state.display && !state.color)
      blend.colorMask = SCE_GXM_COLOR_MASK_NONE;

    bind_program(*native.program, blend);

    if (native.buffer0) {
      if (!state.uniformValid)
        AURORA_THROW(std::runtime_error("Native shader requires an unbound uniform range"));

      draws.bind_uniform_range(state.uniformOffset, state.uniformBytes);
    }

    for (const auto& [uniform, values] : native.uniforms) {
      native.program->set_vertex_uniform(uniform.c_str(), values.data(), 4);
      native.program->set_fragment_uniform(uniform.c_str(), values.data(), 4);
    }

    for (const auto& [samplerName, unit] : native.samplers) {
      const auto& bound = state.textures.at(unit);

      if (!bound.texture)
        AURORA_THROW(std::runtime_error("Native shader requires an unbound sampled texture"));

      auto& source = texture(bound.texture);
      source.note_gpu_use();
      const SamplerDesc desc = bound.sampler ? samplers.at(bound.sampler) : NearestClamp;
      const auto sampled = sampling_view(source, desc);

      // A relocation ends the old scene. Sampling always builds the view
      // from current storage; no cached PDS address survives that move.
      check(sceGxmSetFragmentTexture(device.context(), unit, &sampled), "bind sampled texture");
      const float size[2]{float(source.width), float(source.height)};
      const std::string parameter = "aur_size_" + samplerName;
      native.program->set_vertex_uniform(parameter.c_str(), size, 2);
      native.program->set_fragment_uniform(parameter.c_str(), size, 2);
    }
  }

  PatchedProgram& shaded_program(NativeProgram& native, std::span<const VertexInput> inputs) {
    if (!native.shaded)
      AURORA_THROW(std::runtime_error("Shaded draw uses a non-shaded program"));

    for (auto& [key, program] : native.bindings)
      if (same_inputs(key, inputs))
        return *program;
    auto program = std::make_shared<PatchedProgram>(device, native.vertexHash, native.fragmentHash, SCE_GXM_INDEX_FORMAT_U16, inputs);
    native.bindings.emplace_back(std::vector<VertexInput>(inputs.begin(), inputs.end()), program);
    return *program;
  }

  // Vertex BUFFER[0] Draw, [1] Scene, [2] Palette; fragment BUFFER[0] Shade,
  // [1] Scene. Sampler slot r reads the texture bound at unit units[r].
  void prepare_shaded(NativeProgram& native, PatchedProgram& program, const ShadedBlocks& blocks, const std::array<uint8_t, 3>& units) {
    static const char* dump = std::getenv("MH3_GXM_SHADE_DATA_DUMP");

    if (dump && *dump) {
      static std::set<std::pair<uint64_t, uint64_t>> saved;

      if (saved.emplace(native.vertexHash, native.fragmentHash).second) {
        std::filesystem::create_directories(dump);
        char name[48];
        std::snprintf(name, sizeof(name), "%016llx-%016llx", static_cast<unsigned long long>(native.vertexHash), static_cast<unsigned long long>(native.fragmentHash));
        const auto base = std::filesystem::path(dump) / name;
        const auto write = [&](const char* suffix, const void* data, uint32_t bytes) {
          std::ofstream file(base.string() + suffix, std::ios::binary);
          file.write(static_cast<const char*>(data), bytes);

          if (!file)
            AURORA_THROW(std::runtime_error("Cannot save shaded uniform diagnostic"));
        };

        write("-draw.bin", blocks.draw, blocks.drawBytes);
        write("-scene.bin", blocks.scene, blocks.sceneBytes);
      }
    }

    scene();
    viewport();
    apply_raster(state.raster, device.context());
    auto blend = blend_info(state.raster, state.blendColor);

    if (!state.display && !state.color)
      blend.colorMask = SCE_GXM_COLOR_MASK_NONE;

    if (native.preservesTargetAlphaOne) {
      if (!gfx::target_alpha_one_expand(shaded_alpha_one(), native.shaded, true, native.replacesDepth))
        AURORA_THROW(std::runtime_error("Fixed alpha-one shader lost its target certificate"));

      blend.colorMask = SCE_GXM_COLOR_MASK_ALL;
    }

    bind_program(program, blend);
    draws.bind_shaded(blocks.draw, blocks.drawBytes, blocks.shadeOffset, blocks.shadeBytes, blocks.paletteOffset, blocks.paletteBytes, blocks.scene, blocks.sceneBytes);

    // Material uniforms in GPU memory: persistent CDRAM blocks or the frame
    // slot's uniforms, read in place (fragment BUFFER[2]/[3], vertex BUFFER[3]).
    auto* context = device.context();

    if (blocks.material.data)
      check(sceGxmSetFragmentUniformBuffer(context, 2, blocks.material.data), "bind material block");

    if (blocks.animated.data) {
      check(sceGxmSetFragmentUniformBuffer(context, 3, blocks.animated.data), "bind fragment animated block");
      check(sceGxmSetVertexUniformBuffer(context, 3, blocks.animated.data), "bind vertex animated block");
    }

    for (const auto& [samplerName, slot] : native.samplers) {
      const auto& bound = state.textures.at(units.at(slot));

      if (!bound.texture)
        AURORA_THROW(std::runtime_error("Shaded draw samples an unbound texture unit"));

      auto& source = texture(bound.texture);
      source.note_gpu_use();
      const SamplerDesc desc = bound.sampler ? samplers.at(bound.sampler) : NearestClamp;
      const auto sampled = sampling_view(source, desc);
      check(sceGxmSetFragmentTexture(device.context(), slot, &sampled), "bind shaded texture");
    }
  }

  void bind_program(PatchedProgram& program, const SceGxmBlendInfo& blend) {
    program.bind(blend, output_format());

    if (program.default_stream() >= 0)
      check(sceGxmSetVertexStream(device.context(), program.default_stream(), static_cast<char*>(utility.data()) + DefaultAttributesOffset), "bind default vertex attributes");
  }

  void utility_draw(uint32_t count) {
    check(sceGxmSetVertexStream(device.context(), 0, utility.data()), "bind utility vertex IDs");
    check(sceGxmSetVertexStream(device.context(), 1, utility.data()), "bind utility instance IDs");
    check(sceGxmDraw(device.context(), SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, static_cast<char*>(utility.data()) + 64, count), "draw native utility geometry");
  }

  void clear(const float* color, float depth, bool depthOnly) {
    if (depthOnly && (!state.depth || !state.raster.depthWrite))
      return;

    if (!depthOnly && !state.display && !state.color)
      return;

    if (depthOnly && !device.scene_active() && !state.scissorEnabled) {
      // The scene's first operation: start it from the cleared depth instead
      // of loading the stored one and drawing over it.
      const float value = std::clamp(depth, 0.f, 1.f);
      scene(&value);
      return;
    }

    scene();
    const auto [width, height] = target_size();
    apply_viewport(device.context(), 0, 0, width, height, 0.f, 1.f);
    RasterState raster;
    raster.colorMask = depthOnly ? std::array{false, false, false, false} : state.raster.colorMask;
    raster.depthTest = depthOnly;
    raster.depthWrite = depthOnly;
    raster.depthFunc = GL_ALWAYS;
    apply_raster(raster, device.context());
    clearProgram.bind(blend_info(raster, {}), depthOnly ? SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4 : output_format());
    const float region[4]{-1.f, 1.f, 2.f, -2.f};
    const float clipDepth = 2.f * std::clamp(depth, 0.f, 1.f) - 1.f;
    clearProgram.set_vertex_uniform("region", region, 4);
    clearProgram.set_vertex_uniform("depth", &clipDepth, 1);
    clearProgram.set_fragment_uniform("color", color, 4);
    utility_draw(6);
    capture_draw(depthOnly ? "clear-depth" : "clear-color", 0, 6, 6, 1, &raster);

    if (!depthOnly && state.color)
      entry(state.color).alpha.clear(raster.colorMask[3], color[3], !state.scissorEnabled);
  }

  void collect() {
    pool.collect();

    for (auto it = retiredMeshes.begin(); it != retiredMeshes.end();) {
      if (int32_t(*it->first.address - it->first.value) < 0) {
        ++it;
        continue;
      }

      it = retiredMeshes.erase(it);
    }

    for (auto it = retiredPrograms.begin(); it != retiredPrograms.end();) {
      if (int32_t(*it->first.address - it->first.value) < 0) {
        ++it;
        continue;
      }

      it = retiredPrograms.erase(it);
    }

    for (auto it = uniformRetired.begin(); it != uniformRetired.end();) {
      if (it->first.address && int32_t(*it->first.address - it->first.value) < 0) {
        ++it;
        continue;
      }

      uniformFree.emplace(it->second.second, it->second.first);
      it = uniformRetired.erase(it);
    }
  }

  // Persistent uniform blocks: one CDRAM arena chunk, sub-allocated in
  // 16-byte units (GXM uniform buffer alignment). Retired ranges return to
  // the free lists after their last scene's notification.
  static constexpr uint32_t UniformChunkBytes = 512u << 10;
  void* uniformChunk = nullptr;
  uint32_t uniformCursor = 0;
  size_t uniformBytes = 0;
  std::multimap<uint32_t, uint32_t> uniformFree; // size -> offset
  std::vector<std::pair<SceGxmNotification, std::pair<uint32_t, uint32_t>>> uniformRetired;
  UniformBlock create_uniform_block(const void* bytes, uint32_t size) {
    if (!bytes || !size)
      return {};

    const uint32_t rounded = (size + 15) & ~15u;

    if (!uniformChunk) {
      uniformChunk = device.arena().allocate(UniformChunkBytes);

      if (!uniformChunk)
        return {};
    }

    collect();
    uint32_t offset;

    if (const auto free = uniformFree.find(rounded); free != uniformFree.end()) {
      offset = free->second;
      uniformFree.erase(free);
    } else if (rounded <= UniformChunkBytes - uniformCursor) {
      offset = uniformCursor;
      uniformCursor += rounded;
    } else
      return {};

    auto* data = static_cast<char*>(uniformChunk) + offset;
    std::memcpy(data, bytes, size);
    uniformBytes += rounded;
    return {data, 0, offset, size};
  }

  void retire_uniform_block(const UniformBlock& block) {
    const auto* base = static_cast<const char*>(uniformChunk);
    const auto* data = static_cast<const char*>(block.data);

    if (!uniformChunk || !data || data < base || data >= base + UniformChunkBytes)
      return;

    const uint32_t rounded = (block.size + 15) & ~15u;
    uniformRetired.emplace_back(device.current_fence(), std::make_pair(uint32_t(data - base), rounded));
    uniformBytes -= rounded;
  }

  void forget_program(GLuint id) {
    const auto found = programs.find(id);

    if (found == programs.end())
      return;

    const auto fence = device.completion_fence();

    if (state.program == id)
      state.program = 0;

    retiredPrograms.emplace_back(fence, std::move(found->second));
    programs.erase(found);
    collect();
  }

  void forget_texture(GLuint id) {
    for (auto& bound : state.textures)
      if (bound.texture == id) {
        pin(id, false);
        bound = {};
      }
    if (state.color == id || state.depth == id)
      bind_target(state.color == id ? 0 : state.color, state.depth == id ? 0 : state.depth);

    for (auto it = framebuffers.begin(); it != framebuffers.end();) {
      if (it->first.first == id || it->first.second == id)
        it = framebuffers.erase(it);
      else
        ++it;
    }

    textures.erase(id);
  }

  void capture_draw(std::string_view kind, GLuint program, uint32_t vertices, uint32_t indices, uint32_t instances = 1, const RasterState* actualRaster = nullptr, GLuint texture0 = 0,
                    bool preservesAlphaOne = false) {
    auto r = actualRaster ? *actualRaster : state.raster;

    // Unconditional: tracing may be disabled. Expanded fixed RGB writes
    // output the already-certified one and preserve the certificate.
    if (preservesAlphaOne)
      r.colorMask[3] = true;

    if (state.color && r.colorMask[3] && !preservesAlphaOne)
      entry(state.color).alpha.draw(r.colorMask[3]);

    if (razor_scene_tracing()) {
      const auto found = programs.find(program);
      const auto* native = found == programs.end() ? nullptr : &found->second;
      razor_scene_draw(kind, native ? std::string_view(native->label) : kind, vertices, indices, instances, r.blend,
                       native && native->usesDiscard, native && native->replacesDepth);
    }

    using namespace gfx::port_capture;

    if (!enabled())
      return;

    NativeDraw s;
    s.frame = gfx::current_frame();
    s.program = program;
    s.vertices = vertices;
    s.indices = indices;
    s.instances = instances;
    s.flags = (r.depthTest ? DepthCompare : 0u) | (r.depthWrite ? DepthWrite : 0u) | (r.blend ? Blend : 0u) | ((r.colorMask[0] || r.colorMask[1] || r.colorMask[2]) ? ColorWrite : 0u) |
              (r.colorMask[3] ? AlphaWrite : 0u);
    s.depthFunction = r.depthFunc;
    s.target = state.color;
    s.depthTarget = state.depth;
    s.display = state.display;

    if (actualRaster)
      s.textures[0] = texture0;
    else
      for (unsigned i = 0; i < s.textures.size(); i++)
        s.textures[i] = state.textures[i].texture;
    native_draw(kind, s);
  }

  Device device;
  TexturePool pool;
  DrawMemory draws;
  PixelClip clip;
  PatchedProgram clearProgram, presentProgram;
  std::unique_ptr<PatchedProgram> nativeGeometryPrograms[2];

  // Utility block: quad vertex IDs at 0, quad indices at 64, the constant
  // attribute defaults at DefaultAttributesOffset.
  static constexpr size_t DefaultAttributesOffset = 128;
  MappedMemory utility;
  State state;
  std::map<GLuint, TextureEntry> textures;
  std::map<GLuint, NativeProgram> programs;
  std::vector<std::pair<SceGxmNotification, NativeProgram>> retiredPrograms;
  std::map<GLuint, MappedMemory> meshes;
  std::vector<std::pair<SceGxmNotification, MappedMemory>> retiredMeshes;
  std::map<std::pair<uint64_t, uint64_t>, std::weak_ptr<PatchedProgram>> patchedPrograms;
  std::map<GLuint, SamplerDesc> samplers;
  std::map<std::pair<GLuint, GLuint>, GLuint> framebuffers;
  GLuint nextName = 1;

  // set_depth_liveness: whether later draws read `livenessDepth`, and whether
  // one reads it after the current target scene. unstoredDepth: the depth whose
  // last scene skipped its store. A scene that would have to load it means the
  // liveness was wrong: report it once and always load and store from then on.
  struct DepthLiveness {
    GLuint depth = 0, unstored = 0;
    bool reads = true, readsAfterScene = true, broken = false;
  } liveness;
  Device::DepthScene depth_scene(const float* clearDepth) {
    Device::DepthScene result;

    if (clearDepth) {
      result.load = false;
      result.background = *clearDepth;
    }

    if (state.depth == liveness.depth && !liveness.broken) {
      result.load &= liveness.reads;
      result.store = liveness.reads && liveness.readsAfterScene;
    }

    if (result.load && liveness.unstored == state.depth) {
      std::fprintf(stderr, "Aurora GXM: a scene loads depth its previous scene did not store; depth liveness disabled\n");
      liveness.broken = true;
      result.store = true;
    }

    if (result.store)
      liveness.unstored = liveness.unstored == state.depth ? 0 : liveness.unstored;
    else
      liveness.unstored = state.depth;

    return result;
  }

  gxm::ResidencyStatistics residency() const {
    auto result = pool.statistics();

    for (const auto& mesh : meshes)
      result.model_bytes += mesh.second.size();

    for (const auto& mesh : retiredMeshes)
      result.retired_model_bytes += mesh.second.size();

    return result;
  }
};

// The owner is the thread whose stack is [stackLow, stackHigh); empty while
// no thread owns the context. Every renderer call checks the owner, and a
// stack compare costs a few instructions where std::this_thread::get_id is a
// TLS lookup through pthread_self.
struct RenderContext {
  SDL_Window* window = nullptr;
  uintptr_t stackLow = 0, stackHigh = 0;
  std::unique_ptr<Renderer> renderer;
};

RenderContext* g_context = nullptr;

// Makes the calling thread the context owner.
void own_context(RenderContext& context) {
  SceKernelThreadInfo info{};
  info.size = sizeof(info);

  if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0 || !info.stack || !info.stackSize)
    throw std::runtime_error("GXM context owner has no stack range");

  context.stackLow = reinterpret_cast<uintptr_t>(info.stack);
  context.stackHigh = context.stackLow + info.stackSize;
}

// Residency snapshot for readers off the owner thread (stats reporting).
std::mutex g_residencyLock;
gxm::ResidencyStatistics g_residency;
std::atomic<bool> g_residencyRequested{true};

// Packs are immutable once added. Only metadata is read by the GX producer;
// file reads and texture allocation remain on the render thread.
gfx::NativeTextureSet g_nativeTextures;
Caps g_caps;
Renderer& renderer(unsigned line = __builtin_LINE()) {
  if (!g_context || !on_render_thread() || !g_context->renderer)
    AURORA_THROW(std::runtime_error("Native operation at renderer.cpp:" + std::to_string(line) + " called outside the GXM context owner"));

  return *g_context->renderer;
}

constexpr std::array<FormatInfo, 19> FormatInfos{{
    {0, 0, 0, 4, false, false, true}, {0, 0, 0, 1, false, false, true}, {0, 0, 0, 2, false, false, true}, {0, 0, 0, 2, true, false, false}, {0, 0, 0, 4, false, false, false},
    {0, 0, 0, 4, true, false, false}, {0, 0, 0, 4, false, true, false}, {0, 0, 0, 0, false, false, true}, {0, 0, 0, 0, false, false, true}, {0, 0, 0, 1, false, false, true},
    {0, 0, 0, 1, false, false, true}, {0, 0, 0, 1, false, false, true}, {0, 0, 0, 1, false, false, true}, {0, 0, 0, 2, false, false, true}, {0, 0, 0, 2, false, false, true},
    {0, 0, 0, 0, false, false, true}, {0, 0, 0, 0, false, false, true}, {0, 0, 0, 0, false, false, true}, {0, 0, 0, 0, false, false, true},
}};
} // namespace

NativeMeshBuffer::NativeMeshBuffer(const char* path, uint64_t offset, uint32_t bytes) {
  auto& r = renderer();

  if (!bytes)
    AURORA_THROW(std::runtime_error("Empty native mesh payload"));

  MappedMemory memory(bytes, MappedMemory::Kind::Cdram, "Aurora model pack");
  gfx::read_mesh_payload(path, offset, memory.data(), bytes);
  const auto id = r.name();
  const auto* data = static_cast<const uint8_t*>(memory.data());
  r.meshes.emplace(id, std::move(memory));
  m_buffer = id;
  m_data = data;
  m_bytes = bytes;
}

NativeMeshBuffer::~NativeMeshBuffer() {
  if (!m_buffer || !initialized())
    return;

  post([id = m_buffer] {
    auto& r = renderer();
    const auto it = r.meshes.find(id);

    if (it == r.meshes.end())
      return;
    // Include the current scene without ending it just to retire a resource.
    // Collection polls this notification after normal submission/presentation.
    r.retiredMeshes.emplace_back(r.device.current_fence(), std::move(it->second));
    r.meshes.erase(it);
    r.collect();
  });
}

size_t native_mesh_allocation_bytes(uint32_t bytes) {
  (void)renderer();
  return (size_t(bytes) + 0x3ffff) & ~size_t(0x3ffff);
}

size_t native_mesh_resident_bytes() {
  auto& r = renderer();
  r.collect();
  size_t bytes = 0;

  for (const auto& item : r.meshes)
    bytes += item.second.size();

  for (const auto& item : r.retiredMeshes)
    bytes += item.second.size();

  return bytes;
}

bool initialize(SDL_Window* window) noexcept {
  if (g_context)
    return g_context->window == window;

  // A failure is reported and initialization fails, which stops the game;
  // without exceptions (release builds) it stops at its AURORA_THROW.
#if defined(__cpp_exceptions)
  try {
#endif
    auto thread = std::make_unique<RenderContext>();
    thread->window = window;
    own_context(*thread);
    g_context = thread.get();
    thread->renderer = std::make_unique<Renderer>();
    g_caps = {"Sony", "SGX543 native GXM", "libgxm", 4096, 16, 16384, 1};
    thread.release();
    return true;
#if defined(__cpp_exceptions)
  } catch (const std::exception& error) {
    g_context = nullptr;
    std::printf("Aurora GXM game-thread initialization: %s\n", error.what());
    return false;
  }
#endif
}

void shutdown() noexcept {
  if (!g_context)
    return;

  if (!on_render_thread())
    fail("GXM shutdown called outside the context owner");

  auto* thread = g_context;
  thread->renderer.reset();
  g_context = nullptr;
  delete thread;
}

bool initialized() noexcept {
  return g_context != nullptr;
}

bool on_render_thread() noexcept {
  const char probe = 0;
  return g_context && reinterpret_cast<uintptr_t>(&probe) - g_context->stackLow < g_context->stackHigh - g_context->stackLow;
}

// The libgxm context has one owner at a time: the game thread until the
// runtime's render thread takes it over, and back at shutdown. Releasing
// closes any open scene so the next owner starts at a scene boundary.
void release_context() noexcept {
  if (!on_render_thread())
    fail("GXM context released outside its owner");

  guarded([] { renderer().device.end(); });
  g_context->stackLow = g_context->stackHigh = 0;
}

void acquire_context() noexcept {
  if (!g_context || g_context->stackHigh != 0)
    fail("GXM context acquired while another thread owns it");

  guarded([] { own_context(*g_context); });
}

const Caps& caps() noexcept {
  return g_caps;
}

void post(Task task) noexcept {
  if (!g_context)
    return;

  if (!on_render_thread())
    fail("GXM task called outside the context owner");

  guarded([&] { std::move(task)(); });
}

void call(Task task) noexcept {
  post(std::move(task));
}

bool gpu_frame_pending() noexcept {
  return guarded([] {
    // Finish recording any standalone offscreen work before polling. This
    // submits its notification; it does not wait for the GPU to write it.
    renderer().device.end();
    return gxm::gpu_frame_pending();
  });
}

uint32_t native_frame_slot() noexcept {
  return guarded([] { return renderer().device.frame_slot(); });
}

void discard_native_frame() noexcept {
  guarded([] {
    auto& r = renderer();
    r.device.end();
    r.invalidate_alpha();
    r.collect();
  });
}

void finish() noexcept {
  call([] { renderer().device.finish(); });
}

const FormatInfo& format_info(Format format) noexcept {
  return FormatInfos[size_t(format)];
}

Texture::Texture(uint32_t width_, uint32_t height_, uint32_t mips_, Format format_, std::string_view label) noexcept : width(width_), height(height_), mips(mips_), format(format_), m_name(new Name) {
  gfx::port_capture::texture(this, true, width, height, mips, uint32_t(format), format_info(format).bytesPerPixel, label);
  auto create = [name = m_name, width_, height_, mips_, format_] {
    auto& r = renderer();
    auto storage = std::make_unique<TextureStorage>(r.pool, width_, height_, mips_, format_);
    const auto id = r.name();
    r.textures.emplace(id, TextureEntry{std::move(storage)});
    name->value = id;
  };

  if (on_render_thread())
    guarded(create);
  else
    post(std::move(create));
}

Texture::~Texture() {
  gfx::port_capture::texture(this, false, width, height, mips, uint32_t(format), format_info(format).bytesPerPixel);

  if (!initialized()) {
    delete m_name;
    return;
  }

  post([name = m_name] {
    renderer().forget_texture(name->value);
    delete name;
  });
}

TexturePtr create_texture(uint32_t width, uint32_t height, uint32_t mips, Format format, std::string_view label) noexcept {
  return std::make_shared<Texture>(std::max(width, 1u), std::max(height, 1u), std::max(mips, 1u), format, label);
}

bool render_target_reusable(const Texture& texture) noexcept {
  return guarded([&] {
    const auto& entry = renderer().entry(texture.gl());
    return !entry.pins && entry.storage->in_cdram() && entry.storage->gpu_idle();
  });
}

bool render_target_allocation_available(size_t bytes) noexcept {
  return guarded([&] {
    auto& r = renderer();
    r.pool.collect();
    return r.device.arena().can_allocate(bytes);
  });
}

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
  const auto info = native_texture_info(asset);
  auto texture = create_texture(info.width, info.height, info.mips, info.format, "Offline native texture");
  post([texture, asset] {
    auto& storage = renderer().texture(texture->gl());
    const auto entry = g_nativeTextures.find(asset);
    const auto& r = *entry.record;

    if (storage.width != r.width || storage.height != r.height || storage.mips != r.mips || storage.format != gfx::native_texture_format(r.format) || storage.size() != r.bytes ||
        !storage.in_cdram() || storage.has_gpu_use() ||
        (entry.version == 2 && (storage.image_bytes() != r.image_bytes || storage.palette_offset() != r.palette_offset || storage.palette_bytes() != r.palette_bytes)))
      AURORA_THROW(std::runtime_error("Native texture payload does not match CDRAM layout"));
    // New allocation: no preceding scene refers to these bytes. The queued
    // upload runs before any draw consumes them, with no finish or staging copy.
    gfx::NativeTextureSet::read(entry, storage.pixels());
  });
  return texture;
}

void upload_texture(const Texture& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data, uint32_t bytesPerRow) noexcept {
  gfx::port_capture::upload(&texture, mip, uint64_t(bytesPerRow) * height);
  guarded([&] {
    auto& r = renderer();
    r.texture(texture.gl()).upload(mip, x, y, width, height, data, bytesPerRow);

    if (!mip && width && height)
      r.entry(texture.gl()).alpha.invalidate();
  });
}

void write_texture(const TexturePtr& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t width, uint32_t height, std::vector<uint8_t>&& data, uint32_t bytesPerRow) noexcept {
  if (!texture || !width || !height)
    return;

  if (uint64_t(bytesPerRow) * height > data.size())
    fail("Texture upload source is too short");

  post([texture, mip, x, y, width, height, data = std::move(data), bytesPerRow] { upload_texture(*texture, mip, x, y, width, height, data.data(), bytesPerRow); });
}

void write_texture(const TexturePtr& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data, uint32_t bytesPerRow) noexcept {
  if (!texture || !width || !height)
    return;

  if (!data)
    fail("Null texture upload source");

  const auto* bytes = static_cast<const uint8_t*>(data);
  write_texture(texture, mip, x, y, width, height, std::vector<uint8_t>(bytes, bytes + size_t(bytesPerRow) * height), bytesPerRow);
}

GLuint sampler(const SamplerDesc& desc) noexcept {
  return guarded([&] {
    auto& r = renderer();

    for (const auto& [id, existing] : r.samplers)
      if (desc == existing)
        return id;
    const auto id = r.name();
    r.samplers.emplace(id, desc);
    return id;
  });
}

GLuint framebuffer(GLuint color, GLuint depth) noexcept {
  return guarded([&] {
    if (!color && !depth)
      return GLuint(0);

    auto& r = renderer();
    auto [it, inserted] = r.framebuffers.try_emplace({color, depth}, 0);

    if (inserted)
      it->second = r.name();

    return it->second;
  });
}

void bind_framebuffer(GLuint color, GLuint depth) noexcept {
  guarded([&] { renderer().bind_target(color, depth); });
}

Program::Program(ProgramDesc desc) noexcept : m_desc(std::move(desc)) {
  guarded([&] {
    m_usesAlphaReferences = !m_desc.cg && parameterize_alpha_references(m_desc.fragment, m_alphaReferences);
    const auto captured = gfx::port_capture::shader(m_desc.label, m_desc.vertex, m_desc.fragment, m_desc.origin);
    m_capturePortableHashes = captured.portableHashes;
    m_capturePortableOrigin = captured.originFile;
    m_vertexHash = XXH3_64bits(m_desc.vertex.data(), m_desc.vertex.size());
    m_fragmentHash = XXH3_64bits(m_desc.fragment.data(), m_desc.fragment.size());

    // Construction also covers unused diagnostic programs. Compile/register
    // only when get() actually binds a program; preserve conservative source
    // properties until the compiled program can provide exact flags.
    m_usesDiscard = m_desc.fragment.find("discard;") != std::string::npos;
    m_replacesDepth = m_desc.fragment.find("gl_FragDepth =") != std::string::npos;
  });
}

Program::~Program() {
  if (m_program && initialized())
    post([id = m_program] {
      auto& r = renderer();
      r.forget_program(id);
    });
}

GLuint Program::get() noexcept {
  return guarded([&] {
    if (m_program)
      return m_program;

    auto& r = renderer();
#ifdef MH3_DEVELOPMENT
    // Sources for the development compiler, which serves only bank misses.
    register_runtime_shader_sources(m_vertexHash, m_fragmentHash, m_desc.vertex, m_desc.fragment, m_desc.origin.empty() ? m_desc.label : m_desc.origin + "; " + m_desc.label, m_desc.cg);
#endif
    const auto* binary = fragment_binary(m_fragmentHash);
    m_usesDiscard = sceGxmProgramIsDiscardUsed(binary);
    m_replacesDepth = sceGxmProgramIsDepthReplaceUsed(binary);
    NativeProgram native;

    if (razor_scene_tracing())
      native.label = m_desc.label;

    native.usesDiscard = m_usesDiscard;
    native.replacesDepth = m_replacesDepth;
    native.nativeMaterialVertices = m_desc.nativeMaterialVertices;
    native.nativeMeshLayout = m_desc.nativeMeshLayout;
    native.shaded = m_desc.cg;
    native.preservesTargetAlphaOne = m_desc.preservesTargetAlphaOne;

    if (native.preservesTargetAlphaOne && (!native.shaded || native.replacesDepth))
      AURORA_THROW(std::runtime_error("Alpha-one output promise requires an ordinary fixed shaded program"));

    native.vertexHash = m_vertexHash;
    native.fragmentHash = m_fragmentHash;

    // Shaded programs patch per vertex binding at draw time (shaded_program).
    std::weak_ptr<PatchedProgram> unshared;
    auto& shared = m_desc.cg ? unshared : r.patchedPrograms[{m_vertexHash, m_fragmentHash}];

    // Compact layouts own their patched program. Shader hashes alone cannot
    // distinguish vertex strides/offsets. The frontend caches each full layout.
    if (!m_desc.nativeMeshLayout.stride)
      native.program = shared.lock();

    if (!native.program && !native.shaded) {
      if (m_desc.nativeMeshLayout.stride) {
        gfx::validate_mesh_layout(m_desc.nativeMeshLayout);
        std::vector<VertexInput> inputs;

        for (const auto& attribute : m_desc.nativeMeshLayout.attributes) {
          SceGxmAttributeFormat format = SCE_GXM_ATTRIBUTE_FORMAT_F32;

          switch (attribute.format) {
          case MeshAttributeFormat::F32:
            break;
          case MeshAttributeFormat::F16:
            format = SCE_GXM_ATTRIBUTE_FORMAT_F16;
            break;
          case MeshAttributeFormat::U8N:
            format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
            break;
          case MeshAttributeFormat::U8:
            format = SCE_GXM_ATTRIBUTE_FORMAT_U8;
            break;
          case MeshAttributeFormat::S16N:
            format = SCE_GXM_ATTRIBUTE_FORMAT_S16N;
            break;
          }

          inputs.push_back({gfx::MeshAttributeNames[attribute.location], format, attribute.components, attribute.offset, m_desc.nativeMeshLayout.stride, 0});
        }

        native.program = std::make_shared<PatchedProgram>(r.device, m_vertexHash, m_fragmentHash, SCE_GXM_INDEX_FORMAT_U16, inputs);
      } else if (m_desc.nativeMaterialVertices) {
        constexpr uint16_t stride = sizeof(AuroraNativeMaterialVertex);
        const VertexInput inputs[]{{"a_native_position", SCE_GXM_ATTRIBUTE_FORMAT_F32, 4, 0, stride, 0}, {"a_native_view_position", SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, 16, stride, 0},
                                   {"a_native_normal", SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, 28, stride, 0},  {"a_native_color0", SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4, 136, stride, 0},
                                   {"a_native_color1", SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4, 140, stride, 0}, {"a_native_tex0", SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, 40, stride, 0},
                                   {"a_native_tex1", SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, 52, stride, 0},    {"a_native_tex2", SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, 64, stride, 0},
                                   {"a_native_tex3", SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, 76, stride, 0},    {"a_native_tex4", SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, 88, stride, 0},
                                   {"a_native_tex5", SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, 100, stride, 0},   {"a_native_tex6", SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, 112, stride, 0},
                                   {"a_native_tex7", SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, 124, stride, 0}};
        static_assert(stride == 144 && offsetof(AuroraNativeMaterialVertex, color) == 136);
        native.program = std::make_shared<PatchedProgram>(r.device, m_vertexHash, m_fragmentHash, SCE_GXM_INDEX_FORMAT_U16, inputs);
      } else
        native.program = std::make_shared<PatchedProgram>(r.device, m_vertexHash, m_fragmentHash);

      if (!m_desc.nativeMeshLayout.stride)
        shared = native.program;
    }

    if (m_usesAlphaReferences)
      native.uniforms.emplace("aur_alpha_refs", m_alphaReferences);

    for (const auto& [name, unit] : m_desc.samplers) {
      const auto* parameter = sceGxmProgramFindParameterByName(fragment_binary(m_fragmentHash), name.c_str());

      if (parameter && sceGxmProgramParameterGetCategory(parameter) == SCE_GXM_PARAMETER_CATEGORY_SAMPLER) {
        if (unit < 0 || unit >= 16 || sceGxmProgramParameterGetResourceIndex(parameter) != unsigned(unit))
          AURORA_THROW(std::runtime_error("Offline native sampler unit differs from the frontend"));

        native.samplers.emplace_back(name, unit);
      }
    }

    for (const auto& [name, binding] : m_desc.blocks) {
      if (native.shaded)
        break; // Fixed BUFFER indices, bound by prepare_shaded.
      if (binding != 0)
        AURORA_THROW(std::runtime_error("Native shader uniform block uses an unsupported binding"));

      native.buffer0 = true;
    }

    const auto id = r.name();
    r.programs.emplace(id, std::move(native));
    m_desc.vertex.clear();
    m_desc.fragment.clear();
    m_program = id;
    gfx::port_capture::native_program(id, m_vertexHash, m_fragmentHash, m_usesDiscard, m_replacesDepth, m_desc.label, m_capturePortableHashes, m_capturePortableOrigin);
    return id;
  });
}

void apply_raster_state(const RasterState& value) noexcept {
  guarded([&] { renderer().state.raster = value; });
}

void set_viewport(int32_t x, int32_t y, int32_t width, int32_t height, float minDepth, float maxDepth) noexcept {
  guarded([&] {
    auto& s = renderer().state;
    s.viewport = {x, y, width, height};
    s.depthRange = {minDepth, maxDepth};
  });
}

void set_scissor(int32_t x, int32_t y, int32_t width, int32_t height) noexcept {
  guarded([&] {
    auto& s = renderer().state;
    const std::array value{x, y, width, height};

    if (s.scissor != value) {
      s.scissor = value;
      s.clipDirty = s.scissorEnabled;
    }
  });
}

void set_scissor_enabled(bool enabled) noexcept {
  guarded([&] {
    auto& s = renderer().state;

    if (s.scissorEnabled != enabled) {
      s.scissorEnabled = enabled;
      s.clipDirty = true;
    }
  });
}

void set_blend_color(float r, float g, float b, float a) noexcept {
  guarded([&] { renderer().state.blendColor = {r, g, b, a}; });
}

void use_program(GLuint id) noexcept {
  guarded([&] { renderer().state.program = id; });
}

void bind_texture(uint32_t unit, GLuint texture, GLuint samplerId) noexcept {
  guarded([&] { renderer().texture_binding(unit, texture, samplerId); });
}

void bind_uniform_range(uint32_t binding, GLuint, uint32_t offset, uint32_t bytes) noexcept {
  guarded([&] {
    if (binding != 0)
      AURORA_THROW(std::runtime_error("Unsupported native uniform binding"));

    auto& s = renderer().state;
    s.uniformOffset = offset;
    s.uniformBytes = bytes;
    s.uniformValid = true;
  });
}

void set_color_mask(bool r, bool g, bool b, bool a) noexcept {
  guarded([&] { renderer().state.raster.colorMask = {r, g, b, a}; });
}

void set_depth_write(bool enabled) noexcept {
  guarded([&] { renderer().state.raster.depthWrite = enabled; });
}

void invalidate_state() noexcept {
  guarded([] {
    auto& r = renderer();
    r.device.end();
    r.invalidate_alpha();
    r.reset_texture_bindings();
    r.state.program = 0;
    r.state.uniformValid = false;
    r.state.clipDirty = true;
  });
}

void bind_draw_vao(GLuint) noexcept { /* Active mapped indices belong to DrawMemory. */ }
void draw_fullscreen(GLuint id) noexcept {
  if (!aurora::native_frame_drawing_allowed())
    return;

  guarded([&] {
    auto& r = renderer();
    r.state.program = id;
    r.prepare(id);
    r.utility_draw(3);
    r.capture_draw("fullscreen", id, 3, 3);
  });
}

OffscreenState save_offscreen_state() noexcept {
  return guarded([] {
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

    for (unsigned i = 0; i < s.textures.size(); ++i) {
      out.textures[i] = s.textures[i].texture;
      out.samplers[i] = s.textures[i].sampler;
    }

    out.uniforms[0] = {0, s.uniformOffset, s.uniformBytes};
    out.uniformValid = s.uniformValid;
    return out;
  });
}

void restore_offscreen_state(const OffscreenState& saved) noexcept {
  guarded([&] {
    auto& r = renderer();
    r.bind_target(saved.color, saved.depth);
    r.state.raster = saved.raster;
    r.state.viewport = saved.viewport;
    r.state.depthRange = saved.depthRange;
    r.state.scissor = saved.scissor;
    r.state.scissorEnabled = saved.scissorEnabled;
    r.state.blendColor = saved.blendColor;
    r.state.program = saved.program;

    for (unsigned i = 0; i < saved.textures.size(); ++i)
      r.texture_binding(i, saved.textures[i], saved.samplers[i]);

    r.state.uniformOffset = saved.uniforms[0].offset;
    r.state.uniformBytes = saved.uniforms[0].size;
    r.state.uniformValid = saved.uniformValid;
    r.state.clipDirty = true;
  });
}

GLuint create_buffer() noexcept {
  return guarded([] { return renderer().name(); });
}

void destroy_buffer(GLuint) noexcept { /* Buffer storage belongs to the mapped frame slot. */ }
void write_buffer(GLuint, bool, const void*, size_t bytes) noexcept {
  if (bytes)
    fail("Native draws must bind their mapped payload directly");
}

void copy_texture_region(GLuint source, GLuint dest, int32_t srcX, int32_t srcY, int32_t dstX, int32_t dstY, int32_t width, int32_t height) noexcept {
  guarded([&] {
    if (width <= 0 || height <= 0)
      return;

    auto& r = renderer();
    auto& src = r.texture(source);
    auto& dst = r.texture(dest);

    if (srcX < 0 || srcY < 0 || dstX < 0 || dstY < 0 || uint64_t(srcX) + width > src.width || uint64_t(srcY) + height > src.height || uint64_t(dstX) + width > dst.width ||
        uint64_t(dstY) + height > dst.height || src.format != dst.format)
      AURORA_THROW(std::runtime_error("Native texture copy has incompatible formats or exceeds bounds"));
    SceGxmTransferFormat format;

    switch (src.format) {
    case Format::R8:
      format = SCE_GXM_TRANSFER_FORMAT_U8_R;
      break;
    case Format::RG8:
      format = SCE_GXM_TRANSFER_FORMAT_RAW16;
      break;
    default:
      format = SCE_GXM_TRANSFER_FORMAT_RAW32;
      break;
    }

    r.device.finish();
    check(sceGxmTransferCopy(width, height, 0, 0, SCE_GXM_TRANSFER_COLORKEY_NONE, format, SCE_GXM_TRANSFER_LINEAR, src.pixels(), srcX, srcY, src.row_bytes(), format, SCE_GXM_TRANSFER_LINEAR,
                             dst.pixels(), dstX, dstY, dst.row_bytes(), nullptr, 0, nullptr),
          "copy texture region");
    check(sceGxmTransferFinish(), "finish texture region copy");
    r.entry(dest).alpha.copy(r.entry(source).alpha.one, !dstX && !dstY && uint32_t(width) == dst.width && uint32_t(height) == dst.height);
  });
}

bool shaded_target_alpha_one() noexcept {
  return guarded([] { return renderer().shaded_alpha_one(); });
}

void clear_color(const float* color) noexcept {
  if (aurora::native_frame_drawing_allowed())
    guarded([&] { renderer().clear(color, 0.f, false); });
}

void clear_depth(float depth) noexcept {
  if (aurora::native_frame_drawing_allowed())
    guarded([&] {
      const float ignored[4]{};
      renderer().clear(ignored, depth, true);
    });
}

void set_depth_liveness(GLuint depth, bool reads, bool readsAfterScene) noexcept {
  guarded([&] {
    auto& liveness = renderer().liveness;
    liveness.depth = depth;
    liveness.reads = reads;
    liveness.readsAfterScene = readsAfterScene;
  });
}

void set_present_game_time(uint64_t us) noexcept {
  guarded([&] { renderer().device.set_present_game_time(us); });
}

void clear_depth_full_target(float depth) noexcept {
  if (!aurora::native_frame_drawing_allowed())
    return;

  guarded([&] {
    auto& r = renderer();
    const bool depthWrite = r.state.raster.depthWrite, scissor = r.state.scissorEnabled;
    r.state.raster.depthWrite = true;

    if (scissor) {
      r.state.scissorEnabled = false;
      r.state.clipDirty = true;
    }

    const float ignored[4]{};
    r.clear(ignored, depth, true);
    r.state.raster.depthWrite = depthWrite;

    if (scissor) {
      r.state.scissorEnabled = true;
      r.state.clipDirty = true;
    }
  });
}

void draw_indexed(uint32_t indexCount, uint32_t indexOffset, uint32_t instances) noexcept {
  if (!aurora::native_frame_drawing_allowed())
    return;

  guarded([&] {
    if (!indexCount || !instances)
      return;

    auto& r = renderer();
    r.prepare(r.state.program);
    r.draws.draw(indexCount, indexOffset, instances);
    r.capture_draw("indexed", r.state.program, 0, indexCount, instances);
  });
}

void draw_native_geometry(const AuroraNativeGeometry& geometry, bool textured) noexcept {
  if (!aurora::native_frame_drawing_allowed() || !geometry.index_count)
    return;

  guarded([&] {
    auto& r = renderer();
    auto& program = r.nativeGeometryPrograms[textured];

    if (!program) {
      static_assert(sizeof(AuroraNativeVertex) == 28);
      const VertexInput inputs[]{
          {"position_in", SCE_GXM_ATTRIBUTE_FORMAT_F32, 4, 0, 28, 0}, {"uv_in", SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, 16, 28, 0}, {"color_in", SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4, 24, 28, 0}};
      program = std::make_unique<PatchedProgram>(r.device, 7, textured ? 8 : 7, SCE_GXM_INDEX_FORMAT_U16, inputs);
    }

    r.scene();
    r.viewport();
    apply_raster(r.state.raster, r.device.context());
    auto blend = blend_info(r.state.raster, r.state.blendColor);

    if (!r.state.display && !r.state.color)
      blend.colorMask = SCE_GXM_COLOR_MASK_NONE;

    r.bind_program(*program, blend);

    if (textured) {
      const auto& binding = r.state.textures[0];

      if (!binding.texture)
        AURORA_THROW(std::runtime_error("Native geometry has no texture binding"));

      auto& texture = r.texture(binding.texture);
      texture.note_gpu_use();
      const auto sampled = sampling_view(texture, binding.sampler ? r.samplers.at(binding.sampler) : NearestClamp);
      check(sceGxmSetFragmentTexture(r.device.context(), 0, &sampled), "bind native geometry texture");
    }

    check(sceGxmSetVertexStream(r.device.context(), 0, geometry.vertices), "bind native vertex attributes");
    check(sceGxmDraw(r.device.context(), SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, geometry.indices, geometry.index_count), "draw native triangles");

    // Fixed embedded geometry programs bypass the Program object namespace.
    r.capture_draw(textured ? "geometry-textured" : "geometry-colored", 0, geometry.vertex_count, geometry.index_count);
  });
}

void draw_native_material_geometry(const AuroraNativeMaterialGeometry& geometry, GLuint program, const AuroraNativeUniforms& uniforms) noexcept {
  if (!aurora::native_frame_drawing_allowed() || !geometry.index_count)
    return;

  guarded([&] {
    auto& r = renderer();

    if (!r.programs.at(program).nativeMaterialVertices)
      AURORA_THROW(std::runtime_error("Native material draw has a different vertex layout"));

    r.state.uniformOffset = uniforms.offset;
    r.state.uniformBytes = uniforms.size;
    r.state.uniformValid = true;
    r.prepare(program);
    check(sceGxmSetVertexStream(r.device.context(), 0, geometry.vertices), "bind native material attributes");
    check(sceGxmDraw(r.device.context(), SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, geometry.indices, geometry.index_count), "draw native material triangles");
    r.capture_draw("material", program, geometry.vertex_count, geometry.index_count);
  });
}

void draw_native_mesh(const NativeMeshBuffer& buffer, const MeshLayout& layout, uint32_t vertexOffset, uint32_t vertexCount, uint32_t indexOffset, uint32_t indexCount, GLuint program,
                      const AuroraNativeUniforms& uniforms) {
  if (!aurora::native_frame_drawing_allowed() || !indexCount)
    return;

  gfx::validate_mesh_draw(buffer, layout, vertexOffset, vertexCount, indexOffset, indexCount);
  auto& r = renderer();

  if (!(r.programs.at(program).nativeMeshLayout == layout))
    AURORA_THROW(std::runtime_error("Resident mesh differs from patched vertex layout"));

  const auto found = r.meshes.find(buffer.get());

  if (found == r.meshes.end() || found->second.data() != buffer.data())
    AURORA_THROW(std::runtime_error("Resident mesh backing was retired"));

  if (!uniforms.data || !uniforms.size)
    AURORA_THROW(std::runtime_error("Native mesh has no uniforms"));

  r.state.uniformOffset = uniforms.offset;
  r.state.uniformBytes = uniforms.size;
  r.state.uniformValid = true;
  r.prepare(program);
  check(sceGxmSetVertexStream(r.device.context(), 0, buffer.data() + vertexOffset), "bind resident mesh attributes");
  check(sceGxmDraw(r.device.context(), SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, buffer.data() + indexOffset, indexCount), "draw resident mesh triangles");
  r.capture_draw("mesh", program, vertexCount, indexCount);
}

namespace {
SceGxmAttributeFormat attribute_format(MeshAttributeFormat format, unsigned& componentBytes) {
  switch (format) {
  case MeshAttributeFormat::F32:
    componentBytes = 4;
    return SCE_GXM_ATTRIBUTE_FORMAT_F32;
  case MeshAttributeFormat::F16:
    componentBytes = 2;
    return SCE_GXM_ATTRIBUTE_FORMAT_F16;
  case MeshAttributeFormat::U8N:
    componentBytes = 1;
    return SCE_GXM_ATTRIBUTE_FORMAT_U8N;
  case MeshAttributeFormat::U8:
    componentBytes = 1;
    return SCE_GXM_ATTRIBUTE_FORMAT_U8;
  case MeshAttributeFormat::S16N:
    componentBytes = 2;
    return SCE_GXM_ATTRIBUTE_FORMAT_S16N;
  }

  AURORA_THROW(std::runtime_error("Unknown mesh attribute format"));
}
} // namespace

void draw_shaded_mesh(const NativeMeshBuffer& buffer, const MeshLayout& layout, const ShadedMeshInputs& inputs, uint32_t vertexOffset, uint32_t vertexCount, uint32_t indexOffset, uint32_t indexCount,
                      GLuint program, const ShadedBlocks& blocks, const std::array<uint8_t, 3>& textureUnits) {
  if (!aurora::native_frame_drawing_allowed() || !indexCount)
    return;

  gfx::validate_mesh_draw(buffer, layout, vertexOffset, vertexCount, indexOffset, indexCount);

  if (!blocks.draw || !blocks.paletteBytes || !blocks.scene)
    AURORA_THROW(std::runtime_error("Shaded mesh has no uniform blocks"));

  auto& r = renderer();
  const auto found = r.meshes.find(buffer.get());

  if (found == r.meshes.end() || found->second.data() != buffer.data())
    AURORA_THROW(std::runtime_error("Resident mesh backing was retired"));
  // Same order as the Cg mesh entry point; a selector lane reads one byte.
  static constexpr const char* Names[] = {"a_position", "a_normal", "a_color0", "a_uv0", "a_uv1", "a_uv2", "a_select", "a_select0", "a_select1", "a_select2"};
  static_assert(std::size(Names) == std::tuple_size_v<decltype(inputs.location)>);
  VertexInput bound[std::size(Names)];
  unsigned count = 0;

  for (unsigned i = 0; i < std::size(Names); ++i) {
    if (inputs.location[i] < 0)
      continue;

    const MeshAttribute* attribute = nullptr;

    for (const auto& candidate : layout.attributes)
      if (candidate.location == uint8_t(inputs.location[i]))
        attribute = &candidate;
    if (!attribute)
      continue;

    unsigned componentBytes = 0;
    const auto format = attribute_format(attribute->format, componentBytes);
    const int lane = inputs.lane[i];
    bound[count++] = {Names[i], format, uint16_t(lane >= 0 ? 1 : attribute->components), uint16_t(attribute->offset + (lane >= 0 ? unsigned(lane) * componentBytes : 0)), uint16_t(layout.stride), 0};
  }

  auto& native = r.programs.at(program);
  r.prepare_shaded(native, r.shaded_program(native, {bound, count}), blocks, textureUnits);
  check(sceGxmSetVertexStream(r.device.context(), 0, buffer.data() + vertexOffset), "bind shaded mesh attributes");
  check(sceGxmDraw(r.device.context(), SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, buffer.data() + indexOffset, indexCount), "draw shaded mesh");
  r.capture_draw("shaded-mesh", program, vertexCount, indexCount, 1, nullptr, 0, native.preservesTargetAlphaOne);
}

void draw_shaded_geometry(const AuroraNativeMaterialGeometry& geometry, const std::array<uint8_t, 3>& coordinates, GLuint program, const ShadedBlocks& blocks,
                          const std::array<uint8_t, 3>& textureUnits) noexcept {
  if (!aurora::native_frame_drawing_allowed() || !geometry.index_count)
    return;

  guarded([&] {
    if (!blocks.draw || !blocks.scene)
      AURORA_THROW(std::runtime_error("Shaded geometry has no uniform blocks"));

    auto& r = renderer();
    constexpr uint16_t stride = sizeof(AuroraNativeMaterialVertex);
    const auto texcoord = [&](unsigned role) { return uint16_t(offsetof(AuroraNativeMaterialVertex, texcoord) + std::min<unsigned>(coordinates[role], 7) * 12); };
    const VertexInput inputs[]{{"a_position", SCE_GXM_ATTRIBUTE_FORMAT_F32, 4, offsetof(AuroraNativeMaterialVertex, position), stride, 0},
                               {"a_view_position", SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, offsetof(AuroraNativeMaterialVertex, view_position), stride, 0},
                               {"a_normal", SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, offsetof(AuroraNativeMaterialVertex, normal), stride, 0},
                               {"a_color0", SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4, offsetof(AuroraNativeMaterialVertex, color), stride, 0},
                               {"a_uv0", SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, texcoord(0), stride, 0},
                               {"a_uv1", SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, texcoord(1), stride, 0},
                               {"a_uv2", SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, texcoord(2), stride, 0}};
    auto& native = r.programs.at(program);
    r.prepare_shaded(native, r.shaded_program(native, inputs), blocks, textureUnits);
    check(sceGxmSetVertexStream(r.device.context(), 0, geometry.vertices), "bind shaded geometry attributes");
    check(sceGxmDraw(r.device.context(), SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, geometry.indices, geometry.index_count), "draw shaded geometry");
    r.capture_draw("shaded-geometry", program, geometry.vertex_count, geometry.index_count, 1, nullptr, 0, native.preservesTargetAlphaOne);
  });
}

UniformBlock create_uniform_block(const void* bytes, uint32_t size) noexcept {
  return guarded([&] { return renderer().create_uniform_block(bytes, size); });
}

void retire_uniform_block(const UniformBlock& block) noexcept {
  guarded([&] { renderer().retire_uniform_block(block); });
}

size_t uniform_block_bytes() noexcept {
  return guarded([] { return renderer().uniformBytes; });
}

UniformRing create_uniform_ring(uint32_t bytes) noexcept {
  return guarded([&] {
    auto* data = static_cast<uint8_t*>(renderer().device.arena().allocate(bytes));
    return data ? UniformRing{data, bytes} : UniformRing{};
  });
}

void destroy_uniform_ring(const UniformRing& ring) noexcept {
  guarded([&] {
    if (!ring.data)
      return;
    // Shutdown path: the GPU may still read the last frames' sections.
    auto& r = renderer();
    r.device.finish();
    r.device.arena().free(ring.data, ring.size);
  });
}

GpuFence submitted_gpu_fence() noexcept {
  return guarded([] {
    const auto fence = renderer().device.current_fence();
    return GpuFence{fence.address, fence.value};
  });
}

void uniform4(GLuint id, const char* name, float x, float y, float z, float w) noexcept {
  guarded([&] { renderer().programs.at(id).uniforms[name] = {x, y, z, w}; });
}

void present(const TexturePtr& source, int32_t x, int32_t y, int32_t width, int32_t height) noexcept {
  if (!aurora::native_frame_drawing_allowed())
    return;

  guarded([&] {
    auto& r = renderer();
    r.bind_target(0, 0);
    r.reset_texture_bindings();
    r.state.scissorEnabled = false;
    r.state.clipDirty = false;
    r.state.raster.colorMask = {true, true, true, true};
    const float black[4]{0.f, 0.f, 0.f, 1.f};
    r.clear(black, 0.f, false);

    if (source && width > 0 && height > 0) {
      r.pin(source->gl(), true);
      r.pool.touch(r.texture(source->gl()));
      r.scene();
      apply_viewport(r.device.context(), x, y, width, height, 0.f, 1.f);
      apply_raster({}, r.device.context());
      r.presentProgram.bind(blend_info({}, {}));
      const float region[4]{-1.f, 1.f, 2.f, -2.f}, depth = 0.f;
      r.presentProgram.set_vertex_uniform("region", region, 4);
      r.presentProgram.set_vertex_uniform("depth", &depth, 1);
      r.texture(source->gl()).note_gpu_use();
      const auto sampled = sampling_view(r.texture(source->gl()), LinearClamp);
      check(sceGxmSetFragmentTexture(r.device.context(), 0, &sampled), "bind presentation source");
      r.utility_draw(6);
      const RasterState presentRaster{};
      r.capture_draw("present", 0, 6, 6, 1, &presentRaster, source->gl());
      r.pin(source->gl(), false);
    }

    r.device.flip();
    r.collect();

    if (g_residencyRequested.exchange(false, std::memory_order_relaxed)) {
      const auto residency = r.residency();
      std::lock_guard lock(g_residencyLock);
      g_residency = residency;
    }
  });
}

std::pair<uint32_t, uint32_t> drawable_size() noexcept {
  return {Device::Width, Device::Height};
}

void read_pixels(GLuint id, uint32_t x, uint32_t y, uint32_t width, uint32_t height, void* out) noexcept {
  guarded([&] {
    auto& r = renderer();
    auto& texture = r.texture(id);

    if (texture.format != Format::RGBA8 || uint64_t(x) + width > texture.width || uint64_t(y) + height > texture.height || !out)
      AURORA_THROW(std::runtime_error("Native RGBA readback exceeds bounds or uses another format"));

    r.device.finish();
    const auto* input = static_cast<const char*>(texture.pixels()) + size_t(y) * texture.row_bytes() + x * 4;
    auto* output = static_cast<char*>(out);

    for (unsigned row = 0; row < height; ++row) {
      std::memcpy(output, input, size_t(width) * 4);
      input += texture.row_bytes();
      output += size_t(width) * 4;
    }
  });
}
} // namespace aurora::gl

namespace aurora::gxm {
ResidencyStatistics residency_statistics() noexcept {
  if (!gl::on_render_thread()) {
    // The owner refreshes the snapshot at its next present.
    gl::g_residencyRequested.store(true, std::memory_order_relaxed);
    std::lock_guard lock(gl::g_residencyLock);
    return gl::g_residency;
  }

  return gl::guarded([] { return gl::renderer().residency(); });
}
} // namespace aurora::gxm
