// Hidden line removal: exact projection of shapes onto a view plane, for 2D
// drawings. The shapes hide each other. The visible and hidden parts of every
// edge are returned as 2D curves in view coordinates, each with the shape it
// comes from and the index of its edge (or, for silhouettes, its face).
//
// The edge walk follows HLRBRep_HLRToShape (InternalCompound, DrawFace,
// DrawEdge), which builds compounds and drops where each curve came from.
// HLR works on a copy of each shape with silhouettes added to its faces
// (HLRTopoBRep_OutLiner); the copy's data maps its edges back to the edges
// of the input, and silhouette edges to the input face they lie on.
//
// SPDX-License-Identifier: LGPL-2.1-only
#include "api.hpp"
#include "common.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <HLRAlgo_EdgeIterator.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRBRep.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_Data.hxx>
#include <HLRBRep_EdgeData.hxx>
#include <HLRBRep_FaceIterator.hxx>
#include <HLRBRep_ShapeBounds.hxx>
#include <HLRTopoBRep_OutLiner.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace occtwasm {

namespace {

// Drawn line kinds, as HLRBRep_HLRToShape numbers them.
constexpr int kOutline = 2; // silhouettes of curved faces (internal edges)
constexpr int kSmooth  = 3; // tangent (G1) edges between faces
constexpr int kSharp   = 5; // sharp edges, and faces seen edge on

struct Context
{
  occ::handle<HLRBRep_Data>                      ds;
  std::vector<occ::handle<HLRTopoBRep_OutLiner>> outliners; // per input shape
  std::vector<ShapeMap>                          edges;
  std::vector<ShapeMap>                          faces;
  // Per input shape and edge: the face a seam edge closes, -1 for others.
  std::vector<std::vector<int>> seamFace;
  double                        deflection;
  val                                            out;
  int                                            count = 0;
};

// Where a sub-shape of HLR's copy comes from: an edge of an input shape, or
// (silhouettes, and faces) a face of it.
struct Source
{
  int shape = -1;
  int edge  = -1;
  int face  = -1;
};

Source sourceOf(Context& c, const TopoDS_Shape& sub)
{
  for (size_t i = 0; i < c.outliners.size(); ++i)
  {
    const TopoDS_Shape old  = c.outliners[i]->DataStructure().NewSOldS(sub);
    const int          kind = old.IsNull() ? -1 : old.ShapeType();
    if (kind == TopAbs_EDGE)
    {
      const int e = indexOf(c.edges[i], old);
      if (e >= 0)
        return {static_cast<int>(i), e, -1};
    }
    else if (kind == TopAbs_FACE)
    {
      const int f = indexOf(c.faces[i], old);
      if (f >= 0)
        return {static_cast<int>(i), -1, f};
    }
  }
  return {};
}

// Points of a polyline that lie on one straight line (a circle seen edge on),
// as its two extreme points; false when it bends more than tol.
bool asSegment(const std::vector<gp_Pnt>& pts, double tol, gp_Pnt& a, gp_Pnt& b)
{
  if (pts.size() < 2)
    return false;
  size_t far = 0;
  for (size_t i = 1; i < pts.size(); ++i)
    if (pts[0].SquareDistance(pts[i]) > pts[0].SquareDistance(pts[far]))
      far = i;
  const double len = pts[0].Distance(pts[far]);
  if (len < 1e-12)
    return false;
  const gp_Vec dir(pts[0], pts[far]);
  const gp_Vec u = dir / len;
  double       lo = 0, hi = 0;
  for (const gp_Pnt& p : pts)
  {
    const gp_Vec v(pts[0], p);
    if (v.Crossed(u).Magnitude() > tol)
      return false;
    const double t = v.Dot(u);
    lo             = std::min(lo, t);
    hi             = std::max(hi, t);
  }
  a = pts[0].Translated(u * lo);
  b = pts[0].Translated(u * hi);
  return true;
}

val xy(const gp_Pnt& p)
{
  val a = val::array();
  a.set(0, p.X());
  a.set(1, p.Y());
  return a;
}

// One drawn piece of an edge: an analytic 2D curve or a polyline.
void emit(Context& c, const TopoDS_Edge& e, bool visible, const char* kind, int shape, int edge,
          int face)
{
  BRepAdaptor_Curve curve(e);
  const double      t0 = curve.FirstParameter();
  const double      t1 = curve.LastParameter();
  val               r  = val::object();
  r.set("visible", visible);
  r.set("kind", std::string(kind));
  r.set("shape", shape);
  r.set("edge", edge);
  r.set("face", face);
  switch (curve.GetType())
  {
    case GeomAbs_Line:
      r.set("type", std::string("line"));
      r.set("a", xy(curve.Value(t0)));
      r.set("b", xy(curve.Value(t1)));
      break;
    case GeomAbs_Circle: {
      const gp_Circ circ = curve.Circle();
      r.set("center", xy(circ.Location()));
      r.set("radius", circ.Radius());
      if (std::abs(t1 - t0 - 2 * M_PI) < 1e-9)
      {
        r.set("type", std::string("circle"));
      }
      else
      {
        // Start, a point between and end fix the arc without angle conventions.
        r.set("type", std::string("arc"));
        r.set("a", xy(curve.Value(t0)));
        r.set("m", xy(curve.Value((t0 + t1) / 2)));
        r.set("b", xy(curve.Value(t1)));
      }
      break;
    }
    default: {
      std::vector<gp_Pnt>           pts;
      GCPnts_QuasiUniformDeflection points(curve, c.deflection, t0, t1);
      if (points.IsDone())
      {
        for (int i = 1; i <= points.NbPoints(); ++i)
          pts.push_back(points.Value(i));
      }
      else
      {
        pts.push_back(curve.Value(t0));
        pts.push_back(curve.Value(t1));
      }
      gp_Pnt a, b;
      if (asSegment(pts, c.deflection, a, b))
      {
        r.set("type", std::string("line"));
        r.set("a", xy(a));
        r.set("b", xy(b));
        break;
      }
      r.set("type", std::string("polyline"));
      val flat = val::array();
      for (size_t i = 0; i < pts.size(); ++i)
      {
        flat.set(2 * i, pts[i].X());
        flat.set(2 * i + 1, pts[i].Y());
      }
      r.set("points", flat);
      break;
    }
  }
  c.out.set(c.count++, r);
}

void drawEdge(Context& c, bool visible, bool inFace, int typ, HLRBRep_EdgeData& ed, int ie,
              Source face, bool outline)
{
  bool todraw;
  if (inFace)
    todraw = true;
  else if (typ == kSmooth)
    todraw = ed.Rg1Line() && !ed.RgNLine();
  else
    todraw = !ed.Rg1Line();
  if (!todraw)
    return;

  // A real edge stays an edge even where HLR draws it as an outline (a
  // planar face seen edge on); a silhouette points to its face.
  Source src = ie <= c.ds->EdgeMap().Extent() ? sourceOf(c, c.ds->EdgeMap()(ie)) : Source{};
  if (src.edge < 0 && src.face < 0 && outline)
    src = face;
  // HLR draws a seam only where it lies on the silhouette of its face.
  if (src.edge >= 0 && c.seamFace[src.shape][src.edge] >= 0)
    src = {src.shape, -1, c.seamFace[src.shape][src.edge]};
  const bool  silhouette = src.edge < 0 && (outline || src.face >= 0);
  const char* kind = silhouette ? "outline" : typ == kSmooth ? "smooth" : "sharp";
  double      sta, end;
  float       tolsta, tolend;

  HLRAlgo_EdgeIterator it;
  if (visible)
  {
    for (it.InitVisible(ed.Status()); it.MoreVisible(); it.NextVisible())
    {
      it.Visible(sta, tolsta, end, tolend);
      const TopoDS_Edge e = HLRBRep::MakeEdge(ed.Geometry(), sta, end);
      if (!e.IsNull())
        emit(c, e, true, kind, src.shape, src.edge, silhouette ? src.face : -1);
    }
  }
  else
  {
    for (it.InitHidden(ed.Status()); it.MoreHidden(); it.NextHidden())
    {
      it.Hidden(sta, tolsta, end, tolend);
      const TopoDS_Edge e = HLRBRep::MakeEdge(ed.Geometry(), sta, end);
      if (!e.IsNull())
        emit(c, e, false, kind, src.shape, src.edge, silhouette ? src.face : -1);
    }
  }
}

void drawFace(Context& c, bool visible, int typ, int iface)
{
  const Source face = sourceOf(c, c.ds->FaceMap()(iface));

  HLRBRep_FaceIterator itf;
  for (itf.InitEdge(c.ds->FDataArray().ChangeValue(iface)); itf.MoreEdge(); itf.NextEdge())
  {
    const int         ie  = itf.Edge();
    HLRBRep_EdgeData& edf = c.ds->EDataArray().ChangeValue(ie);
    if (edf.Used())
      continue;
    const bool todraw = typ == kOutline ? itf.Internal()
                        : typ == kSmooth
                          ? edf.Rg1Line() && !edf.RgNLine() && !itf.OutLine()
                          : !itf.IsoLine() && !itf.Internal() && (!edf.Rg1Line() || itf.OutLine());
    if (todraw)
    {
      drawEdge(c, visible, true, typ, edf, ie, face, typ == kOutline || itf.OutLine());
      edf.Used(true);
    }
    else if ((typ == kSharp || typ == kOutline) && edf.Rg1Line() && !itf.OutLine())
    {
      // A smooth edge may still be a silhouette seen from another face.
      const int hc = edf.HideCount();
      if (hc > 0)
        edf.Used(true);
      else
        edf.HideCount(hc + 1);
    }
    else
    {
      edf.Used(true);
    }
  }
}

void drawPass(Context& c, int typ, bool visible)
{
  const int ne = c.ds->NbEdges();
  for (int ie = 1; ie <= ne; ++ie)
  {
    HLRBRep_EdgeData& ed = c.ds->EDataArray().ChangeValue(ie);
    if (ed.Selected() && !ed.Vertical())
    {
      ed.Used(false);
      ed.HideCount(0);
    }
    else
    {
      ed.Used(true);
    }
  }
  for (int iface = 1; iface <= c.ds->NbFaces(); ++iface)
    drawFace(c, visible, typ, iface);
  if (typ == kOutline)
    return;
  // Edges that belong to no face.
  for (int ie = 1; ie <= ne; ++ie)
  {
    HLRBRep_EdgeData& ed = c.ds->EDataArray().ChangeValue(ie);
    if (!ed.Used())
    {
      drawEdge(c, visible, false, typ, ed, ie, Source{}, false);
      ed.Used(true);
    }
  }
}

} // namespace

