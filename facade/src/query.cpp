// Read-only queries: topology, mass properties, tessellation.
//
// SPDX-License-Identifier: LGPL-2.1-only
#include "api.hpp"
#include "common.hpp"
#include "subshapes.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepLib_ToolTriangulatedShape.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <GProp_GProps.hxx>
#include <NCollection_IndexedDataMap.hxx>
#include <NCollection_List.hxx>
#include <Poly_Polygon3D.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp.hxx>
#include <gp_Ax1.hxx>
#include <gp_Circ.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>

#include <algorithm>
#include <cmath>
#include <utility>

namespace occtwasm {

namespace {

using AncestorMap =
  NCollection_IndexedDataMap<TopoDS_Shape, NCollection_List<TopoDS_Shape>, TopTools_ShapeMapHasher>;

val bboxToJs(const Bnd_Box& box)
{
  val a = val::array();
  if (box.IsVoid())
  {
    for (int i = 0; i < 6; ++i)
    {
      a.set(i, 0.0);
    }
    return a;
  }
  double xmin, ymin, zmin, xmax, ymax, zmax;
  box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
  a.set(0, xmin);
  a.set(1, ymin);
  a.set(2, zmin);
  a.set(3, xmax);
  a.set(4, ymax);
  a.set(5, zmax);
  return a;
}

// Codes of the names in SURFACES and CURVES of index.mjs.
double surfaceCode(GeomAbs_SurfaceType t)
{
  switch (t)
  {
    case GeomAbs_Plane:          return 0; // plane
    case GeomAbs_Cylinder:       return 1; // cylinder
    case GeomAbs_Cone:           return 2; // cone
    case GeomAbs_Sphere:         return 3; // sphere
    case GeomAbs_Torus:          return 4; // torus
    case GeomAbs_BezierSurface:
    case GeomAbs_BSplineSurface: return 5; // bspline
    default:                     return 6; // other
  }
}

double curveCode(GeomAbs_CurveType t)
{
  switch (t)
  {
    case GeomAbs_Line:         return 0; // line
    case GeomAbs_Circle:       return 1; // circle
    case GeomAbs_Ellipse:      return 2; // ellipse
    case GeomAbs_BezierCurve:
    case GeomAbs_BSplineCurve: return 3; // bspline
    default:                   return 4; // other
  }
}

void pushBox(std::vector<double>& v, const Bnd_Box& box)
{
  double b[6] = {0, 0, 0, 0, 0, 0};
  if (!box.IsVoid())
  {
    box.Get(b[0], b[1], b[2], b[3], b[4], b[5]);
  }
  v.insert(v.end(), b, b + 6);
}

// Sub-shape by { kind, index }, or the whole shape when sub is null/undefined.
TopoDS_Shape subShape(const TopoDS_Shape& shape, const val& sub, const char* name)
{
  if (sub.isUndefined() || sub.isNull())
  {
    return shape;
  }
  if (!sub["kind"].isString())
  {
    fail("INVALID_ARGUMENT", std::string(name) + ".kind must be 'face', 'edge' or 'vertex'");
  }
  const std::string kind = sub["kind"].as<std::string>();
  TopAbs_ShapeEnum  type = TopAbs_SHAPE;
  if (kind == "face")
  {
    type = TopAbs_FACE;
  }
  else if (kind == "edge")
  {
    type = TopAbs_EDGE;
  }
  else if (kind == "vertex")
  {
    type = TopAbs_VERTEX;
  }
  else
  {
    fail("INVALID_ARGUMENT", std::string(name) + ".kind must be 'face', 'edge' or 'vertex'");
  }
  const int      index = toInt(sub["index"], name);
  const ShapeMap map   = mapOf(shape, type);
  if (index < 0 || index >= map.Extent())
  {
    fail("INVALID_ARGUMENT", std::string(name) + ".index out of range");
  }
  return map(index + 1);
}

} // namespace

// ---------------------------------------------------------------------------
// Packed for speed, unpacked into objects by the JS loader (index.mjs):
// faces 23 numbers each, edges 20 plus their faces in packedEdgeFaces
// (count, then indices), vertices 3. Per face and edge data comes from the sub-shape
// store, so a result only computes the faces and edges an operation changed.
val topology(val shapeArg)
{
  return guarded([&] {
    const TopoDS_Shape& shape = arena().get(toShapeId(shapeArg, "shape"));
    const ShapeMap      faces = mapOf(shape, TopAbs_FACE);
    const ShapeMap      edges = mapOf(shape, TopAbs_EDGE);
    const ShapeMap      verts = mapOf(shape, TopAbs_VERTEX);

    auto pushPnt = [](std::vector<double>& v, const gp_XYZ& p) {
      v.push_back(p.X());
      v.push_back(p.Y());
      v.push_back(p.Z());
    };
    auto pushAxis = [&](std::vector<double>& v, bool has, const gp_Ax1& ax, double radius) {
      v.push_back(has ? 1 : 0);
      pushPnt(v, ax.Location().XYZ());
      pushPnt(v, ax.Direction().XYZ());
      v.push_back(radius);
    };

    // surface, area, centroid 3, normal 3, bbox 6, reversed, axis 8
    std::vector<double> faceOut;
    faceOut.reserve(static_cast<size_t>(faces.Extent()) * 23);
    for (int i = 1; i <= faces.Extent(); ++i)
    {
      const TopoDS_Face face = TopoDS::Face(faces(i));
      const FaceData&   d    = faceData(face);
      const bool        rev  = face.Orientation() == TopAbs_REVERSED;
      faceOut.push_back(surfaceCode(static_cast<GeomAbs_SurfaceType>(d.surface)));
      faceOut.push_back(d.area.Mass());
      pushPnt(faceOut, d.area.CentreOfMass().XYZ());
      pushPnt(faceOut, rev ? d.normal.Reversed().XYZ() : d.normal.XYZ());
      pushBox(faceOut, d.box);
      faceOut.push_back(rev ? 1 : 0);
      pushAxis(faceOut, d.hasAxis, d.axis, d.radius);
    }

    AncestorMap edgeFaces;
    TopExp::MapShapesAndUniqueAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);

    // degenerated, curve, length, midpoint 3, start 3, end 3, circle 8
    std::vector<double>  edgeOut;
    std::vector<int32_t> adjacent;
    edgeOut.reserve(static_cast<size_t>(edges.Extent()) * 20);
    adjacent.reserve(static_cast<size_t>(edges.Extent()) * 3);
    for (int i = 1; i <= edges.Extent(); ++i)
    {
      const TopoDS_Edge edge = TopoDS::Edge(edges(i));
      const EdgeData&   d    = edgeData(edge);
      edgeOut.push_back(d.degenerated ? 1 : 0);
      edgeOut.push_back(curveCode(static_cast<GeomAbs_CurveType>(d.curve)));
      edgeOut.push_back(d.length);
      pushPnt(edgeOut, d.midpoint.XYZ());
      pushPnt(edgeOut, d.start.XYZ());
      pushPnt(edgeOut, d.end.XYZ());
      pushAxis(edgeOut, d.circle, d.axis, d.radius);
      // Faces of each edge: count, then indices.
      const size_t at = adjacent.size();
      adjacent.push_back(0);
      if (const NCollection_List<TopoDS_Shape>* anc = edgeFaces.Seek(edge))
      {
        for (NCollection_List<TopoDS_Shape>::Iterator it(*anc); it.More(); it.Next())
        {
          const int idx = indexOf(faces, it.Value());
          if (idx >= 0
              && std::find(adjacent.begin() + at + 1, adjacent.end(), idx) == adjacent.end())
          {
            adjacent.push_back(idx);
          }
        }
      }
      adjacent[at] = static_cast<int32_t>(adjacent.size() - at - 1);
    }

    std::vector<double> vertOut;
    vertOut.reserve(static_cast<size_t>(verts.Extent()) * 3);
    for (int i = 1; i <= verts.Extent(); ++i)
    {
      pushPnt(vertOut, BRep_Tool::Pnt(TopoDS::Vertex(verts(i))).XYZ());
    }

    val r = okResult();
    r.set("packedFaces", float64ToJs(faceOut));
    r.set("packedEdges", float64ToJs(edgeOut));
    r.set("packedEdgeFaces", int32ToJs(adjacent));
    r.set("packedVertices", float64ToJs(vertOut));
    return r;
  });
}

