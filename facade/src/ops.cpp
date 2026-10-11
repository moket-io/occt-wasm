// Modeling operations and their topological history.
//
// SPDX-License-Identifier: LGPL-2.1-only
#include "api.hpp"
#include "common.hpp"

#include <BRepAlgoAPI_BooleanOperation.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeShape.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <NCollection_List.hxx>
#include <Precision.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <sstream>

namespace occtwasm {

namespace {

struct KindInfo
{
  TopAbs_ShapeEnum type;
  const char*      key;  // property name in the history object
  const char*      name; // value of GeneratedEntry.kind
};

constexpr KindInfo kKinds[3] = {
  {TopAbs_FACE, "faces", "face"},
  {TopAbs_EDGE, "edges", "edge"},
  {TopAbs_VERTEX, "vertices", "vertex"},
};

int kindIndex(TopAbs_ShapeEnum type)
{
  for (int k = 0; k < 3; ++k)
  {
    if (kKinds[k].type == type)
    {
      return k;
    }
  }
  return -1;
}

void pushUnique(std::vector<int>& v, int x)
{
  if (x >= 0 && std::find(v.begin(), v.end(), x) == v.end())
  {
    v.push_back(x);
  }
}

val entry(int input, int from)
{
  val e = val::object();
  e.set("input", input);
  e.set("from", from);
  return e;
}

// Builds the history object for an algorithm. Indices are 0-based positions in
// TopExp::MapShapes order of the input shape (from) and of the result (to).
val buildHistory(BRepBuilderAPI_MakeShape&        algo,
                 const std::vector<TopoDS_Shape>& inputs,
                 const TopoDS_Shape&              result)
{
  ShapeMap res[3];
  for (int k = 0; k < 3; ++k)
  {
    res[k] = mapOf(result, kKinds[k].type);
  }

  val history = val::object();
  for (int k = 0; k < 3; ++k)
  {
    val modified  = val::array();
    val generated = val::array();
    val deleted   = val::array();
    val unchanged = val::array();
    int nMod = 0, nGen = 0, nDel = 0, nSame = 0;

    for (size_t i = 0; i < inputs.size(); ++i)
    {
      const ShapeMap in = mapOf(inputs[i], kKinds[k].type);
      for (int j = 1; j <= in.Extent(); ++j)
      {
        const TopoDS_Shape& s    = in(j);
        const int           from = j - 1;

        const int same = indexOf(res[k], s);
        if (same >= 0)
        {
          val e = entry(static_cast<int>(i), from);
          e.set("to", same);
          unchanged.set(nSame++, e);
        }

        // Individual queries may throw for some algorithms and sub-shape
        // kinds; a failing query means "no information", not a failed op.
        try
        {
          std::vector<int> to;
          for (NCollection_List<TopoDS_Shape>::Iterator it(algo.Modified(s)); it.More(); it.Next())
          {
            if (it.Value().ShapeType() == kKinds[k].type)
            {
              pushUnique(to, indexOf(res[k], it.Value()));
            }
          }
          if (!to.empty())
          {
            val e = entry(static_cast<int>(i), from);
            e.set("to", intsToJs(to));
            modified.set(nMod++, e);
          }
        }
        catch (...)
        {
        }

        try
        {
          std::vector<int> to[3];
          for (NCollection_List<TopoDS_Shape>::Iterator it(algo.Generated(s)); it.More(); it.Next())
          {
            const int kk = kindIndex(it.Value().ShapeType());
            if (kk >= 0)
            {
              pushUnique(to[kk], indexOf(res[kk], it.Value()));
            }
          }
          for (int kk = 0; kk < 3; ++kk)
          {
            if (!to[kk].empty())
            {
              val e = entry(static_cast<int>(i), from);
              e.set("kind", kKinds[kk].name);
              e.set("to", intsToJs(to[kk]));
              generated.set(nGen++, e);
            }
          }
        }
        catch (...)
        {
        }

        try
        {
          if (algo.IsDeleted(s))
          {
            deleted.set(nDel++, entry(static_cast<int>(i), from));
          }
        }
        catch (...)
        {
        }
      }
    }

    val kh = val::object();
    kh.set("modified", modified);
    kh.set("generated", generated);
    kh.set("deleted", deleted);
    kh.set("unchanged", unchanged);
    history.set(kKinds[k].key, kh);
  }
  return history;
}

std::vector<int> faceIndices(const TopoDS_Shape& part, const ShapeMap& resFaces)
{
  std::vector<int> out;
  if (part.IsNull())
  {
    return out;
  }
  const ShapeMap faces = mapOf(part, TopAbs_FACE);
  for (int i = 1; i <= faces.Extent(); ++i)
  {
    pushUnique(out, indexOf(resFaces, faces(i)));
  }
  return out;
}

// Bottom/top/side faces of a sweep (prism or revolution).
template <class Sweep>
val sweepInfo(Sweep& algo, const TopoDS_Shape& input, const TopoDS_Shape& result)
{
  const ShapeMap resFaces = mapOf(result, TopAbs_FACE);
  val            info     = val::object();

  std::vector<int> bottom, top;
  try
  {
    bottom = faceIndices(algo.FirstShape(), resFaces);
  }
  catch (...)
  {
  }
  try
  {
    top = faceIndices(algo.LastShape(), resFaces);
  }
  catch (...)
  {
  }
  info.set("bottom", intsToJs(bottom));
  info.set("top", intsToJs(top));

  val            sides = val::array();
  const ShapeMap edges = mapOf(input, TopAbs_EDGE);
  int            n     = 0;
  for (int i = 1; i <= edges.Extent(); ++i)
  {
    std::vector<int> faces;
    try
    {
      for (NCollection_List<TopoDS_Shape>::Iterator it(algo.Generated(edges(i))); it.More(); it.Next())
      {
        if (it.Value().ShapeType() == TopAbs_FACE)
        {
          pushUnique(faces, indexOf(resFaces, it.Value()));
        }
      }
    }
    catch (...)
    {
    }
    val side = val::object();
    side.set("edge", i - 1);
    side.set("faces", intsToJs(faces));
    sides.set(n++, side);
  }
  info.set("sides", sides);
  return info;
}

val opResult(ShapeId id, const val& history)
{
  val r = okResult();
  r.set("shape", id);
  r.set("history", history);
  return r;
}

std::vector<TopoDS_Edge> pickEdges(const TopoDS_Shape& shape, const val& jsEdges)
{
  const std::vector<int> idx = toInts(jsEdges, "edges");
  if (idx.empty())
  {
    fail("INVALID_ARGUMENT", "edges must not be empty");
  }
  const ShapeMap           map = mapOf(shape, TopAbs_EDGE);
  std::set<int>            seen;
  std::vector<TopoDS_Edge> out;
  for (int i : idx)
  {
    if (i < 0 || i >= map.Extent())
    {
      fail("INVALID_ARGUMENT",
           "edge index " + std::to_string(i) + " out of range [0, " + std::to_string(map.Extent())
             + ")");
    }
    if (seen.insert(i).second)
    {
      out.push_back(TopoDS::Edge(map(i + 1)));
    }
  }
  return out;
}

// Algorithms often wrap a single solid into a compound. Return the solid
// itself so that callers see the same shape type as the input.
TopoDS_Shape unwrap(const TopoDS_Shape& shape)
{
  if (shape.IsNull() || shape.ShapeType() != TopAbs_COMPOUND)
  {
    return shape;
  }
  TopoDS_Iterator it(shape);
  if (!it.More())
  {
    return shape;
  }
  TopoDS_Shape child = it.Value();
  it.Next();
  return it.More() ? shape : child;
}

void requireValid(const TopoDS_Shape& result, const char* what)
{
  if (!BRepCheck_Analyzer(result).IsValid())
  {
    fail("INVALID_RESULT", std::string(what) + " produced an invalid shape");
  }
}

} // namespace

// ---------------------------------------------------------------------------
val extrude(val shape, val dir, val distance)
{
  return guarded([&] {
    const TopoDS_Shape& input = arena().get(toShapeId(shape, "shape"));
    const gp_Dir        d     = toDir(dir, "dir");
    const double        dist  = toNumber(distance, "distance");
    if (std::abs(dist) <= Precision::Confusion())
    {
      fail("INVALID_ARGUMENT", "distance must be non-zero");
    }
    BRepPrimAPI_MakePrism algo(input, gp_Vec(d) * dist, false, true);
    if (!algo.IsDone())
    {
      fail("EXTRUDE_FAILED", "prism construction failed");
    }
    const TopoDS_Shape result = unwrap(algo.Shape());
    val                history = buildHistory(algo, {input}, result);
    val                sweep   = sweepInfo(algo, input, result);
    val                r       = opResult(arena().put(result), history);
    r.set("sweep", sweep);
    return r;
  });
}

val revolve(val shape, val axisOrigin, val axisDir, val angle)
{
  return guarded([&] {
    const TopoDS_Shape& input = arena().get(toShapeId(shape, "shape"));
    const gp_Ax1        axis(toPnt(axisOrigin, "axisOrigin"), toDir(axisDir, "axisDir"));
    const double        a = toNumber(angle, "angle");
    if (std::abs(a) <= Precision::Angular() || std::abs(a) > 2 * M_PI + Precision::Angular())
    {
      fail("INVALID_ARGUMENT", "angle must be in (0, 2*pi] by absolute value");
    }
    BRepPrimAPI_MakeRevol algo(input, axis, std::min(a, 2 * M_PI), false);
    if (!algo.IsDone())
    {
      fail("REVOLVE_FAILED", "revolution failed");
    }
    const TopoDS_Shape result = unwrap(algo.Shape());
    val                history = buildHistory(algo, {input}, result);
    val                sweep   = sweepInfo(algo, input, result);
    val                r       = opResult(arena().put(result), history);
    r.set("sweep", sweep);
    return r;
  });
}

val booleanOp(val op, val a, val b)
{
  return guarded([&] {
    if (!op.isString())
    {
      fail("INVALID_ARGUMENT", "op must be 'fuse', 'cut' or 'common'");
    }
    const std::string   name = op.as<std::string>();
    const TopoDS_Shape& sa   = arena().get(toShapeId(a, "a"));
    const TopoDS_Shape& sb   = arena().get(toShapeId(b, "b"));

    std::unique_ptr<BRepAlgoAPI_BooleanOperation> algo;
    if (name == "fuse")
    {
      algo = std::make_unique<BRepAlgoAPI_Fuse>(sa, sb);
    }
    else if (name == "cut")
    {
      algo = std::make_unique<BRepAlgoAPI_Cut>(sa, sb);
    }
    else if (name == "common")
    {
      algo = std::make_unique<BRepAlgoAPI_Common>(sa, sb);
    }
    else
    {
      fail("INVALID_ARGUMENT", "op must be 'fuse', 'cut' or 'common'");
    }

    if (!algo->IsDone() || algo->HasErrors())
    {
      std::ostringstream os;
      algo->DumpErrors(os);
      std::string msg = os.str();
      fail("BOOLEAN_FAILED", msg.empty() ? "boolean " + name + " failed" : msg);
    }
    const TopoDS_Shape result = unwrap(algo->Shape());
    val                history = buildHistory(*algo, {sa, sb}, result);
    return opResult(arena().put(result), history);
  });
}

val fillet(val shape, val edges, val radius)
{
  return guarded([&] {
    const TopoDS_Shape& input = arena().get(toShapeId(shape, "shape"));
    const double        r     = toNumber(radius, "radius");
    if (r <= Precision::Confusion())
    {
      fail("INVALID_ARGUMENT", "radius must be positive");
    }
    BRepFilletAPI_MakeFillet algo(input);
    for (const TopoDS_Edge& e : pickEdges(input, edges))
    {
      algo.Add(r, e);
    }
    algo.Build();
    if (!algo.IsDone())
    {
      fail("FILLET_FAILED",
           "fillet failed (" + std::to_string(algo.NbFaultyContours()) + " faulty contour(s), "
             + std::to_string(algo.NbFaultyVertices()) + " faulty vertex(es))");
    }
    const TopoDS_Shape result = unwrap(algo.Shape());
    requireValid(result, "fillet");
    val history = buildHistory(algo, {input}, result);
    return opResult(arena().put(result), history);
  });
}

val chamfer(val shape, val edges, val distance)
{
  return guarded([&] {
    const TopoDS_Shape& input = arena().get(toShapeId(shape, "shape"));
    const double        d     = toNumber(distance, "distance");
    if (d <= Precision::Confusion())
    {
      fail("INVALID_ARGUMENT", "distance must be positive");
    }
    BRepFilletAPI_MakeChamfer algo(input);
    for (const TopoDS_Edge& e : pickEdges(input, edges))
    {
      algo.Add(d, e);
    }
    algo.Build();
    if (!algo.IsDone())
    {
      fail("CHAMFER_FAILED", "chamfer failed");
    }
    const TopoDS_Shape result = unwrap(algo.Shape());
    requireValid(result, "chamfer");
    val history = buildHistory(algo, {input}, result);
    return opResult(arena().put(result), history);
  });
}

val transform(val shape, val matrix)
{
  return guarded([&] {
    const TopoDS_Shape&       input = arena().get(toShapeId(shape, "shape"));
    const std::vector<double> m     = toDoubles(matrix, "matrix", 12);
    gp_Trsf                   trsf;
    // Row-major 3x4: rotation (possibly with a reflection) and translation.
    trsf.SetValues(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
    if (std::abs(std::abs(trsf.ScaleFactor()) - 1.0) > 1e-9)
    {
      fail("INVALID_ARGUMENT", "matrix must be a rigid motion or a reflection");
    }
    BRepBuilderAPI_Transform algo(input, trsf, true);
    if (!algo.IsDone())
    {
      fail("TRANSFORM_FAILED", "transformation failed");
    }
    const TopoDS_Shape result  = algo.Shape();
    val                history = buildHistory(algo, {input}, result);
    return opResult(arena().put(result), history);
  });
}

} // namespace occtwasm
