# Builds the occt-wasm module (facade + static OCCT libraries).
# Configure with emcmake; pass OCCT_INSTALL_DIR, FACADE_CXX_FLAGS,
# FACADE_LINK_FLAGS and OCCT_WASM_FACADE_VERSION (see scripts/build-facade.sh).
cmake_minimum_required(VERSION 3.20)
project(occt_wasm_facade CXX)

if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "Configure with emcmake")
endif()

set(OCCT_INSTALL_DIR "" CACHE PATH "OCCT install prefix")
set(FACADE_CXX_FLAGS "" CACHE STRING "Facade compile flags")
set(FACADE_LINK_FLAGS "" CACHE STRING "Module link flags")
set(OCCT_WASM_FACADE_VERSION "0.0.0-dev" CACHE STRING "Facade version")

file(GLOB OCCT_LIBS "${OCCT_INSTALL_DIR}/lib/libTK*.a")
list(SORT OCCT_LIBS)
if(NOT OCCT_LIBS)
  message(FATAL_ERROR "No OCCT libraries in ${OCCT_INSTALL_DIR}/lib")
endif()

file(GLOB FACADE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(SORT FACADE_SOURCES)

add_executable(occt-wasm ${FACADE_SOURCES})
set_target_properties(occt-wasm PROPERTIES SUFFIX ".mjs")
target_include_directories(occt-wasm PRIVATE "${OCCT_INSTALL_DIR}/include/opencascade")
target_compile_definitions(occt-wasm PRIVATE OCCT_WASM_FACADE_VERSION="${OCCT_WASM_FACADE_VERSION}")

separate_arguments(_cxx UNIX_COMMAND "${FACADE_CXX_FLAGS}")
separate_arguments(_link UNIX_COMMAND "${FACADE_LINK_FLAGS}")
target_compile_options(occt-wasm PRIVATE ${_cxx}
  "-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=/facade"
  "-ffile-prefix-map=${OCCT_INSTALL_DIR}=/occt-install")
target_link_options(occt-wasm PRIVATE ${_link})
target_link_libraries(occt-wasm PRIVATE ${OCCT_LIBS})
