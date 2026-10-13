// SPDX-License-Identifier: LGPL-2.1-only
#include "common.hpp"

#include <emscripten/val.h>

#include <TopExp.hxx>

#include <cmath>

namespace occtwasm {

void fail(const std::string& code, const std::string& message)
{
  throw FacadeError{code, message};
}

void failAt(const std::string& code, const std::string& message, int index)
{
  throw FacadeError{code, message, index};
}

// ---------------------------------------------------------------------------
ShapeId Arena::put(const TopoDS_Shape& shape)
{
  if (shape.IsNull())
  {
    fail("EMPTY_RESULT", "Operation produced a null shape");
  }
  const ShapeId id = myNext++;
  myShapes.emplace(id, shape);
  return id;
}

const TopoDS_Shape& Arena::get(ShapeId id) const
{
  auto it = myShapes.find(id);
  if (it == myShapes.end())
  {
    fail("INVALID_HANDLE", "Unknown or released shape id " + std::to_string(id));
  }
  return it->second;
}

bool Arena::release(ShapeId id)
{
  return myShapes.erase(id) > 0;
}

size_t Arena::releaseAll()
{
  const size_t n = myShapes.size();
  myShapes.clear();
  return n;
}

Arena& arena()
{
  static Arena instance;
  return instance;
}

// ---------------------------------------------------------------------------
val okResult()
{
  val r = val::object();
  r.set("ok", true);
  return r;
}

val errorResult(const std::string& code, const std::string& message, int index)
{
  val err = val::object();
  err.set("code", code);
  err.set("message", message);
  if (index >= 0)
  {
    err.set("index", index);
  }
  val r = val::object();
  r.set("ok", false);
  r.set("error", err);
  return r;
}

// ---------------------------------------------------------------------------
double toNumber(const val& v, const char* name)
{
  if (!v.isNumber())
  {
    fail("INVALID_ARGUMENT", std::string(name) + " must be a number");
  }
  const double d = v.as<double>();
  if (!std::isfinite(d))
  {
    fail("INVALID_ARGUMENT", std::string(name) + " must be finite");
  }
  return d;
}

int toInt(const val& v, const char* name)
{
  const double d = toNumber(v, name);
  if (d != std::floor(d) || d < -2147483648.0 || d > 2147483647.0)
  {
    fail("INVALID_ARGUMENT", std::string(name) + " must be an integer");
  }
  return static_cast<int>(d);
}

ShapeId toShapeId(const val& v, const char* name)
{
  return toInt(v, name);
}

static int lengthOf(const val& v, const char* name)
{
  if (v.isUndefined() || v.isNull() || !(v.isArray() || v["length"].isNumber()))
  {
    fail("INVALID_ARGUMENT", std::string(name) + " must be an array");
  }
  return v["length"].as<int>();
}

std::vector<double> toDoubles(const val& v, const char* name, int expectedLength)
{
  const int n = lengthOf(v, name);
  if (expectedLength >= 0 && n != expectedLength)
  {
    fail("INVALID_ARGUMENT",
         std::string(name) + " must have " + std::to_string(expectedLength) + " elements");
  }
  std::vector<double> out(n);
  for (int i = 0; i < n; ++i)
  {
    out[i] = toNumber(v[i], name);
  }
  return out;
}

std::vector<int> toInts(const val& v, const char* name)
{
  const int        n = lengthOf(v, name);
  std::vector<int> out(n);
  for (int i = 0; i < n; ++i)
  {
    out[i] = toInt(v[i], name);
  }
  return out;
}

gp_Pnt toPnt(const val& v, const char* name)
{
  auto d = toDoubles(v, name, 3);
  return gp_Pnt(d[0], d[1], d[2]);
}

gp_Dir toDir(const val& v, const char* name)
{
  auto         d = toDoubles(v, name, 3);
  const double n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  if (n < 1e-12)
  {
    fail("INVALID_ARGUMENT", std::string(name) + " must be a non-zero vector");
  }
  return gp_Dir(d[0] / n, d[1] / n, d[2] / n);
}

std::string toBytes(const val& v, const char* name)
{
  if (v.isUndefined() || v.isNull())
  {
    fail("INVALID_ARGUMENT", std::string(name) + " must be a Uint8Array");
  }
  val bytes = v;
  if (v.instanceof(val::global("ArrayBuffer")))
  {
    bytes = val::global("Uint8Array").new_(v);
  }
  else if (!v.instanceof(val::global("Uint8Array")))
  {
    fail("INVALID_ARGUMENT", std::string(name) + " must be a Uint8Array or ArrayBuffer");
  }
  const size_t n = bytes["length"].as<size_t>();
  std::string  out(n, '\0');
  if (n > 0)
  {
    // Copy straight into our buffer through a heap view.
    val view = val(emscripten::typed_memory_view(n, reinterpret_cast<uint8_t*>(out.data())));
    view.call<void>("set", bytes);
  }
  return out;
}

// ---------------------------------------------------------------------------
val pntToJs(const gp_Pnt& p)
{
  val a = val::array();
  a.set(0, p.X());
  a.set(1, p.Y());
  a.set(2, p.Z());
  return a;
}

val dirToJs(const gp_Dir& d)
{
  val a = val::array();
  a.set(0, d.X());
  a.set(1, d.Y());
  a.set(2, d.Z());
  return a;
}

val intsToJs(const std::vector<int>& v)
{
  val a = val::array();
  for (size_t i = 0; i < v.size(); ++i)
  {
    a.set(i, v[i]);
  }
  return a;
}

template <class T>
static val typedCopy(const char* ctor, const T* data, size_t n)
{
  val out = val::global(ctor).new_(n);
  if (n > 0)
  {
    out.call<void>("set", val(emscripten::typed_memory_view(n, data)));
  }
  return out;
}

val bytesToJs(const std::string& bytes)
{
  return typedCopy("Uint8Array", reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

val float32ToJs(const std::vector<float>& v)
{
  return typedCopy("Float32Array", v.data(), v.size());
}

val uint32ToJs(const std::vector<uint32_t>& v)
{
  return typedCopy("Uint32Array", v.data(), v.size());
}

val float64ToJs(const std::vector<double>& v)
{
  return typedCopy("Float64Array", v.data(), v.size());
}

val int32ToJs(const std::vector<int32_t>& v)
{
  return typedCopy("Int32Array", v.data(), v.size());
}

// ---------------------------------------------------------------------------
ShapeMap mapOf(const TopoDS_Shape& shape, TopAbs_ShapeEnum kind)
{
  ShapeMap map;
  TopExp::MapShapes(shape, kind, map);
  return map;
}

const char* shapeTypeName(TopAbs_ShapeEnum type)
{
  switch (type)
  {
    case TopAbs_COMPOUND:  return "compound";
    case TopAbs_COMPSOLID: return "compsolid";
    case TopAbs_SOLID:     return "solid";
    case TopAbs_SHELL:     return "shell";
    case TopAbs_FACE:      return "face";
    case TopAbs_WIRE:      return "wire";
    case TopAbs_EDGE:      return "edge";
    case TopAbs_VERTEX:    return "vertex";
    default:               return "shape";
  }
}

} // namespace occtwasm
