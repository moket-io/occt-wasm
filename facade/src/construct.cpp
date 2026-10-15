// Primitive and profile construction.
//
// SPDX-License-Identifier: LGPL-2.1-only
#include "api.hpp"
#include "common.hpp"
#include "history.hpp"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepLib.hxx>
#include <BRep_Builder.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2d_Line.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <NCollection_Array1.hxx>
#include <Precision.hxx>
#include <Standard_Real.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt2d.hxx>

#include <algorithm>
#include <cmath>

namespace occtwasm {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Maximum gap between consecutive segment end points in a loop (mm).
constexpr double kClosureTolerance = 1e-6;

val shapeResult(ShapeId id)
{
  val r = okResult();
  r.set("shape", id);
  return r;
}

double positive(const val& v, const char* name)
{
  const double d = toNumber(v, name);
  if (d <= Precision::Confusion())
  {
    fail("INVALID_ARGUMENT", std::string(name) + " must be positive");
  }
  return d;
}

struct Vec2
{
  double x = 0;
  double y = 0;
};

Vec2 toVec2(const val& v, const char* name)
{
  auto d = toDoubles(v, name, 2);
  return {d[0], d[1]};
}

double dist2d(const Vec2& a, const Vec2& b)
{
  return std::hypot(a.x - b.x, a.y - b.y);
}

enum class SegKind
{
  Line,
  Arc,
  Circle,
  BSpline
};

struct Segment
{
  SegKind kind = SegKind::Line;
  Vec2    a, b;        // line: a -> b; arc: start -> end; bspline: first -> last pole
  Vec2    center;      // arc, circle
  double  radius = 0;  // arc, circle
  bool    ccw    = true;
  // bspline: a clamped (non-periodic) B-spline in plane coordinates.
  std::vector<Vec2>                poles;
  std::vector<double>              knots;
  std::vector<int>                 mults;
  std::vector<double>              weights; // empty: non-rational
  int                              degree = 0;
  occ::handle<Geom2d_BSplineCurve> curve; // for sampling
};

// Highest B-spline degree accepted (OCCT goes up to 25; fonts and sketches
// need 2 and 3).
constexpr int kMaxSplineDegree = 8;

// Validates a B-spline segment as Geom_BSplineCurve would, so that bad input
// is INVALID_ARGUMENT naming the segment instead of a construction error, and
// asks for clamped ends so that the curve ends at its first and last poles.
void parseBSpline(const val& s, const std::string& where, Segment& seg)
{
  const val poles = s["poles"];
  if (poles.isUndefined() || poles.isNull() || !poles.isArray())
  {
    fail("INVALID_ARGUMENT", where + ": bspline.poles must be an array of points");
  }
  const int nPoles = poles["length"].as<int>();
  for (int i = 0; i < nPoles; ++i)
  {
    seg.poles.push_back(toVec2(poles[i], "bspline pole"));
  }
  seg.degree = toInt(s["degree"], "bspline.degree");
  seg.knots  = toDoubles(s["knots"], "bspline.knots", -1);
  seg.mults  = toInts(s["multiplicities"], "bspline.multiplicities");
  const int p = seg.degree;
  if (p < 1 || p > kMaxSplineDegree)
  {
    fail("INVALID_ARGUMENT",
         where + ": bspline degree must be 1 to " + std::to_string(kMaxSplineDegree));
  }
  if (nPoles < p + 1)
  {
    fail("INVALID_ARGUMENT", where + ": bspline needs at least degree + 1 poles");
  }
  const size_t nKnots = seg.knots.size();
  if (nKnots < 2 || seg.mults.size() != nKnots)
  {
    fail("INVALID_ARGUMENT",
         where + ": bspline needs at least 2 knots and one multiplicity per knot");
  }
  for (size_t i = 1; i < nKnots; ++i)
  {
    if (!(seg.knots[i] - seg.knots[i - 1] > Epsilon(std::abs(seg.knots[i - 1]))))
    {
      fail("INVALID_ARGUMENT", where + ": bspline knots must be strictly increasing");
    }
  }
  int sum = 0;
  for (size_t i = 0; i < nKnots; ++i)
  {
    const int  m   = seg.mults[i];
    const bool end = i == 0 || i == nKnots - 1;
    if (end ? m != p + 1 : (m < 1 || m > p))
    {
      fail("INVALID_ARGUMENT",
           where
             + ": bspline multiplicities must be degree + 1 at both ends (clamped) and 1 to degree inside");
    }
    sum += m;
  }
  if (sum != nPoles + p + 1)
  {
    fail("INVALID_ARGUMENT", where + ": bspline multiplicities must add up to poles + degree + 1");
  }
  if (!s["weights"].isUndefined() && !s["weights"].isNull())
  {
    seg.weights = toDoubles(s["weights"], "bspline.weights", -1);
    if (seg.weights.size() != static_cast<size_t>(nPoles))
    {
      fail("INVALID_ARGUMENT", where + ": bspline needs one weight per pole");
    }
    for (double w : seg.weights)
    {
      if (w <= gp::Resolution())
      {
        fail("INVALID_ARGUMENT", where + ": bspline weights must be positive");
      }
    }
  }
  seg.a = seg.poles.front();
  seg.b = seg.poles.back();
  const bool degenerate = std::all_of(seg.poles.begin(), seg.poles.end(), [&](const Vec2& q) {
    return dist2d(q, seg.a) <= Precision::Confusion();
  });
  if (degenerate)
  {
    fail("INVALID_PROFILE", where + ": zero-length bspline");
  }

  NCollection_Array1<gp_Pnt2d> pts(1, nPoles);
  for (int i = 0; i < nPoles; ++i)
  {
    pts.SetValue(i + 1, gp_Pnt2d(seg.poles[i].x, seg.poles[i].y));
  }
  NCollection_Array1<double> knots(1, static_cast<int>(nKnots));
  NCollection_Array1<int>    mults(1, static_cast<int>(nKnots));
  for (size_t i = 0; i < nKnots; ++i)
  {
    knots.SetValue(static_cast<int>(i) + 1, seg.knots[i]);
    mults.SetValue(static_cast<int>(i) + 1, seg.mults[i]);
  }
  if (seg.weights.empty())
  {
    seg.curve = new Geom2d_BSplineCurve(pts, knots, mults, p);
  }
  else
  {
    NCollection_Array1<double> weights(1, nPoles);
    for (int i = 0; i < nPoles; ++i)
    {
      weights.SetValue(i + 1, seg.weights[i]);
    }
    seg.curve = new Geom2d_BSplineCurve(pts, weights, knots, mults, p);
  }
}

Segment parseSegmentData(const val& s, const std::string& where);

// A segment from JS; every error names the segment.
Segment parseSegment(const val& s, const std::string& where)
{
  try
  {
    return parseSegmentData(s, where);
  }
  catch (FacadeError& e)
  {
    if (e.message.rfind(where, 0) != 0)
    {
      e.message = where + ": " + e.message;
    }
    throw;
  }
  catch (const Standard_Failure& e)
  {
    fail("INVALID_ARGUMENT", where + ": " + e.ExceptionType() + " " + e.what());
  }
}

Segment parseSegmentData(const val& s, const std::string& where)
{
  if (s.isUndefined() || s.isNull() || !s["kind"].isString())
  {
    fail("INVALID_ARGUMENT", where + ": segment must have a string 'kind'");
  }
  const std::string kind = s["kind"].as<std::string>();
  Segment           seg;
  if (kind == "line")
  {
    seg.kind = SegKind::Line;
    seg.a    = toVec2(s["a"], "line.a");
    seg.b    = toVec2(s["b"], "line.b");
    if (dist2d(seg.a, seg.b) <= Precision::Confusion())
    {
      fail("INVALID_PROFILE", where + ": zero-length line");
    }
  }
  else if (kind == "arc")
  {
    seg.kind   = SegKind::Arc;
    seg.center = toVec2(s["center"], "arc.center");
    seg.radius = positive(s["radius"], "arc.radius");
    seg.a      = toVec2(s["start"], "arc.start");
    seg.b      = toVec2(s["end"], "arc.end");
    seg.ccw    = s["ccw"].isUndefined() ? true : s["ccw"].as<bool>();
    const double tol = kClosureTolerance * std::max(1.0, seg.radius);
    if (std::abs(dist2d(seg.center, seg.a) - seg.radius) > tol
        || std::abs(dist2d(seg.center, seg.b) - seg.radius) > tol)
    {
      fail("INVALID_PROFILE", where + ": arc start/end are not on the circle");
    }
    if (dist2d(seg.a, seg.b) <= Precision::Confusion())
    {
      fail("INVALID_PROFILE", where + ": arc start and end coincide (use kind 'circle')");
    }
  }
  else if (kind == "circle")
  {
    seg.kind   = SegKind::Circle;
    seg.center = toVec2(s["center"], "circle.center");
    seg.radius = positive(s["radius"], "circle.radius");
  }
  else if (kind == "bspline")
  {
    seg.kind = SegKind::BSpline;
    parseBSpline(s, where, seg);
  }
  else
  {
    fail("INVALID_ARGUMENT", where + ": unknown segment kind '" + kind + "'");
  }
  return seg;
}

// Signed area of the loop in plane coordinates (positive = counter-clockwise),
// from a polyline approximation which is enough to decide orientation.
double signedArea(const std::vector<Segment>& loop)
{
  std::vector<Vec2> pts;
  for (const Segment& s : loop)
  {
    if (s.kind == SegKind::Line)
    {
      pts.push_back(s.a);
    }
    else if (s.kind == SegKind::BSpline)
    {
      // Samples in every knot span, the last point is the next segment's start.
      const int perSpan = 4 * s.degree + 4;
      for (size_t k = 0; k + 1 < s.knots.size(); ++k)
      {
        for (int i = 0; i < perSpan; ++i)
        {
          const double   u = s.knots[k] + (s.knots[k + 1] - s.knots[k]) * i / perSpan;
          const gp_Pnt2d q = s.curve->Value(u);
          pts.push_back({q.X(), q.Y()});
        }
      }
    }
    else if (s.kind == SegKind::Circle)
    {
      // Circles are always built counter-clockwise.
      return kPi * s.radius * s.radius;
    }
    else
    {
      double a0 = std::atan2(s.a.y - s.center.y, s.a.x - s.center.x);
      double a1 = std::atan2(s.b.y - s.center.y, s.b.x - s.center.x);
      if (s.ccw && a1 <= a0)
      {
        a1 += 2 * kPi;
      }
      if (!s.ccw && a1 >= a0)
      {
        a1 -= 2 * kPi;
      }
      const int n = 32;
      for (int i = 0; i < n; ++i)
      {
        const double t = a0 + (a1 - a0) * i / n;
        pts.push_back({s.center.x + s.radius * std::cos(t), s.center.y + s.radius * std::sin(t)});
      }
    }
  }
  double area = 0;
  for (size_t i = 0; i < pts.size(); ++i)
  {
    const Vec2& p = pts[i];
    const Vec2& q = pts[(i + 1) % pts.size()];
    area += p.x * q.y - q.x * p.y;
  }
  return area / 2;
}

struct PlaneFrame
{
  gp_Ax3 ax3;
  gp_Pnt to3d(const Vec2& p) const
  {
    const gp_Pnt& o = ax3.Location();
    const gp_Dir& x = ax3.XDirection();
    const gp_Dir& y = ax3.YDirection();
    return gp_Pnt(o.X() + p.x * x.X() + p.y * y.X(),
                  o.Y() + p.x * x.Y() + p.y * y.Y(),
                  o.Z() + p.x * x.Z() + p.y * y.Z());
  }
};

PlaneFrame toFrame(const val& plane)
{
  if (plane.isUndefined() || plane.isNull())
  {
    fail("INVALID_ARGUMENT", "plane is required");
  }
  const gp_Pnt origin = toPnt(plane["origin"], "plane.origin");
  const gp_Dir normal = toDir(plane["normal"], "plane.normal");
  const gp_Dir xDir   = toDir(plane["xDir"], "plane.xDir");
  if (normal.IsParallel(xDir, Precision::Angular()))
  {
    fail("INVALID_ARGUMENT", "plane.xDir must not be parallel to plane.normal");
  }
  return PlaneFrame{gp_Ax3(origin, normal, xDir)};
}

// The B-spline of a segment in 3D: the same knots and weights, the poles
// mapped onto the plane (an affine map keeps the curve exact).
occ::handle<Geom_BSplineCurve> splineOnPlane(const Segment& s, const PlaneFrame& frame)
{
  const occ::handle<Geom2d_BSplineCurve>& c = s.curve;
  NCollection_Array1<gp_Pnt>              poles(1, c->NbPoles());
  for (int i = 1; i <= c->NbPoles(); ++i)
  {
    const gp_Pnt2d& q = c->Pole(i);
    poles.SetValue(i, frame.to3d({q.X(), q.Y()}));
  }
  if (c->IsRational())
  {
    return new Geom_BSplineCurve(poles, *c->Weights(), c->Knots(), c->Multiplicities(), c->Degree());
  }
  return new Geom_BSplineCurve(poles, c->Knots(), c->Multiplicities(), c->Degree());
}

// Builds a wire from a closed loop. Consecutive segments share vertices.
// Returns the wire and the edges in segment order.
TopoDS_Wire buildWire(const std::vector<Segment>& loop,
                      const PlaneFrame&           frame,
                      std::vector<TopoDS_Edge>&   edges,
                      const std::string&          where)
{
  edges.clear();
  if (loop.size() == 1 && loop[0].kind == SegKind::Circle)
  {
    const Segment& s = loop[0];
    gp_Circ circ(gp_Ax2(frame.to3d(s.center), frame.ax3.Direction(), frame.ax3.XDirection()), s.radius);
    BRepBuilderAPI_MakeEdge me(circ);
    if (!me.IsDone())
    {
      fail("INVALID_PROFILE", where + ": cannot build circle edge");
    }
    edges.push_back(me.Edge());
    BRepBuilderAPI_MakeWire mw(me.Edge());
    return mw.Wire();
  }

  const size_t n = loop.size();
  for (size_t i = 0; i < n; ++i)
  {
    if (loop[i].kind == SegKind::Circle)
    {
      fail("INVALID_PROFILE", where + ": a circle must be the only segment of its loop");
    }
    const Segment& cur  = loop[i];
    const Segment& next = loop[(i + 1) % n];
    if (dist2d(cur.b, next.a) > kClosureTolerance)
    {
      fail("INVALID_PROFILE",
           where + ": segment " + std::to_string(i) + " does not end where segment "
             + std::to_string((i + 1) % n) + " starts (loop must be closed and ordered)");
    }
  }

  // Vertex i is the start point of segment i. Its tolerance covers the small
  // gaps accepted above, so that arcs ending near (not exactly at) it still
  // project onto it.
  auto offCircle = [](const Segment& s, const Vec2& p) {
    return s.kind == SegKind::Arc ? std::abs(dist2d(s.center, p) - s.radius) : 0.0;
  };
  BRep_Builder               builder;
  std::vector<TopoDS_Vertex> verts(n);
  for (size_t i = 0; i < n; ++i)
  {
    const Segment& cur  = loop[i];
    const Segment& prev = loop[(i + n - 1) % n];
    const double   dev  = std::max({dist2d(prev.b, cur.a), offCircle(cur, cur.a), offCircle(prev, cur.a)});
    builder.MakeVertex(verts[i], frame.to3d(cur.a), std::max(Precision::Confusion(), 1.5 * dev));
  }

  BRepBuilderAPI_MakeWire mw;
  for (size_t i = 0; i < n; ++i)
  {
    const Segment&       s  = loop[i];
    const TopoDS_Vertex& v1 = verts[i];
    const TopoDS_Vertex& v2 = verts[(i + 1) % n];
    TopoDS_Edge          edge;
    if (s.kind == SegKind::Line)
    {
      BRepBuilderAPI_MakeEdge me(v1, v2);
      if (!me.IsDone())
      {
        fail("INVALID_PROFILE", where + ": cannot build line " + std::to_string(i));
      }
      edge = me.Edge();
    }
    else if (s.kind == SegKind::BSpline)
    {
      // Clamped: the ends are the first and last poles, at the parameter bounds.
      const occ::handle<Geom_BSplineCurve> curve = splineOnPlane(s, frame);
      BRepBuilderAPI_MakeEdge me(curve, v1, v2, curve->FirstParameter(), curve->LastParameter());
      if (!me.IsDone())
      {
        fail("INVALID_PROFILE", where + ": cannot build bspline " + std::to_string(i));
      }
      edge = me.Edge();
    }
    else
    {
      const gp_Dir axis = s.ccw ? frame.ax3.Direction() : frame.ax3.Direction().Reversed();
      gp_Circ      circ(gp_Ax2(frame.to3d(s.center), axis, frame.ax3.XDirection()), s.radius);
      BRepBuilderAPI_MakeEdge me(circ, v1, v2);
      if (!me.IsDone())
      {
        fail("INVALID_PROFILE", where + ": cannot build arc " + std::to_string(i));
      }
      edge = me.Edge();
    }
    edges.push_back(edge);
    mw.Add(edge);
    if (!mw.IsDone())
    {
      fail("INVALID_PROFILE", where + ": segment " + std::to_string(i) + " does not connect");
    }
  }
  TopoDS_Wire wire = mw.Wire();
  if (!wire.Closed())
  {
    // BRepBuilderAPI_MakeWire sets the flag only when it detects closure.
    wire.Closed(true);
  }
  return wire;
}

} // namespace

