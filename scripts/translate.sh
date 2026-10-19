#!/bin/bash
# Translate the DOL and the RSO overlay profiles into generated/ by hand
# (scripts/translate.py). CMake runs the same at configure time, so a build
# translates by itself after a change of symbols.txt, recomp.yml, a site list,
# the translator's C# or a native port. Only the steps whose inputs changed
# run; --force runs them all. Logs in logs/.
set -euo pipefail
cd "$(dirname "$0")/.."
ulimit -c 0
exec python3 scripts/translate.py "$@"
