#!/usr/bin/env bash
# Offline compilation of the known GXP corpus; missing native stages can
# also use the user-authorized Vita runtime compiler/cache.
set -euo pipefail
if [ "$#" -ne 2 ]; then
  echo "Usage: $0 generated-cg-directory embed-output-directory" >&2
  exit 2
fi
task_cg_dir=$(realpath "$1")
task_embed_dir=$(realpath -m "$2")
task_repo_dir=$(cd "$(dirname "$0")/.." && pwd)
task_sony_tools=${VITA_SONY_HOST_TOOLS:-/path/to/psp2-sdk/host_tools}
cp "$task_repo_dir"/aurora-main/lib/gxm/utility/*.cg "$task_cg_dir/"
python3 "$task_repo_dir/scripts/vita-shaders.py" "$task_cg_dir" "$task_cg_dir" --scalar
docker run --rm -v "$task_sony_tools:/sdk:ro" -v "$task_cg_dir:/shaders" \
  -e WINEDEBUG=-all -e BUILD_JOBS="${BUILD_JOBS:-$(nproc)}" psp2cgc-wine bash -c '
    # Initialize the shared prefix before parallel compiler processes race to
    # create it in a fresh container. Compilation still uses all requested CPUs.
    wineboot -u || exit 1
    wineserver -w || exit 1
    compile_one() {
      source=$1
      binary=${source%.cg}.gxp
      options="-O3 -fastmath -nofastprecision -fastint"
      if [ -f "$binary" ] && [ "$binary" -nt "$source" ] &&
         [ -f "$binary.options" ] && [ "$(cat "$binary.options")" = "$options" ]; then return; fi
      case "$source" in *_v.cg) profile=sce_vp_psp2;; *_f.cg|*_r.cg) profile=sce_fp_psp2;; *) exit 2;; esac
      if ! wine /sdk/bin/psp2cgc.exe -O3 -fastmath -nofastprecision -fastint \
          -profile "$profile" "$source" -o "$binary.tmp"; then
        echo "FAILED: $source" >&2
        return 1
      fi
      mv "$binary.tmp" "$binary"
      printf "%s\n" "$options" > "$binary.options"
    }
    export -f compile_one
    printf "%s\0" /shaders/*.cg | xargs -0 -n 1 -P "$BUILD_JOBS" bash -c '\''compile_one "$1"'\'' _
  '
mkdir -p "$task_embed_dir"
python3 "$task_repo_dir/scripts/vita-shaders.py" "$task_cg_dir" "$task_embed_dir/shaders.cpp" --embed
