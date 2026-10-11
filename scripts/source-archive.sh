#!/usr/bin/env bash
# Creates a tarball with the complete corresponding source: this repository
# at HEAD plus the pinned OCCT submodule (without OCCT test data and docs,
# which the build does not use).
# Usage: scripts/source-archive.sh <out.tar.gz>
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

PREFIX="occt-wasm"
git -C "$ROOT" archive --format=tar --prefix="$PREFIX/" HEAD > "$TMP/main.tar"
git -C "$ROOT/third_party/occt" archive --format=tar --prefix="$PREFIX/third_party/occt/" HEAD \
  adm src resources CMakeLists.txt LICENSE_LGPL_21.txt OCCT_LGPL_EXCEPTION.txt README.md > "$TMP/occt.tar"
mkdir "$TMP/x"
tar -xf "$TMP/main.tar" -C "$TMP/x"
tar -xf "$TMP/occt.tar" -C "$TMP/x"
tar -czf "$OUT" -C "$TMP/x" "$PREFIX"
ls -l "$OUT"
