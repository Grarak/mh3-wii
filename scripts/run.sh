#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ ! -x build/metal/MH3 ]]; then
  echo 'Build the native Metal executable first: ./scripts/build-aurora.sh' >&2
  exit 1
fi
exec ./build/metal/MH3 "$@"
