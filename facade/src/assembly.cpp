// STEP export of an assembly through XCAF: one product per part, placed as
// named instances, so other CAD systems see the structure and the names.
//
// SPDX-License-Identifier: LGPL-2.1-only
#include "api.hpp"
#include "common.hpp"

#include <BRep_Builder.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <STEPControl_Writer.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TDF_Label.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Trsf.hxx>

#include <cmath>
#include <sstream>

namespace occtwasm {

namespace {

void setName(const TDF_Label& label, const std::string& utf8)
{
  if (!utf8.empty())
  {
    TDataStd_Name::Set(label, TCollection_ExtendedString(utf8.c_str(), true));
  }
}

std::string stringOf(const val& v, const char* name)
{
  if (v.isUndefined() || v.isNull())
  {
    return std::string();
  }
  if (!v.isString())
  {
    fail("INVALID_ARGUMENT", std::string(name) + " must be a string");
  }
  return v.as<std::string>();
}

} // namespace

val exportStepAssembly(val assembly)
{
  return guarded([&] {
    const val parts     = assembly["parts"];
    const val instances = assembly["instances"];
    if (!parts.isArray() || !instances.isArray())
    {
      fail("INVALID_ARGUMENT", "parts and instances must be arrays");
    }
    const int partCount     = parts["length"].as<int>();
    const int instanceCount = instances["length"].as<int>();
    if (partCount == 0 || instanceCount == 0)
    {
      fail("INVALID_ARGUMENT", "parts and instances must not be empty");
    }

    occ::handle<TDocStd_Document> doc = new TDocStd_Document("MDTV-XCAF");
    occ::handle<XCAFDoc_ShapeTool> tool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());

    const TDF_Label root = tool->NewShape();
    setName(root, stringOf(assembly["name"], "name"));

    std::vector<TDF_Label> products;
    for (int i = 0; i < partCount; ++i)
    {
      const val              part = parts[i];
      const std::vector<int> ids  = toInts(part["shapes"], "parts[].shapes");
      if (ids.empty())
      {
        fail("INVALID_ARGUMENT", "parts[].shapes must not be empty");
      }
      TopoDS_Shape shape;
      if (ids.size() == 1)
      {
        shape = arena().get(ids[0]);
      }
      else
      {
        TopoDS_Compound compound;
        BRep_Builder    builder;
        builder.MakeCompound(compound);
        for (int id : ids)
        {
          builder.Add(compound, arena().get(id));
        }
        shape = compound;
      }
      const TDF_Label label = tool->AddShape(shape, false, false);
      setName(label, stringOf(part["name"], "parts[].name"));
      products.push_back(label);
    }

    for (int i = 0; i < instanceCount; ++i)
    {
      const val instance = instances[i];
      const int part     = toInt(instance["part"], "instances[].part");
      if (part < 0 || part >= partCount)
      {
        fail("INVALID_ARGUMENT", "instances[].part is out of range");
      }
      const std::vector<double> m = toDoubles(instance["matrix"], "instances[].matrix", 12);
      gp_Trsf                   trsf;
      trsf.SetValues(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
      if (std::abs(trsf.ScaleFactor() - 1.0) > 1e-9)
      {
        fail("INVALID_ARGUMENT", "instances[].matrix must be a rigid motion");
      }
      const TDF_Label component =
        tool->AddComponent(root, products[static_cast<size_t>(part)], TopLoc_Location(trsf));
      setName(component, stringOf(instance["name"], "instances[].name"));
    }
    tool->UpdateAssemblies();

    STEPCAFControl_Writer writer;
    writer.SetNameMode(true);
    if (!writer.Transfer(doc, STEPControl_AsIs))
    {
      fail("STEP_WRITE_FAILED", "cannot transfer the assembly to STEP");
    }
    std::ostringstream os;
    if (writer.ChangeWriter().WriteStream(os) != IFSelect_RetDone)
    {
      fail("STEP_WRITE_FAILED", "cannot write STEP stream");
    }
    val r = okResult();
    r.set("bytes", bytesToJs(os.str()));
    return r;
  });
}

} // namespace occtwasm
