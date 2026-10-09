#include "gl.hpp"

#include "../internal.hpp"
#include "../gfx/port_capture.hpp"
#include <aurora/gpu_completion.hpp>
#include <aurora/native_frame_gate.hpp>

#include <SDL3/SDL.h>
#include <xxhash.h>
#include <absl/container/flat_hash_map.h>

#include <algorithm>
#include <cstring>
#include <thread>

#include "tracy/Tracy.hpp"

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

namespace aurora::gl {
namespace {
Module Log("aurora::gl");

struct RenderContext {
  SDL_Window* window = nullptr;
  SDL_GLContext context = nullptr;
  std::thread::id threadId;
};

RenderContext* g_context = nullptr;
Caps g_caps;
std::array<GLsync, aurora::GpuFrameRing::BufferCount> g_frameFences{};
unsigned g_frameSlot = 0;

// ---- Context owner state ----

struct State {
  RasterState raster;
  bool rasterValid = false;
  std::array<int32_t, 4> viewport{-1, -1, -1, -1};
  std::array<float, 2> depthRange{-1.f, -1.f};
  std::array<int32_t, 4> scissor{-1, -1, -1, -1};
  int scissorEnabled = -1;
  std::array<float, 4> blendColor{-1.f, -1.f, -1.f, -1.f};
  GLuint program = UINT32_MAX;
  std::array<GLuint, 16> textures{};
  std::array<GLuint, 16> samplers{};
  uint32_t activeUnit = UINT32_MAX;
  struct UniformRange {
    GLuint buffer = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
  };
  std::array<UniformRange, 4> uniforms{};
  GLuint drawFramebuffer = UINT32_MAX;
  GLuint vao = 0;
  GLuint vaoElementBuffer = UINT32_MAX;
  GLuint emptyVao = 0;
};
State g_state;
ProgramPtr g_nativePrograms[2];
GLuint g_nativeVao = 0, g_nativeVertexBuffer = 0, g_nativeIndexBuffer = 0;
GLuint g_materialVao=0,g_materialVertexBuffer=0,g_materialIndexBuffer=0,g_materialUniformBuffer=0;

struct FramebufferKey {
  GLuint color;
  GLuint depth;
  bool operator==(const FramebufferKey&) const = default;
  template <typename H>
  friend H AbslHashValue(H h, const FramebufferKey& key) {
    return H::combine(std::move(h), key.color, key.depth);
  }
};
absl::flat_hash_map<FramebufferKey, GLuint> g_framebuffers;

struct SamplerKey {
  SamplerDesc desc;
  bool operator==(const SamplerKey& rhs) const { return desc == rhs.desc; }
  template <typename H>
  friend H AbslHashValue(H h, const SamplerKey& key) {
    const auto& d = key.desc;
    return H::combine(std::move(h), d.wrapS, d.wrapT, d.minFilter, d.magFilter, d.minLod, d.maxLod,
                      d.maxAnisotropy, d.lodBias);
  }
};
absl::flat_hash_map<SamplerKey, GLuint> g_samplers;

void forget_texture(GLuint texture) {
  for (auto it = g_framebuffers.begin(); it != g_framebuffers.end();) {
    if (it->first.color == texture || it->first.depth == texture) {
      if (g_state.drawFramebuffer == it->second) {
        g_state.drawFramebuffer = UINT32_MAX;
      }
      glDeleteFramebuffers(1, &it->second);
      g_framebuffers.erase(it++);
    } else {
      ++it;
    }
  }
  for (auto& bound : g_state.textures) {
    if (bound == texture) {
      bound = UINT32_MAX;
    }
  }
}

constexpr std::array<FormatInfo, 7> FormatInfos{{
    {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false, false, true},
    {GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, false, false, true},
    {GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2, false, false, true},
    {GL_R16I, GL_RED_INTEGER, GL_SHORT, 2, true, false, false},
    {GL_R32F, GL_RED, GL_FLOAT, 4, false, false, false},
    {GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 4, true, false, false},
    {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 4, false, true, false},
}};


GLuint compile_shader(GLenum stage, const std::string& source, const std::string& label) {
  GLuint shader = glCreateShader(stage);
  const char* src = source.c_str();
  glShaderSource(shader, 1, &src, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok != GL_TRUE) {
    GLint len = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
    std::string info(static_cast<size_t>(std::max(len, 1)), '\0');
    glGetShaderInfoLog(shader, len, nullptr, info.data());
    Log.error("Failed to compile {} shader '{}':\n{}\n--- source ---\n{}",
              stage == GL_VERTEX_SHADER ? "vertex" : "fragment", label, info, source);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}
} // namespace

// ---- Game-thread context ownership ----

bool initialize(SDL_Window* window, bool vsync) noexcept {
  if (g_context != nullptr) {
    return g_context->window == window;
  }
  SDL_GLContext context = SDL_GL_CreateContext(window);
  if (context == nullptr) {
    Log.error("Failed to create an OpenGL ES 3.0 context: {}", SDL_GetError());
    return false;
  }
  if (!SDL_GL_SetSwapInterval(vsync ? 1 : 0)) {
    Log.warn("Failed to set the swap interval: {}", SDL_GetError());
  }
  auto* rt = new RenderContext;
  rt->window = window;
  rt->context = context;
  rt->threadId = std::this_thread::get_id();
  g_context = rt;
  bool ok = false;
  call([&] {
    const auto string = [](GLenum name) {
      const auto* s = reinterpret_cast<const char*>(glGetString(name));
      return std::string(s != nullptr ? s : "");
    };
    g_caps.vendor = string(GL_VENDOR);
    g_caps.renderer = string(GL_RENDERER);
    g_caps.version = string(GL_VERSION);
    GLint major = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    ok = major >= 3;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g_caps.maxTextureSize);
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &g_caps.uniformOffsetAlignment);
    glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &g_caps.maxUniformBlockSize);
    const auto* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (extensions != nullptr && std::strstr(extensions, "GL_EXT_texture_filter_anisotropic") != nullptr) {
      GLfloat maxAniso = 1.f;
      glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAniso);
      g_caps.maxAnisotropy = static_cast<int32_t>(maxAniso);
    }
    glGetError();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGenVertexArrays(1, &g_state.emptyVao);
  });
  if (!ok) {
    Log.error("OpenGL ES 3.0 is required; the context reports {}", g_caps.version);
    shutdown();
    return false;
  }
  Log.info("OpenGL ES: {} | {} | {}", g_caps.vendor, g_caps.renderer, g_caps.version);
  return true;
}