// ---------------------------------------------------------------------------
val makeBox(val dx, val dy, val dz)
{
  return guarded([&] {
    BRepPrimAPI_MakeBox mk(positive(dx, "dx"), positive(dy, "dy"), positive(dz, "dz"));
    return shapeResult(arena().put(mk.Shape()));
  });
}

val makeCylinder(val axisOrigin, val axisDir, val radius, val height)
{
  return guarded([&] {
    gp_Ax2                   ax(toPnt(axisOrigin, "axisOrigin"), toDir(axisDir, "axisDir"));
    BRepPrimAPI_MakeCylinder mk(ax, positive(radius, "radius"), positive(height, "height"));
    return shapeResult(arena().put(mk.Shape()));
  });
}

// A helix as a wire: a line in the (angle, height) parameters of a
// cylinder, or of a cone when `taper` (the half angle, radians) is set.
// It starts on `xDir` (or any direction perpendicular to the axis) at the
// axis origin, turns right-handed about the axis unless `leftHanded`, and
// rises `pitch` per turn up to `height` along the axis. One edge per turn
// at most, equal pieces: a face swept along an edge that winds around the
// axis more than once is misclassified by booleans (a thread groove cut
// from a cylinder removed nothing), one face per turn is not.
val helix(val axisOrigin, val axisDir, val radius, val pitch, val height, val options)
{
  return guarded([&] {
    const gp_Pnt origin = toPnt(axisOrigin, "axisOrigin");
    const gp_Dir axis   = toDir(axisDir, "axisDir");
    const double r      = positive(radius, "radius");
    const double p      = positive(pitch, "pitch");
    const double h      = positive(height, "height");
    const bool   hasOptions = !(options.isUndefined() || options.isNull());
    const bool   left =
      hasOptions && !options["leftHanded"].isUndefined() && options["leftHanded"].as<bool>();
    const double taper =
      hasOptions && !options["taper"].isUndefined() ? toNumber(options["taper"], "options.taper") : 0;
    if (std::abs(taper) >= M_PI / 2 - 1e-3)
    {
      fail("INVALID_ARGUMENT", "options.taper must be less than 90 degrees");
    }
    gp_Ax3 frame(origin, axis);
    if (hasOptions && !options["xDir"].isUndefined())
    {
      const gp_Dir x = toDir(options["xDir"], "options.xDir");
      if (std::abs(x.Dot(axis)) > 1e-9)
      {
        fail("INVALID_ARGUMENT", "options.xDir must be perpendicular to axisDir");
      }
      frame = gp_Ax3(origin, axis, x);
    }
    occ::handle<Geom_Surface> surface;
    // The surface's v runs along the axis on a cylinder, along the slant on
    // a cone: v = height / cos(taper).
    double slant = 1;
    if (taper == 0)
    {
      surface = new Geom_CylindricalSurface(frame, r);
    }
    else
    {
      surface = new Geom_ConicalSurface(frame, taper, r);
      slant   = 1 / std::cos(taper);
      if (r + h * std::tan(taper) <= Precision::Confusion())
      {
        fail("INVALID_ARGUMENT", "the taper closes the helix before its height");
      }
    }
    const double turns = h / p;
    const double du    = (left ? -2 : 2) * M_PI;
    const double dv    = p * slant;
    const double perTurn = std::sqrt(du * du + dv * dv);
    occ::handle<Geom2d_Line> line = new Geom2d_Line(gp_Pnt2d(0, 0), gp_Dir2d(du, dv));
    const int                pieces = std::max(1, static_cast<int>(std::ceil(turns - 1e-9)));
    const double             step   = turns * perTurn / pieces;
    const auto vertexAt = [&](double t) {
      const gp_Pnt2d uv = line->Value(t);
      return BRepBuilderAPI_MakeVertex(surface->Value(uv.X(), uv.Y())).Vertex();
    };
    BRepBuilderAPI_MakeWire wire;
    TopoDS_Vertex           start = vertexAt(0);
    for (int i = 0; i < pieces; ++i)
    {
      const double            t0  = i * step;
      const double            t1  = (i + 1) * step;
      const TopoDS_Vertex     end = vertexAt(t1);
      BRepBuilderAPI_MakeEdge edge(line, surface, start, end, t0, t1);
      if (!edge.IsDone())
      {
        fail("INVALID_ARGUMENT", "cannot build the helix");
      }
      TopoDS_Edge e = edge.Edge();
      BRepLib::BuildCurves3d(e, 1e-7, GeomAbs_C2, 14, 32);
      wire.Add(e);
      if (!wire.IsDone())
      {
        fail("INVALID_ARGUMENT", "cannot build the helix");
      }
      start = end;
    }
    return shapeResult(arena().put(wire.Wire()));
  });
}

