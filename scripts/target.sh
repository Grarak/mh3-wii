# Sourced by the build and run scripts. MH3 targets arm32 (armhf) and arm64
# Linux with Vulkan. MH3_ARCH selects the target (default arm32). When the
# build machine's architecture differs, the scripts cross-compile with the
# matching toolchain file; scripts/docker-build.sh provides an arm32 cross
# environment.
MH3_ARCH=${MH3_ARCH:-arm32}
case $MH3_ARCH in
  arm32) mh3_native_arch='^(armv7l|armv8l|arm)$' ;;
  arm64) mh3_native_arch='^(aarch64|arm64)$' ;;
  *) echo "MH3_ARCH must be arm32 or arm64, not $MH3_ARCH" >&2; exit 1 ;;
esac
# The toolchain files choose the compiler (arm32: clang-21, override with
# MH3_CLANG); native builds use MH3_CLANG or clang from PATH.
MH3_CROSS_ARGS=()
MH3_COMPILER_ARGS=()
# arm32 always uses its toolchain file, which also carries the Cortex-A9
# code-generation flags, so native armhf builds match cross builds.
if [[ $MH3_ARCH == arm32 || $(uname -s) != Linux || ! $(uname -m) =~ $mh3_native_arch ]]; then
  MH3_CROSS_ARGS=(-DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/linux-$MH3_ARCH.cmake")
else
  mh3_clang=${MH3_CLANG:-clang}
  MH3_COMPILER_ARGS=(-DCMAKE_C_COMPILER="$mh3_clang" -DCMAKE_CXX_COMPILER="${mh3_clang/clang/clang++}")
fi
MH3_BUILD_DIR=${MH3_BUILD_DIR:-build/linux-$MH3_ARCH}
