// SPDX-License-Identifier: LGPL-2.1-only
#include "subshapes.hpp"

#include "common.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_Shell.hxx>
#include <BRepCheck_Solid.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Domain.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepGProp_Vinert.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS.hxx>
#include <gp.hxx>
#include <gp_Circ.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace occtwasm {

namespace {

struct Same
{
  bool operator()(const TopoDS_Shape& a, const TopoDS_Shape& b) const { return a.IsSame(b); }
};

template <class V>
using SubShapeMap = std::unordered_map<TopoDS_Shape, V, std::hash<TopoDS_Shape>, Same>;

struct FaceEntry
{
  std::optional<FaceData> data;
  // Volume properties by orientation (FORWARD, REVERSED), origin as reference.
  std::optional<GProp_GProps> volume[2];
  bool                        valid = false;
};

struct Store
{
  SubShapeMap<FaceEntry>                                         faces;
  SubShapeMap<EdgeData>                                          edges;
  std::unordered_set<TopoDS_Shape, std::hash<TopoDS_Shape>, Same> validShapes;
  size_t                                                         dropAt = 4096;

  size_t size() const { return faces.size() + edges.size() + validShapes.size(); }
};

Store& store()
{
  static Store instance;
  return instance;
}

// Outward normal at the middle of the face parameter range.
gp_Dir forwardNormal(const TopoDS_Face& face)
{
  const BRepAdaptor_Surface surf(face);
  double                    u0, u1, v0, v1;
  BRepTools::UVBounds(face, u0, u1, v0, v1);
  BRepLProp_SLProps props(surf, (u0 + u1) / 2, (v0 + v1) / 2, 1, 1e-7);
  return props.IsNormalDefined() ? props.Normal() : gp_Dir(0, 0, 1);
}

FaceData computeFace(const TopoDS_Face& any)
{
  const TopoDS_Face         face = TopoDS::Face(any.Oriented(TopAbs_FORWARD));
  const BRepAdaptor_Surface surf(face);
  FaceData                  d;
  d.surface = static_cast<int>(surf.GetType());
  BRepGProp::SurfaceProperties(face, d.area);
  d.normal = forwardNormal(face);
  BRepBndLib::AddOptimal(face, d.box, false, false);
  switch (surf.GetType())
  {
    case GeomAbs_Cylinder: {
      const gp_Cylinder c = surf.Cylinder();
      d.hasAxis           = true;
      d.axis              = c.Axis();
      d.radius            = c.Radius();
      break;
    }
    case GeomAbs_Cone: {
      const gp_Cone c = surf.Cone();
      d.hasAxis       = true;
      d.axis          = c.Axis();
      d.radius        = c.RefRadius();
      break;
    }
    case GeomAbs_Sphere: {
      const gp_Sphere c = surf.Sphere();
      d.hasAxis         = true;
      d.axis            = gp_Ax1(c.Location(), c.Position().Direction());
      d.radius          = c.Radius();
      break;
    }
    case GeomAbs_Torus: {
      const gp_Torus c = surf.Torus();
      d.hasAxis        = true;
      d.axis           = c.Axis();
      d.radius         = c.MajorRadius();
      break;
    }
    default:
      break;
  }
  return d;
}

EdgeData computeEdge(const TopoDS_Edge& edge)
{
  EdgeData d;
  d.degenerated = BRep_Tool::Degenerated(edge);
  if (d.degenerated)
  {
    d.curve    = static_cast<int>(GeomAbs_OtherCurve);
    d.midpoint = BRep_Tool::Pnt(TopExp::FirstVertex(edge));
    return d;
  }
  const BRepAdaptor_Curve curve(edge);
  GProp_GProps            props;
  BRepGProp::LinearProperties(edge, props);
  d.curve    = static_cast<int>(curve.GetType());
  d.length   = props.Mass();
  d.midpoint = curve.Value((curve.FirstParameter() + curve.LastParameter()) / 2);
  d.start    = curve.Value(curve.FirstParameter());
  d.end      = curve.Value(curve.LastParameter());
  if (curve.GetType() == GeomAbs_Circle)
  {
    const gp_Circ c = curve.Circle();
    d.circle        = true;
    d.axis          = c.Axis();
    d.radius        = c.Radius();
  }
  return d;
}

// As BRepGProp::VolumeProperties does per face, but always about the origin
// so that the contributions of faces add up independently of the shape.
GProp_GProps computeVolume(const TopoDS_Face& face)
{
  TopLoc_Location loc;
  if (BRep_Tool::Surface(face, loc).IsNull())
  {
    return GProp_GProps(gp::Origin());
  }
  BRepGProp_Vinert G;
  G.SetLocation(gp::Origin());
  BRepGProp_Face BF;
  BF.Load(face);
  if (face.NbChildren() == 0)
  {
    G.Perform(BF);
  }
  else
  {
    BRepGProp_Domain BD;
    BD.Init(face);
    G.Perform(BF, BD);
  }
  return G;
}

FaceEntry& faceEntry(const TopoDS_Face& face)
{
  return store().faces[face];
}

} // namespace

