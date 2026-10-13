# Build inputs, sourced by every build script (local and Docker).
# Keep versions.json in sync (scripts/check-versions.mjs).

source "$(dirname "${BASH_SOURCE[0]}")/config-occt.sh"

EMSDK_VERSION="6.0.12"
FACADE_VERSION="0.4.0"

FACADE_CXX_FLAGS="-std=c++17 -O3 -fwasm-exceptions -msimd128"

FACADE_LINK_FLAGS="-lembind -fwasm-exceptions -msimd128 -O3 -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createOcctModule -sENVIRONMENT=web,worker,node -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=8MB -sEXPORTED_RUNTIME_METHODS=HEAPU8 --no-entry --emit-tsd occt-wasm.d.ts"
