#pragma once

// Renderer interface, implemented by GLES or native GXM. One render thread
// owns the context and executes tasks in posting order. Objects created on
// other threads carry their metadata immediately and receive their GPU name
// when the creation task runs, which always precedes any task using them.

#ifdef AURORA_GXM
#include "../gxm/state_values.hpp"
#else
#include <GLES3/gl3.h>
#endif

#include <absl/functional/any_invocable.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "../gfx/format.hpp"
#include <aurora/native_geometry.h>

struct SDL_Window;

namespace aurora::gl {

using Task = absl::AnyInvocable<void() &&>;

struct Caps {
  std::string vendor;
  std::string renderer;
  std::string version;
  int32_t maxTextureSize = 2048;
  int32_t uniformOffsetAlignment = 256;
  int32_t maxUniformBlockSize = 16384;
  int32_t maxAnisotropy = 1;
};

bool initialize(SDL_Window* window, bool vsync) noexcept;
void shutdown() noexcept;
bool initialized() noexcept;
const Caps& caps() noexcept;

// Queue `task` behind already posted work; runs inline on the render thread.
void post(Task task) noexcept;
// Post `task` and block until it has run. Runs inline on the render thread.
void call(Task task) noexcept;
// Wait until every task posted so far has run and the GPU has finished it.
void finish() noexcept;
bool on_render_thread() noexcept;
// Move context ownership between threads: the owner releases the context,
// then the next owner makes it current on itself. Never while a frame is open.
void release_context() noexcept;
void acquire_context() noexcept;

struct FormatInfo {
  GLenum internalFormat;
  GLenum format;
  GLenum type;
  uint8_t bytesPerPixel;
  bool integer;
  bool depth;
  bool filterable;
};
const FormatInfo& format_info(Format format) noexcept;

// A GL name shared between the owning object (any thread) and the render
// thread tasks that create, use and delete it.
struct Name {
  GLuint value = 0;
};

class Texture {
public:
  Texture(uint32_t width, uint32_t height, uint32_t mips, Format format, std::string_view label) noexcept;
  ~Texture();
  Texture(const Texture&) = delete;
  Texture& operator=(const Texture&) = delete;

  // Render thread only.
  [[nodiscard]] GLuint gl() const noexcept { return m_name->value; }

  const uint32_t width;
  const uint32_t height;
  const uint32_t mips;
  const Format format;

private:
  Name* m_name;
};
using TexturePtr = std::shared_ptr<Texture>;

TexturePtr create_texture(uint32_t width, uint32_t height, uint32_t mips, Format format, std::string_view label) noexcept;
// Owner only; never submits or waits. Reuse also requires exclusive shared
// ownership. GXM checks the texture's latest actual GPU use, not a frame slot;
// ordered host GL draws may overwrite after earlier queued consumers.
bool render_target_reusable(const Texture& texture) noexcept;
// Owner only; poll completed retirements and check one exact backing block.
// This query does not close a scene, submit work, or wait.
bool render_target_allocation_available(size_t bytes) noexcept;
struct NativeTextureInfo {
  Format format;
  uint32_t width, height, mips, bytes;
  bool arbitraryMips;
  uint32_t imageBytes = 0, paletteOffset = 0, paletteBytes = 0;
  int32_t sourcePaletteDelta = 0;
  uint32_t sourcePaletteEntries = 0, sourcePaletteFormat = 0, sourceFormat = 0;
  bool sourceProvenance = false;
};
// Adds a native texture pack; any thread. Packs hold disjoint asset ID ranges.
void open_native_texture_pack(const char* path) noexcept;
// Called for an asset no open pack holds; it opens the pack that holds it.
void set_native_texture_resolver(void (*resolver)(uint32_t asset)) noexcept;
NativeTextureInfo native_texture_info(uint32_t asset) noexcept;
// Reads an immutable offline payload directly into a fresh CDRAM allocation.
// The queued read precedes every draw using the returned texture.
TexturePtr native_texture_asset(uint32_t asset) noexcept;
// Uploads `data` (rows of `bytesPerRow`, tightly following each other) into
// the given region. The bytes are copied before returning.
void write_texture(const TexturePtr& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data, uint32_t bytesPerRow) noexcept;
void write_texture(const TexturePtr& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t width, uint32_t height, std::vector<uint8_t>&& data, uint32_t bytesPerRow) noexcept;

// Render thread only: upload immediately.
void upload_texture(const Texture& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data, uint32_t bytesPerRow) noexcept;

struct SamplerDesc {
  GLenum wrapS = GL_CLAMP_TO_EDGE;
  GLenum wrapT = GL_CLAMP_TO_EDGE;
  GLenum minFilter = GL_NEAREST;
  GLenum magFilter = GL_NEAREST;
  float minLod = 0.f;
  float maxLod = 1000.f;
  uint32_t maxAnisotropy = 1;
  float lodBias = 0.f;

