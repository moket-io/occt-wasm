// Hidden line removal: exact projection of a shape onto a view plane, for
// 2D drawings. The visible and hidden parts of every edge are returned as 2D
// curves in view coordinates, each with the index of the edge (or, for
// silhouettes, the face) of the shape it comes from.
//
// The edge walk follows HLRBRep_HLRToShape (InternalCompound, DrawFace,
// DrawEdge), which builds compounds and drops where each curve came from.
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
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>

#include <chrono>
#include <cmath>

namespace occtwasm {

namespace {

// Drawn line kinds, as HLRBRep_HLRToShape numbers them.
constexpr int kSmooth = 3; // tangent (G1) edges between faces
constexpr int kSharp  = 5; // sharp edges and silhouettes

struct Context
{
  occ::handle<HLRBRep_Data> ds;
  ShapeMap                  edges; // of the input shape
  ShapeMap                  faces;
  double                    deflection;
  val                       out;
  int                       count = 0;
};

val xy(const gp_Pnt& p)
{
  val a = val::array();
  a.set(0, p.X());
  a.set(1, p.Y());
  return a;
}

// One drawn piece of an edge: an analytic 2D curve or a polyline.
void emit(Context& c, const TopoDS_Edge& e, bool visible, const char* kind, int edge, int face)
{
  BRepAdaptor_Curve curve(e);
  const double      t0 = curve.FirstParameter();
  const double      t1 = curve.LastParameter();
  val               r  = val::object();
  r.set("visible", visible);
  r.set("kind", std::string(kind));
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
      r.set("type", std::string("polyline"));
      GCPnts_QuasiUniformDeflection points(curve, c.deflection, t0, t1);
      val                           pts = val::array();
      if (points.IsDone())
      {
        for (int i = 1; i <= points.NbPoints(); ++i)
        {
          const gp_Pnt p = points.Value(i);
          pts.set(2 * (i - 1), p.X());
          pts.set(2 * (i - 1) + 1, p.Y());
        }
      }
      else
      {
        const gp_Pnt a = curve.Value(t0);
        const gp_Pnt b = curve.Value(t1);
        pts.set(0, a.X());
        pts.set(1, a.Y());
        pts.set(2, b.X());
        pts.set(3, b.Y());
      }
      r.set("points", pts);
      break;
    }
  }
  c.out.set(c.count++, r);
}

void drawEdge(Context& c, bool visible, bool inFace, int typ, HLRBRep_EdgeData& ed, int ie, int face,
              bool outline)
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

  // Edges HLR adds itself (silhouettes) are beyond the shape's edge map.
  const int   edge = ie <= c.ds->EdgeMap().Extent() ? indexOf(c.edges, c.ds->EdgeMap()(ie)) : -1;
  const char* kind = outline ? "outline" : typ == kSmooth ? "smooth" : "sharp";
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
        emit(c, e, true, kind, edge, outline ? face : -1);
    }
  }
  else
  {
    for (it.InitHidden(ed.Status()); it.MoreHidden(); it.NextHidden())
    {
      it.Hidden(sta, tolsta, end, tolend);
      const TopoDS_Edge e = HLRBRep::MakeEdge(ed.Geometry(), sta, end);
      if (!e.IsNull())
        emit(c, e, false, kind, edge, outline ? face : -1);
    }
  }
}

void drawFace(Context& c, bool visible, int typ, int iface)
{
  const int face = indexOf(c.faces, c.ds->FaceMap()(iface));

  HLRBRep_FaceIterator itf;
  for (itf.InitEdge(c.ds->FDataArray().ChangeValue(iface)); itf.MoreEdge(); itf.NextEdge())
  {
    const int         ie  = itf.Edge();
    HLRBRep_EdgeData& edf = c.ds->EDataArray().ChangeValue(ie);
    if (edf.Used())
      continue;
    const bool todraw = typ == kSmooth
                          ? edf.Rg1Line() && !edf.RgNLine() && !itf.OutLine()
                          : !itf.IsoLine() && !itf.Internal() && (!edf.Rg1Line() || itf.OutLine());
    if (todraw)
    {
      drawEdge(c, visible, true, typ, edf, ie, face, itf.OutLine());
      edf.Used(true);
    }
    else if (typ == kSharp && edf.Rg1Line() && !itf.OutLine())
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
  // Edges that belong to no face.
  for (int ie = 1; ie <= ne; ++ie)
  {
    HLRBRep_EdgeData& ed = c.ds->EDataArray().ChangeValue(ie);
    if (!ed.Used())
    {
      drawEdge(c, visible, false, typ, ed, ie, -1, false);
      ed.Used(true);
    }
  }
}

} // namespace

val project(val shape, val view)
{
  return guarded([&] {
    const auto         start = std::chrono::steady_clock::now();
    const TopoDS_Shape s     = arena().get(toShapeId(shape, "shape"));
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
    algo->Add(s);
    algo->Projector(HLRAlgo_Projector(frame));
    algo->Update();
    algo->Hide();

    Context c;
    c.ds         = algo->DataStructure();
    c.edges      = mapOf(s, TopAbs_EDGE);
    c.faces      = mapOf(s, TopAbs_FACE);
    c.deflection = deflection;
    c.out        = val::array();
    if (!c.ds.IsNull())
    {
      c.ds->Projector().Scaled(true);
      drawPass(c, kSharp, true);
      if (smooth)
        drawPass(c, kSmooth, true);
      if (hidden)
      {
        drawPass(c, kSharp, false);
        if (smooth)
          drawPass(c, kSmooth, false);
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
