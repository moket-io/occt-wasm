// STEP export through XCAF: an assembly as one product per part, placed as
// named instances, so other CAD systems see the structure and the names;
// nested assemblies; colours on products, shapes, faces and instances.
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
#include <vector>

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

namespace {

// One instance: a part or an assembly placed in its parent.
struct Use
{
  bool assembly = false;
  int  index    = 0;
  val  spec = val::undefined();
};

std::vector<Use> usesOf(const val& instances, const std::string& where, int partCount, int assemblyCount)
{
  if (!instances.isArray())
  {
    fail("INVALID_ARGUMENT", where + " must be an array");
  }
  const int count = instances["length"].as<int>();
  if (count == 0)
  {
    fail("INVALID_ARGUMENT", where + " must not be empty");
  }
  std::vector<Use> uses;
  for (int i = 0; i < count; ++i)
  {
    const val  instance   = instances[i];
    const bool isPart     = given(instance["part"]);
    const bool isAssembly = given(instance["assembly"]);
    if (isPart == isAssembly)
    {
      fail("INVALID_ARGUMENT", where + "[] must have exactly one of part and assembly");
    }
    Use use;
    use.assembly = isAssembly;
    use.index    = isPart ? toInt(instance["part"], (where + "[].part").c_str())
                          : toInt(instance["assembly"], (where + "[].assembly").c_str());
    if (use.index < 0 || use.index >= (isPart ? partCount : assemblyCount))
    {
      fail("INVALID_ARGUMENT", where + (isPart ? "[].part" : "[].assembly") + " is out of range");
    }
    use.spec = instance;
    uses.push_back(use);
  }
  return uses;
}

// Depth-first search for an assembly that contains itself: 1 on the path, 2 done.
bool cyclic(int a, const std::vector<std::vector<Use>>& assemblies, std::vector<int>& state)
{
  if (state[static_cast<size_t>(a)] == 1)
  {
    return true;
  }
  if (state[static_cast<size_t>(a)] == 2)
  {
    return false;
  }
  state[static_cast<size_t>(a)] = 1;
  for (const Use& use : assemblies[static_cast<size_t>(a)])
  {
    if (use.assembly && cyclic(use.index, assemblies, state))
    {
      return true;
    }
  }
  state[static_cast<size_t>(a)] = 2;
  return false;
}

void addComponents(const occ::handle<XCAFDoc_ShapeTool>& tool,
                   const occ::handle<XCAFDoc_ColorTool>& colors,
                   const TDF_Label&                      parent,
                   const std::vector<Use>&               uses,
                   const std::vector<TDF_Label>&         products,
                   const std::vector<TDF_Label>&         assemblies,
                   const std::string&                    where)
{
  for (const Use& use : uses)
  {
    const std::vector<double> m = toDoubles(use.spec["matrix"], (where + "[].matrix").c_str(), 12);
    gp_Trsf                   trsf;
    trsf.SetValues(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
    if (std::abs(trsf.ScaleFactor() - 1.0) > 1e-9)
    {
      fail("INVALID_ARGUMENT", where + "[].matrix must be a rigid motion");
    }
    const TDF_Label& referred = use.assembly ? assemblies[static_cast<size_t>(use.index)]
                                             : products[static_cast<size_t>(use.index)];
    const TDF_Label  component = tool->AddComponent(parent, referred, TopLoc_Location(trsf));
    if (component.IsNull())
    {
      fail("STEP_WRITE_FAILED", "cannot add a component to " + where);
    }
    setName(component, stringOf(use.spec["name"], (where + "[].name").c_str()));
    if (given(use.spec["color"]))
    {
      colors->SetColor(component, colorOf(use.spec["color"], (where + "[].color").c_str()), XCAFDoc_ColorSurf);
    }
  }
}

} // namespace

val exportStepAssembly(val assembly)
{
  return guarded([&] {
    const val parts = assembly["parts"];
    if (!parts.isArray() || !assembly["instances"].isArray())
    {
      fail("INVALID_ARGUMENT", "parts and instances must be arrays");
    }
    const int partCount = parts["length"].as<int>();
    if (partCount == 0)
    {
      fail("INVALID_ARGUMENT", "parts and instances must not be empty");
    }
    const val nested        = assembly["assemblies"];
    int       assemblyCount = 0;
    if (given(nested))
    {
      if (!nested.isArray())
      {
        fail("INVALID_ARGUMENT", "assemblies must be an array");
      }
      assemblyCount = nested["length"].as<int>();
    }

    // The structure first: indices, empty assemblies, cycles.
    const std::vector<Use>        top = usesOf(assembly["instances"], "instances", partCount, assemblyCount);
    std::vector<std::vector<Use>> inside;
    for (int a = 0; a < assemblyCount; ++a)
    {
      inside.push_back(usesOf(nested[a]["instances"], "assemblies[].instances", partCount, assemblyCount));
    }
    std::vector<int> state(static_cast<size_t>(assemblyCount), 0);
    for (int a = 0; a < assemblyCount; ++a)
    {
      if (cyclic(a, inside, state))
      {
        fail("INVALID_ARGUMENT", "assemblies contain themselves");
      }
    }

    occ::handle<TDocStd_Document>  doc    = new TDocStd_Document("MDTV-XCAF");
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

    // Every assembly label before any component, so instances can refer to
    // earlier and later assemblies alike; the compounds come from
    // UpdateAssemblies once everything is in place.
    std::vector<TDF_Label> assemblies;
    for (int a = 0; a < assemblyCount; ++a)
    {
      const TDF_Label label = tool->NewShape();
      setName(label, stringOf(nested[a]["name"], "assemblies[].name"));
      assemblies.push_back(label);
    }
    for (int a = 0; a < assemblyCount; ++a)
    {
      addComponents(tool, colors, assemblies[static_cast<size_t>(a)], inside[static_cast<size_t>(a)], products,
                    assemblies, "assemblies[].instances");
    }
    addComponents(tool, colors, root, top, products, assemblies, "instances");
    tool->UpdateAssemblies();
    return writeDocument(doc);
  });
}

} // namespace occtwasm
