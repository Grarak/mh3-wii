#!/bin/bash
# Build the Vulkan executable for arm32 or arm64 Linux (MH3_ARCH, default
# arm32): build/linux-$MH3_ARCH/MH3.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/target.sh
# arm64 uses Aurora's pinned prebuilt linux-aarch64 Dawn package (with Vulkan).
# To use a local Dawn build instead: AURORA_DAWN_PROVIDER=system DAWN_DIR=<install prefix>.
default_dawn_provider=package
dawn_args=()
if [[ -n ${DAWN_DIR:-} ]]; then dawn_args=(-DCMAKE_PREFIX_PATH="$DAWN_DIR"); fi
mkdir -p logs
python3 scripts/prepare_audio_assets.py
cmake -S runtime -B "$MH3_BUILD_DIR" -G Ninja "${MH3_CROSS_ARGS[@]}" "${dawn_args[@]}" \
  -DMH3_AURORA_ROOT="${AURORA_ROOT:-$PWD/aurora-main}" \
  -DFETCHCONTENT_BASE_DIR="$PWD/build/aurora-$MH3_ARCH/_deps" \
  -DCMAKE_BUILD_TYPE=Release "${MH3_COMPILER_ARGS[@]}" \
  -DAURORA_DAWN_PROVIDER="${AURORA_DAWN_PROVIDER:-$default_dawn_provider}" -DAURORA_SDL3_PROVIDER=auto -DAURORA_ENABLE_CARD=OFF \
  -DCMAKE_PROJECT_aurora_INCLUDE="$PWD/scripts/aurora-compat.cmake" > logs/aurora-configure.log 2>&1
cmake --build "$MH3_BUILD_DIR" --target MH3 MH3ControllerHostTest --parallel "${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}" > logs/aurora-build.log 2>&1
echo "Built $MH3_BUILD_DIR/MH3 ($MH3_ARCH Linux, Vulkan)"
