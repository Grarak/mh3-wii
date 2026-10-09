#include "../gl/gl.hpp"
#include "../gfx/port_capture.hpp"
#include "../gfx/native_mesh_io.hpp"
#include "../gfx/native_mesh_layout.hpp"
#include "alpha_compare.hpp"
#include "native_assets.hpp"
#include "device.hpp"
#include "runtime_shader.hpp"
#include <aurora/native_frame_gate.hpp>
#include "draw_memory.hpp"
#include "raster.hpp"
#include "texture.hpp"
#include "sampler_bias.hpp"
#include <aurora/thread_role.hpp>
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <psp2/kernel/processmgr.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <thread>

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
template <typename F> decltype(auto) guarded(F&& operation) noexcept {
  try { return operation(); }
  catch (const std::exception& error) { fail(error.what()); }
  catch (...) { fail("Unknown native renderer failure"); }
}
struct TextureEntry {
  std::unique_ptr<TextureStorage> storage;
  unsigned pins = 0;
};
struct NativeProgram {
  std::shared_ptr<PatchedProgram> program;
  std::vector<std::pair<std::string, int>> samplers;
  std::map<std::string, std::array<float, 4>> uniforms;
  bool buffer0 = false;
  bool nativeMaterialVertices = false;
  MeshLayout nativeMeshLayout;
};
struct State {
  RasterState raster;
  std::array<int32_t, 4> viewport{0, 0, Device::Width, Device::Height};
  std::array<float, 2> depthRange{0.f, 1.f};
  std::array<int32_t, 4> scissor{};
  std::array<float, 4> blendColor{};
  bool scissorEnabled = false, clipDirty = true, display = true;
  GLuint color = 0, depth = 0, program = 0;
  struct TextureBinding { GLuint texture = 0, sampler = 0; };
  std::array<TextureBinding, 16> textures{};
  uint32_t uniformOffset = 0, uniformBytes = 0;
  bool uniformValid = false;
};
uint32_t address_mode(GLenum mode) {
  switch (mode) {
  case GL_REPEAT: return 0;
  case GL_MIRRORED_REPEAT: return 1;
  case GL_CLAMP_TO_EDGE: return 2;
  default: throw std::runtime_error("Unsupported native texture address mode");
  }
}
SceGxmTexture sampling_view(const TextureStorage& texture, const SamplerDesc& desc) {
  SceGxmTexture result = texture.sampled();
  set_texture_address_modes(result, address_mode(desc.wrapS), address_mode(desc.wrapT));
  const bool minLinear = desc.minFilter == GL_LINEAR || desc.minFilter == GL_LINEAR_MIPMAP_NEAREST ||
                         desc.minFilter == GL_LINEAR_MIPMAP_LINEAR;
  const bool mipmapped = desc.minFilter != GL_NEAREST && desc.minFilter != GL_LINEAR;
  const bool mipLinear = desc.minFilter == GL_NEAREST_MIPMAP_LINEAR || desc.minFilter == GL_LINEAR_MIPMAP_LINEAR;
  if (texture.format == Format::Depth32F) {
    // LINEAR_STRIDED shares min/mip/LOD bits with its explicit stride.
    if (minLinear || desc.magFilter != GL_NEAREST || mipmapped || desc.lodBias != 0.f)
      throw std::runtime_error("Strided depth sampling requires nearest filtering");
    return result;
  }
  const int bias = sampler_bias_encoding(desc.lodBias);
  if (bias < 0) throw std::runtime_error("Native GXM sampler bias is not exactly representable");
  check(sceGxmTextureSetLodBias(&result, uint32_t(bias)), "set sampler LOD bias");
  check(sceGxmTextureSetMinFilter(&result, minLinear ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT), "set min filter");
  check(sceGxmTextureSetMagFilter(&result, desc.magFilter == GL_LINEAR ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT), "set mag filter");
  check(sceGxmTextureSetMipFilter(&result, mipLinear ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED : SCE_GXM_TEXTURE_MIP_FILTER_DISABLED), "set mip filter");
  const float last = float(texture.mips - 1);
  const float minLod = mipmapped ? std::clamp(desc.minLod, 0.f, last) : 0.f;
  const float maxLod = mipmapped ? std::clamp(desc.maxLod, minLod, last) : 0.f;
  if (std::floor(minLod) != minLod || std::floor(maxLod) != maxLod)
    throw std::runtime_error("Fractional sampler LOD bounds require an offline shader clamp");
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
  explicit Renderer(bool vsync)
      : device(vsync), pool(device), draws(device), clip(device),
        clearProgram(device, 1, 1), presentProgram(device, 1, 2),
        utility(4096, MappedMemory::Kind::Draw, "Aurora utility draws") {
    const float ids[4]{0, 1, 2, 3};
    const uint16_t indices[6]{0, 1, 2, 2, 1, 3};
    std::memcpy(utility.data(), ids, sizeof(ids));
    std::memcpy(static_cast<char*>(utility.data()) + 64, indices, sizeof(indices));
  }
  ~Renderer() {
    device.finish();
    programs.clear();
    textures.clear();
  }
  GLuint name() {
    if (!nextName) throw std::runtime_error("Native GPU name space exhausted");
    return nextName++;
  }
  TextureEntry& entry(GLuint id) {
    const auto it = textures.find(id);
    if (it == textures.end()) throw std::runtime_error("Unknown native texture name");
    return it->second;
  }
  TextureStorage& texture(GLuint id) { return *entry(id).storage; }
  void pin(GLuint id, bool add) {
    if (!id) return;
    auto& value = entry(id);
    if (add) ++value.pins;
    else {
      if (!value.pins) throw std::runtime_error("Native texture pin accounting underflow");
      --value.pins;
    }
    value.storage->pin(value.pins != 0);
  }
  void bind_target(GLuint color, GLuint depth) {
    if (state.color == color && state.depth == depth && state.display == (!color && !depth)) return;
    device.end();
    pin(state.color, false); pin(state.depth, false);
    state.color = color; state.depth = depth;
    state.display = !color && !depth;
    pin(color, true); pin(depth, true);
    if (color && texture(color).format == Format::Depth32F)
      throw std::runtime_error("Native color attachment has a depth format");
    if (depth && texture(depth).format != Format::Depth32F)
      throw std::runtime_error("Native depth attachment has a color format");
    if (color && depth && (texture(color).width != texture(depth).width || texture(color).height != texture(depth).height))
      throw std::runtime_error("Native framebuffer attachment dimensions differ");
    state.clipDirty = true;
  }
  std::pair<uint32_t, uint32_t> target_size() {
    if (state.display) return {Device::Width, Device::Height};
    const auto& target = texture(state.color ? state.color : state.depth);
    return {target.width, target.height};
  }
  void scene() {
    const auto [width, height] = target_size();
    if (!device.scene_active()) {
      if (state.display) device.begin_display();
      else device.begin(width, height, state.color ? texture(state.color).color_surface() : nullptr,
                         state.depth ? texture(state.depth).depth_surface() : nullptr);
      // Stored DF32M contains coverage from the previous scene. Recreate the
      // current coverage even when the logical scissor has not changed.
      state.clipDirty = state.depth || state.scissorEnabled;
      if (state.color) texture(state.color).note_gpu_use(true);
      if (state.depth) texture(state.depth).note_gpu_use(true);
    }
    if (state.clipDirty) {
      clip.apply(width, height, state.scissorEnabled, state.scissor[0], state.scissor[1], state.scissor[2], state.scissor[3]);
      state.clipDirty = false;
    }
  }
  SceGxmOutputRegisterFormat output_format() {
    return state.color ? texture(state.color).output_format() : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4;
  }
  void viewport() {
    const auto& v = state.viewport;
    apply_viewport(device.context(), v[0], v[1], v[2], v[3], state.depthRange[0], state.depthRange[1]);
  }
  void texture_binding(uint32_t unit, GLuint id, GLuint samplerId) {
    if (unit >= state.textures.size()) throw std::runtime_error("Native texture unit out of range");
    auto& bound = state.textures[unit];
    if (bound.texture != id) {
      pin(bound.texture, false); pin(id, true);
      bound.texture = id;
      if (id) pool.touch(texture(id));
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
    if (found == programs.end()) throw std::runtime_error("Unknown native program name");
    auto& native = found->second;
    scene();
    viewport();
    apply_raster(state.raster, device.context());
    auto blend = blend_info(state.raster, state.blendColor);
    if (!state.display && !state.color) blend.colorMask = SCE_GXM_COLOR_MASK_NONE;
    native.program->bind(blend, output_format());
    if (native.buffer0) {
      if (!state.uniformValid) throw std::runtime_error("Native shader requires an unbound uniform range");
      draws.bind_uniform_range(state.uniformOffset, state.uniformBytes);
    }
    for (const auto& [uniform, values] : native.uniforms) {
      native.program->set_vertex_uniform(uniform.c_str(), values.data(), 4);
      native.program->set_fragment_uniform(uniform.c_str(), values.data(), 4);
    }
    for (const auto& [samplerName, unit] : native.samplers) {
      const auto& bound = state.textures.at(unit);
      if (!bound.texture) throw std::runtime_error("Native shader requires an unbound sampled texture");
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
  void utility_draw(uint32_t count) {
    check(sceGxmSetVertexStream(device.context(), 0, utility.data()), "bind utility vertex IDs");
    check(sceGxmSetVertexStream(device.context(), 1, utility.data()), "bind utility instance IDs");
    check(sceGxmDraw(device.context(), SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16,
                    static_cast<char*>(utility.data()) + 64, count), "draw native utility geometry");
  }
  void clear(const float* color, float depth, bool depthOnly) {
    if (depthOnly && (!state.depth || !state.raster.depthWrite)) return;
    if (!depthOnly && !state.display && !state.color) return;
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
  }
  void collect() {
    pool.collect();
    for (auto it = retiredMeshes.begin(); it != retiredMeshes.end();) {
      if (int32_t(*it->first.address - it->first.value) < 0) { ++it; continue; }
      it = retiredMeshes.erase(it);
    }
    for (auto it = retiredPrograms.begin(); it != retiredPrograms.end();) {
      if (int32_t(*it->first.address - it->first.value) < 0) { ++it; continue; }
      it = retiredPrograms.erase(it);
    }
  }
  void forget_program(GLuint id) {
    const auto found = programs.find(id);
    if (found == programs.end()) return;
    const auto fence = device.completion_fence();
    if (state.program == id) state.program = 0;
    retiredPrograms.emplace_back(fence, std::move(found->second));
    programs.erase(found);
    collect();
  }
  void forget_texture(GLuint id) {
    for (auto& bound : state.textures) if (bound.texture == id) {
      pin(id, false); bound = {};
    }
    if (state.color == id || state.depth == id)
      bind_target(state.color == id ? 0 : state.color, state.depth == id ? 0 : state.depth);
    for (auto it = framebuffers.begin(); it != framebuffers.end();) {
      if (it->first.first == id || it->first.second == id) it = framebuffers.erase(it);
      else ++it;
    }
    textures.erase(id);
  }
  Device device;
  TexturePool pool;
  NativeAssets assets;
  DrawMemory draws;
  PixelClip clip;
  PatchedProgram clearProgram, presentProgram;
  std::unique_ptr<PatchedProgram> nativeGeometryPrograms[2];
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
};
struct RenderContext {
  SDL_Window* window = nullptr;
  std::thread::id id;
  std::unique_ptr<Renderer> renderer;
};
RenderContext* g_context = nullptr;
// Published once after opening the pack. Only immutable metadata is read by
// the GX producer; file reads and texture allocation remain on the render thread.
std::atomic<const NativeAssets*> g_nativeAssets{nullptr};
Caps g_caps;
Renderer& renderer() {
  if (!g_context || !on_render_thread() || !g_context->renderer)
    throw std::runtime_error("Native operation called outside the game thread");
  return *g_context->renderer;
}
constexpr std::array<FormatInfo, 19> FormatInfos{{
    {0, 0, 0, 4, false, false, true}, {0, 0, 0, 1, false, false, true},
    {0, 0, 0, 2, false, false, true}, {0, 0, 0, 2, true, false, false},
    {0, 0, 0, 4, false, false, false}, {0, 0, 0, 4, true, false, false},
    {0, 0, 0, 4, false, true, false},
    {0, 0, 0, 0, false, false, true}, {0, 0, 0, 0, false, false, true},
    {0, 0, 0, 1, false, false, true}, {0, 0, 0, 1, false, false, true},
    {0, 0, 0, 1, false, false, true}, {0, 0, 0, 1, false, false, true},
    {0, 0, 0, 2, false, false, true}, {0, 0, 0, 2, false, false, true},
    {0, 0, 0, 0, false, false, true}, {0, 0, 0, 0, false, false, true},
    {0, 0, 0, 0, false, false, true}, {0, 0, 0, 0, false, false, true},
}};
}
NativeMeshBuffer::NativeMeshBuffer(const char* path, uint64_t offset, uint32_t bytes) {
  auto& r = renderer();
  if (!bytes) throw std::runtime_error("Empty native mesh payload");
  MappedMemory memory(bytes, MappedMemory::Kind::Cdram, "Aurora model pack");
  gfx::read_mesh_payload(path, offset, memory.data(), bytes);
  const auto id = r.name();
  const auto* data = static_cast<const uint8_t*>(memory.data());
  r.meshes.emplace(id, std::move(memory));
  m_buffer = id; m_data = data; m_bytes = bytes;
}
NativeMeshBuffer::~NativeMeshBuffer() {
  if (!m_buffer || !initialized()) return;
  post([id = m_buffer] {
    auto& r = renderer();
    const auto it = r.meshes.find(id);
    if (it == r.meshes.end()) return;
    // Include the current scene without ending it just to retire a resource.
    // Collection polls this notification after normal submission/presentation.
    r.retiredMeshes.emplace_back(r.device.current_fence(), std::move(it->second));
    r.meshes.erase(it);
    r.collect();
  });
}
bool initialize(SDL_Window* window, bool vsync) noexcept {
  if (g_context) return g_context->window == window;
  try {
    auto thread = std::make_unique<RenderContext>();
    thread->window = window;
    thread->id = std::this_thread::get_id();
    g_context = thread.get();
    thread->renderer = std::make_unique<Renderer>(vsync);
    g_caps = {"Sony", "SGX543 native GXM", "libgxm", 4096, 16, 16384, 1};
    thread.release();
    return true;
  } catch (const std::exception& error) {
    g_context = nullptr;
    std::printf("Aurora GXM game-thread initialization: %s\n", error.what());
    return false;
  }
}
void shutdown() noexcept {
  if (!g_context) return;
  if (!on_render_thread()) fail("GXM shutdown called outside the game thread");
  auto* thread = g_context;
  g_nativeAssets.store(nullptr, std::memory_order_release);
  thread->renderer.reset();
  g_context = nullptr;
  delete thread;
}
bool initialized() noexcept { return g_context != nullptr; }
bool on_render_thread() noexcept { return g_context && g_context->id == std::this_thread::get_id(); }
const Caps& caps() noexcept { return g_caps; }
void post(Task task) noexcept {
  if (!g_context) return;
  if (!on_render_thread()) fail("GXM task called outside the game thread");
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
uint32_t native_frame_slot() noexcept { return guarded([] { return renderer().device.frame_slot(); }); }
void finish() noexcept { call([] { renderer().device.finish(); }); }
const FormatInfo& format_info(Format format) noexcept { return FormatInfos[size_t(format)]; }
Texture::Texture(uint32_t width_, uint32_t height_, uint32_t mips_, Format format_, std::string_view label)
    noexcept : width(width_), height(height_), mips(mips_), format(format_), m_name(new Name) {
  gfx::port_capture::texture(this, true, width, height, mips, uint32_t(format), format_info(format).bytesPerPixel, label);
  auto create = [name = m_name, width_, height_, mips_, format_] {
    auto& r = renderer();
    auto storage = std::make_unique<TextureStorage>(r.pool, width_, height_, mips_, format_);
    const auto id = r.name();
    r.textures.emplace(id, TextureEntry{std::move(storage)});
    name->value = id;
  };
  if (on_render_thread()) guarded(create);
  else post(std::move(create));
}
Texture::~Texture() {
  gfx::port_capture::texture(this, false, width, height, mips, uint32_t(format), format_info(format).bytesPerPixel);
  if (!initialized()) { delete m_name; return; }
  post([name = m_name] { renderer().forget_texture(name->value); delete name; });
}
TexturePtr create_texture(uint32_t width, uint32_t height, uint32_t mips, Format format, std::string_view label) noexcept {
  return std::make_shared<Texture>(std::max(width, 1u), std::max(height, 1u), std::max(mips, 1u), format, label);
}
void open_native_texture_pack(const char* path) noexcept {
  call([path = std::string(path)] {
    auto& assets = renderer().assets;
    assets.open(path.c_str());
    g_nativeAssets.store(&assets, std::memory_order_release);
  });
}
NativeTextureInfo native_texture_info(uint32_t asset) noexcept {
  return guarded([&] {
    const auto* assets = g_nativeAssets.load(std::memory_order_acquire);
    if (!assets) throw std::runtime_error("Native texture pack is not open");
    return assets->info(asset);
  });
}
TexturePtr native_texture_asset(uint32_t asset) noexcept {
  const auto info = native_texture_info(asset);
  auto texture = create_texture(info.width, info.height, info.mips, info.format, "Offline native texture");
  post([texture, asset] {
    auto& r = renderer();
    r.assets.load(asset, r.texture(texture->gl()));
  });
  return texture;
}
void upload_texture(const Texture& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                    const void* data, uint32_t bytesPerRow) noexcept {
  gfx::port_capture::upload(&texture, mip, uint64_t(bytesPerRow) * height);
  guarded([&] { renderer().texture(texture.gl()).upload(mip, x, y, width, height, data, bytesPerRow); });
}
void write_texture(const TexturePtr& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t width,
                   uint32_t height, std::vector<uint8_t>&& data, uint32_t bytesPerRow) noexcept {
  if (!texture || !width || !height) return;
  if (uint64_t(bytesPerRow) * height > data.size()) fail("Texture upload source is too short");
  post([texture, mip, x, y, width, height, data = std::move(data), bytesPerRow] {
    upload_texture(*texture, mip, x, y, width, height, data.data(), bytesPerRow);
  });
}
void write_texture(const TexturePtr& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t width,
                   uint32_t height, const void* data, uint32_t bytesPerRow) noexcept {
  if (!texture || !width || !height) return;
  if (!data) fail("Null texture upload source");
  const auto* bytes = static_cast<const uint8_t*>(data);
  write_texture(texture, mip, x, y, width, height,
                std::vector<uint8_t>(bytes, bytes + size_t(bytesPerRow) * height), bytesPerRow);
}
GLuint sampler(const SamplerDesc& desc) noexcept {
  return guarded([&] {
    auto& r = renderer();
    for (const auto& [id, existing] : r.samplers) if (desc == existing) return id;
    const auto id = r.name(); r.samplers.emplace(id, desc); return id;
  });
}
GLuint framebuffer(GLuint color, GLuint depth) noexcept {
  return guarded([&] {
    if (!color && !depth) return GLuint(0);
    auto& r = renderer();
    auto [it, inserted] = r.framebuffers.try_emplace({color, depth}, 0);
    if (inserted) it->second = r.name();
    return it->second;
  });
}
void bind_framebuffer(GLuint color, GLuint depth) noexcept { guarded([&] { renderer().bind_target(color, depth); }); }
Program::Program(ProgramDesc desc) noexcept : m_desc(std::move(desc)) {
  guarded([&] {
    m_usesAlphaReferences = parameterize_alpha_references(m_desc.fragment, m_alphaReferences);
    gfx::port_capture::shader(m_desc.label, m_desc.vertex, m_desc.fragment, m_desc.origin);
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
  if (m_program && initialized()) post([id = m_program] {
    auto& r = renderer();
    r.forget_program(id);
  });
}
GLuint Program::get() noexcept {
  return guarded([&] {
    if (m_program) return m_program;
    auto& r = renderer();
    register_runtime_shader_sources(m_vertexHash, m_fragmentHash, m_desc.vertex,
        m_desc.fragment, m_desc.origin.empty() ? m_desc.label : m_desc.origin + "; " + m_desc.label);
    const auto* binary = fragment_binary(m_fragmentHash);
    m_usesDiscard = sceGxmProgramIsDiscardUsed(binary);
    m_replacesDepth = sceGxmProgramIsDepthReplaceUsed(binary);
    NativeProgram native;
    native.nativeMaterialVertices=m_desc.nativeMaterialVertices;
    native.nativeMeshLayout=m_desc.nativeMeshLayout;
    auto& shared = r.patchedPrograms[{m_vertexHash, m_fragmentHash}];
    // Compact layouts own their patched program. Shader hashes alone cannot
    // distinguish vertex strides/offsets. The frontend caches each full layout.
    if (!m_desc.nativeMeshLayout.stride) native.program = shared.lock();
    if (!native.program) {
      if(m_desc.nativeMeshLayout.stride) {
        gfx::validate_mesh_layout(m_desc.nativeMeshLayout);
        std::vector<VertexInput> inputs;
        for (const auto& attribute : m_desc.nativeMeshLayout.attributes) {
          SceGxmAttributeFormat format=SCE_GXM_ATTRIBUTE_FORMAT_F32;
          switch(attribute.format) {
          case MeshAttributeFormat::F32: break;
          case MeshAttributeFormat::F16: format=SCE_GXM_ATTRIBUTE_FORMAT_F16; break;
          case MeshAttributeFormat::U8N: format=SCE_GXM_ATTRIBUTE_FORMAT_U8N; break;
          case MeshAttributeFormat::S16N: format=SCE_GXM_ATTRIBUTE_FORMAT_S16N; break;
          }
          inputs.push_back({gfx::MeshAttributeNames[attribute.location],format,
                            attribute.components,attribute.offset,m_desc.nativeMeshLayout.stride,0});
        }
        native.program=std::make_shared<PatchedProgram>(r.device,m_vertexHash,m_fragmentHash,SCE_GXM_INDEX_FORMAT_U16,inputs);
      } else if(m_desc.nativeMaterialVertices) {
        constexpr uint16_t stride=sizeof(AuroraNativeMaterialVertex);
        const VertexInput inputs[]{
          {"a_native_position",SCE_GXM_ATTRIBUTE_FORMAT_F32,4,0,stride,0},
          {"a_native_view_position",SCE_GXM_ATTRIBUTE_FORMAT_F32,3,16,stride,0},
          {"a_native_normal",SCE_GXM_ATTRIBUTE_FORMAT_F32,3,28,stride,0},
          {"a_native_color0",SCE_GXM_ATTRIBUTE_FORMAT_U8N,4,136,stride,0},
          {"a_native_color1",SCE_GXM_ATTRIBUTE_FORMAT_U8N,4,140,stride,0},
          {"a_native_tex0",SCE_GXM_ATTRIBUTE_FORMAT_F32,3,40,stride,0},
          {"a_native_tex1",SCE_GXM_ATTRIBUTE_FORMAT_F32,3,52,stride,0},
          {"a_native_tex2",SCE_GXM_ATTRIBUTE_FORMAT_F32,3,64,stride,0},
          {"a_native_tex3",SCE_GXM_ATTRIBUTE_FORMAT_F32,3,76,stride,0},
          {"a_native_tex4",SCE_GXM_ATTRIBUTE_FORMAT_F32,3,88,stride,0},
          {"a_native_tex5",SCE_GXM_ATTRIBUTE_FORMAT_F32,3,100,stride,0},
          {"a_native_tex6",SCE_GXM_ATTRIBUTE_FORMAT_F32,3,112,stride,0},
          {"a_native_tex7",SCE_GXM_ATTRIBUTE_FORMAT_F32,3,124,stride,0}};
        static_assert(stride==144 && offsetof(AuroraNativeMaterialVertex,color)==136);
        native.program=std::make_shared<PatchedProgram>(r.device,m_vertexHash,m_fragmentHash,SCE_GXM_INDEX_FORMAT_U16,inputs);
      } else native.program = std::make_shared<PatchedProgram>(r.device, m_vertexHash, m_fragmentHash);
      if (!m_desc.nativeMeshLayout.stride) shared = native.program;
    }
    if (m_usesAlphaReferences) native.uniforms.emplace("aur_alpha_refs", m_alphaReferences);
    for (const auto& [name, unit] : m_desc.samplers) {
      const auto* parameter = sceGxmProgramFindParameterByName(fragment_binary(m_fragmentHash), name.c_str());
      if (parameter && sceGxmProgramParameterGetCategory(parameter) == SCE_GXM_PARAMETER_CATEGORY_SAMPLER) {
        if (unit < 0 || unit >= 16 || sceGxmProgramParameterGetResourceIndex(parameter) != unsigned(unit))
          throw std::runtime_error("Offline native sampler unit differs from the frontend");
        native.samplers.emplace_back(name, unit);
      }
    }
    for (const auto& [name, binding] : m_desc.blocks) {
      if (binding != 0) throw std::runtime_error("Native shader uniform block uses an unsupported binding");
      native.buffer0 = true;
    }
    const auto id = r.name();
    r.programs.emplace(id, std::move(native));
    m_desc.vertex.clear(); m_desc.fragment.clear();
    m_program = id;
    return id;
  });
}
void apply_raster_state(const RasterState& value) noexcept { guarded([&] { renderer().state.raster = value; }); }
void set_viewport(int32_t x, int32_t y, int32_t width, int32_t height, float minDepth, float maxDepth) noexcept {
  guarded([&] { auto& s = renderer().state; s.viewport = {x, y, width, height}; s.depthRange = {minDepth, maxDepth}; });
}
void set_scissor(int32_t x, int32_t y, int32_t width, int32_t height) noexcept {
  guarded([&] { auto& s = renderer().state; const std::array value{x, y, width, height};
    if (s.scissor != value) { s.scissor = value; s.clipDirty = s.scissorEnabled; } });
}
void set_scissor_enabled(bool enabled) noexcept {
  guarded([&] { auto& s = renderer().state; if (s.scissorEnabled != enabled) { s.scissorEnabled = enabled; s.clipDirty = true; } });
}
void set_blend_color(float r, float g, float b, float a) noexcept { guarded([&] { renderer().state.blendColor = {r, g, b, a}; }); }
void use_program(GLuint id) noexcept { guarded([&] { renderer().state.program = id; }); }
void bind_texture(uint32_t unit, GLuint texture, GLuint samplerId) noexcept {
  guarded([&] { renderer().texture_binding(unit, texture, samplerId); });
}
void bind_uniform_range(uint32_t binding, GLuint, uint32_t offset, uint32_t bytes) noexcept {
  guarded([&] { if (binding != 0) throw std::runtime_error("Unsupported native uniform binding");
    auto& s = renderer().state; s.uniformOffset = offset; s.uniformBytes = bytes; s.uniformValid = true; });
}
void set_color_mask(bool r, bool g, bool b, bool a) noexcept { guarded([&] { renderer().state.raster.colorMask = {r, g, b, a}; }); }
void set_depth_write(bool enabled) noexcept { guarded([&] { renderer().state.raster.depthWrite = enabled; }); }
void invalidate_state() noexcept {
  guarded([] { auto& r = renderer(); r.device.end(); r.reset_texture_bindings();
    r.state.program = 0; r.state.uniformValid = false; r.state.clipDirty = true; });
}
void bind_draw_vao(GLuint) noexcept { /* Active mapped indices belong to DrawMemory. */ }
void draw_fullscreen(GLuint id) noexcept {
  if (!aurora::native_frame_drawing_allowed()) return;
  guarded([&] { auto& r = renderer(); r.state.program = id; r.prepare(id); r.utility_draw(3); });
}
OffscreenState save_offscreen_state() noexcept {
  return guarded([]{const auto &s=renderer().state;OffscreenState out;out.raster=s.raster;out.rasterValid=true;out.viewport=s.viewport;out.depthRange=s.depthRange;out.scissor=s.scissor;out.scissorEnabled=s.scissorEnabled;out.blendColor=s.blendColor;out.color=s.color;out.depth=s.depth;out.program=s.program;
    for(unsigned i=0;i<s.textures.size();++i){out.textures[i]=s.textures[i].texture;out.samplers[i]=s.textures[i].sampler;}
    out.uniforms[0]={0,s.uniformOffset,s.uniformBytes};out.uniformValid=s.uniformValid;return out;});
}
void restore_offscreen_state(const OffscreenState& saved) noexcept {
  guarded([&]{auto &r=renderer();r.bind_target(saved.color,saved.depth);r.state.raster=saved.raster;r.state.viewport=saved.viewport;r.state.depthRange=saved.depthRange;r.state.scissor=saved.scissor;r.state.scissorEnabled=saved.scissorEnabled;r.state.blendColor=saved.blendColor;r.state.program=saved.program;
    for(unsigned i=0;i<saved.textures.size();++i)r.texture_binding(i,saved.textures[i],saved.samplers[i]);
    r.state.uniformOffset=saved.uniforms[0].offset;r.state.uniformBytes=saved.uniforms[0].size;r.state.uniformValid=saved.uniformValid;r.state.clipDirty=true;});
}
GLuint create_buffer() noexcept { return guarded([] { return renderer().name(); }); }
void destroy_buffer(GLuint) noexcept { /* Buffer storage belongs to the mapped frame slot. */ }
void write_buffer(GLuint, bool, const void*, size_t bytes) noexcept { if (bytes) fail("Native draws must bind their mapped payload directly"); }
void copy_texture_region(GLuint source, GLuint dest, int32_t srcX, int32_t srcY,
                         int32_t dstX, int32_t dstY, int32_t width, int32_t height) noexcept {
  guarded([&] {
    if (width <= 0 || height <= 0) return;
    auto& r = renderer(); auto& src = r.texture(source); auto& dst = r.texture(dest);
    if (srcX < 0 || srcY < 0 || dstX < 0 || dstY < 0 ||
        uint64_t(srcX) + width > src.width || uint64_t(srcY) + height > src.height ||
        uint64_t(dstX) + width > dst.width || uint64_t(dstY) + height > dst.height || src.format != dst.format)
      throw std::runtime_error("Native texture copy has incompatible formats or exceeds bounds");
    SceGxmTransferFormat format;
    switch (src.format) {
    case Format::R8: format = SCE_GXM_TRANSFER_FORMAT_U8_R; break;
    case Format::RG8: format = SCE_GXM_TRANSFER_FORMAT_RAW16; break;
    default: format = SCE_GXM_TRANSFER_FORMAT_RAW32; break;
    }
    r.device.finish();
    check(sceGxmTransferCopy(width, height, 0, 0, SCE_GXM_TRANSFER_COLORKEY_NONE,
        format, SCE_GXM_TRANSFER_LINEAR, src.pixels(), srcX, srcY, src.row_bytes(),
        format, SCE_GXM_TRANSFER_LINEAR, dst.pixels(), dstX, dstY, dst.row_bytes(), nullptr, 0, nullptr), "copy texture region");
    check(sceGxmTransferFinish(), "finish texture region copy");
  });
}
void clear_color(const float* color) noexcept {
  if (aurora::native_frame_drawing_allowed()) guarded([&] { renderer().clear(color, 0.f, false); });
}
void clear_depth(float depth) noexcept {
  if (aurora::native_frame_drawing_allowed()) guarded([&] { const float ignored[4]{}; renderer().clear(ignored, depth, true); });
}
void clear_depth_full_target(float depth) noexcept {
  if (!aurora::native_frame_drawing_allowed()) return;
  guarded([&] {
    auto& r=renderer();
    const bool depthWrite=r.state.raster.depthWrite,scissor=r.state.scissorEnabled;
    r.state.raster.depthWrite=true;
    if(scissor) {r.state.scissorEnabled=false;r.state.clipDirty=true;}
    const float ignored[4]{};
    r.clear(ignored,depth,true);
    r.state.raster.depthWrite=depthWrite;
    if(scissor) {r.state.scissorEnabled=true;r.state.clipDirty=true;}
  });
}
void draw_indexed(uint32_t indexCount, uint32_t indexOffset, uint32_t instances) noexcept {
  if (!aurora::native_frame_drawing_allowed()) return;
  guarded([&] { if (!indexCount || !instances) return; auto& r = renderer(); r.prepare(r.state.program); r.draws.draw(indexCount, indexOffset, instances); });
}
void draw_native_geometry(const AuroraNativeGeometry& geometry, bool textured) noexcept {
  if (!aurora::native_frame_drawing_allowed() || !geometry.index_count) return;
  guarded([&] {
    auto& r = renderer();
    auto& program = r.nativeGeometryPrograms[textured];
    if (!program) {
      static_assert(sizeof(AuroraNativeVertex) == 28);
      const VertexInput inputs[]{
        {"position_in", SCE_GXM_ATTRIBUTE_FORMAT_F32, 4, 0, 28, 0},
        {"uv_in", SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, 16, 28, 0},
        {"color_in", SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4, 24, 28, 0}};
      program = std::make_unique<PatchedProgram>(r.device, 7, textured ? 8 : 7,
                                                SCE_GXM_INDEX_FORMAT_U16, inputs);
    }
    r.scene();
    r.viewport();
    apply_raster(r.state.raster, r.device.context());
    auto blend = blend_info(r.state.raster, r.state.blendColor);
    if (!r.state.display && !r.state.color) blend.colorMask = SCE_GXM_COLOR_MASK_NONE;
    program->bind(blend, r.output_format());
    if (textured) {
      const auto& binding = r.state.textures[0];
      if (!binding.texture) throw std::runtime_error("Native geometry has no texture binding");
      auto& texture = r.texture(binding.texture);
      texture.note_gpu_use();
      const auto sampled = sampling_view(texture, binding.sampler ? r.samplers.at(binding.sampler) : NearestClamp);
      check(sceGxmSetFragmentTexture(r.device.context(), 0, &sampled), "bind native geometry texture");
    }
    check(sceGxmSetVertexStream(r.device.context(), 0, geometry.vertices), "bind native vertex attributes");
    check(sceGxmDraw(r.device.context(), SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16,
                    geometry.indices, geometry.index_count), "draw native triangles");
  });
}
void draw_native_material_geometry(const AuroraNativeMaterialGeometry& geometry,GLuint program,
                                  const AuroraNativeUniforms& uniforms) noexcept {
  if(!aurora::native_frame_drawing_allowed() || !geometry.index_count) return;
  guarded([&] {
    auto& r=renderer();
    if(!r.programs.at(program).nativeMaterialVertices)
      throw std::runtime_error("Native material draw has a different vertex layout");
    r.state.uniformOffset=uniforms.offset;r.state.uniformBytes=uniforms.size;r.state.uniformValid=true;
    r.prepare(program);
    check(sceGxmSetVertexStream(r.device.context(),0,geometry.vertices),"bind native material attributes");
    check(sceGxmDraw(r.device.context(),SCE_GXM_PRIMITIVE_TRIANGLES,SCE_GXM_INDEX_FORMAT_U16,
                    geometry.indices,geometry.index_count),"draw native material triangles");
  });
}
void draw_native_mesh(const NativeMeshBuffer& buffer, const MeshLayout& layout,
                      uint32_t vertexOffset, uint32_t vertexCount,
                      uint32_t indexOffset, uint32_t indexCount, GLuint program,
                      const AuroraNativeUniforms& uniforms) {
  if (!aurora::native_frame_drawing_allowed() || !indexCount) return;
  gfx::validate_mesh_draw(buffer,layout,vertexOffset,vertexCount,indexOffset,indexCount);
  auto& r=renderer();
  if (!(r.programs.at(program).nativeMeshLayout==layout))
    throw std::runtime_error("Resident mesh differs from patched vertex layout");
  const auto found=r.meshes.find(buffer.get());
  if(found==r.meshes.end() || found->second.data()!=buffer.data())
    throw std::runtime_error("Resident mesh backing was retired");
  if (!uniforms.data || !uniforms.size) throw std::runtime_error("Native mesh has no uniforms");
  r.state.uniformOffset=uniforms.offset;r.state.uniformBytes=uniforms.size;r.state.uniformValid=true;
  r.prepare(program);
  check(sceGxmSetVertexStream(r.device.context(),0,buffer.data()+vertexOffset),"bind resident mesh attributes");
  check(sceGxmDraw(r.device.context(),SCE_GXM_PRIMITIVE_TRIANGLES,SCE_GXM_INDEX_FORMAT_U16,
                  buffer.data()+indexOffset,indexCount),"draw resident mesh triangles");
}
void uniform4(GLuint id, const char* name, float x, float y, float z, float w) noexcept {
  guarded([&] { renderer().programs.at(id).uniforms[name] = {x, y, z, w}; });
}
void present(const TexturePtr& source, int32_t x, int32_t y, int32_t width, int32_t height) noexcept {
  if (!aurora::native_frame_drawing_allowed()) return;
  guarded([&] {
    auto& r = renderer(); r.bind_target(0, 0); r.reset_texture_bindings();
    r.state.scissorEnabled = false; r.state.clipDirty = false;
    r.state.raster.colorMask = {true, true, true, true};
    const float black[4]{0.f, 0.f, 0.f, 1.f};
    r.clear(black, 0.f, false);
    if (source && width > 0 && height > 0) {
      r.pin(source->gl(), true); r.pool.touch(r.texture(source->gl()));
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
      r.pin(source->gl(), false);
    }
    r.device.flip();
    r.collect();
  });
}
std::pair<uint32_t, uint32_t> drawable_size() noexcept { return {Device::Width, Device::Height}; }
void read_pixels(GLuint id, uint32_t x, uint32_t y, uint32_t width, uint32_t height, void* out) noexcept {
  guarded([&] {
    auto& r = renderer(); auto& texture = r.texture(id);
    if (texture.format != Format::RGBA8 || uint64_t(x) + width > texture.width || uint64_t(y) + height > texture.height || !out)
      throw std::runtime_error("Native RGBA readback exceeds bounds or uses another format");
    r.device.finish();
    const auto* input = static_cast<const char*>(texture.pixels()) + size_t(y) * texture.row_bytes() + x * 4;
    auto* output = static_cast<char*>(out);
    for (unsigned row = 0; row < height; ++row) {
      std::memcpy(output, input, size_t(width) * 4);
      input += texture.row_bytes(); output += size_t(width) * 4;
    }
  });
}
}