val makeVertex(val point)
{
  return guarded([&] {
    BRepBuilderAPI_MakeVertex mk(toPnt(point, "point"));
    return shapeResult(arena().put(mk.Vertex()));
  });
}

val subShape(val shape, val kind, val index)
{
  return guarded([&] {
    const TopoDS_Shape& s = arena().get(toShapeId(shape, "shape"));
    if (!kind.isString())
    {
      fail("INVALID_ARGUMENT", "kind must be a string");
    }
    const std::string k = kind.as<std::string>();
    TopAbs_ShapeEnum  type;
    if (k == "solid") type = TopAbs_SOLID;
    else if (k == "shell") type = TopAbs_SHELL;
    else if (k == "face") type = TopAbs_FACE;
    else if (k == "wire") type = TopAbs_WIRE;
    else if (k == "edge") type = TopAbs_EDGE;
    else if (k == "vertex") type = TopAbs_VERTEX;
    else fail("INVALID_ARGUMENT", "kind must be solid, shell, face, wire, edge or vertex");
    const ShapeMap map = mapOf(s, type);
    const int      i   = toInt(index, "index");
    if (i < 0 || i >= map.Extent())
    {
      fail("INVALID_ARGUMENT",
           k + " index " + std::to_string(i) + " out of range [0, " + std::to_string(map.Extent()) + ")");
    }
    return shapeResult(arena().put(map(i + 1)));
  });
}