  bool operator==(const SamplerDesc& rhs) const noexcept {
    return wrapS == rhs.wrapS && wrapT == rhs.wrapT && minFilter == rhs.minFilter && magFilter == rhs.magFilter && minLod == rhs.minLod && maxLod == rhs.maxLod && maxAnisotropy == rhs.maxAnisotropy &&
           lodBias == rhs.lodBias;
  }
};
// Render thread only: the cached sampler object for `desc`.
GLuint sampler(const SamplerDesc& desc) noexcept;
inline constexpr SamplerDesc NearestClamp{};
inline constexpr SamplerDesc LinearClamp{
    .minFilter = GL_LINEAR,
    .magFilter = GL_LINEAR,
};

// Render thread only: a framebuffer with the given attachments (0 = none).
GLuint framebuffer(GLuint color, GLuint depth) noexcept;
void bind_framebuffer(GLuint color, GLuint depth) noexcept;

// Immutable compact mesh input contract. Locations and names are shared by
// the generated model vertex shader and both hardware backends.
enum class MeshAttributeFormat : uint8_t { F32, F16, U8N, S16N, U8 };
struct MeshAttribute {
  uint16_t offset = 0;
  uint8_t location = 0, components = 0;
  MeshAttributeFormat format = MeshAttributeFormat::F32;
  bool operator==(const MeshAttribute&) const = default;
};
struct MeshLayout {
  uint16_t stride = 0;
  std::vector<MeshAttribute> attributes;
  // PN bit0, texture matrix selectors0..7 bits1..8. A missing bit uses the
  // inherited uniform matrix even when another lane shares its packed input.
  uint16_t matrixIndexMask = 0;
  bool operator==(const MeshLayout&) const = default;
};

struct ProgramDesc {
  std::string label;
  // Development shader capture/cache provenance; never part of GPU state.
  std::string origin;
  std::string vertex;
  std::string fragment;
  // Host capture can retain GLES pixels/hashes while compiling the counterpart
  // selected by the authoritative admitted GXM sampler policy.
  std::string portableVertex, portableFragment, portableOrigin;
  // Sampler uniform name -> texture unit.
  std::vector<std::pair<std::string, int>> samplers;
  // Uniform block name -> binding point.
  std::vector<std::pair<std::string, int>> blocks;
  // Direct interleaved AuroraNativeMaterialVertex attributes, already projected
  // and with texture coordinates prepared by the native application producer.
  bool nativeMaterialVertices = false;
  MeshLayout nativeMeshLayout;
  // GXM: vertex/fragment are hand-written Cg for draw_shaded_* (no GLSL
  // conversion); blocks name BUFFER indices, vertex attributes bind per draw.
  bool cg = false;
  // Fixed fragment computes original discard first, then outputs A=1. GXM
  // may expand RGB-only writes only while its target is certified A=1.
  bool preservesTargetAlphaOne = false;
};

// Render owner only. GLES returns false. No submission, waits or pixel reads.
bool shaded_target_alpha_one() noexcept;

// One immutable converted-pack payload. Vita reads directly into CDRAM; GLES
// uploads once and retains a 16-byte-aligned CPU view for common pack validation.
// Creation/destruction belong to the context owner. Retiring a live GPU buffer
// never waits; its backing allocation survives all preceding submissions.
class NativeMeshBuffer {
public:
  NativeMeshBuffer(const char* path, uint64_t offset, uint32_t bytes);
  ~NativeMeshBuffer();
  NativeMeshBuffer(const NativeMeshBuffer&) = delete;
  NativeMeshBuffer& operator=(const NativeMeshBuffer&) = delete;
  GLuint get() const noexcept { return m_buffer; }
  const uint8_t* data() const noexcept { return m_data; }
  uint32_t size() const noexcept { return m_bytes; }

private:
  GLuint m_buffer = 0;
  const uint8_t* m_data = nullptr;
  uint32_t m_bytes = 0;
};
// Context-owner queries. Retirement polls completion and never waits; backing
// pending the GPU notification remains part of the physical model budget.
size_t native_mesh_allocation_bytes(uint32_t bytes);
size_t native_mesh_resident_bytes();

class Program {
public:
  explicit Program(ProgramDesc desc) noexcept;
  ~Program();
  Program(const Program&) = delete;
  Program& operator=(const Program&) = delete;

