// embind registrations and module-level entry points.
//
// SPDX-License-Identifier: LGPL-2.1-only
#include "api.hpp"
#include "common.hpp"

#include <emscripten/bind.h>
#include <emscripten/heap.h>
#include <emscripten/version.h>

#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Standard_Version.hxx>

#include <malloc.h>

#ifndef OCCT_WASM_FACADE_VERSION
#define OCCT_WASM_FACADE_VERSION "0.0.0-dev"
#endif

namespace occtwasm {

#define OCCT_WASM_STR2(x) #x
#define OCCT_WASM_STR(x) OCCT_WASM_STR2(x)

val version()
{
  val r = val::object();
  r.set("occt", std::string(OCC_VERSION_COMPLETE));
  r.set("facade", std::string(OCCT_WASM_FACADE_VERSION));
  r.set("emscripten",
        std::string(OCCT_WASM_STR(__EMSCRIPTEN_MAJOR__) "." OCCT_WASM_STR(
          __EMSCRIPTEN_MINOR__) "." OCCT_WASM_STR(__EMSCRIPTEN_TINY__)));
  return r;
}

val heapStats()
{
  struct mallinfo mi = mallinfo();
  val             r  = val::object();
  r.set("heapBytes", static_cast<double>(emscripten_get_heap_size()));
  r.set("usedBytes", static_cast<double>(mi.uordblks));
  r.set("liveShapes", static_cast<double>(arena().size()));
  return r;
}

val release(val id)
{
  return guarded([&] {
    const ShapeId sid = toShapeId(id, "id");
    if (!arena().release(sid))
    {
      fail("INVALID_HANDLE", "Unknown or released shape id " + std::to_string(sid));
    }
    return okResult();
  });
}

val releaseAll()
{
  return guarded([&] {
    val r = okResult();
    r.set("released", static_cast<double>(arena().releaseAll()));
    return r;
  });
}

namespace {

// OCCT prints algorithm traces (STEP statistics, etc.) to std::cout through
// the default messenger. Drop them: callers get structured results instead.
struct SilenceMessenger
{
  SilenceMessenger()
  {
    const occ::handle<Message_Messenger>& m = Message::DefaultMessenger();
    m->ChangePrinters().Clear();
  }
};

SilenceMessenger silenceMessenger;

} // namespace

} // namespace occtwasm

EMSCRIPTEN_BINDINGS(occt_wasm)
{
  using namespace emscripten;
  using namespace occtwasm;

  function("version", &version);
  function("heapStats", &heapStats);
  function("release", &release);
  function("releaseAll", &releaseAll);

  function("makeBox", &makeBox);
  function("makeCylinder", &makeCylinder);
  function("makeVertex", &makeVertex);
  function("subShape", &subShape);
  function("makeProfileFace", &makeProfileFace);
  function("makeWire", &makeWire);
  function("joinEdges", &joinEdges);

  function("extrude", &extrude);
  function("revolve", &revolve);
  function("boolean", &booleanOp);
  function("fillet", &fillet);
  function("chamfer", &chamfer);
  function("transform", &transform);
  function("thickSolid", &thickSolid);
  function("offsetShape", &offsetShape);
  function("draft", &draft);
  function("sweep", &sweep);
  function("loft", &loft);

  function("topology", &topology);
  function("properties", &properties);
  function("mesh", &mesh);
  function("check", &check);
  function("distance", &distance);

  function("project", &project);

  function("exportStep", &exportStep);
  function("importStep", &importStep);
  function("importStepParts", &importStepParts);
  function("exportBrep", &exportBrep);
  function("importBrep", &importBrep);
  function("exportStepAssembly", &exportStepAssembly);
}
