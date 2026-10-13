// Shell (thick solid), offset, draft, sweep and loft, and the wires they take.
//
// SPDX-License-Identifier: LGPL-2.1-only
#include "api.hpp"
#include "common.hpp"
#include "history.hpp"

#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepOffsetAPI_DraftAngle.hxx>
#include <BRepOffsetAPI_MakeOffsetShape.hxx>
#include <BRepOffsetAPI_MakePipe.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepOffset_Error.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <GeomFill_Trihedron.hxx>
#include <NCollection_HSequence.hxx>
#include <Precision.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>

#include <cmath>
#include <limits>
#include <sstream>

namespace occtwasm {

namespace {

// Gap accepted between the ends of joined edges (mm).
constexpr double kJoinTolerance = 1e-6;

const char* offsetErrorName(BRepOffset_Error e)
{
  switch (e)
  {
    case BRepOffset_NoError:            return "no error";
    case BRepOffset_BadNormalsOnGeometry: return "bad normals on the geometry";
    case BRepOffset_C0Geometry:         return "C0 geometry";
    case BRepOffset_NullOffset:         return "null offset";
    case BRepOffset_NotConnectedShell:  return "not connected shell";
    case BRepOffset_CannotTrimEdges:    return "cannot trim edges";
    case BRepOffset_CannotFuseVertices: return "cannot fuse vertices";
    case BRepOffset_CannotExtentEdge:   return "cannot extend edges";
    case BRepOffset_UserBreak:          return "user break";
    case BRepOffset_MixedConnectivity:  return "mixed connectivity";
    default:                            return "unknown error";
  }
}

double toTolerance(const val& options)
{
  if (options.isUndefined() || options.isNull() || options["tolerance"].isUndefined())
  {
    return 1e-3;
  }
  const double t = toNumber(options["tolerance"], "options.tolerance");
  if (t <= 0)
  {
    fail("INVALID_ARGUMENT", "options.tolerance must be positive");
  }
  return t;
}

bool flag(const val& options, const char* name, bool fallback)
{
  if (options.isUndefined() || options.isNull() || options[name].isUndefined())
  {
    return fallback;
  }
  return options[name].as<bool>();
}

std::string text(const val& options, const char* name, const std::string& fallback)
{
  if (options.isUndefined() || options.isNull() || options[name].isUndefined())
  {
    return fallback;
  }
  if (!options[name].isString())
  {
    fail("INVALID_ARGUMENT", std::string("options.") + name + " must be a string");
  }
  return options[name].as<std::string>();
}

std::string pointText(const gp_Pnt& p)
{
  std::ostringstream os;
  os.precision(6);
  os << "(" << p.X() << ", " << p.Y() << ", " << p.Z() << ")";
  return os.str();
}

// The wire to sweep along or to loft through: a wire, or an edge as a wire.
TopoDS_Wire asWire(const TopoDS_Shape& s, const char* name)
{
  if (s.ShapeType() == TopAbs_WIRE)
  {
    return TopoDS::Wire(s);
  }
  if (s.ShapeType() == TopAbs_EDGE)
  {
    return BRepBuilderAPI_MakeWire(TopoDS::Edge(s)).Wire();
  }
  fail("INVALID_ARGUMENT", std::string(name) + " must be a wire or an edge");
}

} // namespace

// ---------------------------------------------------------------------------
TopoDS_Wire connectEdges(const std::vector<TopoDS_Edge>& edges, std::vector<int>& wireEdge, bool& closed)
{
  if (edges.empty())
  {
    fail("INVALID_ARGUMENT", "no edges to join");
  }
  occ::handle<NCollection_HSequence<TopoDS_Shape>> seq = new NCollection_HSequence<TopoDS_Shape>();
  for (const TopoDS_Edge& e : edges)
  {
    seq->Append(e);
  }
  occ::handle<NCollection_HSequence<TopoDS_Shape>> wires =
    ShapeAnalysis_FreeBounds::ConnectEdgesToWires(seq, kJoinTolerance, false);
  if (wires.IsNull() || wires->Length() == 0)
  {
    fail("PATH_NOT_CONNECTED", "the edges do not form a path");
  }
  if (wires->Length() > 1)
  {
    // Name the smallest gap between the ends of two pieces.
    double best = std::numeric_limits<double>::max();
    gp_Pnt at;
    std::vector<gp_Pnt> ends;
    for (int i = 1; i <= wires->Length(); ++i)
    {
      TopoDS_Vertex v1, v2;
      TopExp::Vertices(TopoDS::Wire(wires->Value(i)), v1, v2);
      ends.push_back(BRep_Tool::Pnt(v1));
      ends.push_back(BRep_Tool::Pnt(v2));
    }
    for (size_t a = 0; a < ends.size(); ++a)
    {
      for (size_t b = a + 1; b < ends.size(); ++b)
      {
        if (a / 2 != b / 2 && ends[a].Distance(ends[b]) < best)
        {
          best = ends[a].Distance(ends[b]);
          at   = ends[a];
        }
      }
    }
    fail("PATH_NOT_CONNECTED",
         "the path has " + std::to_string(wires->Length()) + " pieces; the closest gap is at "
           + pointText(at));
  }

  // Rebuild in path order so that consecutive edges share their vertices.
  const TopoDS_Wire       ordered = TopoDS::Wire(wires->Value(1));
  BRepBuilderAPI_MakeWire mw;
  std::vector<TopoDS_Edge> added(edges.size());
  for (TopExp_Explorer it(ordered, TopAbs_EDGE); it.More(); it.Next())
  {
    const TopoDS_Edge& e = TopoDS::Edge(it.Current());
    mw.Add(e);
    if (!mw.IsDone())
    {
      fail("PATH_NOT_CONNECTED", "an edge does not connect at " + pointText(BRep_Tool::Pnt(TopExp::FirstVertex(e))));
    }
    for (size_t i = 0; i < edges.size(); ++i)
    {
      if (edges[i].IsSame(e))
      {
        added[i] = mw.Edge();
      }
    }
  }
  TopoDS_Wire    wire = mw.Wire();
  const ShapeMap map  = mapOf(wire, TopAbs_EDGE);
  wireEdge.assign(edges.size(), -1);
  for (size_t i = 0; i < edges.size(); ++i)
  {
    if (!added[i].IsNull())
    {
      wireEdge[i] = indexOf(map, added[i]);
    }
  }
  closed = BRep_Tool::IsClosed(wire);
  wire.Closed(closed);
  return wire;
}

val joinEdges(val parts)
{
  return guarded([&] {
    if (!parts.isArray() || parts["length"].as<int>() < 1)
    {
      fail("INVALID_ARGUMENT", "parts must be a non-empty array");
    }
    const int                         n = parts["length"].as<int>();
    std::vector<TopoDS_Edge>          edges;
    std::vector<std::pair<int, int>>  source; // part, position in part
    std::vector<int>                  counts(n, 0);
    for (int p = 0; p < n; ++p)
    {
      const TopoDS_Shape& s = arena().get(toShapeId(parts[p]["shape"], "parts[].shape"));
      if (parts[p]["edges"].isUndefined())
      {
        const ShapeMap all = mapOf(s, TopAbs_EDGE);
        for (int i = 1; i <= all.Extent(); ++i)
        {
          edges.push_back(TopoDS::Edge(all(i)));
          source.push_back({p, counts[p]++});
        }
      }
      else
      {
        for (const TopoDS_Edge& e : pickEdges(s, parts[p]["edges"]))
        {
          edges.push_back(e);
          source.push_back({p, counts[p]++});
        }
      }
    }
    std::vector<int> wireEdge;
    bool             closed = false;
    const TopoDS_Wire wire  = connectEdges(edges, wireEdge, closed);

    val out = val::array();
    for (int p = 0; p < n; ++p)
    {
      out.set(p, val::array());
    }
    for (size_t i = 0; i < edges.size(); ++i)
    {
      out[source[i].first].set(source[i].second, wireEdge[i]);
    }
    val r = okResult();
    r.set("shape", arena().put(wire));
    r.set("closed", closed);
    r.set("edges", out);
    return r;
  });
}

// ---------------------------------------------------------------------------
val thickSolid(val shape, val faces, val offset, val options)
{
  return guarded([&] {
    const TopoDS_Shape& input = arena().get(toShapeId(shape, "shape"));
    const double        d     = toNumber(offset, "offset");
    if (std::abs(d) <= Precision::Confusion())
    {
      fail("INVALID_ARGUMENT", "offset must be non-zero");
    }
    NCollection_List<TopoDS_Shape> closing;
    for (const TopoDS_Face& f : pickFaces(input, faces, true))
    {
      closing.Append(f);
    }
    BRepOffsetAPI_MakeThickSolid algo;
    algo.MakeThickSolidByJoin(input, closing, d, toTolerance(options), BRepOffset_Skin, false,
                              false, GeomAbs_Arc);
    if (!algo.IsDone())
    {
      fail("SHELL_FAILED",
           std::string("shell failed (") + offsetErrorName(algo.MakeOffset().Error()) + ")");
    }
    const TopoDS_Shape result = unwrap(algo.Shape());
    requireValid(result, "shell");
    val history = buildHistory(algo, {input}, result);
    return opResult(arena().put(result), history);
  });
}

val offsetShape(val shape, val offset, val options)
{
  return guarded([&] {
    const TopoDS_Shape& input = arena().get(toShapeId(shape, "shape"));
    const double        d     = toNumber(offset, "offset");
    if (std::abs(d) <= Precision::Confusion())
    {
      fail("INVALID_ARGUMENT", "offset must be non-zero");
    }
    BRepOffsetAPI_MakeOffsetShape algo;
    algo.PerformByJoin(input, d, toTolerance(options), BRepOffset_Skin, false, false, GeomAbs_Arc);
    if (!algo.IsDone())
    {
      fail("OFFSET_FAILED",
           std::string("offset failed (") + offsetErrorName(algo.MakeOffset().Error()) + ")");
    }
    const TopoDS_Shape result = unwrap(algo.Shape());
    requireValid(result, "offset");
    val history = buildHistory(algo, {input}, result);
    return opResult(arena().put(result), history);
  });
}

// ---------------------------------------------------------------------------
val draft(val shape, val faces, val direction, val angle, val neutral)
{
  return guarded([&] {
    const TopoDS_Shape& input = arena().get(toShapeId(shape, "shape"));
    const gp_Dir        dir   = toDir(direction, "direction");
    const double        a     = toNumber(angle, "angle");
    if (std::abs(a) <= Precision::Angular() || std::abs(a) >= M_PI / 2)
    {
      fail("INVALID_ARGUMENT", "angle must be in (0, pi/2) by absolute value");
    }
    if (neutral.isUndefined() || neutral.isNull())
    {
      fail("INVALID_ARGUMENT", "neutral plane is required");
    }
    const gp_Pln plane(toPnt(neutral["origin"], "neutral.origin"), toDir(neutral["normal"], "neutral.normal"));

    const std::vector<TopoDS_Face> picked = pickFaces(input, faces, false);
    const ShapeMap          all = mapOf(input, TopAbs_FACE);
    BRepOffsetAPI_DraftAngle algo(input);
    for (const TopoDS_Face& f : picked)
    {
      algo.Add(f, dir, a, plane, true);
      if (!algo.AddDone())
      {
        const TopoDS_Shape& bad = algo.ProblematicShape();
        int                 at  = bad.IsNull() ? indexOf(all, f) : indexOf(all, bad);
        if (at < 0)
        {
          at = indexOf(all, f);
        }
        failAt("DRAFT_FAILED", "face " + std::to_string(at) + " cannot be drafted", at);
      }
    }
    algo.Build();
    if (!algo.IsDone())
    {
      fail("DRAFT_FAILED", "draft failed");
    }
    const TopoDS_Shape result = unwrap(algo.Shape());
    requireValid(result, "draft");
    val history = buildHistory(algo, {input}, result);
    return opResult(arena().put(result), history);
  });
}

// ---------------------------------------------------------------------------
val sweep(val profile, val spine, val options)
{
  return guarded([&] {
    const TopoDS_Shape& input = arena().get(toShapeId(profile, "profile"));
    const TopoDS_Wire   path  = asWire(arena().get(toShapeId(spine, "spine")), "spine");
    const std::string   mode  = text(options, "orientation", "perpendicular");
    GeomFill_Trihedron  trihedron;
    if (mode == "perpendicular")
    {
      trihedron = GeomFill_IsCorrectedFrenet;
    }
    else if (mode == "parallel")
    {
      trihedron = GeomFill_IsFixed;
    }
    else
    {
      fail("INVALID_ARGUMENT", "options.orientation must be 'perpendicular' or 'parallel'");
    }
    BRepOffsetAPI_MakePipe algo(path, input, trihedron, false);
    algo.Build();
    if (!algo.IsDone())
    {
      fail("SWEEP_FAILED", "sweep failed");
    }
    const TopoDS_Shape result = unwrap(algo.Shape());
    requireValid(result, "sweep");
    val history = buildHistory(algo, {input}, result);
    val info    = sweepInfo(algo, input, result);
    val r       = opResult(arena().put(result), history);
    r.set("sweep", info);
    return r;
  });
}

// ---------------------------------------------------------------------------
val loft(val sections, val options)
{
  return guarded([&] {
    const std::vector<int> ids = toInts(sections, "sections");
    if (ids.size() < 2)
    {
      fail("INVALID_ARGUMENT", "a loft needs at least two sections");
    }
    const bool solid = flag(options, "solid", true);
    const bool ruled = flag(options, "ruled", false);

    std::vector<TopoDS_Shape> inputs;
    BRepOffsetAPI_ThruSections algo(solid, ruled);
    for (size_t i = 0; i < ids.size(); ++i)
    {
      const TopoDS_Shape& s = arena().get(ids[i]);
      inputs.push_back(s);
      switch (s.ShapeType())
      {
        case TopAbs_FACE: {
          const TopoDS_Face& f = TopoDS::Face(s);
          int                wires = 0;
          for (TopExp_Explorer it(f, TopAbs_WIRE); it.More(); it.Next())
          {
            ++wires;
          }
          if (wires > 1)
          {
            failAt("LOFT_FAILED", "section " + std::to_string(i) + " has holes", static_cast<int>(i));
          }
          algo.AddWire(BRepTools::OuterWire(f));
          break;
        }
        case TopAbs_WIRE:
        case TopAbs_EDGE:
          algo.AddWire(asWire(s, "sections[]"));
          break;
        case TopAbs_VERTEX:
          if (i != 0 && i + 1 != ids.size())
          {
            failAt("LOFT_FAILED", "only the first and the last section can be a point",
                   static_cast<int>(i));
          }
          algo.AddVertex(TopoDS::Vertex(s));
          break;
        default:
          failAt("INVALID_ARGUMENT", "section " + std::to_string(i) + " must be a face, wire, edge or vertex",
                 static_cast<int>(i));
      }
    }
    algo.CheckCompatibility(true);
    algo.Build();
    if (!algo.IsDone())
    {
      fail("LOFT_FAILED", "loft failed");
    }
    const TopoDS_Shape result = unwrap(algo.Shape());
    requireValid(result, "loft");

    const ShapeMap resFaces = mapOf(result, TopAbs_FACE);
    val            info     = val::object();
    info.set("first", intsToJs(faceIndices(algo.FirstShape(), resFaces)));
    info.set("last", intsToJs(faceIndices(algo.LastShape(), resFaces)));
    val sides = val::array();
    for (size_t i = 0; i < inputs.size(); ++i)
    {
      sides.set(i, sidesOf(algo, inputs[i], resFaces));
    }
    info.set("sections", sides);

    val history = buildHistory(algo, inputs, result);
    val r       = opResult(arena().put(result), history);
    r.set("loft", info);
    return r;
  });
}

} // namespace occtwasm