// ---------------------------------------------------------------------------
val properties(val shapeArg)
{
  return guarded([&] {
    const TopoDS_Shape& shape = arena().get(toShapeId(shapeArg, "shape"));

    // Sums of the faces' contributions, as BRepGProp does, from the store.
    GProp_GProps vol(gp::Origin());
    GProp_GProps surf(gp::Origin());
    Bnd_Box      box;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next())
    {
      const TopoDS_Face face = TopoDS::Face(ex.Current());
      const FaceData&   d    = faceData(face);
      vol.Add(faceVolume(face));
      surf.Add(d.area);
      box.Add(d.box);
    }
    // Edges and vertices outside faces count for the box (wires, points).
    if (!shape.IsNull() && shape.ShapeType() > TopAbs_FACE)
    {
      BRepBndLib::AddOptimal(shape, box, false, false);
    }
    else
    {
      for (TopExp_Explorer ex(shape, TopAbs_EDGE, TopAbs_FACE); ex.More(); ex.Next())
      {
        BRepBndLib::AddOptimal(ex.Current(), box, false, false);
      }
      for (TopExp_Explorer ex(shape, TopAbs_VERTEX, TopAbs_EDGE); ex.More(); ex.Next())
      {
        BRepBndLib::AddOptimal(ex.Current(), box, false, false);
      }
    }

    const double volume   = vol.Mass();
    const gp_Pnt centroid = std::abs(volume) > 1e-12 ? vol.CentreOfMass() : surf.CentreOfMass();

    val r = okResult();
    r.set("shapeType", std::string(shapeTypeName(shape.ShapeType())));
    r.set("volume", volume);
    r.set("area", surf.Mass());
    r.set("centroid", pntToJs(centroid));
    r.set("bbox", bboxToJs(box));
    r.set("valid", isValid(shape));
    r.set("solids", mapOf(shape, TopAbs_SOLID).Extent());
    r.set("shells", mapOf(shape, TopAbs_SHELL).Extent());
    r.set("faces", mapOf(shape, TopAbs_FACE).Extent());
    r.set("edges", mapOf(shape, TopAbs_EDGE).Extent());
    r.set("vertices", mapOf(shape, TopAbs_VERTEX).Extent());
    return r;
  });
}

