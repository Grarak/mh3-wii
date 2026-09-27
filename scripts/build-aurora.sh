#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p logs
python3 scripts/prepare_audio_assets.py
cmake -S runtime -B build/metal -G Ninja \
  -DMH3_AURORA_ROOT="${AURORA_ROOT:-$PWD/../Wiicompiled/aurora-main}" \
  -DFETCHCONTENT_BASE_DIR="$PWD/build/aurora/_deps" \
  -DCMAKE_AR=/usr/bin/ar -DCMAKE_RANLIB=/usr/bin/ranlib \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DAURORA_DAWN_PROVIDER=package -DAURORA_SDL3_PROVIDER=auto -DAURORA_ENABLE_CARD=OFF \
  -DCMAKE_PROJECT_aurora_INCLUDE="$PWD/scripts/aurora-compat.cmake" > logs/aurora-configure.log 2>&1
cmake --build build/metal --target MH3 --parallel "${BUILD_JOBS:-6}" > logs/aurora-build.log 2>&1