void shutdown() noexcept {
  RenderContext* rt = g_context;
  if (rt == nullptr) {
    return;
  }
  if (!on_render_thread()) Log.fatal("GL shutdown called outside the game thread");
  post([] {
    g_nativePrograms[0].reset();
    g_nativePrograms[1].reset();
    if (g_nativeVao) glDeleteVertexArrays(1, &g_nativeVao);
    if (g_nativeVertexBuffer) glDeleteBuffers(1, &g_nativeVertexBuffer);
    if (g_nativeIndexBuffer) glDeleteBuffers(1, &g_nativeIndexBuffer);
    g_nativeVao = g_nativeVertexBuffer = g_nativeIndexBuffer = 0;
    if(g_materialVao) glDeleteVertexArrays(1,&g_materialVao);
    if(g_materialVertexBuffer) glDeleteBuffers(1,&g_materialVertexBuffer);
    if(g_materialIndexBuffer) glDeleteBuffers(1,&g_materialIndexBuffer);
    if(g_materialUniformBuffer) glDeleteBuffers(1,&g_materialUniformBuffer);
    g_materialVao=g_materialVertexBuffer=g_materialIndexBuffer=g_materialUniformBuffer=0;
    for (auto& [_, fbo] : g_framebuffers) {
      glDeleteFramebuffers(1, &fbo);
    }
    g_framebuffers.clear();
    for (auto& [_, sampler] : g_samplers) {
      glDeleteSamplers(1, &sampler);
    }
    g_samplers.clear();
    if (g_state.vao != 0) {
      glDeleteVertexArrays(1, &g_state.vao);
    }
    if (g_state.emptyVao != 0) {
      glDeleteVertexArrays(1, &g_state.emptyVao);
    }
    g_state = {};
  });
  // Teardown is the only unconditional finish in this ownership path.
  glFinish();
  for (auto& fence : g_frameFences) {
    if (fence) glDeleteSync(fence);
    fence = nullptr;
  }
  g_frameSlot = 0;
  SDL_GL_MakeCurrent(rt->window, nullptr);
  g_context = nullptr;
  SDL_GL_DestroyContext(rt->context);
  delete rt;
}

