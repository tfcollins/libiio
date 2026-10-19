#!/bin/sh
# Build and install the pinned DAQIRI revision into a prefix.
# Usage: build-daqiri.sh <absolute-prefix> [<engine-list>]
# Env:   DAQIRI_CMAKE_EXTRA  extra space-separated CMake arguments
set -eu

if [ $# -lt 1 ]; then
	echo "usage: $0 <absolute-prefix> [<engine-list>]" >&2
	exit 2
fi
prefix=$1
engines=${2:-}
case "$prefix" in
/*) ;;
*) echo "prefix must be an absolute path" >&2; exit 2 ;;
esac

here=$(cd "$(dirname "$0")" && pwd)
ref=$(cat "$here/DAQIRI_REF")
src=${TMPDIR:-/tmp}/daqiri-src-$ref

if [ ! -d "$src/.git" ]; then
	git clone https://github.com/NVIDIA/daqiri "$src"
fi
git -C "$src" checkout "$ref"
git -C "$src" submodule update --init --recursive

# shellcheck disable=SC2086
cmake -S "$src" -B "$src/build" \
	-DBUILD_SHARED_LIBS=ON \
	-DDAQIRI_BUILD_PYTHON=OFF \
	-DDAQIRI_BUILD_EXAMPLES=OFF \
	-DDAQIRI_ENGINE="$engines" \
	-DCMAKE_INSTALL_PREFIX="$prefix" \
	${DAQIRI_CMAKE_EXTRA:-}
cmake --build "$src/build" -j"$(nproc)"
cmake --install "$src/build"

PKG_CONFIG_PATH=$prefix/lib/pkgconfig pkg-config --modversion daqiri
