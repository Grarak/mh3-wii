#!/bin/bash
# CPU-only runtime build (no Aurora/Vulkan): build/linux-$MH3_ARCH-cpu/MH3.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/target.sh
build_dir=${MH3_BUILD_DIR%/}-cpu
mkdir -p logs
python3 scripts/prepare_audio_assets.py
cmake -S runtime -B "$build_dir" -G Ninja "${MH3_CROSS_ARGS[@]}" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo "${MH3_COMPILER_ARGS[@]}" 2>&1 | tee logs/cmake.log
cmake --build "$build_dir" --parallel "${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}" 2>&1 | tee logs/native-build.log
if (( ${#MH3_CROSS_ARGS[@]} == 0 )); then "$build_dir/MH3" --self-test; fi
