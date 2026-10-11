// Read-only queries: topology, mass properties, tessellation.
//
// SPDX-License-Identifier: LGPL-2.1-only
#include "api.hpp"
#include "common.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepCheck_Analyzer.hxx>
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
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>
#include <gp_Circ.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>

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

Bnd_Box optimalBox(const TopoDS_Shape& s)
{
  Bnd_Box box;
  BRepBndLib::AddOptimal(s, box, false, false);
  return box;
}

const char* surfaceName(GeomAbs_SurfaceType t)
{
  switch (t)
  {
    case GeomAbs_Plane:         return "plane";
    case GeomAbs_Cylinder:      return "cylinder";
    case GeomAbs_Cone:          return "cone";
    case GeomAbs_Sphere:        return "sphere";
    case GeomAbs_Torus:         return "torus";
    case GeomAbs_BezierSurface:
    case GeomAbs_BSplineSurface: return "bspline";
    default:                    return "other";
  }
}

const char* curveName(GeomAbs_CurveType t)
{
  switch (t)
  {
    case GeomAbs_Line:        return "line";
    case GeomAbs_Circle:      return "circle";
    case GeomAbs_Ellipse:     return "ellipse";
    case GeomAbs_BezierCurve:
    case GeomAbs_BSplineCurve: return "bspline";
    default:                  return "other";
  }
}

// Outward normal at the middle of the face parameter range.
gp_Dir faceNormal(const TopoDS_Face& face, const BRepAdaptor_Surface& surf)
{
  double u0, u1, v0, v1;
  BRepTools::UVBounds(face, u0, u1, v0, v1);
  BRepLProp_SLProps props(surf, (u0 + u1) / 2, (v0 + v1) / 2, 1, 1e-7);
  gp_Dir            n(0, 0, 1);
  if (props.IsNormalDefined())
  {
    n = props.Normal();
  }
  if (face.Orientation() == TopAbs_REVERSED)
  {
    n.Reverse();
  }
  return n;
}

val axisToJs(const gp_Ax1& ax)
{
  val a = val::object();
  a.set("origin", pntToJs(ax.Location()));
  a.set("dir", dirToJs(ax.Direction()));
  return a;
}

