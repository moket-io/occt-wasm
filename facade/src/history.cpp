// SPDX-License-Identifier: LGPL-2.1-only
#include "history.hpp"

#include "subshapes.hpp"

#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>

#include <algorithm>
#include <set>

namespace occtwasm {

namespace {

struct KindInfo
{
  TopAbs_ShapeEnum type;
  const char*      key;  // property name in the history object
  const char*      name; // value of GeneratedEntry.kind
};

constexpr KindInfo kKinds[3] = {
  {TopAbs_FACE, "faces", "face"},
  {TopAbs_EDGE, "edges", "edge"},
  {TopAbs_VERTEX, "vertices", "vertex"},
};

int kindIndex(TopAbs_ShapeEnum type)
{
  for (int k = 0; k < 3; ++k)
  {
    if (kKinds[k].type == type)
    {
      return k;
    }
  }
  return -1;
}

} // namespace

void pushUnique(std::vector<int>& v, int x)
{
  if (x >= 0 && std::find(v.begin(), v.end(), x) == v.end())
  {
    v.push_back(x);
  }
}

// Builds the history object for an algorithm. Indices are 0-based positions in
// TopExp::MapShapes order of the input shape (from) and of the result (to).
// Lists are packed into Int32Arrays (every sub-shape of a large input is at
// least in `unchanged`) and unpacked into objects by the JS loader:
//   modified  [input, from, n, to * n]...
//   generated [input, from, kind, n, to * n]...
//   deleted   [input, from]...
//   unchanged [input, from, to]...
val buildHistory(BRepBuilderAPI_MakeShape&        algo,
                 const std::vector<TopoDS_Shape>& inputs,
                 const TopoDS_Shape&              result)
{
  ShapeMap res[3];
  for (int k = 0; k < 3; ++k)
  {
    res[k] = mapOf(result, kKinds[k].type);
  }

  val history = val::object();
  history.set("packed", true);
  for (int k = 0; k < 3; ++k)
  {
    std::vector<int32_t> modified, generated, deleted, unchanged;
    auto                 list = [](std::vector<int32_t>& out, const std::vector<int>& to) {
      out.push_back(static_cast<int32_t>(to.size()));
      out.insert(out.end(), to.begin(), to.end());
    };

    for (size_t i = 0; i < inputs.size(); ++i)
    {
      const int      input = static_cast<int>(i);
      const ShapeMap in    = mapOf(inputs[i], kKinds[k].type);
      for (int j = 1; j <= in.Extent(); ++j)
      {
        const TopoDS_Shape& s    = in(j);
        const int           from = j - 1;

        const int same = indexOf(res[k], s);
        if (same >= 0)
        {
          unchanged.insert(unchanged.end(), {input, from, same});
        }

        // Individual queries may throw for some algorithms and sub-shape
        // kinds; a failing query means "no information", not a failed op.
        try
        {
          std::vector<int> to;
          for (NCollection_List<TopoDS_Shape>::Iterator it(algo.Modified(s)); it.More(); it.Next())
          {
            if (it.Value().ShapeType() == kKinds[k].type)
            {
              pushUnique(to, indexOf(res[k], it.Value()));
            }
          }
          if (!to.empty())
          {
            modified.insert(modified.end(), {input, from});
            list(modified, to);
          }
        }
        catch (...)
        {
        }

        try
        {
          std::vector<int> to[3];
          for (NCollection_List<TopoDS_Shape>::Iterator it(algo.Generated(s)); it.More(); it.Next())
          {
            const int kk = kindIndex(it.Value().ShapeType());
            if (kk >= 0)
            {
              pushUnique(to[kk], indexOf(res[kk], it.Value()));
            }
          }
          for (int kk = 0; kk < 3; ++kk)
          {
            if (!to[kk].empty())
            {
              generated.insert(generated.end(), {input, from, kk});
              list(generated, to[kk]);
            }
          }
        }
        catch (...)
        {
        }

        try
        {
          if (algo.IsDeleted(s))
          {
            deleted.insert(deleted.end(), {input, from});
          }
        }
        catch (...)
        {
        }
      }
    }

    val kh = val::object();
    kh.set("modified", int32ToJs(modified));
    kh.set("generated", int32ToJs(generated));
    kh.set("deleted", int32ToJs(deleted));
    kh.set("unchanged", int32ToJs(unchanged));
    history.set(kKinds[k].key, kh);
  }
  return history;
}

std::vector<int> faceIndices(const TopoDS_Shape& part, const ShapeMap& resFaces)
{
  std::vector<int> out;
  if (part.IsNull())
  {
    return out;
  }
  const ShapeMap faces = mapOf(part, TopAbs_FACE);
  for (int i = 1; i <= faces.Extent(); ++i)
  {
    pushUnique(out, indexOf(resFaces, faces(i)));
  }
  return out;
}

val opResult(ShapeId id, const val& history)
{
  val r = okResult();
  r.set("shape", id);
  r.set("history", history);
  return r;
}

std::vector<TopoDS_Edge> pickEdges(const TopoDS_Shape& shape, const val& jsEdges)
{
  const std::vector<int> idx = toInts(jsEdges, "edges");
  if (idx.empty())
  {
    fail("INVALID_ARGUMENT", "edges must not be empty");
  }
  const ShapeMap           map = mapOf(shape, TopAbs_EDGE);
  std::set<int>            seen;
  std::vector<TopoDS_Edge> out;
  for (int i : idx)
  {
    if (i < 0 || i >= map.Extent())
    {
      fail("INVALID_ARGUMENT",
           "edge index " + std::to_string(i) + " out of range [0, " + std::to_string(map.Extent())
             + ")");
    }
    if (seen.insert(i).second)
    {
      out.push_back(TopoDS::Edge(map(i + 1)));
    }
  }
  return out;
}

std::vector<TopoDS_Face> pickFaces(const TopoDS_Shape& shape, const val& jsFaces, bool allowEmpty)
{
  const std::vector<int> idx = toInts(jsFaces, "faces");
  if (idx.empty() && !allowEmpty)
  {
    fail("INVALID_ARGUMENT", "faces must not be empty");
  }
  const ShapeMap           map = mapOf(shape, TopAbs_FACE);
  std::set<int>            seen;
  std::vector<TopoDS_Face> out;
  for (int i : idx)
  {
    if (i < 0 || i >= map.Extent())
    {
      fail("INVALID_ARGUMENT",
           "face index " + std::to_string(i) + " out of range [0, " + std::to_string(map.Extent())
             + ")");
    }
    if (seen.insert(i).second)
    {
      out.push_back(TopoDS::Face(map(i + 1)));
    }
  }
  return out;
}

// Algorithms often wrap a single solid into a compound. Return the solid
// itself so that callers see the same shape type as the input.
TopoDS_Shape unwrap(const TopoDS_Shape& shape)
{
  if (shape.IsNull() || shape.ShapeType() != TopAbs_COMPOUND)
  {
    return shape;
  }
  TopoDS_Iterator it(shape);
  if (!it.More())
  {
    return shape;
  }
  TopoDS_Shape child = it.Value();
  it.Next();
  return it.More() ? shape : child;
}

void requireValid(const TopoDS_Shape& result, const char* what)
{
  if (!isValid(result))
  {
    fail("INVALID_RESULT", std::string(what) + " produced an invalid shape");
  }
}

} // namespace occtwasm
