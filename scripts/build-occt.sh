#!/usr/bin/env bash
# Builds static OCCT libraries for wasm.
# Usage: build-occt.sh <occt-src> <build-dir> <install-dir>
# Requires an activated emsdk (emcc, emcmake on PATH).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/config-occt.sh"

SRC="$(cd "$1" && pwd)"
BUILD="$2"
INSTALL="$3"
JOBS="${JOBS:-$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu)}"

# Map absolute source paths to stable prefixes so that local and Docker builds
# embed identical __FILE__ strings.
PREFIX_MAP="-ffile-prefix-map=$SRC=/occt"
FLAGS="$OCCT_CXX_FLAGS $PREFIX_MAP"

mkdir -p "$BUILD" "$INSTALL"
emcmake cmake -S "$SRC" -B "$BUILD" -G "Unix Makefiles" -Wno-dev \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_LIBRARY_TYPE=Static \
  -DINSTALL_DIR="$INSTALL" \
  -DBUILD_MODULE_FoundationClasses=OFF \
  -DBUILD_MODULE_ModelingData=OFF \
  -DBUILD_MODULE_ModelingAlgorithms=OFF \
  -DBUILD_MODULE_Visualization=OFF \
  -DBUILD_MODULE_ApplicationFramework=OFF \
  -DBUILD_MODULE_DataExchange=OFF \
  -DBUILD_MODULE_Draw=OFF \
  -DBUILD_ADDITIONAL_TOOLKITS="$OCCT_TOOLKITS" \
  -DUSE_FREETYPE=OFF -DUSE_TK=OFF -DUSE_RAPIDJSON=OFF -DUSE_DRACO=OFF -DUSE_TBB=OFF \
  -DUSE_FREEIMAGE=OFF -DUSE_OPENVR=OFF -DUSE_FFMPEG=OFF -DUSE_VTK=OFF -DUSE_GLES2=OFF \
  -DBUILD_GTEST=OFF -DBUILD_DOC_Overview=OFF \
  -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS"

cmake --build "$BUILD" -j "$JOBS"
cmake --install "$BUILD" >/dev/null

# Record what these libraries were built from.
{
  echo "OCCT_SHA=$OCCT_SHA"
  echo "OCCT_TOOLKITS=$OCCT_TOOLKITS"
  echo "OCCT_CXX_FLAGS=$OCCT_CXX_FLAGS"
  echo "EMCC=$(emcc --version | head -1)"
} > "$INSTALL/occt-wasm-build.stamp"
echo "OCCT installed to $INSTALL"
