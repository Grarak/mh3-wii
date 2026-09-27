# Sourced by the build and run scripts. MH3 targets arm64 Linux with Vulkan.
# On any other machine the scripts cross-compile with the arm64 toolchain file.
MH3_CROSS_ARGS=()
if [[ $(uname -s) != Linux || ! $(uname -m) =~ ^(aarch64|arm64)$ ]]; then
  MH3_CROSS_ARGS=(-DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/linux-arm64.cmake")
fi
MH3_BUILD_DIR=${MH3_BUILD_DIR:-build/linux-arm64}
