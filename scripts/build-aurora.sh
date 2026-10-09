#!/bin/bash
# Build the Vulkan executable for arm64 Linux: build/linux-arm64/MH3.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/target.sh
# Aurora's pinned prebuilt linux-aarch64 Dawn package includes Vulkan. To use a
# local Dawn build instead: AURORA_DAWN_PROVIDER=system DAWN_DIR=<install prefix>.
dawn_args=()
if [[ -n ${DAWN_DIR:-} ]]; then dawn_args=(-DCMAKE_PREFIX_PATH="$DAWN_DIR"); fi
mkdir -p logs
python3 scripts/prepare_audio_assets.py
cmake -S runtime -B "$MH3_BUILD_DIR" -G Ninja "${MH3_CROSS_ARGS[@]}" "${dawn_args[@]}" \
  -DMH3_AURORA_ROOT="${AURORA_ROOT:-$PWD/aurora-main}" \
  -DFETCHCONTENT_BASE_DIR="$PWD/build/aurora-arm64/_deps" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DAURORA_DAWN_PROVIDER="${AURORA_DAWN_PROVIDER:-package}" -DAURORA_SDL3_PROVIDER=auto -DAURORA_ENABLE_CARD=OFF \
  -DCMAKE_PROJECT_aurora_INCLUDE="$PWD/scripts/aurora-compat.cmake" > logs/aurora-configure.log 2>&1
cmake --build "$MH3_BUILD_DIR" --target MH3 MH3ControllerHostTest --parallel "${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}" > logs/aurora-build.log 2>&1
echo "Built $MH3_BUILD_DIR/MH3 (arm64 Linux, Vulkan)"
