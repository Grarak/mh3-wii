#!/bin/bash
# Build clang's profile runtime (compiler-rt lib/profile) for arm32 Linux,
# which Debian's clang-21 packages leave out, into
# build/pgo/libclang_rt.profile-armhf.a for MH3_PGO_RUNTIME (docs/optimizations/cpu/pgo-and-thinlto.md).
# The source is the compiler-rt release of the compiler's own version, so the
# raw profile format matches. Run it inside scripts/docker-build.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
clang=${MH3_CLANG:-clang-21}
version=$("$clang" -dumpversion)
out=$PWD/build/pgo
src=$out/compiler-rt-$version.src
mkdir -p "$out/obj"

if [[ ! -d $src ]]; then
  # The container has no xz; Python's tarfile reads the archive.
  curl -sSLf -o "$out/compiler-rt.tar.xz" "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/compiler-rt-$version.src.tar.xz"
  python3 -c 'import sys, tarfile; tarfile.open(sys.argv[1]).extractall(sys.argv[2], filter="data")' "$out/compiler-rt.tar.xz" "$out"
  rm "$out/compiler-rt.tar.xz"
fi

flags=(--target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -O2 -fPIC
  -fno-stack-protector -fvisibility=hidden -fno-builtin -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64
  -DCOMPILER_RT_HAS_ATOMICS=1 -DCOMPILER_RT_HAS_FCNTL_LCK=1 -DCOMPILER_RT_HAS_FLOCK=1 -DCOMPILER_RT_HAS_UNAME=1
  -I"$src/include" -I"$src/lib")
objects=()

for name in GCDAProfiling InstrProfiling InstrProfilingInternal InstrProfilingValue InstrProfilingBuffer InstrProfilingFile \
  InstrProfilingMerge InstrProfilingMergeFile InstrProfilingNameVar InstrProfilingVersionVar InstrProfilingWriter \
  InstrProfilingPlatformLinux InstrProfilingPlatformOther InstrProfilingUtil; do
  "$clang" "${flags[@]}" -std=c11 -D_GNU_SOURCE -c "$src/lib/profile/$name.c" -o "$out/obj/$name.o"
  objects+=("$out/obj/$name.o")
done

"${clang/clang/clang++}" "${flags[@]}" -fno-exceptions -fno-rtti -nostdinc++ -c "$src/lib/profile/InstrProfilingRuntime.cpp" -o "$out/obj/InstrProfilingRuntime.o"
objects+=("$out/obj/InstrProfilingRuntime.o")
rm -f "$out/libclang_rt.profile-armhf.a"
"llvm-ar${clang#clang}" rcs "$out/libclang_rt.profile-armhf.a" "${objects[@]}"
echo "Built $out/libclang_rt.profile-armhf.a (compiler-rt $version)"
