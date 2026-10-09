#!/bin/bash
# Build the web build (docs/optimizations/platforms/web-build.md): $MH3_BUILD_DIR (default build/web)
# gets MH3.js, MH3.wasm and the page (index.html, mh3-web.js). Needs the
# Emscripten SDK (EMSDK, default ~/emsdk), and for the WGSL shader bank
# glslangValidator, naga (cargo install naga-cli) and zstd. A development
# build (MH3_DEVELOPMENT=ON: statistics, no LTO); MH3_EXTRA_CMAKE_ARGS adds
# CMake options, BUILD_JOBS sets the parallelism. Serve the result with
# scripts/serve-web.py.
set -euo pipefail
cd "$(dirname "$0")/.."
export EMSDK=${EMSDK:-$HOME/emsdk}
build=${MH3_BUILD_DIR:-build/web}
mkdir -p logs
cmake -S runtime -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/web.cmake" \
  -DMH3_AURORA_ROOT="$PWD/aurora-main" -DFETCHCONTENT_BASE_DIR="$PWD/build/aurora-web/_deps" \
  -DCMAKE_BUILD_TYPE=Release -DMH3_DEVELOPMENT=ON -DAURORA_SDL3_PROVIDER=vendor -DAURORA_ENABLE_CARD=OFF \
  ${MH3_EXTRA_CMAKE_ARGS:-} -DCMAKE_PROJECT_aurora_INCLUDE="$PWD/scripts/aurora-compat.cmake" > logs/web-configure.log 2>&1
cmake --build "$build" --target MH3 --parallel "${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}" -- ${MH3_NINJA_ARGS:-} > logs/web-build.log 2>&1
echo "Built $build/MH3.js and MH3.wasm (WebGPU); serve them with scripts/serve-web.py $build"
