#!/usr/bin/env bash
# Compiles the facade, links the module and assembles dist/.
# Usage: build-facade.sh <occt-install-dir> <build-dir> <dist-dir>
# Requires an activated emsdk.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
source "$HERE/config.sh"

OCCT_INSTALL="$(cd "$1" && pwd)"
BUILD="$2"
DIST="$3"
JOBS="${JOBS:-$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu)}"

emcmake cmake -S "$ROOT/facade" -B "$BUILD" -G "Unix Makefiles" -Wno-dev \
  -DCMAKE_BUILD_TYPE=Release \
  -DOCCT_INSTALL_DIR="$OCCT_INSTALL" \
  -DFACADE_CXX_FLAGS="$FACADE_CXX_FLAGS" \
  -DFACADE_LINK_FLAGS="$FACADE_LINK_FLAGS ${FACADE_EXTRA_LINK_FLAGS:-}" \
  -DOCCT_WASM_FACADE_VERSION="$FACADE_VERSION" >/dev/null
cmake --build "$BUILD" -j "$JOBS"

"$HERE/assemble-dist.sh" "$BUILD" "$DIST"