// Axis and radius of analytic faces, for picking hole axes and measuring.
void setSurfaceAxis(val& f, const BRepAdaptor_Surface& surf)
{
  switch (surf.GetType())
  {
    case GeomAbs_Cylinder: {
      const gp_Cylinder c = surf.Cylinder();
      f.set("axis", axisToJs(c.Axis()));
      f.set("radius", c.Radius());
      break;
    }
    case GeomAbs_Cone: {
      const gp_Cone c = surf.Cone();
      f.set("axis", axisToJs(c.Axis()));
      f.set("radius", c.RefRadius());
      break;
    }
    case GeomAbs_Sphere: {
      const gp_Sphere c = surf.Sphere();
      f.set("axis", axisToJs(gp_Ax1(c.Location(), c.Position().Direction())));
      f.set("radius", c.Radius());
      break;
    }
    case GeomAbs_Torus: {
      const gp_Torus c = surf.Torus();
      f.set("axis", axisToJs(c.Axis()));
      f.set("radius", c.MajorRadius());
      break;
    }
    default:
      break;
  }
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
val topology(val shapeArg)
{
  return guarded([&] {
    const TopoDS_Shape& shape = arena().get(toShapeId(shapeArg, "shape"));
    const ShapeMap      faces = mapOf(shape, TopAbs_FACE);
    const ShapeMap      edges = mapOf(shape, TopAbs_EDGE);
    const ShapeMap      verts = mapOf(shape, TopAbs_VERTEX);

    val jsFaces = val::array();
    for (int i = 1; i <= faces.Extent(); ++i)
    {
      const TopoDS_Face   face = TopoDS::Face(faces(i));
      BRepAdaptor_Surface surf(face);
      GProp_GProps        props;
      BRepGProp::SurfaceProperties(face, props);

      val f = val::object();
      f.set("index", i - 1);
      f.set("surface", std::string(surfaceName(surf.GetType())));
      f.set("area", props.Mass());
      f.set("centroid", pntToJs(props.CentreOfMass()));
      f.set("normal", dirToJs(faceNormal(face, surf)));
      f.set("bbox", bboxToJs(optimalBox(face)));
      f.set("reversed", face.Orientation() == TopAbs_REVERSED);
      setSurfaceAxis(f, surf);
      jsFaces.set(i - 1, f);
    }

    AncestorMap edgeFaces;
    TopExp::MapShapesAndUniqueAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);

    val jsEdges = val::array();
    for (int i = 1; i <= edges.Extent(); ++i)
    {
      const TopoDS_Edge edge = TopoDS::Edge(edges(i));
      val               e    = val::object();
      e.set("index", i - 1);
      const bool degenerated = BRep_Tool::Degenerated(edge);
      e.set("degenerated", degenerated);
      if (degenerated)
      {
        e.set("curve", std::string("other"));
        e.set("length", 0.0);
        e.set("midpoint", pntToJs(BRep_Tool::Pnt(TopExp::FirstVertex(edge))));
      }
      else
      {
        BRepAdaptor_Curve curve(edge);
        GProp_GProps      props;
        BRepGProp::LinearProperties(edge, props);
        e.set("curve", std::string(curveName(curve.GetType())));
        e.set("length", props.Mass());
        e.set("midpoint", pntToJs(curve.Value((curve.FirstParameter() + curve.LastParameter()) / 2)));
        e.set("start", pntToJs(curve.Value(curve.FirstParameter())));
        e.set("end", pntToJs(curve.Value(curve.LastParameter())));
        if (curve.GetType() == GeomAbs_Circle)
        {
          const gp_Circ c = curve.Circle();
          e.set("axis", axisToJs(c.Axis()));
          e.set("radius", c.Radius());
        }
      }
      std::vector<int> adj;
      if (const NCollection_List<TopoDS_Shape>* anc = edgeFaces.Seek(edge))
      {
        for (NCollection_List<TopoDS_Shape>::Iterator it(*anc); it.More(); it.Next())
        {
          const int idx = indexOf(faces, it.Value());
          if (idx >= 0)
          {
            adj.push_back(idx);
          }
        }
      }
      e.set("faces", intsToJs(adj));
      jsEdges.set(i - 1, e);
    }

    val jsVerts = val::array();
    for (int i = 1; i <= verts.Extent(); ++i)
    {
      val v = val::object();
      v.set("index", i - 1);
      v.set("point", pntToJs(BRep_Tool::Pnt(TopoDS::Vertex(verts(i)))));
      jsVerts.set(i - 1, v);
    }

    val r = okResult();
    r.set("faces", jsFaces);
    r.set("edges", jsEdges);
    r.set("vertices", jsVerts);
    return r;
  });
}

// ---------------------------------------------------------------------------
val properties(val shapeArg)
{
  return guarded([&] {
    const TopoDS_Shape& shape = arena().get(toShapeId(shapeArg, "shape"));

    GProp_GProps vol;
    BRepGProp::VolumeProperties(shape, vol);
    GProp_GProps surf;
    BRepGProp::SurfaceProperties(shape, surf);

    const double volume   = vol.Mass();
    const gp_Pnt centroid = std::abs(volume) > 1e-12 ? vol.CentreOfMass() : surf.CentreOfMass();

    val r = okResult();
    r.set("shapeType", std::string(shapeTypeName(shape.ShapeType())));
    r.set("volume", volume);
    r.set("area", surf.Mass());
    r.set("centroid", pntToJs(centroid));
    r.set("bbox", bboxToJs(optimalBox(shape)));
    r.set("valid", BRepCheck_Analyzer(shape).IsValid());
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

    // Drop any previous triangulation so the result depends only on the
    // requested deflection.
    BRepTools::Clean(shape);
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
    r.set("valid", BRepCheck_Analyzer(shape).IsValid());
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
