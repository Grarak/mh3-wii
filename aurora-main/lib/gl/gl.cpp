#include "gl.hpp"

#include "../internal.hpp"
#include "../gfx/port_capture.hpp"

#include <SDL3/SDL.h>
#include <absl/container/flat_hash_map.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
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

struct RenderThread {
  SDL_Window* window = nullptr;
  SDL_GLContext context = nullptr;
  std::thread thread;
  std::thread::id threadId;
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<Task> queue;
  bool stopping = false;
};

RenderThread* g_thread = nullptr;
Caps g_caps;

void run_render_thread(RenderThread* rt) {
  if (!SDL_GL_MakeCurrent(rt->window, rt->context)) {
    Log.fatal("Failed to make the GL context current on the render thread: {}", SDL_GetError());
  }
  std::deque<Task> batch;
  for (;;) {
    {
      std::unique_lock lock(rt->mutex);
      rt->cv.wait(lock, [&] { return rt->stopping || !rt->queue.empty(); });
      if (rt->queue.empty() && rt->stopping) {
        break;
      }
      batch.swap(rt->queue);
    }
    for (auto& task : batch) {
      std::move(task)();
    }
    batch.clear();
  }
  glFinish();
  SDL_GL_MakeCurrent(rt->window, nullptr);
}

// ---- Render thread state ----

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
                      d.maxAnisotropy);
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

constexpr std::string_view Prelude = R"(precision highp float;
precision highp int;
precision highp sampler2D;
precision highp usampler2D;
precision highp isampler2D;
#define f32 float
#define i32 int
#define u32 uint
#define vec2f vec2
#define vec3f vec3
#define vec4f vec4
#define vec2i ivec2
#define vec3i ivec3
#define vec4i ivec4
#define vec2u uvec2
#define vec3u uvec3
#define vec4u uvec4
#define mat4x4f mat4
#define mat3x4f mat3x4
float select(float f, float t, bool c) { return c ? t : f; }
vec2 select(vec2 f, vec2 t, bool c) { return c ? t : f; }
vec3 select(vec3 f, vec3 t, bool c) { return c ? t : f; }
vec4 select(vec4 f, vec4 t, bool c) { return c ? t : f; }
int select(int f, int t, bool c) { return c ? t : f; }
uint select(uint f, uint t, bool c) { return c ? t : f; }
ivec2 select(ivec2 f, ivec2 t, bool c) { return c ? t : f; }
uvec2 select(uvec2 f, uvec2 t, bool c) { return c ? t : f; }
vec2 select(vec2 f, vec2 t, bvec2 c) { return mix(f, t, c); }
vec3 select(vec3 f, vec3 t, bvec3 c) { return mix(f, t, c); }
vec4 select(vec4 f, vec4 t, bvec4 c) { return mix(f, t, c); }
// Aurora works in WebGPU conventions: clip-space z in [0, w] and framebuffer
// rows counted from the top. Every pass renders vertically flipped, so GL
// window row 0 is the top row and gl_FragCoord matches WebGPU's position.
vec4 aur_position(vec4 p) { return vec4(p.x, -p.y, 2.0 * p.z - p.w, p.w); }
// Read-only word buffers live in R32UI textures, 2048 words per row.
uint aur_word(highp usampler2D b, uint i) { return texelFetch(b, ivec2(int(i & 2047u), int(i >> 11u)), 0).r; }
)";

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

// ---- Render thread ----

bool initialize(SDL_Window* window, bool vsync) noexcept {
  if (g_thread != nullptr) {
    return g_thread->window == window;
  }
  SDL_GLContext context = SDL_GL_CreateContext(window);
  if (context == nullptr) {
    Log.error("Failed to create an OpenGL ES 3.0 context: {}", SDL_GetError());
    return false;
  }
  if (!SDL_GL_SetSwapInterval(vsync ? 1 : 0)) {
    Log.warn("Failed to set the swap interval: {}", SDL_GetError());
  }
  SDL_GL_MakeCurrent(window, nullptr);
  auto* rt = new RenderThread;
  rt->window = window;
  rt->context = context;
  rt->thread = std::thread([rt] {
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    run_render_thread(rt);
  });
  rt->threadId = rt->thread.get_id();
  g_thread = rt;
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
  RenderThread* rt = g_thread;
  if (rt == nullptr) {
    return;
  }
  post([] {
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
  {
    std::lock_guard lock(rt->mutex);
    rt->stopping = true;
  }
  rt->cv.notify_all();
  rt->thread.join();
  g_thread = nullptr;
  SDL_GL_DestroyContext(rt->context);
  delete rt;
}

bool initialized() noexcept { return g_thread != nullptr; }
const Caps& caps() noexcept { return g_caps; }

bool on_render_thread() noexcept { return g_thread != nullptr && std::this_thread::get_id() == g_thread->threadId; }

void post(Task task) noexcept {
  RenderThread* rt = g_thread;
  if (rt == nullptr) {
    return;
  }
  {
    std::lock_guard lock(rt->mutex);
    rt->queue.push_back(std::move(task));
  }
  rt->cv.notify_one();
}

void call(Task task) noexcept {
  if (g_thread == nullptr) {
    return;
  }
  if (on_render_thread()) {
    std::move(task)();
    return;
  }
  std::mutex doneMutex;
  std::condition_variable doneCv;
  bool done = false;
  post([&, task = std::move(task)]() mutable {
    std::move(task)();
    std::lock_guard lock(doneMutex);
    done = true;
    doneCv.notify_one();
  });
  std::unique_lock lock(doneMutex);
  doneCv.wait(lock, [&] { return done; });
}

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
  // Render thread code uses a texture right after creating it.
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

std::string_view glsl_prelude() noexcept { return Prelude; }

std::string vertex_source(std::string_view body) {
  std::string out = "#version 300 es\n";
  out += Prelude;
  out += "#line 1\n";
  out += body;
  return out;
}

std::string fragment_source(std::string_view body) {
  std::string out = "#version 300 es\n";
  out += Prelude;
  out += "#line 1\n";
  out += body;
  return out;
}

Program::Program(ProgramDesc desc) noexcept : m_desc(std::move(desc)) {
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
  use_program(program);
  glBindVertexArray(g_state.emptyVao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

// ---- Presentation and readback ----

std::pair<uint32_t, uint32_t> drawable_size() noexcept {
  int w = 0;
  int h = 0;
  if (g_thread != nullptr) {
    SDL_GetWindowSizeInPixels(g_thread->window, &w, &h);
  }
  return {static_cast<uint32_t>(std::max(w, 0)), static_cast<uint32_t>(std::max(h, 0))};
}

void present(const TexturePtr& source, int32_t x, int32_t y, int32_t width, int32_t height) noexcept {
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
  SDL_GL_SwapWindow(g_thread->window);
}

void read_pixels(GLuint texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height, void* out) noexcept {
  glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer(texture, 0));
  glReadPixels(static_cast<GLint>(x), static_cast<GLint>(y), static_cast<GLsizei>(width),
               static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, out);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

} // namespace aurora::gl