bool initialized() noexcept { return g_context != nullptr; }
const Caps& caps() noexcept { return g_caps; }

bool on_render_thread() noexcept { return g_context != nullptr && std::this_thread::get_id() == g_context->threadId; }

void post(Task task) noexcept {
  if (!g_context) return;
  if (!on_render_thread()) Log.fatal("GL task called outside the game thread");
  std::move(task)();
}

void call(Task task) noexcept {
  post(std::move(task));
}

bool gpu_frame_pending() noexcept {
  auto& fence = g_frameFences[g_frameSlot];
  if (!g_context || !fence) return false;
  if (!on_render_thread()) Log.fatal("GL GPU completion queried outside the game thread");
  const GLenum result = glClientWaitSync(fence, 0, 0);
  if (result == GL_TIMEOUT_EXPIRED) return true;
  if (result == GL_WAIT_FAILED) Log.fatal("GL frame completion query failed");
  glDeleteSync(fence);
  fence = nullptr;
  return false;
}
uint32_t native_frame_slot() noexcept { return g_frameSlot; }

void finish() noexcept {
  ZoneScoped;
  call([] { glFinish(); });
}

// ---- Formats and textures ----

const FormatInfo& format_info(Format format) noexcept { return FormatInfos[static_cast<size_t>(format)]; }

Texture::Texture(uint32_t width, uint32_t height, uint32_t mips, Format format, std::string_view label) noexcept
: width(width), height(height), mips(mips), format(format), m_name(new Name) {
  gfx::port_capture::texture(this, true, width, height, mips, uint32_t(format), format_info(format).bytesPerPixel, label);
  auto create = [name = m_name, width, height, mips, format] {
    const auto& info = format_info(format);
    glGenTextures(1, &name->value);
    glBindTexture(GL_TEXTURE_2D, name->value);
    glTexStorage2D(GL_TEXTURE_2D, static_cast<GLsizei>(std::max(mips, 1u)), info.internalFormat,
                   static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(std::max(mips, 1u) - 1));
    // Samplers override filtering; keep the texture complete without one.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g_state.activeUnit = UINT32_MAX;
    for (auto& bound : g_state.textures) {
      bound = UINT32_MAX;
    }
  };
  // Context-owner code uses a texture right after creating it.
  if (on_render_thread()) {
    create();
  } else {
    post(std::move(create));
  }
}

Texture::~Texture() {
  gfx::port_capture::texture(this, false, width, height, mips, uint32_t(format), format_info(format).bytesPerPixel);
  post([name = m_name] {
    if (name->value != 0) {
      forget_texture(name->value);
      glDeleteTextures(1, &name->value);
    }
    delete name;
  });
}

TexturePtr create_texture(uint32_t width, uint32_t height, uint32_t mips, Format format,
                          std::string_view label) noexcept {
  return std::make_shared<Texture>(std::max(width, 1u), std::max(height, 1u), std::max(mips, 1u), format, label);
}

void write_texture(const TexturePtr& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t width,
                   uint32_t height, std::vector<uint8_t>&& data, uint32_t bytesPerRow) noexcept {
  if (!texture || width == 0 || height == 0) {
    return;
  }
  post([texture, mip, x, y, width, height, data = std::move(data), bytesPerRow] {
    upload_texture(*texture, mip, x, y, width, height, data.data(), bytesPerRow);
  });
}

