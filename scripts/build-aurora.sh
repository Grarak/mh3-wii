#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p logs
cmake -S "${AURORA_ROOT:-../Wiicompiled/aurora-main}" -B build/aurora -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DAURORA_DAWN_PROVIDER=package -DAURORA_SDL3_PROVIDER=auto -DAURORA_ENABLE_CARD=OFF \
  -DCMAKE_PROJECT_aurora_INCLUDE="$PWD/scripts/aurora-compat.cmake" > logs/aurora-configure.log 2>&1
cmake --build build/aurora --target aurora_core aurora_gx --parallel "${BUILD_JOBS:-6}" > logs/aurora-build.log 2>&1
