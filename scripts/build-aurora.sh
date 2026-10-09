#!/bin/bash
# Build the OpenGL ES 3.0 executable for arm32 or arm64 Linux (MH3_ARCH,
# default arm32): build/linux-$MH3_ARCH/MH3. Aurora renders natively through
# OpenGL ES 3.0 on its own render thread.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/target.sh
mkdir -p logs
python3 scripts/prepare_audio_assets.py
cmake -S runtime -B "$MH3_BUILD_DIR" -G Ninja "${MH3_CROSS_ARGS[@]}" \
  -DMH3_AURORA_ROOT="${AURORA_ROOT:-$PWD/aurora-main}" \
  -DFETCHCONTENT_BASE_DIR="$PWD/build/aurora-$MH3_ARCH/_deps" \
  -DCMAKE_BUILD_TYPE=Release "${MH3_COMPILER_ARGS[@]}" \
  -DAURORA_SDL3_PROVIDER=auto -DAURORA_ENABLE_CARD=OFF ${MH3_EXTRA_CMAKE_ARGS:-} \
  -DCMAKE_PROJECT_aurora_INCLUDE="$PWD/scripts/aurora-compat.cmake" > logs/aurora-configure.log 2>&1
cmake --build "$MH3_BUILD_DIR" --target MH3 MH3ControllerHostTest --parallel "${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}" -- ${MH3_NINJA_ARGS:-} > logs/aurora-build.log 2>&1
echo "Built $MH3_BUILD_DIR/MH3 ($MH3_ARCH Linux, OpenGL ES 3.0)"
