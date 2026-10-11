#!/usr/bin/env bash
# Local (non-Docker) build for fast iteration. Docker (scripts/build.sh) is the
# reference build for releases.
#
# Uses $EMSDK if it points to an activated emsdk of the pinned version,
# otherwise installs emsdk into .cache/emsdk. OCCT is built once into
# .cache/occt-install and rebuilt only when its inputs change.
#
# Usage: scripts/build-local.sh [--clean-occt]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
source "$HERE/config.sh"

CACHE="${OCCT_WASM_CACHE:-$ROOT/.cache}"
mkdir -p "$CACHE"

if [ ! -f "$ROOT/third_party/occt/CMakeLists.txt" ]; then
  git -C "$ROOT" submodule update --init --depth 1 third_party/occt
fi

if ! command -v emcc >/dev/null || ! emcc --version | head -1 | grep -q " $EMSDK_VERSION "; then
  if [ ! -d "$CACHE/emsdk" ]; then
    git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$CACHE/emsdk"
  fi
  "$CACHE/emsdk/emsdk" install "$EMSDK_VERSION"
  "$CACHE/emsdk/emsdk" activate "$EMSDK_VERSION" >/dev/null
  # shellcheck disable=SC1091
  source "$CACHE/emsdk/emsdk_env.sh" >/dev/null 2>&1
fi
emcc --version | head -1

OCCT_INSTALL="$CACHE/occt-install"
STAMP_WANTED="$(printf 'OCCT_SHA=%s\nOCCT_TOOLKITS=%s\nOCCT_CXX_FLAGS=%s\nEMCC=%s\n' \
  "$OCCT_SHA" "$OCCT_TOOLKITS" "$OCCT_CXX_FLAGS" "$(emcc --version | head -1)")"
if [ "${1:-}" = "--clean-occt" ] || [ "$(cat "$OCCT_INSTALL/occt-wasm-build.stamp" 2>/dev/null)" != "$STAMP_WANTED" ]; then
  rm -rf "$OCCT_INSTALL" "$CACHE/occt-build"
  START=$(date +%s)
  "$HERE/build-occt.sh" "$ROOT/third_party/occt" "$CACHE/occt-build" "$OCCT_INSTALL"
  echo "OCCT build: $(( $(date +%s) - START )) s"
  # The install tree is all we need; the object files are ~0.8 GB.
  rm -rf "$CACHE/occt-build"
fi

START=$(date +%s)
"$HERE/build-facade.sh" "$OCCT_INSTALL" "$CACHE/facade-build" "$ROOT/dist"
echo "Facade build: $(( $(date +%s) - START )) s"
node "$HERE/size-report.mjs"