// ---------------------------------------------------------------------------
val mesh(val shapeArg, val linearDeflection, val angularDeflection)
{
  return guarded([&] {
    const TopoDS_Shape& shape = arena().get(toShapeId(shapeArg, "shape"));
    const double        lin   = toNumber(linearDeflection, "linearDeflection");
    const double        ang   = toNumber(angularDeflection, "angularDeflection");
    if (lin <= 0 || ang <= 0)
    {
      fail("INVALID_ARGUMENT", "deflections must be positive");
    }

    // Faces keep their triangulation while the deflections stay the same:
    // a result shares the faces an operation did not touch with its input,
    // so only the changed faces are meshed again. Other deflections drop
    // what this shape has, so the result depends only on them.
    static double lastLin = 0, lastAng = 0;
    if (lin != lastLin || ang != lastAng)
    {
      BRepTools::Clean(shape);
      lastLin = lin;
      lastAng = ang;
    }
    BRepMesh_IncrementalMesh mesher(shape, lin, false, ang, false);
    if (!mesher.IsDone())
    {
      fail("MESH_FAILED", "tessellation failed");
    }

    const ShapeMap faces = mapOf(shape, TopAbs_FACE);
    const ShapeMap edges = mapOf(shape, TopAbs_EDGE);

    std::vector<float>    positions, normals, edgePositions;
    std::vector<uint32_t> indices, faceRanges, edgeRanges;
    faceRanges.reserve(2 * faces.Extent());
    edgeRanges.reserve(2 * edges.Extent());

    for (int i = 1; i <= faces.Extent(); ++i)
    {
      const TopoDS_Face face  = TopoDS::Face(faces(i));
      const uint32_t    start = static_cast<uint32_t>(indices.size());
      TopLoc_Location   loc;
      const occ::handle<Poly_Triangulation>& tri = BRep_Tool::Triangulation(face, loc);
      if (!tri.IsNull())
      {
        BRepLib_ToolTriangulatedShape::ComputeNormals(face, tri);
        const gp_Trsf  trsf     = loc.Transformation();
        const bool     reversed = face.Orientation() == TopAbs_REVERSED;
        const uint32_t base     = static_cast<uint32_t>(positions.size() / 3);
        for (int n = 1; n <= tri->NbNodes(); ++n)
        {
          const gp_Pnt p = tri->Node(n).Transformed(trsf);
          positions.push_back(static_cast<float>(p.X()));
          positions.push_back(static_cast<float>(p.Y()));
          positions.push_back(static_cast<float>(p.Z()));
          gp_Dir nd = tri->HasNormals() ? gp_Dir(tri->Normal(n)) : gp_Dir(0, 0, 1);
          nd.Transform(trsf);
          if (reversed)
          {
            nd.Reverse();
          }
          normals.push_back(static_cast<float>(nd.X()));
          normals.push_back(static_cast<float>(nd.Y()));
          normals.push_back(static_cast<float>(nd.Z()));
        }
        for (int t = 1; t <= tri->NbTriangles(); ++t)
        {
          int a, b, c;
          tri->Triangle(t).Get(a, b, c);
          if (reversed)
          {
            std::swap(b, c);
          }
          indices.push_back(base + a - 1);
          indices.push_back(base + b - 1);
          indices.push_back(base + c - 1);
        }
      }
      faceRanges.push_back(start);
      faceRanges.push_back(static_cast<uint32_t>(indices.size()) - start);
    }

    AncestorMap edgeFaces;
    TopExp::MapShapesAndUniqueAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);

    auto pushPoint = [&](const gp_Pnt& p) {
      edgePositions.push_back(static_cast<float>(p.X()));
      edgePositions.push_back(static_cast<float>(p.Y()));
      edgePositions.push_back(static_cast<float>(p.Z()));
    };

    for (int i = 1; i <= edges.Extent(); ++i)
    {
      const TopoDS_Edge edge  = TopoDS::Edge(edges(i));
      const uint32_t    start = static_cast<uint32_t>(edgePositions.size() / 3);
      bool              done  = BRep_Tool::Degenerated(edge);

      // Prefer the polygon on the adjacent face triangulation so that edge
      // points coincide with mesh vertices.
      if (!done)
      {
        if (const NCollection_List<TopoDS_Shape>* anc = edgeFaces.Seek(edge))
        {
          for (NCollection_List<TopoDS_Shape>::Iterator it(*anc); it.More() && !done; it.Next())
          {
            TopLoc_Location                        loc;
            const occ::handle<Poly_Triangulation>& tri =
              BRep_Tool::Triangulation(TopoDS::Face(it.Value()), loc);
            if (tri.IsNull())
            {
              continue;
            }
            occ::handle<Poly_PolygonOnTriangulation> poly =
              BRep_Tool::PolygonOnTriangulation(edge, tri, loc);
            if (poly.IsNull())
            {
              continue;
            }
            const gp_Trsf trsf = loc.Transformation();
            for (int k = 1; k <= poly->NbNodes(); ++k)
            {
              pushPoint(tri->Node(poly->Node(k)).Transformed(trsf));
            }
            done = true;
          }
        }
      }
      if (!done)
      {
        TopLoc_Location                    loc;
        const occ::handle<Poly_Polygon3D>& poly = BRep_Tool::Polygon3D(edge, loc);
        if (!poly.IsNull())
        {
          const gp_Trsf trsf = loc.Transformation();
          for (int k = 1; k <= poly->NbNodes(); ++k)
          {
            pushPoint(poly->Nodes().Value(k).Transformed(trsf));
          }
          done = true;
        }
      }
      if (!done)
      {
        BRepAdaptor_Curve           curve(edge);
        GCPnts_TangentialDeflection disc(curve, ang, lin);
        for (int k = 1; k <= disc.NbPoints(); ++k)
        {
          pushPoint(disc.Value(k));
        }
      }
      edgeRanges.push_back(start);
      edgeRanges.push_back(static_cast<uint32_t>(edgePositions.size() / 3) - start);
    }

    val r = okResult();
    r.set("positions", float32ToJs(positions));
    r.set("normals", float32ToJs(normals));
    r.set("indices", uint32ToJs(indices));
    r.set("faceRanges", uint32ToJs(faceRanges));
    r.set("edgePositions", float32ToJs(edgePositions));
    r.set("edgeRanges", uint32ToJs(edgeRanges));
    return r;
  });
}

