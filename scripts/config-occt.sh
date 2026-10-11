# Inputs of the OCCT library build. Changing anything here invalidates the
# cached OCCT layer (Docker) and the local .cache/occt-install.
# Keep versions.json in sync (scripts/check-versions.mjs).

OCCT_TAG="V8_0_1"
OCCT_SHA="b8f597c677811d1f9f4d8a97f5ae2825c0353a42"

# OCCT toolkits on top of FoundationClasses/ModelingData which are pulled in
# as dependencies. All BUILD_MODULE_* are OFF.
OCCT_TOOLKITS="TKPrim TKBO TKBool TKFillet TKOffset TKFeat TKMesh TKShHealing TKHLR TKDESTEP"

# -UOCC_CONVERT_SIGNALS is required: OCCT signal conversion relies on
# setjmp/longjmp, which together with wasm exceptions yields an invalid module
# once STEP and shape healing are linked.
OCCT_CXX_FLAGS="-fwasm-exceptions -msimd128 -DOCCT_NO_PLUGINS -DIGNORE_NO_ATOMICS=1 -UOCC_CONVERT_SIGNALS"
