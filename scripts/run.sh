#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/target.sh
if [[ ! -x $MH3_BUILD_DIR/MH3 ]]; then
  echo "Build the $MH3_ARCH executable first: ./scripts/build-aurora.sh" >&2
  exit 1
fi
exec "./$MH3_BUILD_DIR/MH3" "$@"