const FaceData& faceData(const TopoDS_Face& face)
{
  FaceEntry& e = faceEntry(face);
  if (!e.data)
  {
    e.data = computeFace(face);
  }
  return *e.data;
}

const EdgeData& edgeData(const TopoDS_Edge& edge)
{
  auto& edges = store().edges;
  auto  it    = edges.find(edge);
  if (it == edges.end())
  {
    it = edges.emplace(edge, computeEdge(edge)).first;
  }
  return it->second;
}

const GProp_GProps& faceVolume(const TopoDS_Face& face)
{
  static const GProp_GProps none(gp::Origin());
  const TopAbs_Orientation o = face.Orientation();
  if (o != TopAbs_FORWARD && o != TopAbs_REVERSED)
  {
    return none; // BRepGProp skips internal and external faces
  }
  FaceEntry&                  e    = faceEntry(face);
  std::optional<GProp_GProps>& slot = e.volume[o == TopAbs_FORWARD ? 0 : 1];
  if (!slot)
  {
    slot = computeVolume(face);
  }
  return *slot;
}

bool isValid(const TopoDS_Shape& shape)
{
  Store& s = store();
  if (s.validShapes.count(shape))
  {
    return true;
  }
  const ShapeMap faces = mapOf(shape, TopAbs_FACE);
  for (int i = 1; i <= faces.Extent(); ++i)
  {
    const TopoDS_Face face = TopoDS::Face(faces(i));
    FaceEntry&        e    = faceEntry(face);
    if (e.valid)
    {
      continue;
    }
    if (!BRepCheck_Analyzer(face).IsValid())
    {
      return false;
    }
    e.valid = true;
  }
  // Faces are checked with their wires, edges and vertices above. What
  // BRepCheck_Analyzer adds for the whole shape: connected shells, closed
  // and oriented in solids, and the solids' own checks.
  auto fine = [](const BRepCheck_Result& r) {
    for (const BRepCheck_Status st : r.Status())
    {
      if (st != BRepCheck_NoError)
      {
        return false;
      }
    }
    return true;
  };
  for (TopExp_Explorer ex(shape, TopAbs_SOLID); ex.More(); ex.Next())
  {
    const TopoDS_Solid& solid = TopoDS::Solid(ex.Current());
    if (!fine(BRepCheck_Solid(solid)))
    {
      return false;
    }
    for (TopoDS_Iterator it(solid); it.More(); it.Next())
    {
      if (it.Value().ShapeType() != TopAbs_SHELL)
      {
        continue;
      }
      BRepCheck_Shell shell(TopoDS::Shell(it.Value()));
      if (!fine(shell) || shell.Closed() != BRepCheck_NoError)
      {
        return false;
      }
      if (!shell.IsUnorientable() && shell.Orientation() != BRepCheck_NoError)
      {
        return false;
      }
    }
  }
  for (TopExp_Explorer ex(shape, TopAbs_SHELL, TopAbs_SOLID); ex.More(); ex.Next())
  {
    if (!fine(BRepCheck_Shell(TopoDS::Shell(ex.Current()))))
    {
      return false;
    }
  }
  s.validShapes.insert(shape);
  return true;
}

void dropUnusedSubShapes()
{
  Store& s = store();
  if (s.size() < s.dropAt)
  {
    return;
  }
  // A TShape is unused when only our keys hold it.
  std::unordered_map<const TopoDS_TShape*, int> ours;
  for (const auto& [k, v] : s.faces)
  {
    ++ours[k.TShape().get()];
  }
  for (const auto& [k, v] : s.edges)
  {
    ++ours[k.TShape().get()];
  }
  for (const auto& k : s.validShapes)
  {
    ++ours[k.TShape().get()];
  }
  auto unused = [&](const TopoDS_Shape& k) {
    return k.TShape()->GetRefCount() <= ours[k.TShape().get()];
  };
  for (auto it = s.faces.begin(); it != s.faces.end();)
  {
    it = unused(it->first) ? s.faces.erase(it) : std::next(it);
  }
  for (auto it = s.edges.begin(); it != s.edges.end();)
  {
    it = unused(it->first) ? s.edges.erase(it) : std::next(it);
  }
  for (auto it = s.validShapes.begin(); it != s.validShapes.end();)
  {
    it = unused(*it) ? s.validShapes.erase(it) : std::next(it);
  }
  s.dropAt = std::max<size_t>(4096, 2 * s.size());
}

void clearSubShapes()
{
  Store& s = store();
  s.faces.clear();
  s.edges.clear();
  s.validShapes.clear();
  s.dropAt = 4096;
}

size_t cachedSubShapes()
{
  return store().size();
}

} // namespace occtwasm
