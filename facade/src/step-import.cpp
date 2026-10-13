// STEP import through XCAF: one result per solid with the names and colors
// from the file, open shells apart, units and optional healing.
//
// SPDX-License-Identifier: LGPL-2.1-only
#include "api.hpp"
#include "common.hpp"

#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <NCollection_Sequence.hxx>
#include <Precision.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPControl_Reader.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeFix_Solid.hxx>
#include <TCollection_AsciiString.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TDF_Label.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shell.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <algorithm>
#include <sstream>
#include <vector>

namespace occtwasm {

namespace {

struct Leaf
{
  TDF_Label                product;  // the shape's own label (names, colors)
  TDF_Label                instance; // the component that placed it, if any
  TopLoc_Location          location;
  std::vector<std::string> path;     // instance names from the root down
};

std::string nameOf(const TDF_Label& label)
{
  occ::handle<TDataStd_Name> name;
  if (label.IsNull() || !label.FindAttribute(TDataStd_Name::GetID(), name))
  {
    return std::string();
  }
  const TCollection_ExtendedString& s   = name->Get();
  const int                         len = s.LengthOfCString();
  std::string                       out(static_cast<size_t>(len) + 1, '\0');
  char*                             buf = out.data();
  s.ToUTF8CString(buf);
  out.resize(std::char_traits<char>::length(out.c_str()));
  return out;
}

void collect(const TDF_Label&                  label,
             const TDF_Label&                  instance,
             const TopLoc_Location&            location,
             std::vector<std::string>          path,
             std::vector<Leaf>&                leaves)
{
  if (XCAFDoc_ShapeTool::IsAssembly(label))
  {
    path.push_back(nameOf(instance.IsNull() ? label : instance));
    NCollection_Sequence<TDF_Label> components;
    XCAFDoc_ShapeTool::GetComponents(label, components, false);
    for (int i = 1; i <= components.Length(); ++i)
    {
      const TDF_Label& component = components(i);
      TDF_Label        referred;
      if (XCAFDoc_ShapeTool::GetReferredShape(component, referred))
      {
        collect(referred, component, location * XCAFDoc_ShapeTool::GetLocation(component), path,
                leaves);
      }
    }
    return;
  }
  leaves.push_back(Leaf{label, instance, location, path});
}

bool colorOf(const occ::handle<XCAFDoc_ColorTool>& colors,
             const TopoDS_Shape&                   shape,
             const Leaf&                           leaf,
             Quantity_Color&                       color)
{
  for (XCAFDoc_ColorType type : {XCAFDoc_ColorSurf, XCAFDoc_ColorGen})
  {
    if (colors->GetColor(shape, type, color) || colors->GetColor(shape.Located(TopLoc_Location()), type, color)
        || (!leaf.instance.IsNull() && XCAFDoc_ColorTool::GetColor(leaf.instance, type, color))
        || XCAFDoc_ColorTool::GetColor(leaf.product, type, color))
    {
      return true;
    }
  }
  return false;
}

val colorToJs(const Quantity_Color& c)
{
  double r = 0, g = 0, b = 0;
  c.Values(r, g, b, Quantity_TOC_sRGB);
  val a = val::array();
  a.set(0, r);
  a.set(1, g);
  a.set(2, b);
  return a;
}

// A shell or loose faces sewn together; a closed shell becomes a solid.
TopoDS_Shape sew(const TopoDS_Shape& shape, double tolerance)
{
  BRepBuilderAPI_Sewing sewing(tolerance);
  sewing.Add(shape);
  sewing.Perform();
  TopoDS_Shape sewn = sewing.SewedShape();
  if (sewn.IsNull())
  {
    return shape;
  }
  if (sewn.ShapeType() == TopAbs_SHELL && BRep_Tool::IsClosed(sewn))
  {
    BRepBuilderAPI_MakeSolid solid(TopoDS::Shell(sewn));
    if (solid.IsDone())
    {
      ShapeFix_Solid fix;
      fix.Init(solid.Solid());
      fix.Perform();
      return fix.Solid();
    }
  }
  return sewn;
}

TopoDS_Shape heal(const TopoDS_Shape& shape, double tolerance)
{
  ShapeFix_Shape fix(shape);
  fix.SetPrecision(Precision::Confusion());
  fix.SetMaxTolerance(tolerance);
  fix.Perform();
  return fix.Shape();
}

} // namespace

val importStepParts(val bytes, val options)
{
  return guarded([&] {
    const bool hasOptions = !(options.isUndefined() || options.isNull());
    const bool healing =
      !hasOptions || options["heal"].isUndefined() ? true : options["heal"].as<bool>();
    const double tolerance = !hasOptions || options["tolerance"].isUndefined()
                               ? 1e-3
                               : toNumber(options["tolerance"], "options.tolerance");
    if (tolerance <= 0)
    {
      fail("INVALID_ARGUMENT", "options.tolerance must be positive");
    }

    std::istringstream    is(toBytes(bytes, "bytes"));
    STEPCAFControl_Reader reader;
    reader.SetNameMode(true);
    reader.SetColorMode(true);
    reader.SetLayerMode(false);
    reader.SetPropsMode(false);
    reader.SetGDTMode(false);
    reader.SetMatMode(false);
    reader.SetViewMode(false);
    if (reader.ReadStream("model.step", is) != IFSelect_RetDone)
    {
      fail("STEP_READ_FAILED", "cannot parse STEP data");
    }

    NCollection_Sequence<TCollection_AsciiString> lengths, angles, solidAngles;
    reader.ChangeReader().FileUnits(lengths, angles, solidAngles);

    occ::handle<TDocStd_Document> doc = new TDocStd_Document("MDTV-XCAF");
    // Results in millimeters whatever the file uses.
    XCAFDoc_DocumentTool::SetLengthUnit(doc, 1.0, UnitsMethods_LengthUnit_Millimeter);
    if (!reader.Transfer(doc))
    {
      fail("STEP_READ_FAILED", "STEP data contains no transferable shapes");
    }
    occ::handle<XCAFDoc_ShapeTool> shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    occ::handle<XCAFDoc_ColorTool> colors = XCAFDoc_DocumentTool::ColorTool(doc->Main());

    NCollection_Sequence<TDF_Label> roots;
    shapes->GetFreeShapes(roots);
    std::vector<Leaf> leaves;
    for (int i = 1; i <= roots.Length(); ++i)
    {
      collect(roots(i), TDF_Label(), TopLoc_Location(), {}, leaves);
    }

    val parts = val::array();
    int n = 0, solids = 0, open = 0, healed = 0;
    for (const Leaf& leaf : leaves)
    {
      const TopoDS_Shape located = XCAFDoc_ShapeTool::GetShape(leaf.product).Moved(leaf.location);
      if (located.IsNull())
      {
        continue;
      }
      // Solids first, then shells and faces outside any solid.
      std::vector<std::pair<TopoDS_Shape, TopoDS_Shape>> found; // original, result
      for (TopExp_Explorer it(located, TopAbs_SOLID); it.More(); it.Next())
      {
        found.push_back({it.Current(), it.Current()});
      }
      for (TopExp_Explorer it(located, TopAbs_SHELL, TopAbs_SOLID); it.More(); it.Next())
      {
        found.push_back({it.Current(), it.Current()});
      }
      TopoDS_Compound loose;
      BRep_Builder    builder;
      builder.MakeCompound(loose);
      bool hasLoose = false;
      for (TopExp_Explorer it(located, TopAbs_FACE, TopAbs_SHELL); it.More(); it.Next())
      {
        builder.Add(loose, it.Current());
        hasLoose = true;
      }
      if (hasLoose)
      {
        found.push_back({loose, loose});
      }

      for (auto& [original, result] : found)
      {
        bool wasHealed = false;
        if (healing)
        {
          if (original.ShapeType() != TopAbs_SOLID)
          {
            result    = sew(original, tolerance);
            wasHealed = result.ShapeType() == TopAbs_SOLID;
          }
          if (!BRepCheck_Analyzer(result).IsValid())
          {
            result    = heal(result, tolerance);
            wasHealed = true;
          }
        }
        const bool isSolid = result.ShapeType() == TopAbs_SOLID;
        solids += isSolid ? 1 : 0;
        open += isSolid ? 0 : 1;
        healed += wasHealed ? 1 : 0;

        val part = val::object();
        part.set("shape", arena().put(result));
        part.set("type", std::string(shapeTypeName(result.ShapeType())));
        part.set("valid", BRepCheck_Analyzer(result).IsValid());
        part.set("healed", wasHealed);
        part.set("name", nameOf(leaf.instance.IsNull() ? leaf.product : leaf.instance));
        part.set("product", nameOf(leaf.product));
        val path = val::array();
        for (size_t i = 0; i < leaf.path.size(); ++i)
        {
          path.set(i, leaf.path[i]);
        }
        part.set("path", path);
        Quantity_Color color;
        if (colorOf(colors, original, leaf, color))
        {
          part.set("color", colorToJs(color));
        }
        parts.set(n++, part);
      }
    }
    if (n == 0)
    {
      fail("STEP_READ_FAILED", "STEP data contains no shapes");
    }

    val units = val::array();
    for (int i = 1; i <= lengths.Length(); ++i)
    {
      units.set(i - 1, std::string(lengths(i).ToCString()));
    }
    val r = okResult();
    r.set("parts", parts);
    r.set("units", units);
    r.set("solids", solids);
    r.set("open", open);
    r.set("healed", healed);
    return r;
  });
}

} // namespace occtwasm
