#!/bin/bash
# Build the web build: $MH3_BUILD_DIR gets MH3.js,
# MH3.wasm, MH3.data and the page (index.html, mh3-web.js). Needs the
# Emscripten SDK (EMSDK, default ~/emsdk), and for the WGSL shader bank
# glslangValidator, naga (cargo install naga-cli) and zstd. A release build
# (ThinLTO, default build/web) unless MH3_DEVELOPMENT=ON (statistics, debug
# tools, the page's args and env parameters, no LTO; default build/web-dev);
# MH3_EXTRA_CMAKE_ARGS adds CMake options, BUILD_JOBS sets the parallelism.
# Serve the result with scripts/serve-web.py.
set -euo pipefail
cd "$(dirname "$0")/.."
export EMSDK=${EMSDK:-$HOME/emsdk}
development=${MH3_DEVELOPMENT:-OFF}
if [[ $development == ON ]]; then
  build=${MH3_BUILD_DIR:-build/web-dev}
else
  build=${MH3_BUILD_DIR:-build/web}
fi
# Each build compiles the dependencies in its own directory (release objects
# are LLVM bitcode); sources another web build downloaded are reused.
dependencies=()
for dependency in sdl xxhash fmt imgui sqlite3 zstd freetype png zlib; do
  for other in build/aurora-web build/web build/web-dev; do
    if [[ $other != "$build" && -d $other/_deps/$dependency-src ]]; then
      dependencies+=("-DFETCHCONTENT_SOURCE_DIR_${dependency^^}=$PWD/$other/_deps/$dependency-src")
      break
    fi
  done
done
mkdir -p logs
# zlib writes zconf.h.cmakein once and caches that it did: a directory whose
# dependencies built elsewhere before (the shared build/aurora-web) lacks it.
if [[ ! -f $build/_deps/zlib-build/zconf.h.cmakein ]]; then
  dependencies+=(-UZLIB_CONF_WRITTEN)
fi
cmake -S runtime -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/web.cmake" \
  -DMH3_AURORA_ROOT="$PWD/aurora-main" -DFETCHCONTENT_BASE_DIR="$PWD/$build/_deps" "${dependencies[@]}" \
  -DCMAKE_BUILD_TYPE=Release -DMH3_DEVELOPMENT="$development" -DAURORA_SDL3_PROVIDER=vendor -DAURORA_ENABLE_CARD=OFF \
  ${MH3_EXTRA_CMAKE_ARGS:-} -DCMAKE_PROJECT_aurora_INCLUDE="$PWD/scripts/aurora-compat.cmake" > logs/web-configure.log 2>&1
cmake --build "$build" --target MH3 --parallel "${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}" -- ${MH3_NINJA_ARGS:-} > logs/web-build.log 2>&1
echo "Built $build/MH3.js and MH3.wasm (WebGPU); serve them with scripts/serve-web.py $build"
