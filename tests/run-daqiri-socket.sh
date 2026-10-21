#!/bin/sh
# Real installed DAQIRI dependency required; never substitutes API mocks.
set -eu
: "${DAQIRI_PREFIX:?Set DAQIRI_PREFIX to the real DAQIRI installation}"
CUDA_HOME=${CUDA_HOME:-/usr/local/cuda}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
${CXX:-c++} -std=c++17 -pthread -g ${DAQIRI_TEST_CXXFLAGS:--fsanitize=address,undefined} \
 -I"$root" -I"$DAQIRI_PREFIX/include" -I"$CUDA_HOME/include" \
 "$root/daqiri-stream.cpp" "$root/daqiri-transport.cpp" "$root/tests/daqiri-socket.cpp" \
 -L"$DAQIRI_PREFIX/lib" -Wl,-rpath,"$DAQIRI_PREFIX/lib" -ldaqiri -o "$build/test"
LD_LIBRARY_PATH="$DAQIRI_PREFIX/lib:$CUDA_HOME/lib64:${LD_LIBRARY_PATH:-}" \
 ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1}" timeout 30 "$build/test" "$root/tests/daqiri-socket.yaml"
