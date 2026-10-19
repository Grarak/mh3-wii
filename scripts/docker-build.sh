#!/bin/bash
# Run a command inside the arm32 (armhf) cross-compilation container, e.g.
#   ./scripts/docker-build.sh ./scripts/build-aurora.sh
# The checkout is mounted at the same absolute path because translated data
# blobs are assembled with absolute .incbin paths.
set -euo pipefail
cd "$(dirname "$0")/.."
image=${MH3_DOCKER_IMAGE:-mh3-armhf-cross}
if ! docker image inspect "$image" > /dev/null 2>&1; then
  docker build -t "$image" -f docker/armhf-cross.Dockerfile docker
fi
mkdir -p build/.docker-home
tty_args=()
if [[ -t 0 && -t 1 ]]; then tty_args=(-it); fi
exec docker run --rm "${tty_args[@]}" -u "$(id -u):$(id -g)" \
  -e HOME="$PWD/build/.docker-home" -e MH3_ARCH="${MH3_ARCH:-arm32}" \
  -e DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 \
  -e BUILD_JOBS -e MH3_BUILD_DIR -e MH3_EXTRA_CMAKE_ARGS \
  -e MH3_CLANG -e MH3_ARM32_CPU -e MH3_ARM32_FPU \
  -v "$PWD:$PWD" -w "$PWD" "$image" "$@"
