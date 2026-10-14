// STEP export through XCAF: an assembly as one product per part, placed as
// named instances, so other CAD systems see the structure and the names;
// colours on products, shapes, faces and instances.
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
#include <Quantity_Color.hxx>
#include <XCAFDoc_ColorTool.hxx>
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

bool given(const val& v)
{
  return !(v.isUndefined() || v.isNull());
}

Quantity_Color colorOf(const val& v, const char* name)
{
  const std::vector<double> c = toDoubles(v, name, 3);
  for (double x : c)
  {
    if (!(x >= 0 && x <= 1))
    {
      fail("INVALID_ARGUMENT", std::string(name) + " must be sRGB in [0, 1]");
    }
  }
  return Quantity_Color(c[0], c[1], c[2], Quantity_TOC_sRGB);
}

// A product of `ids` (a compound when there are several) with its colours:
// `color` on the product, `shapeColors[i]` on shape i, and `faceColors`
// ({shape, faces, color}, faces by TopExp::MapShapes index of that shape).
TDF_Label addProduct(const occ::handle<XCAFDoc_ShapeTool>& tool,
                     const occ::handle<XCAFDoc_ColorTool>& colors,
                     const std::vector<int>&               ids,
                     const val&                            spec,
                     const std::string&                    where)
{
  if (ids.empty())
  {
    fail("INVALID_ARGUMENT", where + ".shapes must not be empty");
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
  setName(label, stringOf(spec["name"], (where + ".name").c_str()));
  if (given(spec["color"]))
  {
    colors->SetColor(label, colorOf(spec["color"], (where + ".color").c_str()), XCAFDoc_ColorSurf);
  }
  const val shapeColors = spec["shapeColors"];
  if (given(shapeColors))
  {
    if (!shapeColors.isArray() || shapeColors["length"].as<size_t>() != ids.size())
    {
      fail("INVALID_ARGUMENT", where + ".shapeColors must have one entry per shape");
    }
    for (size_t i = 0; i < ids.size(); ++i)
    {
      const val c = shapeColors[i];
      if (!given(c))
      {
        continue;
      }
      const TDF_Label sub = ids.size() == 1 ? label : tool->AddSubShape(label, arena().get(ids[i]));
      if (sub.IsNull())
      {
        fail("INVALID_ARGUMENT", where + ".shapeColors: shape is not part of the product");
      }
      colors->SetColor(sub, colorOf(c, (where + ".shapeColors[]").c_str()), XCAFDoc_ColorSurf);
    }
  }
  const val faceColors = spec["faceColors"];
  if (given(faceColors))
  {
    if (!faceColors.isArray())
    {
      fail("INVALID_ARGUMENT", where + ".faceColors must be an array");
    }
    const int count = faceColors["length"].as<int>();
    for (int i = 0; i < count; ++i)
    {
      const val entry = faceColors[i];
      const int index = toInt(entry["shape"], "faceColors[].shape");
      if (index < 0 || static_cast<size_t>(index) >= ids.size())
      {
        fail("INVALID_ARGUMENT", where + ".faceColors[].shape is out of range");
      }
      const ShapeMap         faces = mapOf(arena().get(ids[static_cast<size_t>(index)]), TopAbs_FACE);
      const Quantity_Color   color = colorOf(entry["color"], "faceColors[].color");
      const std::vector<int> picks = toInts(entry["faces"], "faceColors[].faces");
      for (int f : picks)
      {
        if (f < 0 || f >= faces.Extent())
        {
          fail("INVALID_ARGUMENT", where + ".faceColors[].faces: no face " + std::to_string(f));
        }
        const TDF_Label sub = tool->AddSubShape(label, faces(f + 1));
        if (sub.IsNull())
        {
          fail("INVALID_ARGUMENT", where + ".faceColors: face is not part of the product");
        }
        colors->SetColor(sub, color, XCAFDoc_ColorSurf);
      }
    }
  }
  return label;
}

val writeDocument(const occ::handle<TDocStd_Document>& doc)
{
  STEPCAFControl_Writer writer;
  writer.SetNameMode(true);
  writer.SetColorMode(true);
  if (!writer.Transfer(doc, STEPControl_AsIs))
  {
    fail("STEP_WRITE_FAILED", "cannot transfer the document to STEP");
  }
  std::ostringstream os;
  if (writer.ChangeWriter().WriteStream(os) != IFSelect_RetDone)
  {
    fail("STEP_WRITE_FAILED", "cannot write STEP stream");
  }
  val r = okResult();
  r.set("bytes", bytesToJs(os.str()));
  return r;
}

} // namespace

val exportStepColored(val shapes, val options)
{
  return guarded([&] {
    occ::handle<TDocStd_Document>  doc    = new TDocStd_Document("MDTV-XCAF");
    occ::handle<XCAFDoc_ShapeTool> tool   = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    occ::handle<XCAFDoc_ColorTool> colors = XCAFDoc_DocumentTool::ColorTool(doc->Main());
    addProduct(tool, colors, toInts(shapes, "shapes"), options, "options");
    return writeDocument(doc);
  });
}

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
    occ::handle<XCAFDoc_ShapeTool> tool   = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    occ::handle<XCAFDoc_ColorTool> colors = XCAFDoc_DocumentTool::ColorTool(doc->Main());

    const TDF_Label root = tool->NewShape();
    setName(root, stringOf(assembly["name"], "name"));

    std::vector<TDF_Label> products;
    for (int i = 0; i < partCount; ++i)
    {
      const val part = parts[i];
      products.push_back(addProduct(tool, colors, toInts(part["shapes"], "parts[].shapes"), part, "parts[]"));
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
      if (given(instance["color"]))
      {
        colors->SetColor(component, colorOf(instance["color"], "instances[].color"), XCAFDoc_ColorSurf);
      }
    }
    tool->UpdateAssemblies();
    return writeDocument(doc);
  });
}

} // namespace occtwasm
