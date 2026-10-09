#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd "$(dirname "$0")/.." && pwd)
task_sdk=${VITASDK:-/usr/local/vitasdk}
task_target=${1:-"$task_root/build/vita-rom-decoder"}
export CC_armv7_sony_vita_newlibeabihf=clang-21
export AR_armv7_sony_vita_newlibeabihf=llvm-ar-21
export CFLAGS_armv7_sony_vita_newlibeabihf="--target=armv7a-none-eabihf --sysroot=$task_sdk/arm-vita-eabi -mcpu=cortex-a9 -mfpu=neon -mthumb -O3"
if [[ ${MH3_ROM_DECODER_UNWIND:-OFF} == ON ]]; then
    export CFLAGS_armv7_sony_vita_newlibeabihf="$CFLAGS_armv7_sony_vita_newlibeabihf -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -mframe-chain=aapcs+leaf -funwind-tables"
    export RUSTFLAGS="${RUSTFLAGS:-} -Cforce-frame-pointers=yes -Cforce-unwind-tables=yes"
fi
export CARGO_TARGET_DIR="$task_target"
# The decoder's C ABI is linked by the C++ application; it has no Rust entry
# point and never starts a Rayon pool (hash reconstruction is disabled).
cargo +nightly-2025-09-28 build --manifest-path "$task_root/runtime/vita/rom_decoder/Cargo.toml" \
  --locked --offline --release --target armv7-sony-vita-newlibeabihf \
  -Zbuild-std=std,panic_abort --jobs "$(nproc)"
