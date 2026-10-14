# Aurora

`aurora-main/` started as a source snapshot of Aurora taken from the
WiiCompiled project (parent commit
`85f250155f8f397c1c463dc51dd4dc2707371d06`). Aurora's MIT license is kept at
`aurora-main/LICENSE`.

It is now MH3's own copy and is edited in place. Commit `0313223` replaced
WebGPU/Dawn with **OpenGL ES 3.0** (`aurora-main/lib/gl`); the Vita backend uses
native **GXM** (`aurora-main/lib/gxm`). The web build adds **WebGPU** again
(`aurora-main/lib/webgpu`, Emscripten's emdawnwebgpu, `AURORA_WEBGPU`; see
[web build](web-build.md)); Dawn native and Vulkan are not built.
`runtime/src/graphics_host.cpp` selects `BACKEND_OPENGLES` on Linux,
`BACKEND_GXM` on Vita and `BACKEND_WEBGPU` on the web and rejects an
unexpected backend.

The runtime's render thread owns the graphics context (core 0 on Vita) and
replays frame packets recorded by the game thread; backend tasks execute on
it. No GX queue remains; GX compatibility APIs/FIFO routes panic. Native rendering
reuses pure material equations and typed resource helpers from the tree,
without restoring GX submission. See [GX replacement](optimizations/rendering/gx-replacement.md)
for how the game's draws reach the renderer.

`scripts/build-aurora.sh` configures the runtime against this tree (override
with `AURORA_ROOT`). `scripts/aurora-compat.cmake` only fixes header-order
quirks of some toolchains. Common libraries (SDL3, fmt, PNG, zlib, zstd, ImGui,
SQLite and xxHash) come from Aurora's CMake providers: installed packages or
pinned downloads into `build/aurora-<arch>/_deps`. A first build may need
network access. For arm32 they build inside the `mh3-armhf-cross` Docker image
against Debian's armhf packages. Aurora's hash containers are the runtime's
`FastMap`/`FastSet` (`aurora-main/include/aurora/fast_map.h`).

The translator and its notices are under `toolchain/`.
