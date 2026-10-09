#!/usr/bin/env bash
# Offline Sony compilation. The installed game uses only the embedded GXP blob.
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
docker run --rm -v "$task_sony_tools:/sdk:ro" -v "$task_cg_dir:/shaders" \
  -e WINEDEBUG=-all psp2cgc-wine bash -c '
    status=0
    for source in /shaders/*.cg; do
      binary=${source%.cg}.gxp
      if [ -f "$binary" ] && [ "$binary" -nt "$source" ]; then continue; fi
      case "$source" in *_v.cg) profile=sce_vp_psp2;; *_f.cg) profile=sce_fp_psp2;; *) exit 2;; esac
      if ! wine /sdk/bin/psp2cgc.exe -profile "$profile" "$source" -o "$binary"; then
        echo "FAILED: $source" >&2
        status=1
      fi
    done
    exit "$status"
  '
mkdir -p "$task_embed_dir"
python3 "$task_repo_dir/scripts/vita-shaders.py" "$task_cg_dir" "$task_embed_dir/shaders.cpp" --embed