val project(val shapes, val view)
{
  return guarded([&] {
    const auto start = std::chrono::steady_clock::now();
    // One shape id or an array of them.
    std::vector<TopoDS_Shape> inputs;
    if (shapes.isArray())
    {
      for (const int id : toInts(shapes, "shapes"))
        inputs.push_back(arena().get(id));
    }
    else
    {
      inputs.push_back(arena().get(toShapeId(shapes, "shapes")));
    }
    if (inputs.empty())
      fail("INVALID_ARGUMENT", "shapes must not be empty");
    if (view.isUndefined() || view.isNull())
      fail("INVALID_ARGUMENT", "view is required");
    const gp_Dir dir = toDir(view["direction"], "view.direction");
    const gp_Dir up  = toDir(view["up"], "view.up");
    if (dir.IsParallel(up, 1e-9))
      fail("INVALID_ARGUMENT", "view.up must not be parallel to view.direction");
    const bool hidden = !view["hidden"].isUndefined() && view["hidden"].as<bool>();
    const bool smooth = !view["smooth"].isUndefined() && view["smooth"].as<bool>();
    const double deflection =
      view["deflection"].isUndefined() ? 0.01 : toNumber(view["deflection"], "view.deflection");
    if (!(deflection > 0))
      fail("INVALID_ARGUMENT", "view.deflection must be positive");

    // The view plane: x to the right, y up, z towards the viewer.
    const gp_Dir toViewer = dir.Reversed();
    const gp_Dir right    = up.Crossed(toViewer);
    const gp_Ax2 frame(gp_Pnt(0, 0, 0), toViewer, right);

    occ::handle<HLRBRep_Algo> algo = new HLRBRep_Algo();
    for (const TopoDS_Shape& s : inputs)
      algo->Add(s);
    algo->Projector(HLRAlgo_Projector(frame));
    algo->Update();
    algo->Hide();

    Context c;
    c.ds         = algo->DataStructure();
    for (size_t i = 0; i < inputs.size(); ++i)
    {
      c.outliners.push_back(algo->ShapeBounds(static_cast<int>(i) + 1).Shape());
      c.edges.push_back(mapOf(inputs[i], TopAbs_EDGE));
      c.faces.push_back(mapOf(inputs[i], TopAbs_FACE));
      std::vector<int> seams(c.edges[i].Extent(), -1);
      for (int f = 1; f <= c.faces[i].Extent(); ++f)
      {
        const TopoDS_Face& face = TopoDS::Face(c.faces[i](f));
        for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next())
          if (BRep_Tool::IsClosed(TopoDS::Edge(ex.Current()), face))
            seams[indexOf(c.edges[i], ex.Current())] = f - 1;
      }
      c.seamFace.push_back(std::move(seams));
    }
    c.deflection = deflection;
    c.out        = val::array();
    if (!c.ds.IsNull())
    {
      c.ds->Projector().Scaled(true);
      for (const bool visible : {true, false})
      {
        if (!visible && !hidden)
          break;
        drawPass(c, kSharp, visible);
        drawPass(c, kOutline, visible);
        if (smooth)
          drawPass(c, kSmooth, visible);
      }
      c.ds->Projector().Scaled(false);
    }

    val r = okResult();
    r.set("curves", c.out);
    val x = val::array();
    x.set(0, right.X());
    x.set(1, right.Y());
    x.set(2, right.Z());
    r.set("xDir", x);
    r.set(
      "ms",
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    return r;
  });
}

} // namespace occtwasm
