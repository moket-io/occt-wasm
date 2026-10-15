// STEP import through XCAF: one result per solid with the names and colors
// from the file, open shells apart, units and optional healing, and the
// assembly structure (products, instances and their placements).
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
#include <TDF_Tool.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shell.hxx>
#include <gp_Trsf.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_GraphNode.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace occtwasm {

namespace {

struct Leaf
{
  TDF_Label                product;  // the shape's own label (names, colors)
  TDF_Label                instance; // the component that placed it, if any
  TopLoc_Location          location; // from the root, the components' locations composed
  std::vector<std::string> path;     // instance names from the root down
  std::vector<TDF_Label>   chain;    // the components from the root down, `instance` last
  TDF_Label                root;     // the free shape it was found under
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

std::string entryOf(const TDF_Label& label)
{
  TCollection_AsciiString entry;
  TDF_Tool::Entry(label, entry);
  return entry.ToCString();
}

// Row-major 3x4, as gp_Trsf::SetValues reads it.
val matrixToJs(const TopLoc_Location& location)
{
  const gp_Trsf trsf = location.Transformation();
  val           m    = val::array();
  for (int r = 1; r <= 3; ++r)
  {
    for (int c = 1; c <= 4; ++c)
    {
      m.set((r - 1) * 4 + (c - 1), trsf.Value(r, c));
    }
  }
  return m;
}

void collect(const TDF_Label&         label,
             const TDF_Label&         instance,
             const TopLoc_Location&   location,
             std::vector<std::string> path,
             std::vector<TDF_Label>   chain,
             const TDF_Label&         root,
             std::vector<Leaf>&       leaves)
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
        std::vector<TDF_Label> below = chain;
        below.push_back(component);
        collect(referred, component, location * XCAFDoc_ShapeTool::GetLocation(component), path,
                below, root, leaves);
      }
    }
    return;
  }
  leaves.push_back(Leaf{label, instance, location, path, chain, root});
}

// The components from the root down to the part, each with the product it
// places and its own location in its parent.
val instancesToJs(const Leaf& leaf)
{
  val out = val::array();
  for (size_t i = 0; i < leaf.chain.size(); ++i)
  {
    const TDF_Label& component = leaf.chain[i];
    TDF_Label        referred;
    XCAFDoc_ShapeTool::GetReferredShape(component, referred);
    const std::string product = nameOf(referred);
    const std::string own     = nameOf(component);
    val               entry   = val::object();
    entry.set("id", entryOf(component));
    entry.set("product", entryOf(referred));
    entry.set("name", own.empty() ? product : own);
    entry.set("productName", product);
    entry.set("matrix", matrixToJs(XCAFDoc_ShapeTool::GetLocation(component)));
    out.set(i, entry);
  }
  return out;
}

// A colour for the occurrence along `chain` (a SHUO, as readers store
// colours of nested instances), the longest path first.
bool occurrenceColorOf(const std::vector<TDF_Label>& chain, XCAFDoc_ColorType type, Quantity_Color& color)
{
  for (size_t n = chain.size(); n >= 2; --n)
  {
    NCollection_Sequence<TDF_Label> labels;
    for (size_t i = 0; i < n; ++i)
    {
      labels.Append(chain[i]);
    }
    occ::handle<XCAFDoc_GraphNode> shuo;
    if (XCAFDoc_ShapeTool::FindSHUO(labels, shuo) && XCAFDoc_ColorTool::GetColor(shuo->Label(), type, color))
    {
      return true;
    }
  }
  return false;
}

// The solid's own colour, then the occurrence's, the instance's, the
// product's, and last the instances of the assemblies above, inner first.
bool colorOf(const occ::handle<XCAFDoc_ColorTool>& colors,
             const TopoDS_Shape&                   shape,
             const Leaf&                           leaf,
             Quantity_Color&                       color)
{
  for (XCAFDoc_ColorType type : {XCAFDoc_ColorSurf, XCAFDoc_ColorGen})
  {
    if (colors->GetColor(shape, type, color) || colors->GetColor(shape.Located(TopLoc_Location()), type, color)
        || occurrenceColorOf(leaf.chain, type, color)
        || (!leaf.instance.IsNull() && XCAFDoc_ColorTool::GetColor(leaf.instance, type, color))
        || XCAFDoc_ColorTool::GetColor(leaf.product, type, color))
    {
      return true;
    }
  }
  for (size_t i = leaf.chain.size(); i-- > 1;)
  {
    for (XCAFDoc_ColorType type : {XCAFDoc_ColorSurf, XCAFDoc_ColorGen})
    {
      if (XCAFDoc_ColorTool::GetColor(leaf.chain[i - 1], type, color))
      {
        return true;
      }
    }
  }
  return false;
}

// A face's own colour: on its sub-shape label in the product, found by the
// face in product coordinates (or as it is, for files that colour placed
// faces). `placement` is the location the part was moved by.
bool faceColorOf(const occ::handle<XCAFDoc_ColorTool>& colors,
                 const TopoDS_Shape&                   face,
                 const TopLoc_Location&                placement,
                 Quantity_Color&                       color)
{
  const TopoDS_Shape inProduct = face.Moved(placement.Inverted());
  for (XCAFDoc_ColorType type : {XCAFDoc_ColorSurf, XCAFDoc_ColorGen})
  {
    if (colors->GetColor(inProduct, type, color) || colors->GetColor(face, type, color))
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
    // Shapes in their product's coordinates, placed by `matrix`.
    const bool local = hasOptions && !(options["local"].isUndefined() || options["local"].isNull())
                         ? options["local"].as<bool>()
                         : false;

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
      collect(roots(i), TDF_Label(), TopLoc_Location(), {}, {}, roots(i), leaves);
    }

    val parts = val::array();
    int n = 0, solids = 0, open = 0, healed = 0;
    for (const Leaf& leaf : leaves)
    {
      const TopLoc_Location placement = local ? TopLoc_Location() : leaf.location;
      const TopoDS_Shape    located   = XCAFDoc_ShapeTool::GetShape(leaf.product).Moved(placement);
      if (located.IsNull())
      {
        continue;
      }
      const std::string productId = entryOf(leaf.product);
      const std::string rootId    = entryOf(leaf.root);
      const std::string rootName  = nameOf(leaf.root);
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
        part.set("matrix", matrixToJs(leaf.location));
        part.set("productId", productId);
        part.set("instances", instancesToJs(leaf));
        val root = val::object();
        root.set("id", rootId);
        root.set("name", rootName);
        part.set("root", root);
        Quantity_Color color;
        const bool     hasColor = colorOf(colors, original, leaf, color);
        if (hasColor)
        {
          part.set("color", colorToJs(color));
        }
        // Faces whose colour differs from the part's, by face map index.
        const ShapeMap faces      = mapOf(result, TopAbs_FACE);
        val            faceColors = val::array();
        int            colored    = 0;
        for (int f = 1; f <= faces.Extent(); ++f)
        {
          Quantity_Color own;
          if (faceColorOf(colors, faces(f), placement, own) && (!hasColor || own.IsDifferent(color)))
          {
            val entry = val::object();
            entry.set("face", f - 1);
            entry.set("color", colorToJs(own));
            faceColors.set(colored++, entry);
          }
        }
        if (colored > 0)
        {
          part.set("faceColors", faceColors);
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
      std::string unit = lengths(i).ToCString();
      std::transform(unit.begin(), unit.end(), unit.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      units.set(i - 1, unit);
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