void upload_texture(const Texture& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                    const void* data, uint32_t bytesPerRow) noexcept {
  gfx::port_capture::upload(&texture, mip, uint64_t(bytesPerRow) * height);
  const auto& info = format_info(texture.format);
  if (g_state.activeUnit != 0) {
    glActiveTexture(GL_TEXTURE0);
    g_state.activeUnit = 0;
  }
  if (g_state.textures[0] != texture.gl()) {
    glBindTexture(GL_TEXTURE_2D, texture.gl());
    g_state.textures[0] = texture.gl();
  }
  const uint32_t pixelsPerRow = bytesPerRow / info.bytesPerPixel;
  glPixelStorei(GL_UNPACK_ROW_LENGTH, pixelsPerRow == width ? 0 : static_cast<GLint>(pixelsPerRow));
  glTexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(mip), static_cast<GLint>(x), static_cast<GLint>(y),
                  static_cast<GLsizei>(width), static_cast<GLsizei>(height), info.format, info.type, data);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void write_texture(const TexturePtr& texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t width,
                   uint32_t height, const void* data, uint32_t bytesPerRow) noexcept {
  const auto* bytes = static_cast<const uint8_t*>(data);
  write_texture(texture, mip, x, y, width, height,
                std::vector<uint8_t>(bytes, bytes + static_cast<size_t>(bytesPerRow) * height), bytesPerRow);
}

// ---- Samplers and framebuffers ----

GLuint sampler(const SamplerDesc& desc) noexcept {
  if (desc.lodBias != 0.f) Log.fatal("GLES sampler bias must be passed through the material shader");
  const auto [it, inserted] = g_samplers.try_emplace(SamplerKey{desc}, 0);
  if (inserted) {
    GLuint s = 0;
    glGenSamplers(1, &s);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, static_cast<GLint>(desc.wrapS));
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, static_cast<GLint>(desc.wrapT));
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(desc.minFilter));
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(desc.magFilter));
    glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, desc.minLod);
    glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, desc.maxLod);
    if (desc.maxAnisotropy > 1 && g_caps.maxAnisotropy > 1) {
      glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                          static_cast<float>(std::min<int32_t>(desc.maxAnisotropy, g_caps.maxAnisotropy)));
    }
    it->second = s;
  }
  return it->second;
}

GLuint framebuffer(GLuint color, GLuint depth) noexcept {
  const auto [it, inserted] = g_framebuffers.try_emplace(FramebufferKey{color, depth}, 0);
  if (inserted) {
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    g_state.drawFramebuffer = fbo;
    if (color != 0) {
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    } else {
      const GLenum none = GL_NONE;
      glDrawBuffers(1, &none);
    }
    if (depth != 0) {
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
    }
    const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
      Log.error("Framebuffer (color {}, depth {}) is incomplete: 0x{:X}", color, depth, status);
    }
    it->second = fbo;
  }
  return it->second;
}

void bind_framebuffer(GLuint color, GLuint depth) noexcept {
  const GLuint fbo = framebuffer(color, depth);
  if (g_state.drawFramebuffer != fbo) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    g_state.drawFramebuffer = fbo;
  }
}

// ---- Programs ----


Program::Program(ProgramDesc desc) noexcept : m_desc(std::move(desc)) {
  m_vertexHash = XXH3_64bits(m_desc.vertex.data(), m_desc.vertex.size());
  m_fragmentHash = XXH3_64bits(m_desc.fragment.data(), m_desc.fragment.size());
  m_usesDiscard = m_desc.fragment.find("discard;") != std::string::npos;
  m_replacesDepth = m_desc.fragment.find("gl_FragDepth =") != std::string::npos;
  gfx::port_capture::shader(m_desc.label, m_desc.vertex, m_desc.fragment);
}

Program::~Program() {
  if (const GLuint program = m_program; program != 0) {
    post([program] {
      if (g_state.program == program) {
        g_state.program = UINT32_MAX;
      }
      glDeleteProgram(program);
    });
  }
}