// ---------------------------------------------------------------------------
val check(val shapeArg)
{
  return guarded([&] {
    const TopoDS_Shape& shape = arena().get(toShapeId(shapeArg, "shape"));
    val                 r     = okResult();
    r.set("valid", isValid(shape));
    r.set("shapeType", std::string(shapeTypeName(shape.ShapeType())));
    r.set("solids", mapOf(shape, TopAbs_SOLID).Extent());
    return r;
  });
}

// ---------------------------------------------------------------------------
val distance(val a, val subA, val b, val subB)
{
  return guarded([&] {
    const TopoDS_Shape sa = subShape(arena().get(toShapeId(a, "a")), subA, "subA");
    const TopoDS_Shape sb = subShape(arena().get(toShapeId(b, "b")), subB, "subB");
    BRepExtrema_DistShapeShape algo(sa, sb);
    if (!algo.IsDone() || algo.NbSolution() < 1)
    {
      fail("DISTANCE_FAILED", "minimum distance could not be computed");
    }
    val r = okResult();
    r.set("value", algo.Value());
    r.set("pointA", pntToJs(algo.PointOnShape1(1)));
    r.set("pointB", pntToJs(algo.PointOnShape2(1)));
    r.set("solutions", static_cast<int>(algo.NbSolution()));
    return r;
  });
}

} // namespace occtwasm
