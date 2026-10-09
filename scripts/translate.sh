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
python3 scripts/prepare_rso_translation.py
"$dotnet_bin" "$translator" translate-recursive 0x80006310 \
    --project build/recomp-analysis.yml --outdir generated/functions \
    --output-metadata generated/translation.json --threads 8 --prune-stale 2>&1 | tee logs/translate.log
"$dotnet_bin" "$translator" emit-build-shards --project recomp.yml \
    --base-metadata generated/translation.json --base-functions-dir generated/functions \
    --native-source-dir runtime/src --out generated/build_shards 2>&1 | tee logs/shards.log
python3 scripts/translate_rso_overlays.py 2>&1 | tee logs/rso-overlays.log

python3 scripts/translate_rso_overlays.py --profile quest_field 2>&1 | tee logs/quest-field-translate.log

python3 scripts/translate_rso_overlays.py --profile enemy_field 2>&1 | tee logs/enemy-field-translate.log

python3 scripts/translate_rso_overlays.py --profile map01_field 2>&1 | tee logs/map01-field-translate.log

python3 scripts/translate_rso_overlays.py --profile map00_field 2>&1 | tee logs/map00-field-translate.log

python3 scripts/translate_rso_overlays.py --profile map02_field 2>&1 | tee logs/map02-field-translate.log

python3 scripts/translate_rso_overlays.py --profile map03_field 2>&1 | tee logs/map03-field-translate.log

python3 scripts/translate_rso_overlays.py --profile map04_field 2>&1 | tee logs/map04-field-translate.log

python3 scripts/translate_rso_overlays.py --profile map05_field 2>&1 | tee logs/map05-field-translate.log

python3 scripts/translate_rso_overlays.py --profile map06_field 2>&1 | tee logs/map06-field-translate.log

python3 scripts/translate_rso_overlays.py --profile map07_field 2>&1 | tee logs/map07-field-translate.log

python3 scripts/translate_rso_overlays.py --profile map08_field 2>&1 | tee logs/map08-field-translate.log

python3 scripts/translate_rso_overlays.py --profile map09_field 2>&1 | tee logs/map09-field-translate.log

python3 scripts/translate_rso_overlays.py --profile map10_field 2>&1 | tee logs/map10-field-translate.log

python3 scripts/translate_rso_overlays.py --profile map11_field 2>&1 | tee logs/map11-field-translate.log
