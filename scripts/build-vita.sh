#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
task_build_dir=${MH3_VITA_BUILD_DIR:-build/vita}
task_sdk=${VITASDK:-/usr/local/vitasdk}
task_args=()
# A corrected converter registers real ARM exception-table ranges. Keep the
# override local to this build instead of replacing tools in the system SDK.
if [[ -n "${MH3_VITA_ELF_CREATE:-}" ]]; then
  task_args+=("-DVITA_ELF_CREATE:PATH=$MH3_VITA_ELF_CREATE")
fi
# Reuse the ARM64 renderer capture build's source downloads when available.
# Each target still gets independent objects compiled for the proper ABI.
for task_dependency in abseil-cpp sdl xxhash fmt imgui sqlite3 zstd tracy; do
  task_source="$PWD/build/aurora-arm64/_deps/$task_dependency-src"
  if [[ -d "$task_source" ]]; then
    task_args+=("-DFETCHCONTENT_SOURCE_DIR_${task_dependency^^}=$task_source")
  fi
done
python3 scripts/prepare_audio_assets.py
cmake -S runtime -B "$task_build_dir" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/vita-clang.cmake" \
  -DVITASDK="$task_sdk" -DCMAKE_BUILD_TYPE=Release \
  -DMH3_AURORA_ROOT="$PWD/aurora-main" \
  -DMH3_DEVELOPMENT="${MH3_DEVELOPMENT:-ON}" \
  -DAURORA_GXM=ON -DAURORA_ENABLE_CARD=OFF \
  -DAURORA_SDL3_PROVIDER=vendor -DAURORA_SDL3_LINKAGE=static \
  -DBUILD_SHARED_LIBS=OFF "${task_args[@]}"
cmake --build "$task_build_dir" --target mh3-vita.vpk --parallel "${BUILD_JOBS:-$(nproc)}"
python3 scripts/vita-check-unwind.py \
  --elf "$task_build_dir/MH3" --velf "$task_build_dir/MH3.velf" \
  --build-dir "$task_build_dir" --report "$task_build_dir/unwind-packaging.json"
echo "Built $task_build_dir/mh3-vita.vpk (clang-21, native GXM, validated unwind tables)"
