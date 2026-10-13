// Per sub-shape results shared by every shape that holds the same face or
// edge. OCCT operations keep the sub-shapes they do not touch (the same
// TShape and location), so queries on a result only compute what the
// operation changed. Entries are dropped once no shape holds their sub-shape.
//
// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <cstddef>

namespace occtwasm {

struct FaceData
{
  int          surface = 0; // GeomAbs_SurfaceType
  GProp_GProps area;        // surface properties: area and centroid
  gp_Dir       normal;      // outward normal of the FORWARD face
  Bnd_Box      box;         // optimal box, no tolerance
  bool         hasAxis = false;
  gp_Ax1       axis;
  double       radius = 0;
};

struct EdgeData
{
  bool   degenerated = false;
  int    curve       = 0; // GeomAbs_CurveType
  double length      = 0;
  gp_Pnt midpoint, start, end;
  bool   circle = false;
  gp_Ax1 axis;
  double radius = 0;
};

const FaceData& faceData(const TopoDS_Face& face);
const EdgeData& edgeData(const TopoDS_Edge& edge);

// Volume properties of a face with its orientation (BRepGProp sums these).
const GProp_GProps& faceVolume(const TopoDS_Face& face);

// BRepCheck_Analyzer(shape).IsValid(): every face not seen valid before is
// analysed with geometric controls, the whole shape with topological ones.
bool isValid(const TopoDS_Shape& shape);

// After shapes were released: drops entries no shape holds any more.
void dropUnusedSubShapes();
void clearSubShapes();
size_t cachedSubShapes();

} // namespace occtwasm