GLuint Program::get() noexcept {
  if (m_program != 0 || m_failed) {
    return m_program;
  }
  ZoneScoped;
  const GLuint vs = compile_shader(GL_VERTEX_SHADER, m_desc.vertex, m_desc.label);
  const GLuint fs = vs != 0 ? compile_shader(GL_FRAGMENT_SHADER, m_desc.fragment, m_desc.label) : 0;
  if (vs == 0 || fs == 0) {
    if (vs != 0) {
      glDeleteShader(vs);
    }
    m_failed = true;
    return 0;
  }
  GLuint program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (ok != GL_TRUE) {
    GLint len = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
    std::string info(static_cast<size_t>(std::max(len, 1)), '\0');
    glGetProgramInfoLog(program, len, nullptr, info.data());
    Log.error("Failed to link program '{}':\n{}", m_desc.label, info);
    glDeleteProgram(program);
    m_failed = true;
    return 0;
  }
  glUseProgram(program);
  g_state.program = program;
  for (const auto& [name, unit] : m_desc.samplers) {
    const GLint location = glGetUniformLocation(program, name.c_str());
    if (location >= 0) {
      glUniform1i(location, unit);
    }
  }
  for (const auto& [name, binding] : m_desc.blocks) {
    const GLuint index = glGetUniformBlockIndex(program, name.c_str());
    if (index != GL_INVALID_INDEX) {
      glUniformBlockBinding(program, index, static_cast<GLuint>(binding));
    }
  }
  // The sources are no longer needed; keep only the label.
  m_desc.vertex = {};
  m_desc.fragment = {};
  m_program = program;
  return program;
}

// ---- State cache ----

void apply_raster_state(const RasterState& s) noexcept {
  auto& c = g_state.raster;
  const bool force = !g_state.rasterValid;
  if (force || s.blend != c.blend) {
    s.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
  }
  if (s.blend) {
    if (force || !c.blend || s.colorEquation != c.colorEquation || s.alphaEquation != c.alphaEquation) {
      glBlendEquationSeparate(s.colorEquation, s.alphaEquation);
    }
    if (force || !c.blend || s.srcColor != c.srcColor || s.dstColor != c.dstColor || s.srcAlpha != c.srcAlpha ||
        s.dstAlpha != c.dstAlpha) {
      glBlendFuncSeparate(s.srcColor, s.dstColor, s.srcAlpha, s.dstAlpha);
    }
  }
  if (force || s.depthTest != c.depthTest) {
    s.depthTest ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
  }
  if (force || s.depthWrite != c.depthWrite) {
    glDepthMask(s.depthWrite ? GL_TRUE : GL_FALSE);
  }
  if (s.depthTest && (force || !c.depthTest || s.depthFunc != c.depthFunc)) {
    glDepthFunc(s.depthFunc);
  }
  if (force || s.cull != c.cull) {
    s.cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
  }
  if (s.cull && (force || !c.cull || s.cullFace != c.cullFace)) {
    glCullFace(s.cullFace);
  }
  if (force || s.colorMask != c.colorMask) {
    glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
  }
  // Keep the fields that were not applied (blend factors while blending is
  // off, the depth function while testing is off) so the next comparison
  // re-applies them when they become relevant.
  const RasterState previous = c;
  c = s;
  if (!s.blend && !force) {
    c.colorEquation = previous.colorEquation;
    c.alphaEquation = previous.alphaEquation;
    c.srcColor = previous.srcColor;
    c.dstColor = previous.dstColor;
    c.srcAlpha = previous.srcAlpha;
    c.dstAlpha = previous.dstAlpha;
  }
  if (!s.depthTest && !force) {
    c.depthFunc = previous.depthFunc;
  }
  if (!s.cull && !force) {
    c.cullFace = previous.cullFace;
  }
  if (force) {
    // The skipped fields were never applied; mark them unknown.
    if (!s.blend) {
      c.colorEquation = c.alphaEquation = c.srcColor = c.dstColor = c.srcAlpha = c.dstAlpha = 0;
    }
    if (!s.depthTest) {
      c.depthFunc = 0;
    }
    if (!s.cull) {
      c.cullFace = 0;
    }
  }
  g_state.rasterValid = true;
}

void set_color_mask(bool r, bool g, bool b, bool a) noexcept {
  const std::array<bool, 4> mask{r, g, b, a};
  if (!g_state.rasterValid || g_state.raster.colorMask != mask) {
    glColorMask(r, g, b, a);
    g_state.raster.colorMask = mask;
  }
}

void set_depth_write(bool enabled) noexcept {
  if (!g_state.rasterValid || g_state.raster.depthWrite != enabled) {
    glDepthMask(enabled ? GL_TRUE : GL_FALSE);
    g_state.raster.depthWrite = enabled;
  }
}