val makeProfileFace(val plane, val loops)
{
  return guarded([&] {
    const PlaneFrame frame = toFrame(plane);

    if (loops.isUndefined() || loops.isNull() || !loops.isArray() || loops["length"].as<int>() < 1)
    {
      fail("INVALID_ARGUMENT", "loops must be a non-empty array");
    }
    const int nLoops = loops["length"].as<int>();

    std::vector<TopoDS_Wire>              wires;
    std::vector<std::vector<TopoDS_Edge>> loopEdges(nLoops);
    for (int li = 0; li < nLoops; ++li)
    {
      const std::string where = "loop " + std::to_string(li);
      val               jsLoop = loops[li];
      if (!jsLoop.isArray() || jsLoop["length"].as<int>() < 1)
      {
        fail("INVALID_ARGUMENT", where + " must be a non-empty array of segments");
      }
      std::vector<Segment> segs;
      const int            nSegs = jsLoop["length"].as<int>();
      for (int si = 0; si < nSegs; ++si)
      {
        segs.push_back(parseSegment(jsLoop[si], where + " segment " + std::to_string(si)));
      }
      TopoDS_Wire  wire = buildWire(segs, frame, loopEdges[li], where);
      const double area = signedArea(segs);
      // Outer loop counter-clockwise around the normal, holes clockwise.
      if ((li == 0 && area < 0) || (li > 0 && area > 0))
      {
        wire.Reverse();
      }
      wires.push_back(wire);
    }

    BRepBuilderAPI_MakeFace mf(gp_Pln(frame.ax3), wires[0], true);
    if (!mf.IsDone())
    {
      fail("INVALID_PROFILE", "cannot build a face from the outer loop");
    }
    for (size_t i = 1; i < wires.size(); ++i)
    {
      mf.Add(wires[i]);
    }
    if (!mf.IsDone())
    {
      fail("INVALID_PROFILE", "cannot add hole loops to the face");
    }
    TopoDS_Face face = mf.Face();
    if (!BRepCheck_Analyzer(face).IsValid())
    {
      fail("INVALID_PROFILE",
           "profile face is not valid (self-intersecting loops or holes outside the outer loop?)");
    }

    // Map each segment to its edge index in the face (TopExp::MapShapes order).
    ShapeMap edgeMap = mapOf(face, TopAbs_EDGE);
    val      segmentEdges = val::array();
    for (int li = 0; li < nLoops; ++li)
    {
      std::vector<int> idx;
      for (const TopoDS_Edge& e : loopEdges[li])
      {
        idx.push_back(indexOf(edgeMap, e));
      }
      segmentEdges.set(li, intsToJs(idx));
    }

    val r = shapeResult(arena().put(face));
    r.set("segmentEdges", segmentEdges);
    return r;
  });
}

