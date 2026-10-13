// SPDX-License-Identifier: LGPL-2.1-only
#include "history.hpp"

#include <BRepCheck_Analyzer.hxx>
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

val entry(int input, int from)
{
  val e = val::object();
  e.set("input", input);
  e.set("from", from);
  return e;
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
  for (int k = 0; k < 3; ++k)
  {
    val modified  = val::array();
    val generated = val::array();
    val deleted   = val::array();
    val unchanged = val::array();
    int nMod = 0, nGen = 0, nDel = 0, nSame = 0;

    for (size_t i = 0; i < inputs.size(); ++i)
    {
      const ShapeMap in = mapOf(inputs[i], kKinds[k].type);
      for (int j = 1; j <= in.Extent(); ++j)
      {
        const TopoDS_Shape& s    = in(j);
        const int           from = j - 1;

        const int same = indexOf(res[k], s);
        if (same >= 0)
        {
          val e = entry(static_cast<int>(i), from);
          e.set("to", same);
          unchanged.set(nSame++, e);
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
            val e = entry(static_cast<int>(i), from);
            e.set("to", intsToJs(to));
            modified.set(nMod++, e);
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
              val e = entry(static_cast<int>(i), from);
              e.set("kind", kKinds[kk].name);
              e.set("to", intsToJs(to[kk]));
              generated.set(nGen++, e);
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
            deleted.set(nDel++, entry(static_cast<int>(i), from));
          }
        }
        catch (...)
        {
        }
      }
    }

    val kh = val::object();
    kh.set("modified", modified);
    kh.set("generated", generated);
    kh.set("deleted", deleted);
    kh.set("unchanged", unchanged);
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
  if (!BRepCheck_Analyzer(result).IsValid())
  {
    fail("INVALID_RESULT", std::string(what) + " produced an invalid shape");
  }
}

} // namespace occtwasm
