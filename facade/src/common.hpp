// Shared helpers for the occt-wasm facade: shape arena, structured results,
// exception guard and JS value conversion.
//
// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include <emscripten/val.h>

#include <NCollection_IndexedMap.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace occtwasm {

using emscripten::val;
using ShapeMap = NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher>;
using ShapeId  = int;

// Error raised by facade argument validation. Converted to
// { ok: false, error: { code, message } } by guarded().
struct FacadeError
{
  std::string code;
  std::string message;
  int         index = -1; // error.index: the input that failed, when known
};

[[noreturn]] void fail(const std::string& code, const std::string& message);
[[noreturn]] void failAt(const std::string& code, const std::string& message, int index);

// ---------------------------------------------------------------------------
// Shape arena. Ids are positive, never reused within a module instance.
class Arena
{
public:
  ShapeId             put(const TopoDS_Shape& shape);
  const TopoDS_Shape& get(ShapeId id) const; // throws INVALID_HANDLE
  bool                release(ShapeId id);
  size_t              releaseAll();
  size_t              size() const { return myShapes.size(); }

private:
  std::unordered_map<ShapeId, TopoDS_Shape> myShapes;
  ShapeId                                   myNext = 1;
};

Arena& arena();

// ---------------------------------------------------------------------------
// Results.
val okResult();
val errorResult(const std::string& code, const std::string& message, int index = -1);

// Runs body and converts any exception into an error result.
template <class F>
val guarded(F&& body);

// ---------------------------------------------------------------------------
// JS input conversion (all throw FacadeError INVALID_ARGUMENT on bad input).
double              toNumber(const val& v, const char* name);
int                 toInt(const val& v, const char* name);
ShapeId             toShapeId(const val& v, const char* name);
gp_Pnt              toPnt(const val& v, const char* name);
gp_Dir              toDir(const val& v, const char* name);
std::vector<double> toDoubles(const val& v, const char* name, int expectedLength);
std::vector<int>    toInts(const val& v, const char* name);
std::string         toBytes(const val& v, const char* name);

// JS output conversion (copies, the caller owns the result).
val pntToJs(const gp_Pnt& p);
val dirToJs(const gp_Dir& d);
val intsToJs(const std::vector<int>& v);
val bytesToJs(const std::string& bytes);
val float32ToJs(const std::vector<float>& v);
val uint32ToJs(const std::vector<uint32_t>& v);
val float64ToJs(const std::vector<double>& v);
val int32ToJs(const std::vector<int32_t>& v);

// 0-based index of sub-shape in the map, -1 if absent.
inline int indexOf(const ShapeMap& map, const TopoDS_Shape& s)
{
  return map.FindIndex(s) - 1;
}

ShapeMap mapOf(const TopoDS_Shape& shape, TopAbs_ShapeEnum kind);

const char* shapeTypeName(TopAbs_ShapeEnum type);

} // namespace occtwasm

#include "guard.inl"
