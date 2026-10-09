#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
task_build_dir=${MH3_VITA_BUILD_DIR:-build/vita}
task_sdk=${VITASDK:-/usr/local/vitasdk}
task_args=()
# The patched converter (/path/to/vita-toolchain, or MH3_VITA_ELF_CREATE)
# registers the ARM exception-table ranges in the module info, which the
# Vita's own stack walker needs (vprof's flat-profile stacks, crash
# backtraces). Without it the SDK's stock converter builds the eboot without
# them, and the unwind check is skipped. The override stays local to this
# build instead of replacing tools in the system SDK.
task_elf_create=${MH3_VITA_ELF_CREATE:-/path/to/vita-toolchain/build-unwind/src/vita-elf-create}
task_unwind=0
if [[ -x "$task_elf_create" ]]; then
  task_unwind=1
else
  echo "No patched vita-elf-create ($task_elf_create): the stock one, without registered unwind tables" >&2
  task_elf_create="$task_sdk/bin/vita-elf-create"
fi
task_args+=("-DVITA_ELF_CREATE:PATH=$task_elf_create")
# Reuse the ARM64 renderer capture build's source downloads when available.
# Each target still gets independent objects compiled for the proper ABI.
for task_dependency in abseil-cpp sdl xxhash fmt imgui sqlite3 zstd; do
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
if [[ $task_unwind == 1 ]]; then
  python3 scripts/vita-check-unwind.py \
    --elf "$task_build_dir/MH3" --velf "$task_build_dir/MH3.velf" \
    --build-dir "$task_build_dir" --report "$task_build_dir/unwind-packaging.json"
  echo "Built $task_build_dir/mh3-vita.vpk (clang-21, native GXM, validated unwind tables)"
else
  echo "Built $task_build_dir/mh3-vita.vpk (clang-21, native GXM, no registered unwind tables)"
fi
