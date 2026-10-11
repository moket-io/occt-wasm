#!/usr/bin/env bash
# Collects the linked module, loader, types and license files into dist/.
# Usage: assemble-dist.sh <facade-build-dir> <dist-dir>
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD="$1"
DIST="$2"

rm -rf "$DIST"
mkdir -p "$DIST"
cp "$BUILD/occt-wasm.mjs" "$BUILD/occt-wasm.wasm" "$DIST/"
cp "$BUILD/occt-wasm.d.ts" "$DIST/occt-wasm.generated.d.ts"
cp "$ROOT/js/index.mjs" "$ROOT/js/index.d.ts" "$DIST/"
cp "$ROOT/LICENSE" "$ROOT/OCCT_LGPL_EXCEPTION.txt" "$ROOT/NOTICE" "$ROOT/versions.json" "$DIST/"
(
  cd "$DIST"
  if command -v sha256sum >/dev/null; then SHA="sha256sum"; else SHA="shasum -a 256"; fi
  $SHA $(ls | grep -v '^SHA256SUMS$' | sort) > SHA256SUMS
)
ls -l "$DIST"
