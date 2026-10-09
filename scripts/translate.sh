#!/bin/bash
# Reproduce the current strict translation attempt. This does not build a playable port.
set -euo pipefail
cd "$(dirname "$0")/.."
ulimit -c 0
wiicompiled_root="${WIICOMPILED_ROOT:-toolchain}"
dotnet_bin="${DOTNET_BIN:-$PWD/tools/dotnet/dotnet}"
translator="$wiicompiled_root/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll"
mkdir -p logs generated
python3 scripts/inspect_disc.py
python3 scripts/generate_symbols.py
"$dotnet_bin" "$translator" generate-data-init --project recomp.yml 2>&1 | tee logs/data-init.log
"$dotnet_bin" "$translator" translate-recursive 0x80006310 \
    --project recomp.yml --outdir generated/functions \
    --output-metadata generated/translation.json --threads 8 --prune-stale 2>&1 | tee logs/translate.log
"$dotnet_bin" "$translator" emit-build-shards --project recomp.yml \
    --base-metadata generated/translation.json --base-functions-dir generated/functions \
    --native-source-dir runtime/src --out generated/build_shards 2>&1 | tee logs/shards.log
