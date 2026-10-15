// STEP export through XCAF: an assembly as one product per part, placed as
// named instances, so other CAD systems see the structure and the names;
// nested assemblies; colours on products, shapes, faces and instances;
// names on faces.
//
// SPDX-License-Identifier: LGPL-2.1-only
#include "api.hpp"
#include "common.hpp"

#include <BRep_Builder.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <STEPControl_Writer.hxx>
#include <StepRepr_RepresentationItem.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TDF_Label.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <TransferBRep.hxx>
#include <TransferBRep_ShapeMapper.hxx>
#include <Transfer_FinderProcess.hxx>
#include <Quantity_Color.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XSControl_TransferWriter.hxx>
#include <XSControl_WorkSession.hxx>
#include <gp_Trsf.hxx>

#include <cmath>
#include <sstream>
#include <utility>
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

// Faces given by name and the STEP name of each.
using FaceNames = std::vector<std::pair<TopoDS_Shape, std::string>>;

// The faces of one entry {shape, faces} of `list` (faceColors or faceNames),
// faces by TopExp::MapShapes index of shape `shape` of the product.
std::vector<TopoDS_Shape> facesOf(const val&              entry,
                                  const std::vector<int>& ids,
                                  const std::string&      where,
                                  const std::string&      list)
{
  const int index = toInt(entry["shape"], (list + "[].shape").c_str());
  if (index < 0 || static_cast<size_t>(index) >= ids.size())
  {
    fail("INVALID_ARGUMENT", where + "." + list + "[].shape is out of range");
  }
  const ShapeMap            faces = mapOf(arena().get(ids[static_cast<size_t>(index)]), TopAbs_FACE);
  std::vector<TopoDS_Shape> out;
  for (int f : toInts(entry["faces"], (list + "[].faces").c_str()))
  {
    if (f < 0 || f >= faces.Extent())
    {
      fail("INVALID_ARGUMENT", where + "." + list + "[].faces: no face " + std::to_string(f));
    }
    out.push_back(faces(f + 1));
  }
  return out;
}

// The entries of an optional array `list` of the spec.
int entriesOf(const val& spec, const std::string& where, const std::string& list)
{
  const val entries = spec[list];
  if (!given(entries))
  {
    return 0;
  }
  if (!entries.isArray())
  {
    fail("INVALID_ARGUMENT", where + "." + list + " must be an array");
  }
  return entries["length"].as<int>();
}

// A product of `ids` (a compound when there are several) with its colours:
// `color` on the product, `shapeColors[i]` on shape i, and `faceColors`
// ({shape, faces, color}, faces by TopExp::MapShapes index of that shape);
// and `faceNames` ({shape, faces, name}) on face sub-shape labels, also
// collected into `named` for the writer.
TDF_Label addProduct(const occ::handle<XCAFDoc_ShapeTool>& tool,
                     const occ::handle<XCAFDoc_ColorTool>& colors,
                     const std::vector<int>&               ids,
                     const val&                            spec,
                     const std::string&                    where,
                     FaceNames&                            named)
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
  const int colored = entriesOf(spec, where, "faceColors");
  for (int i = 0; i < colored; ++i)
  {
    const val            entry = spec["faceColors"][i];
    const Quantity_Color color = colorOf(entry["color"], "faceColors[].color");
    for (const TopoDS_Shape& face : facesOf(entry, ids, where, "faceColors"))
    {
      const TDF_Label sub = tool->AddSubShape(label, face);
      if (sub.IsNull())
      {
        fail("INVALID_ARGUMENT", where + ".faceColors: face is not part of the product");
      }
      colors->SetColor(sub, color, XCAFDoc_ColorSurf);
    }
  }
  const int names = entriesOf(spec, where, "faceNames");
  for (int i = 0; i < names; ++i)
  {
    const val         entry = spec["faceNames"][i];
    const std::string name  = stringOf(entry["name"], (where + ".faceNames[].name").c_str());
    if (name.empty())
    {
      fail("INVALID_ARGUMENT", where + ".faceNames[].name must be a non-empty string");
    }
    for (const TopoDS_Shape& face : facesOf(entry, ids, where, "faceNames"))
    {
      const TDF_Label sub = tool->AddSubShape(label, face);
      if (sub.IsNull())
      {
        fail("INVALID_ARGUMENT", where + ".faceNames: face is not part of the product");
      }
      setName(sub, name);
      named.emplace_back(face, name);
    }
  }
  return label;
}

// Face names go on the faces' representation items (ADVANCED_FACE), as
// STEPCAFControl_Writer writes names of sub-shape labels with
// write.stepcaf.subshapes.name on; that switch also walks the components
// of assemblies, so only the faces of products are named here.
val writeDocument(const occ::handle<TDocStd_Document>& doc, const FaceNames& named)
{
  STEPCAFControl_Writer writer;
  writer.SetNameMode(true);
  writer.SetColorMode(true);
  if (!writer.Transfer(doc, STEPControl_AsIs))
  {
    fail("STEP_WRITE_FAILED", "cannot transfer the document to STEP");
  }
  if (!named.empty())
  {
    const occ::handle<Transfer_FinderProcess>& process =
      writer.ChangeWriter().WS()->TransferWriter()->FinderProcess();
    for (const auto& [face, name] : named)
    {
      occ::handle<StepRepr_RepresentationItem> item;
      if (!process->FindTypedTransient(TransferBRep::ShapeMapper(process, face),
                                       STANDARD_TYPE(StepRepr_RepresentationItem), item))
      {
        fail("STEP_WRITE_FAILED", "cannot name face '" + name + "'");
      }
      item->SetName(new TCollection_HAsciiString(name.c_str()));
    }
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
    FaceNames named;
    addProduct(tool, colors, toInts(shapes, "shapes"), options, "options", named);
    return writeDocument(doc, named);
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
    FaceNames              named;
    for (int i = 0; i < partCount; ++i)
    {
      const val part = parts[i];
      products.push_back(
        addProduct(tool, colors, toInts(part["shapes"], "parts[].shapes"), part, "parts[]", named));
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
    return writeDocument(doc, named);
  });
}

} // namespace occtwasm
