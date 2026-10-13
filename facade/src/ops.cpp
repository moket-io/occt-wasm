// Modeling operations and their topological history.
//
// SPDX-License-Identifier: LGPL-2.1-only
#include "api.hpp"
#include "common.hpp"
#include "history.hpp"

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
