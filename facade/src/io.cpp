// STEP and BRep exchange through in-memory streams (no virtual file system).
//
// SPDX-License-Identifier: LGPL-2.1-only
#include "api.hpp"
#include "common.hpp"

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BinTools.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>

#include <sstream>

namespace occtwasm {

val exportStep(val shapes)
{
  return guarded([&] {
    const std::vector<int> ids = toInts(shapes, "shapes");
    if (ids.empty())
    {
      fail("INVALID_ARGUMENT", "shapes must not be empty");
    }
    STEPControl_Writer writer;
    for (int id : ids)
    {
      const TopoDS_Shape& s = arena().get(id);
      if (writer.Transfer(s, STEPControl_AsIs) != IFSelect_RetDone)
      {
        fail("STEP_WRITE_FAILED", "cannot transfer shape " + std::to_string(id) + " to STEP");
      }
    }
    std::ostringstream os;
    if (writer.WriteStream(os) != IFSelect_RetDone)
    {
      fail("STEP_WRITE_FAILED", "cannot write STEP stream");
    }
    val r = okResult();
    r.set("bytes", bytesToJs(os.str()));
    return r;
  });
}

val importStep(val bytes)
{
  return guarded([&] {
    std::istringstream is(toBytes(bytes, "bytes"));
    STEPControl_Reader reader;
    if (reader.ReadStream("model.step", is) != IFSelect_RetDone)
    {
      fail("STEP_READ_FAILED", "cannot parse STEP data");
    }
    if (reader.TransferRoots() == 0)
    {
      fail("STEP_READ_FAILED", "STEP data contains no transferable shapes");
    }
    const TopoDS_Shape shape = reader.OneShape();
    if (shape.IsNull())
    {
      fail("STEP_READ_FAILED", "STEP data produced a null shape");
    }
    val r = okResult();
    r.set("shape", arena().put(shape));
    return r;
  });
}

val exportBrep(val shape)
{
  return guarded([&] {
    const TopoDS_Shape& s = arena().get(toShapeId(shape, "shape"));
    std::ostringstream  os;
    BinTools::Write(s, os, false, false, BinTools_FormatVersion_CURRENT);
    val r = okResult();
    r.set("bytes", bytesToJs(os.str()));
    return r;
  });
}

val importBrep(val bytes)
{
  return guarded([&] {
    const std::string data = toBytes(bytes, "bytes");
    TopoDS_Shape      shape;
    // Binary BRep (what exportBrep writes) or the text BRep format.
    if (data.find("Open CASCADE Topology") < 16)
    {
      std::istringstream is(data);
      BinTools::Read(shape, is);
    }
    else
    {
      std::istringstream is(data);
      BRep_Builder       builder;
      BRepTools::Read(shape, is, builder);
    }
    if (shape.IsNull())
    {
      fail("BREP_READ_FAILED", "cannot read BRep data");
    }
    val r = okResult();
    r.set("shape", arena().put(shape));
    return r;
  });
}

} // namespace occtwasm
