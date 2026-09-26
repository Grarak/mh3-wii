#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p logs
cmake -S runtime -B build/native -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++ 2>&1 | tee logs/cmake.log
cmake --build build/native --parallel "${BUILD_JOBS:-6}" 2>&1 | tee logs/native-build.log
build/native/MH3 --self-test
