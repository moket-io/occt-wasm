#!/usr/bin/env bash
# Reference build in Docker. Produces dist/.
# Extra arguments go to `docker buildx build` (e.g. --cache-from/--cache-to in CI).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

if [ ! -f "$ROOT/third_party/occt/CMakeLists.txt" ]; then
  git -C "$ROOT" submodule update --init --depth 1 third_party/occt
fi

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
START=$(date +%s)
docker buildx build \
  -f "$ROOT/docker/Dockerfile" \
  --target dist \
  --output "type=local,dest=$OUT" \
  "$@" \
  "$ROOT"
rm -rf "$ROOT/dist"
mkdir -p "$ROOT/dist"
cp -R "$OUT"/. "$ROOT/dist/"
echo "Docker build: $(( $(date +%s) - START )) s"
node "$HERE/size-report.mjs"
