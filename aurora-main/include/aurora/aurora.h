#ifndef AURORA_AURORA_H
#define AURORA_AURORA_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

extern "C" {
#else
#include "stdbool.h"
#include "stddef.h"
#include "stdint.h"
#endif

typedef enum {
  BACKEND_AUTO,
  BACKEND_D3D11,
  BACKEND_D3D12,
  BACKEND_METAL,
  BACKEND_VULKAN,
  BACKEND_OPENGL,
  BACKEND_OPENGLES,
  BACKEND_WEBGPU,
  BACKEND_NULL,
  BACKEND_GXM,
} AuroraBackend;

typedef enum {
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARNING,
  LOG_ERROR,
  LOG_FATAL,
} AuroraLogLevel;

typedef enum {
  AURORA_DISPLAY_MODE_WINDOWED,
  AURORA_DISPLAY_MODE_BORDERLESS,
  AURORA_DISPLAY_MODE_EXCLUSIVE,
} AuroraDisplayMode;

typedef struct {
  int32_t x;
  int32_t y;
} AuroraWindowPos;

typedef struct {
  uint32_t width;
  uint32_t height;

  /**
   * Width of the main GX framebuffer.
   */
  uint32_t fb_width;

  /**
   * Height of the main GX framebuffer.
   */
  uint32_t fb_height;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_width if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_width;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_height if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_height;
  float scale;
} AuroraWindowSize;

typedef struct SDL_Window SDL_Window;
typedef struct AuroraEvent AuroraEvent;

typedef void (*AuroraLogCallback)(AuroraLogLevel level, const char* module, const char* message, unsigned int len);
typedef void (*AuroraImGuiInitCallback)(const AuroraWindowSize* size);

typedef struct {
  const char* appName;
  const char* userPath;
  const char* cachePath;
  // Read-only application resources. Defaults to SDL_GetBasePath(), which is
  // where release builds place initial_pipeline_cache.db.
  const char* resourcesPath;
  AuroraBackend desiredBackend;
  uint32_t msaa;
  uint16_t maxTextureAnisotropy;
  // No VSync knob exists: GL uses swap interval 0; GXM flips at VBlank.
  bool startFullscreen;
  bool allowJoystickBackgroundEvents;
  bool pauseOnFocusLost;
  bool allowTextureReplacements;
  bool allowTextureDumps;
  bool disableCopyFilter;
  // When false, Aurora centers the first window. When true, windowPosX/Y are restored verbatim,
  // including negative coordinates on monitors left of or above the primary display.
  bool hasWindowPosition;
  int32_t windowPosX;
  int32_t windowPosY;
  uint32_t windowWidth;
  uint32_t windowHeight;
  void* iconRGBA8;
  uint32_t iconWidth;
  uint32_t iconHeight;
  AuroraLogCallback logCallback;
  AuroraLogLevel logLevel;
  AuroraImGuiInitCallback imGuiInitCallback;

  /*
   * The size of the GameCube's main memory, or MEM1 on the Wii.
   * Note that it will not be allocated at the exact 0x80000000 address, as that cannot be guaranteed.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem1Size;

  /*
   * The size of the GameCube's ARAM, or MEM2 on the Wii.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem2Size;

  // Optional directory for the portable GX pipeline database. When null, the
  // database is stored in cachePath with Dawn's machine-specific cache.
  const char* pipelineCachePath;
} AuroraConfig;

typedef enum {
  AURORA_INITIALIZATION_SUCCESS = 0,
  AURORA_INITIALIZATION_GRAPHICS_UNAVAILABLE = 1,
} AuroraInitializationStatus;

typedef struct {
  AuroraBackend backend;
  const char* userPath;
  const char* cachePath;
  SDL_Window* window;
  AuroraWindowSize windowSize;
  AuroraInitializationStatus initializationStatus;
  // On failure, owned by SDL on the calling thread. Copy before another SDL call.
  const char* initializationError;
} AuroraInfo;

AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config);
void aurora_shutdown();
// Enable the Vita system-dialog overlay on submitted display frames. Call on
// the graphics owner thread between dialog initialization and termination.
// Other backends ignore this toggle.
void aurora_set_common_dialog_active(bool active);
const AuroraEvent* aurora_update();
bool aurora_begin_frame();
void aurora_end_frame();
// Set the native target's base dimensions before initialization or at a frame
// boundary. These dimensions do not reserve GX's extra EFB scratch rows.
// Invalid zero dimensions leave the configuration unchanged.
bool aurora_configure_native_target(uint32_t width, uint32_t height);
// How the native target covers the display surface. FIT keeps the base
// dimensions' aspect, centered (bars on other surfaces). FILL sizes the target
// to the whole surface and scales the base dimensions per axis, so a game's
// anamorphic 16:9 picture fills a 16:9 surface. ASPECT_16_9 is a 16:9 target
// centered on any surface. Call on the graphics owner thread; the target is
// resized at the next frame boundary.
typedef enum {
  AURORA_NATIVE_TARGET_FIT = 0,
  AURORA_NATIVE_TARGET_FILL = 1,
  AURORA_NATIVE_TARGET_ASPECT_16_9 = 2,
} AuroraNativeTargetAspect;
void aurora_set_native_target_aspect(AuroraNativeTargetAspect aspect);
// Size the native target for a display surface of width x height (the aspect
// rules above apply to it) whatever the window size; presentation scales the
// target into the window, keeping its aspect. 0 x 0 follows the window again.
// Same thread rules as aurora_set_native_target_aspect.
void aurora_set_native_surface_size(uint32_t width, uint32_t height);
// Direct native rendering: call begin before preparing any draws or writing
// their mapped GPU payload. False skips all drawing/clears/present for this
// game frame without waiting. Logic still advances; always call end, including
// after a skipped frame. Cancellation can close admission but never reopen it.
bool aurora_begin_native_frame();
// Main thread: pump window events, then report whether a frame can present.
// A host that renders on another thread polls this before recording a frame.
bool aurora_native_window_presentable();
bool aurora_native_frame_should_render();
// Seal any recorded GPU work without presenting. The current mutable slot
// retains its completion lease; CPU state continues until the normal end.
void aurora_cancel_native_frame();
// The three display and mutable draw slots rotate only on rendered frames.
uint32_t aurora_native_frame_slot();
void aurora_end_native_frame();
typedef void (*AuroraFrameWorkerWaitCallback)();
// Called from the producer thread at bounded intervals while Aurora waits for
// the asynchronous frame worker. The callback must not enter Aurora.
void aurora_set_frame_worker_wait_callback(AuroraFrameWorkerWaitCallback callback);
void aurora_wait_for_frame_worker();
bool aurora_wait_for_frame_worker_for(uint32_t timeoutMicros);
// Absolute schedule for the next sealed frame, on steady_clock: baseNanos anchors the group and
// intervalNanos is the period, so slot k of N+1 fires at base + k*interval/(N+1). Zeros clear it.
void aurora_set_present_schedule(uint64_t baseNanos, uint64_t intervalNanos);
// Reports whether the frame about to be sealed met its display boundary. Interpolation sizes its
// slot group from this, backing off after misses. Only paced presents may report.
void aurora_report_producer_paced(bool paced);
void aurora_request_frame_capture(uint32_t frame, const char* outputPath);
bool aurora_flush_efb_copies_to_ram();
bool aurora_flush_efb_copy_to_ram(void* dest);
// Submits the draws recorded so far and resumes the same frame without clearing the EFB.
bool aurora_flush_draws();
// Blocks until the render thread and the GPU have finished all submitted work.
void aurora_wait_for_gpu_idle();
// Frames the render thread has swapped to the window so far.
// Frameskip: poll GPU completion before native draw preparation. GXM reads its
// latest scene notification; GLES uses a zero-timeout fence poll. Off by default.
void aurora_set_frameskip(bool enabled);
uint64_t aurora_skipped_frame_count();
uint64_t aurora_presented_frame_count();

void aurora_set_log_level(AuroraLogLevel level);
void aurora_set_pause_on_focus_lost(bool value);
void aurora_set_background_input(bool value);
void aurora_set_display_mode(AuroraDisplayMode mode);
AuroraDisplayMode aurora_get_display_mode();

AuroraBackend aurora_get_backend();
const AuroraBackend* aurora_get_available_backends(size_t* count);

#ifdef __cplusplus
}
#endif

#endif