void set_viewport(int32_t x, int32_t y, int32_t width, int32_t height, float minDepth, float maxDepth) noexcept {
  const std::array vp{x, y, width, height};
  if (vp != g_state.viewport) {
    glViewport(x, y, width, height);
    g_state.viewport = vp;
  }
  const std::array range{minDepth, maxDepth};
  if (range != g_state.depthRange) {
    glDepthRangef(minDepth, maxDepth);
    g_state.depthRange = range;
  }
}

void set_scissor(int32_t x, int32_t y, int32_t width, int32_t height) noexcept {
  const std::array sc{x, y, width, height};
  if (sc != g_state.scissor) {
    glScissor(x, y, width, height);
    g_state.scissor = sc;
  }
}

void set_scissor_enabled(bool enabled) noexcept {
  if (g_state.scissorEnabled != static_cast<int>(enabled)) {
    enabled ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
    g_state.scissorEnabled = enabled;
  }
}

void set_blend_color(float r, float g, float b, float a) noexcept {
  const std::array color{r, g, b, a};
  if (color != g_state.blendColor) {
    glBlendColor(r, g, b, a);
    g_state.blendColor = color;
  }
}

void use_program(GLuint program) noexcept {
  if (g_state.program != program) {
    glUseProgram(program);
    g_state.program = program;
  }
}