  // Render thread only. GLES compiles on first use; GXM patches embedded GXP.
  GLuint get() noexcept;
  [[nodiscard]] const std::string& label() const noexcept { return m_desc.label; }
  [[nodiscard]] uint64_t vertex_hash() const noexcept { return m_vertexHash; }
  [[nodiscard]] uint64_t fragment_hash() const noexcept { return m_fragmentHash; }
  [[nodiscard]] bool uses_discard() const noexcept { return m_usesDiscard; }
  [[nodiscard]] bool replaces_depth() const noexcept { return m_replacesDepth; }

private:
  ProgramDesc m_desc;
  uint64_t m_vertexHash = 0, m_fragmentHash = 0;
  std::array<uint64_t, 2> m_capturePortableHashes{};
  std::string m_capturePortableOrigin;
  std::array<float, 4> m_alphaReferences{};
  bool m_usesAlphaReferences = false;
  bool m_usesDiscard = false, m_replacesDepth = false;
  GLuint m_program = 0;
  bool m_failed = false;
};
using ProgramPtr = std::shared_ptr<Program>;

// The shared GLSL ES prelude: version, precision, WGSL-style aliases and helpers.
std::string_view glsl_prelude() noexcept;
std::string vertex_source(std::string_view body);
std::string fragment_source(std::string_view body);

// Fixed-function state for one draw. Compared field by field on the render thread.
struct RasterState {
  bool blend = false;
  GLenum colorEquation = GL_FUNC_ADD;
  GLenum alphaEquation = GL_FUNC_ADD;
  GLenum srcColor = GL_ONE;
  GLenum dstColor = GL_ZERO;
  GLenum srcAlpha = GL_ONE;
  GLenum dstAlpha = GL_ZERO;
  bool depthTest = false;
  bool depthWrite = false;
  GLenum depthFunc = GL_ALWAYS;
  bool cull = false;
  GLenum cullFace = GL_BACK;
  std::array<bool, 4> colorMask{true, true, true, true};
};

// Graphics-owner snapshot for isolated native offscreen work. These values are
// backend state, not framebuffer pixels. Restore includes generic draw inputs.
struct OffscreenState {
  RasterState raster;
  std::array<int32_t, 4> viewport{}, scissor{};
  std::array<float, 2> depthRange{};
  std::array<float, 4> blendColor{};
  std::array<GLuint, 16> textures{}, samplers{};
  struct UniformRange {
    GLuint buffer = 0;
    uint32_t offset = 0, size = 0;
  };
  std::array<UniformRange, 4> uniforms{};
  GLuint framebuffer = 0, color = 0, depth = 0, program = 0, vertexArray = 0, arrayBuffer = 0, activeTexture = 0;
  int scissorEnabled = 0;
  bool rasterValid = false, uniformValid = false;
};
OffscreenState save_offscreen_state() noexcept;
void restore_offscreen_state(const OffscreenState& state) noexcept;

// Render thread state cache.
void apply_raster_state(const RasterState& state) noexcept;
void set_viewport(int32_t x, int32_t y, int32_t width, int32_t height, float minDepth, float maxDepth) noexcept;
void set_scissor(int32_t x, int32_t y, int32_t width, int32_t height) noexcept;
void set_scissor_enabled(bool enabled) noexcept;
void set_blend_color(float r, float g, float b, float a) noexcept;
void use_program(GLuint program) noexcept;
void bind_texture(uint32_t unit, GLuint texture, GLuint sampler) noexcept;
void bind_uniform_range(uint32_t binding, GLuint buffer, uint32_t offset, uint32_t size) noexcept;
void set_color_mask(bool r, bool g, bool b, bool a) noexcept;
void set_depth_write(bool enabled) noexcept;
// Forget cached bindings, e.g. after code that touched GL state directly.
void invalidate_state() noexcept;
// Empty vertex array for attribute-less draws; binds its element buffer.
void bind_draw_vao(GLuint indexBuffer) noexcept;

// Draws a fullscreen triangle with `program`, sampling `source` on unit 0.
// The caller binds the target framebuffer and viewport.
void draw_fullscreen(GLuint program) noexcept;

// Backend operations used by the recorded GX frame executor. GPU handles
// are opaque: the GXM backend owns mapped buffers and native surfaces.
GLuint create_buffer() noexcept;
void destroy_buffer(GLuint buffer) noexcept;
void write_buffer(GLuint buffer, bool indices, const void* bytes, size_t size) noexcept;
void copy_texture_region(GLuint source, GLuint dest, int32_t srcX, int32_t srcY, int32_t dstX, int32_t dstY, int32_t width, int32_t height) noexcept;
void clear_color(const float* color) noexcept;
void clear_depth(float depth) noexcept;
// Full current-target depth clear, independent of the application's scissor
// and depth-write mask. Retains backend state and never switches render targets.
void clear_depth_full_target(float depth) noexcept;
void draw_indexed(uint32_t indexCount, uint32_t indexOffset, uint32_t instances) noexcept;
// Direct interleaved attributes and U16 triangles. GXM consumes the admitted
// CDRAM allocation directly; GLES uploads the host allocation into VBOs.
void draw_native_geometry(const AuroraNativeGeometry& geometry, bool textured) noexcept;
void draw_native_material_geometry(const AuroraNativeMaterialGeometry& geometry, GLuint program, const AuroraNativeUniforms& uniforms) noexcept;
// Immutable resident U16 triangle payload; only the small uniform block is
// uploaded per draw. Byte offsets are relative to this buffer's file range.
void draw_native_mesh(const NativeMeshBuffer& buffer, const MeshLayout& layout, uint32_t vertexOffset, uint32_t vertexCount, uint32_t indexOffset, uint32_t indexCount, GLuint program,
                      const AuroraNativeUniforms& uniforms);
void uniform4(GLuint program, const char* name, float x, float y, float z, float w) noexcept;

// Fixed native shading programs (runtime/src/native_shader_source.cpp). Blocks:
// Draw 0 (vertex), Scene 1, Palette 2 (mesh vertex), Shade 3 (fragment). Draw
// and Shade share one allocation; Shade starts at shadeOffset. Scene is
// re-uploaded only when its revision changes.
// One per-draw allocation: Draw at 0, Shade at shadeOffset and (mesh draws)
// Palette at paletteOffset (paletteBytes 0 = none); drawBytes covers all.
struct ShadedBlocks {
  const void* draw = nullptr;
  uint32_t drawBytes = 0, shadeOffset = 0, shadeBytes = 0;
  uint32_t paletteOffset = 0, paletteBytes = 0;
  const void* scene = nullptr;
  uint32_t sceneBytes = 0;
  uint64_t sceneRevision = 0;
};
// Shader input locations 0..9 (position, normal, color0, uv0..2, PN selector,
// role selectors 0..2) each read one compact mesh attribute by its layout
// location, or one byte lane of a U8 selector attribute; -1 leaves it unbound.
struct ShadedMeshInputs {
  std::array<int8_t, 10> location{};
  std::array<int8_t, 10> lane{};
  bool operator==(const ShadedMeshInputs&) const = default;
};
// u_texture0..2 read the native texture units (GX map IDs) given per draw.
void draw_shaded_mesh(const NativeMeshBuffer& buffer, const MeshLayout& layout, const ShadedMeshInputs& inputs, uint32_t vertexOffset, uint32_t vertexCount, uint32_t indexOffset, uint32_t indexCount,
                      GLuint program, const ShadedBlocks& blocks, const std::array<uint8_t, 3>& textureUnits);
// Application geometry: role r reads texcoord[coordinates[r]].
void draw_shaded_geometry(const AuroraNativeMaterialGeometry& geometry, const std::array<uint8_t, 3>& coordinates, GLuint program, const ShadedBlocks& blocks,
                          const std::array<uint8_t, 3>& textureUnits) noexcept;

// Render thread only: blit `source` into the window with the letterboxed
// viewport and swap. Rows are stored top-down, so the blit flips.
void present(const TexturePtr& source, int32_t x, int32_t y, int32_t width, int32_t height) noexcept;
// Nonblocking GPU completion check used for frameskip, independent of CPU tasks.
bool gpu_frame_pending() noexcept;
// Seal the current native scene and protect its mutable slot without a flip.
void discard_native_frame() noexcept;
// Slot 0..2 selected for the next rendered frame. Skipping does not rotate it.
uint32_t native_frame_slot() noexcept;
// Render thread only: window framebuffer size in pixels.
std::pair<uint32_t, uint32_t> drawable_size() noexcept;

// Render thread only: read RGBA8 pixels of `texture` (level 0).
void read_pixels(GLuint texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height, void* out) noexcept;

} // namespace aurora::gl