val makeWire(val plane, val segments)
{
  return guarded([&] {
    const PlaneFrame frame = toFrame(plane);
    if (segments.isUndefined() || segments.isNull() || !segments.isArray()
        || segments["length"].as<int>() < 1)
    {
      fail("INVALID_ARGUMENT", "segments must be a non-empty array");
    }
    const int                n = segments["length"].as<int>();
    std::vector<TopoDS_Edge> edges;
    for (int i = 0; i < n; ++i)
    {
      const std::string where = "segment " + std::to_string(i);
      const Segment     s     = parseSegment(segments[i], where);
      if (s.kind == SegKind::Line)
      {
        BRepBuilderAPI_MakeEdge me(frame.to3d(s.a), frame.to3d(s.b));
        if (!me.IsDone())
        {
          fail("INVALID_PROFILE", where + ": cannot build the line");
        }
        edges.push_back(me.Edge());
        continue;
      }
      if (s.kind == SegKind::BSpline)
      {
        BRepBuilderAPI_MakeEdge me(splineOnPlane(s, frame));
        if (!me.IsDone())
        {
          fail("INVALID_PROFILE", where + ": cannot build the bspline");
        }
        edges.push_back(me.Edge());
        continue;
      }
      const gp_Dir axis =
        s.kind == SegKind::Arc && !s.ccw ? frame.ax3.Direction().Reversed() : frame.ax3.Direction();
      gp_Circ circ(gp_Ax2(frame.to3d(s.center), axis, frame.ax3.XDirection()), s.radius);
      BRepBuilderAPI_MakeEdge me = s.kind == SegKind::Circle
                                     ? BRepBuilderAPI_MakeEdge(circ)
                                     : BRepBuilderAPI_MakeEdge(circ, frame.to3d(s.a), frame.to3d(s.b));
      if (!me.IsDone())
      {
        fail("INVALID_PROFILE", where + ": cannot build the arc");
      }
      edges.push_back(me.Edge());
    }
    std::vector<int> wireEdge;
    bool             closed = false;
    const TopoDS_Wire wire  = connectEdges(edges, wireEdge, closed);
    val               r     = shapeResult(arena().put(wire));
    r.set("segmentEdges", intsToJs(wireEdge));
    r.set("closed", closed);
    return r;
  });
}

} // namespace occtwasm
