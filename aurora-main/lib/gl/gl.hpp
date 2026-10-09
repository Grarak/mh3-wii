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

TexturePtr create_texture(uint32_t width, uint32_t height, uint32_t mips, Format format,
                          std::string_view label) noexcept;
#ifdef AURORA_GXM
struct NativeTextureInfo {
  Format format;
  uint32_t width, height, mips, bytes;
  bool arbitraryMips;
};
void open_native_texture_pack(const char* path) noexcept;
NativeTextureInfo native_texture_info(uint32_t asset) noexcept;
// Reads an immutable offline payload directly into a fresh CDRAM allocation.
// The queued read precedes every draw using the returned texture.
TexturePtr native_texture_asset(uint32_t asset) noexcept;
#endif
// Uploads `data` (rows of `bytesPerRow`, tightly following each other) into
// the given region. The bytes are copied before returning.
void write_texture(const TexturePtr& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t width,
                   uint32_t height, const void* data, uint32_t bytesPerRow) noexcept;
void write_texture(const TexturePtr& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t width,
                   uint32_t height, std::vector<uint8_t>&& data, uint32_t bytesPerRow) noexcept;

// Render thread only: upload immediately.
void upload_texture(const Texture& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                    const void* data, uint32_t bytesPerRow) noexcept;

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
    return wrapS == rhs.wrapS && wrapT == rhs.wrapT && minFilter == rhs.minFilter && magFilter == rhs.magFilter &&
           minLod == rhs.minLod && maxLod == rhs.maxLod && maxAnisotropy == rhs.maxAnisotropy && lodBias == rhs.lodBias;
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

struct ProgramDesc {
  std::string label;
  std::string vertex;
  std::string fragment;
  // Sampler uniform name -> texture unit.
  std::vector<std::pair<std::string, int>> samplers;
  // Uniform block name -> binding point.
  std::vector<std::pair<std::string, int>> blocks;
  // Direct interleaved AuroraNativeMaterialVertex attributes, already projected
  // and with texture coordinates prepared by the native application producer.
  bool nativeMaterialVertices = false;
};

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
void copy_texture_region(GLuint source, GLuint dest, int32_t srcX, int32_t srcY,
                         int32_t dstX, int32_t dstY, int32_t width, int32_t height) noexcept;
void clear_color(const float* color) noexcept;
void clear_depth(float depth) noexcept;
void draw_indexed(uint32_t indexCount, uint32_t indexOffset, uint32_t instances) noexcept;
// Direct interleaved attributes and U16 triangles. GXM consumes the admitted
// CDRAM allocation directly; GLES uploads the host allocation into VBOs.
void draw_native_geometry(const AuroraNativeGeometry& geometry, bool textured) noexcept;
void draw_native_material_geometry(const AuroraNativeMaterialGeometry& geometry,GLuint program,
                                  const AuroraNativeUniforms& uniforms) noexcept;
void uniform4(GLuint program, const char* name, float x, float y, float z, float w) noexcept;

// Render thread only: blit `source` into the window with the letterboxed
// viewport and swap. Rows are stored top-down, so the blit flips.
void present(const TexturePtr& source, int32_t x, int32_t y, int32_t width, int32_t height) noexcept;
// Nonblocking GPU completion check used for frameskip, independent of CPU tasks.
bool gpu_frame_pending() noexcept;
// Slot 0..2 selected for the next rendered frame. Skipping does not rotate it.
uint32_t native_frame_slot() noexcept;
// Render thread only: window framebuffer size in pixels.
std::pair<uint32_t, uint32_t> drawable_size() noexcept;

// Render thread only: read RGBA8 pixels of `texture` (level 0).
void read_pixels(GLuint texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height, void* out) noexcept;

} // namespace aurora::gl