void bind_texture(uint32_t unit, GLuint texture, GLuint samplerObject) noexcept {
  if (g_state.textures[unit] != texture) {
    if (g_state.activeUnit != unit) {
      glActiveTexture(GL_TEXTURE0 + unit);
      g_state.activeUnit = unit;
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    g_state.textures[unit] = texture;
  }
  if (g_state.samplers[unit] != samplerObject) {
    glBindSampler(unit, samplerObject);
    g_state.samplers[unit] = samplerObject;
  }
}

void bind_uniform_range(uint32_t binding, GLuint buffer, uint32_t offset, uint32_t size) noexcept {
  auto& bound = g_state.uniforms[binding];
  if (bound.buffer != buffer || bound.offset != offset || bound.size != size) {
    glBindBufferRange(GL_UNIFORM_BUFFER, binding, buffer, offset, size);
    bound = {buffer, offset, size};
  }
}

void invalidate_state() noexcept {
  const GLuint emptyVao = g_state.emptyVao;
  const GLuint vao = g_state.vao;
  g_state = {};
  g_state.emptyVao = emptyVao;
  g_state.vao = vao;
  g_state.samplers.fill(UINT32_MAX);
  g_state.textures.fill(UINT32_MAX);
  for (auto& u : g_state.uniforms) {
    u.buffer = UINT32_MAX;
  }
}

void bind_draw_vao(GLuint indexBuffer) noexcept {
  if (g_state.vao == 0) {
    glGenVertexArrays(1, &g_state.vao);
  }
  glBindVertexArray(g_state.vao);
  if (g_state.vaoElementBuffer != indexBuffer) {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
    g_state.vaoElementBuffer = indexBuffer;
  }
}

void draw_fullscreen(GLuint program) noexcept {
  if (!aurora::native_frame_drawing_allowed()) return;
  use_program(program);
  glBindVertexArray(g_state.emptyVao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

// ---- Presentation and readback ----

std::pair<uint32_t, uint32_t> drawable_size() noexcept {
  int w = 0;
  int h = 0;
  if (g_context != nullptr) {
    SDL_GetWindowSizeInPixels(g_context->window, &w, &h);
  }
  return {static_cast<uint32_t>(std::max(w, 0)), static_cast<uint32_t>(std::max(h, 0))};
}

void present(const TexturePtr& source, int32_t x, int32_t y, int32_t width, int32_t height) noexcept {
  if (!aurora::native_frame_drawing_allowed()) return;
  ZoneScoped;
  const auto [drawableWidth, drawableHeight] = drawable_size();
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  g_state.drawFramebuffer = 0;
  set_scissor_enabled(false);
  set_color_mask(true, true, true, true);
  glClearColor(0.f, 0.f, 0.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  if (source && width > 0 && height > 0) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer(source->gl(), 0));
    // Texture rows are top-down while the window's row 0 is the bottom one.
    const int32_t top = static_cast<int32_t>(drawableHeight) - y;
    glBlitFramebuffer(0, 0, static_cast<GLint>(source->width), static_cast<GLint>(source->height), x, top,
                      x + width, top - height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  }
  // Submit the scene without waiting for completion. The next game frame can
  // run while the GPU consumes it; frameskip polls this fence with timeout 0.
  auto& fence = g_frameFences[g_frameSlot];
  if (fence) glDeleteSync(fence);
  fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  if (!fence) Log.fatal("GL frame fence allocation failed");
  glFlush();
  SDL_GL_SwapWindow(g_context->window);
  g_frameSlot = (g_frameSlot + 1) % aurora::GpuFrameRing::BufferCount;
}

void read_pixels(GLuint texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height, void* out) noexcept {
  glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer(texture, 0));
  glReadPixels(static_cast<GLint>(x), static_cast<GLint>(y), static_cast<GLsizei>(width),
               static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, out);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

GLuint create_buffer() noexcept {
  GLuint result = 0;
  glGenBuffers(1, &result);
  return result;
}
void destroy_buffer(GLuint buffer) noexcept { glDeleteBuffers(1, &buffer); }
void write_buffer(GLuint buffer, bool indices, const void* bytes, size_t size) noexcept {
  const GLenum target = indices ? GL_ELEMENT_ARRAY_BUFFER : GL_UNIFORM_BUFFER;
  glBindBuffer(target, buffer);
  glBufferData(target, static_cast<GLsizeiptr>(size), bytes, GL_STREAM_DRAW);
}
void copy_texture_region(GLuint source, GLuint dest, int32_t srcX, int32_t srcY,
                         int32_t dstX, int32_t dstY, int32_t width, int32_t height) noexcept {
  glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer(source, 0));
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, dest);
  glCopyTexSubImage2D(GL_TEXTURE_2D, 0, dstX, dstY, srcX, srcY, width, height);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  invalidate_state();
}
void clear_color(const float* color) noexcept {
  if (aurora::native_frame_drawing_allowed()) glClearBufferfv(GL_COLOR, 0, color);
}
void clear_depth(float depth) noexcept {
  if (aurora::native_frame_drawing_allowed()) glClearBufferfv(GL_DEPTH, 0, &depth);
}
void draw_indexed(uint32_t indexCount, uint32_t indexOffset, uint32_t instances) noexcept {
  if (!aurora::native_frame_drawing_allowed()) return;
  glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(indexCount), GL_UNSIGNED_SHORT,
                         reinterpret_cast<const void*>(static_cast<uintptr_t>(indexOffset)),
                         static_cast<GLsizei>(instances));
}
void draw_native_geometry(const AuroraNativeGeometry& geometry, bool textured) noexcept {
  if (!aurora::native_frame_drawing_allowed() || !geometry.index_count) return;
  auto& program = g_nativePrograms[textured];
  if (!program) {
    ProgramDesc desc;
    desc.label = textured ? "Native textured geometry" : "Native colored geometry";
    desc.vertex = vertex_source(R"(
layout(location=0) in vec4 position_in;
layout(location=1) in vec2 uv_in;
layout(location=2) in vec4 color_in;
out vec2 native_uv;
out vec4 native_color;
void main() {
  gl_Position = vec4(position_in.x, -position_in.y, position_in.zw);
  native_uv = uv_in;
  native_color = color_in;
})");
    desc.fragment = fragment_source(textured ? R"(
in vec2 native_uv;
in vec4 native_color;
uniform sampler2D native_texture;
layout(location=0) out vec4 native_output;
void main() { native_output = texture(native_texture, native_uv) * native_color; }
)" : R"(
in vec4 native_color;
layout(location=0) out vec4 native_output;
void main() { native_output = native_color; }
)");
    if (textured) desc.samplers.emplace_back("native_texture", 0);
    program = std::make_shared<Program>(std::move(desc));
  }
  use_program(program->get());
  if (!g_nativeVao) {
    glGenVertexArrays(1, &g_nativeVao);
    glGenBuffers(1, &g_nativeVertexBuffer);
    glGenBuffers(1, &g_nativeIndexBuffer);
    glBindVertexArray(g_nativeVao);
    glBindBuffer(GL_ARRAY_BUFFER, g_nativeVertexBuffer);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(AuroraNativeVertex), nullptr);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(AuroraNativeVertex),
                          reinterpret_cast<void*>(offsetof(AuroraNativeVertex, uv)));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(AuroraNativeVertex),
                          reinterpret_cast<void*>(offsetof(AuroraNativeVertex, color)));
    for (unsigned i = 0; i < 3; ++i) glEnableVertexAttribArray(i);
  } else glBindVertexArray(g_nativeVao);
  glBindBuffer(GL_ARRAY_BUFFER, g_nativeVertexBuffer);
  glBufferData(GL_ARRAY_BUFFER, size_t(geometry.vertex_count) * sizeof(AuroraNativeVertex),
               geometry.vertices, GL_STREAM_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_nativeIndexBuffer);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, size_t(geometry.index_count) * sizeof(uint16_t),
               geometry.indices, GL_STREAM_DRAW);
  glDrawElements(GL_TRIANGLES, geometry.index_count, GL_UNSIGNED_SHORT, nullptr);
  // The recorded/native generic draw VAO has its own element-buffer binding.
  glBindVertexArray(g_state.emptyVao);
}
void draw_native_material_geometry(const AuroraNativeMaterialGeometry& geometry,GLuint program,
                                  const AuroraNativeUniforms& uniforms) noexcept {
  if(!aurora::native_frame_drawing_allowed() || !geometry.index_count) return;
  use_program(program);
  if(!g_materialVao) {
    glGenVertexArrays(1,&g_materialVao);
    glGenBuffers(1,&g_materialVertexBuffer);glGenBuffers(1,&g_materialIndexBuffer);glGenBuffers(1,&g_materialUniformBuffer);
    glBindVertexArray(g_materialVao);glBindBuffer(GL_ARRAY_BUFFER,g_materialVertexBuffer);
    const auto attribute=[](unsigned location,int components,GLenum type,bool normalized,size_t offset) {
      glVertexAttribPointer(location,components,type,normalized,sizeof(AuroraNativeMaterialVertex),reinterpret_cast<void*>(offset));
      glEnableVertexAttribArray(location);
    };
    attribute(0,4,GL_FLOAT,false,offsetof(AuroraNativeMaterialVertex,position));
    attribute(1,3,GL_FLOAT,false,offsetof(AuroraNativeMaterialVertex,view_position));
    attribute(2,3,GL_FLOAT,false,offsetof(AuroraNativeMaterialVertex,normal));
    attribute(3,4,GL_UNSIGNED_BYTE,true,offsetof(AuroraNativeMaterialVertex,color));
    attribute(4,4,GL_UNSIGNED_BYTE,true,offsetof(AuroraNativeMaterialVertex,color)+4);
    for(unsigned i=0;i<8;++i) attribute(i+5,3,GL_FLOAT,false,offsetof(AuroraNativeMaterialVertex,texcoord)+i*12);
  } else glBindVertexArray(g_materialVao);
  glBindBuffer(GL_ARRAY_BUFFER,g_materialVertexBuffer);
  glBufferData(GL_ARRAY_BUFFER,size_t(geometry.vertex_count)*sizeof(*geometry.vertices),geometry.vertices,GL_STREAM_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,g_materialIndexBuffer);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,size_t(geometry.index_count)*2,geometry.indices,GL_STREAM_DRAW);
  glBindBuffer(GL_UNIFORM_BUFFER,g_materialUniformBuffer);
  glBufferData(GL_UNIFORM_BUFFER,uniforms.size,uniforms.data,GL_STREAM_DRAW);
  bind_uniform_range(0,g_materialUniformBuffer,0,uniforms.size);
  glDrawElements(GL_TRIANGLES,geometry.index_count,GL_UNSIGNED_SHORT,nullptr);
  glBindVertexArray(g_state.emptyVao);
}
void uniform4(GLuint program, const char* name, float x, float y, float z, float w) noexcept {
  glUniform4f(glGetUniformLocation(program, name), x, y, z, w);
}
} // namespace aurora::gl
