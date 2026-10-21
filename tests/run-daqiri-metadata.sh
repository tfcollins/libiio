#!/bin/sh
# NIC-free production adapter seam; requires real headers/library and pinned source.
set -eu
: "${DAQIRI_PREFIX:?Set DAQIRI_PREFIX to the real DAQIRI installation}"
: "${DAQIRI_SOURCE:?Set DAQIRI_SOURCE to pinned NVIDIA DAQIRI source}"
CUDA_HOME=${CUDA_HOME:-/usr/local/cuda}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
${CXX:-c++} -std=c++17 -pthread -g -Wall -Wextra -Werror \
 ${DAQIRI_TEST_CXXFLAGS:--fsanitize=address,undefined} \
 -I"$root" -I"$DAQIRI_SOURCE" -isystem "$DAQIRI_PREFIX/include" -isystem "$CUDA_HOME/include" \
 "$root/tests/daqiri-metadata.cpp" -L"$DAQIRI_PREFIX/lib" \
 -Wl,-rpath,"$DAQIRI_PREFIX/lib" -ldaqiri -o "$build/test"
LD_LIBRARY_PATH="$DAQIRI_PREFIX/lib:$CUDA_HOME/lib64:${LD_LIBRARY_PATH:-}" \
 ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1}" timeout 30 "$build/test"
