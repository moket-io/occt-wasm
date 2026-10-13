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
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GeomFill_Trihedron.hxx>
#include <Precision.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
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
  const size_t n = edges.size();
  if (n == 0)
  {
    fail("INVALID_ARGUMENT", "no edges to join");
  }
  // End points in the edges' own direction.
  std::vector<gp_Pnt> first(n), last(n);
  for (size_t i = 0; i < n; ++i)
  {
    first[i] = BRep_Tool::Pnt(TopExp::FirstVertex(edges[i], true));
    last[i]  = BRep_Tool::Pnt(TopExp::LastVertex(edges[i], true));
  }
  auto touching = [&](const gp_Pnt& p, size_t skip) {
    std::vector<size_t> out;
    for (size_t j = 0; j < n; ++j)
    {
      if (j != skip && (first[j].Distance(p) <= kJoinTolerance || last[j].Distance(p) <= kJoinTolerance))
      {
        out.push_back(j);
      }
    }
    return out;
  };

  // Start at an open end when there is one, so that an open path is walked
  // from one end to the other.
  size_t start    = 0;
  bool   reversed = false;
  for (size_t i = 0; i < n; ++i)
  {
    if (touching(first[i], i).empty())
    {
      start = i;
      break;
    }
    if (touching(last[i], i).empty())
    {
      start    = i;
      reversed = true;
      break;
    }
  }

  std::vector<bool>   used(n, false);
  std::vector<size_t> order{start};
  std::vector<bool>   flip{reversed};
  used[start] = true;
  gp_Pnt end  = reversed ? first[start] : last[start];
  for (size_t step = 1; step < n; ++step)
  {
    std::vector<size_t> next;
    for (size_t j : touching(end, n))
    {
      if (!used[j])
      {
        next.push_back(j);
      }
    }
    if (next.size() > 1)
    {
      fail("PATH_NOT_CONNECTED", "the path branches at " + pointText(end));
    }
    if (next.empty())
    {
      // Name the gap between this end and the closest unused edge.
      double best = std::numeric_limits<double>::max();
      for (size_t j = 0; j < n; ++j)
      {
        if (!used[j])
        {
          best = std::min({best, first[j].Distance(end), last[j].Distance(end)});
        }
      }
      fail("PATH_NOT_CONNECTED", "the path has a gap of " + std::to_string(best) + " mm at " + pointText(end));
    }
    const size_t j = next[0];
    used[j]        = true;
    const bool rev = last[j].Distance(end) <= kJoinTolerance;
    order.push_back(j);
    flip.push_back(rev);
    end = rev ? first[j] : last[j];
  }

  // Consecutive edges share their vertices in the wire.
  BRepBuilderAPI_MakeWire  mw;
  std::vector<TopoDS_Edge> added(n);
  for (size_t i = 0; i < order.size(); ++i)
  {
    const TopoDS_Edge e = flip[i] ? TopoDS::Edge(edges[order[i]].Reversed()) : edges[order[i]];
    mw.Add(e);
    if (!mw.IsDone())
    {
      fail("PATH_NOT_CONNECTED", "an edge does not connect at " + pointText(flip[i] ? last[order[i]] : first[order[i]]));
    }
    added[order[i]] = mw.Edge();
  }
  TopoDS_Wire    wire = mw.Wire();
  const ShapeMap map  = mapOf(wire, TopAbs_EDGE);
  wireEdge.assign(n, -1);
  for (size_t i = 0; i < n; ++i)
  {
    wireEdge[i] = indexOf(map, added[i]);
  }
  const gp_Pnt begin = flip.front() ? last[order.front()] : first[order.front()];
  closed             = begin.Distance(end) <= kJoinTolerance;
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
    if (closing.IsEmpty())
    {
      // Nothing removed: the offset shell becomes a cavity (inside) or the
      // new outside (outside) of one solid.
      BRepOffsetAPI_MakeOffsetShape offset;
      offset.PerformByJoin(input, d, toTolerance(options), BRepOffset_Skin, false, false, GeomAbs_Arc);
      if (!offset.IsDone())
      {
        fail("SHELL_FAILED",
             std::string("shell failed (") + offsetErrorName(offset.MakeOffset().Error()) + ")");
      }
      const TopoDS_Shape grown = unwrap(offset.Shape());
      TopExp_Explorer    a(d < 0 ? input : grown, TopAbs_SHELL);
      TopExp_Explorer    b(d < 0 ? grown : input, TopAbs_SHELL);
      if (!a.More() || !b.More())
      {
        fail("SHELL_FAILED", "shell needs a solid");
      }
      TopoDS_Solid solid;
      BRep_Builder builder;
      builder.MakeSolid(solid);
      builder.Add(solid, a.Current());
      builder.Add(solid, b.Current().Reversed());
      requireValid(solid, "shell");
      val history = buildHistory(offset, {input}, solid);
      return opResult(arena().put(solid), history);
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
