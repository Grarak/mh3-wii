# Linux targets: OpenGL ES 3.0 and the Cortex-A9 build

## What the original does

The project began as a port to macOS with Aurora's WebGPU (Dawn) and Metal
backends (the bring-up notes in `docs/` still mention it). Aurora is a
source-level GameCube/Wii compatibility layer that turns GX calls into
WebGPU draws ([Aurora dependency](../../aurora-dependency.md)).

## What the port does instead

- **OpenGL ES 3.0** (`aurora-main/lib/gl/gl.cpp`): Dawn and WebGPU were
  replaced by a GLES 3.0 backend, used on every Linux target. It runs the
  same fixed programs as the Vita, written in GLSL ES 3.00 next to their Cg
  versions ([fixed shading](../rendering/fixed-shading-models.md)), from the same
  render thread and frame packets ([render thread](../threading/render-thread.md)),
  with material blocks as uniform buffers and swap interval 0. The GX
  emulation and TEV generation that came with Aurora are no longer reached
  ([GX replacement](../rendering/gx-replacement.md)). WebGPU came back later as a third
  backend for the browser build only ([web build](web-build.md)).
- **arm32 (armhf) with Cortex-A9 code** (`cmake/toolchains/linux-arm32.cmake`,
  `docker/armhf-cross.Dockerfile`): Clang 21 with `-mcpu=cortex-a9
  -mfpu=neon -mthumb`, cross-compiled in a Debian trixie container against
  Debian's armhf libraries, matching the Vita's CPU. It runs on a Raspberry
  Pi 5 under 64-bit Raspberry Pi OS with armhf multiarch. The Pi's cores are
  Cortex-A76, so it measures the A9 code's relative costs, not the Vita's
  speed. Paired singles and FPSCR[NI] are handled as on the Vita
  ([Cortex-A9 floating point](../recompilation/cortex-a9-floating-point.md)); the guest
  window wraps around the 32-bit address space.
- **arm64** for development: gdb, port checks, step probes and software
  GLES (llvmpipe) without a GPU.
- **The 960×544 surface** on every host, scaled into the window
  ([widescreen](../rendering/widescreen.md)), so all hosts render the Vita's picture.

## Why

The Vita is the end target, but iterating on it is slow and its tools are
limited. A Linux build with the same CPU model (Cortex-A9 code), the same
renderer structure and the same picture size makes most work testable on a
Pi or another ARM Linux machine first; the Pi's game-thread numbers decide what to try on the
Vita. GLES 3.0 is available on the Pi's V3D and in Mesa's software
renderer, and maps onto the same fixed programs as GXM.

## Example

The arm32 toolchain flags (`cmake/toolchains/linux-arm32.cmake`):

```cmake
set(mh3_arm32_flags "--target=${mh3_triple} -mcpu=${mh3_arm32_cpu} -mfpu=${mh3_arm32_fpu} -mfloat-abi=hard -mthumb -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64")
```

with `mh3_arm32_cpu` `cortex-a9` and `mh3_arm32_fpu` `neon` by default
(`MH3_ARM32_CPU`, `MH3_ARM32_FPU` override them).

## Applying it to another Wii game

Generic: the GLES backend, the toolchains, the Docker image and the build
scripts do not depend on MH3, except for the build's game-specific sources.
Keeping an ARM Linux build with the target's CPU flags next to a console
target is the practical part: profile there with `perf`, then confirm on
the console.
