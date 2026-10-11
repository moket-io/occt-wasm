# occt-wasm API

The module exports one ESM entry point, `dist/index.mjs`, with hand-written types in `dist/index.d.ts`. This document describes the API of facade version 0.1.0 (OCCT 8.0.1).

```ts
import { createOcctKernel } from '@moket/occt-wasm';

const k = await createOcctKernel({ wasmUrl: hashedWasmUrl });
const profile = k.makeProfileFace(plane, loops);
if (!profile.ok) throw new Error(profile.error.message);
```

## Conventions

- **Results, not exceptions.** Every call returns a plain object. Fallible calls return `{ ok: true, ... }` or `{ ok: false, error: { code, message } }`. No C++ exception crosses the boundary. `version()` and `heapStats()` cannot fail and return their data directly.
- **Shape handles.** Shapes live in an arena inside the module and are addressed by integer `ShapeId`s (positive, never reused within one kernel instance). Results are new shapes; inputs are never modified. Free shapes with `release(id)` or `releaseAll()`. The kernel does not free anything on its own.
- **Sub-shape indices.** Faces, edges and vertices are identified by 0-based indices in `TopExp::MapShapes(shape, kind)` order. `topology()`, `mesh()`, `fillet()/chamfer()` edge arguments and all history entries use the same indices. They are stable for a given shape (same handle, or the same shape re-read from `exportBrep` bytes), not across operations. Use the history to follow sub-shapes across operations.
- **Units.** Millimetres and radians.
- **Vectors.** `Vec3` is any array-like of 3 numbers, `Vec2` of 2 numbers. Directions do not need to be normalized but must be non-zero.
- **Typed arrays.** Typed arrays returned by `mesh()`, `exportStep()` and `exportBrep()` are copies owned by the caller and can be transferred to another thread.
- **Traps.** If the wasm instance traps (out of bounds access, stack overflow, abort), the call returns `{ ok: false, error: { code: 'WASM_TRAP' } }` and `kernel.trapped` becomes `true`. The instance must then be dropped and a new kernel created.

## Loading

```ts
function createOcctKernel(options?: OcctKernelOptions): Promise<OcctKernel>;

interface OcctKernelOptions {
  wasmUrl?: string | URL; // URL of occt-wasm.wasm (e.g. a hashed asset URL)
  locateFile?: (path: string, prefix: string) => string; // ignored when wasmUrl is set
  wasmBinary?: ArrayBuffer | Uint8Array; // pre-fetched bytes, skips fetching
  print?: (text: string) => void;
  printErr?: (text: string) => void;
}
```

Without options the module resolves `occt-wasm.wasm` next to `occt-wasm.mjs` (`new URL(..., import.meta.url)`). This works in Node 24, in browsers and in module Web Workers (`new Worker(url, { type: 'module' })`). Each `createOcctKernel()` call creates an independent instance with its own memory and arena.

## Module

| Call           | Result                                                           |
| -------------- | ---------------------------------------------------------------- |
| `version()`    | `{ occt: '8.0.1', facade: '0.1.0', emscripten: '6.0.12' }`       |
| `heapStats()`  | `{ heapBytes, usedBytes, liveShapes }`                           |
| `release(id)`  | `Result<{}>`, `INVALID_HANDLE` if the id is unknown or released  |
| `releaseAll()` | `Result<{ released: number }>`                                   |

`heapBytes` is the size of the wasm linear memory (it only grows), `usedBytes` the bytes currently allocated by `malloc`.

## Construction

```ts
makeBox(dx: number, dy: number, dz: number): Result<{ shape: ShapeId }>;
makeCylinder(axisOrigin: Vec3, axisDir: Vec3, radius: number, height: number): Result<{ shape: ShapeId }>;
makeProfileFace(plane: Plane, loops: Loop[]): Result<{ shape: ShapeId; segmentEdges: number[][] }>;
```

`makeBox` has a corner at the origin. `makeCylinder` starts at `axisOrigin` and extends `height` along `axisDir`.

### makeProfileFace

Builds a planar face from sketch loops.

```ts
interface Plane { origin: Vec3; normal: Vec3; xDir: Vec3 }
type Loop = Segment[];
type Segment =
  | { kind: 'line'; a: Vec2; b: Vec2 }
  | { kind: 'arc'; center: Vec2; radius: number; start: Vec2; end: Vec2; ccw?: boolean }
  | { kind: 'circle'; center: Vec2; radius: number };
```

- 2D coordinates are in the plane frame: `u` along `xDir` (projected to be orthogonal to `normal`), `v` along `yDir = normal x xDir`.
- `loops[0]` is the outer boundary, the other loops are holes.
- A loop is an ordered, closed chain: each segment ends where the next one starts (gap up to 1e-6 mm) and the last ends where the first starts. A `circle` is a loop on its own.
- An arc goes from `start` to `end` counter-clockwise around the plane normal when `ccw` is `true` (default), clockwise otherwise. `start` and `end` must lie on the circle (1e-6 relative).
- Loop direction does not matter: the outer loop is oriented counter-clockwise and holes clockwise automatically, so the face normal is always `plane.normal`.
- The result face is checked with `BRepCheck_Analyzer`; self-intersecting loops or holes outside the outer loop give `INVALID_PROFILE`.

`segmentEdges[loop][segment]` is the edge index of the face created for that segment. Extrude/revolve history refers to the same indices, which lets a caller name a side face by the sketch segment it came from.

## Operations

```ts
extrude(shape: ShapeId, dir: Vec3, distance: number): Result<{ shape; history: History; sweep: SweepInfo }>;
revolve(shape: ShapeId, axisOrigin: Vec3, axisDir: Vec3, angle: number): Result<{ shape; history: History; sweep: SweepInfo }>;
boolean(op: 'fuse' | 'cut' | 'common', a: ShapeId, b: ShapeId): Result<{ shape; history: History }>;
fillet(shape: ShapeId, edges: number[], radius: number): Result<{ shape; history: History }>;
chamfer(shape: ShapeId, edges: number[], distance: number): Result<{ shape; history: History }>;
transform(shape: ShapeId, matrix: number[12]): Result<{ shape; history: History }>;
```

- `extrude`: prism of a face (or wire, edge, shell) along `normalize(dir) * distance`; a negative distance extrudes backwards.
- `revolve`: revolution about the axis; `|angle|` in (0, 2 pi].
- `boolean`: `BRepAlgoAPI_Fuse/Cut/Common`. Input `0` is `a`, input `1` is `b`. The result is not simplified (no `UnifySameDomain`), so co-planar faces of a fuse stay split.
- `fillet`/`chamfer`: constant radius fillet / symmetric chamfer on the given edge indices of `shape` (duplicates ignored). The result is validated with `BRepCheck_Analyzer`; an invalid result is reported as `INVALID_RESULT` instead of being returned.
- `transform`: a transformed copy (`BRepBuilderAPI_Transform` with copy). `matrix` is row-major 3x4 `[r11 r12 r13 tx r21 r22 r23 ty r31 r32 r33 tz]`, a rigid motion or a reflection (a scale other than 1 is `INVALID_ARGUMENT`). History maps every face, edge and vertex to its copy (`modified`). Used for patterns and mirrors.
- A result that is a compound with a single child is returned as that child (OCCT wraps fillet results in a compound).

### History

The history describes how sub-shapes of the inputs relate to sub-shapes of the result. It is the input to topological naming.

```ts
interface History {
  faces: KindHistory;    // keyed by the kind of the INPUT sub-shape
  edges: KindHistory;
  vertices: KindHistory;
}
interface KindHistory {
  modified: { input: number; from: number; to: number[] }[];
  generated: { input: number; from: number; kind: 'face' | 'edge' | 'vertex'; to: number[] }[];
  deleted: { input: number; from: number }[];
  unchanged: { input: number; from: number; to: number }[];
}
```

- `input` is the argument index of the input shape (`boolean`: 0 = `a`, 1 = `b`; all other operations: 0).
- `from` is the sub-shape index in the input shape, `to` the index in the result, both in `TopExp::MapShapes` order of their own shape.
- `modified`: OCCT `Modified(s)`. The input sub-shape became the listed result sub-shapes of the same kind (for example a face split by a boolean, or trimmed by a fillet). Shapes not present in the result are dropped.
- `generated`: OCCT `Generated(s)`. The listed result sub-shapes were created from the input sub-shape; `kind` is the kind of the result sub-shapes in `to`. If one input sub-shape generates several kinds, there is one entry per kind. Examples: a profile edge generates a prism side `face`, a profile vertex generates a vertical `edge`, a filleted edge generates a fillet `face`, a boolean face generates intersection `edge`s.
- `deleted`: OCCT `IsDeleted(s)`, for example the caps of a cut tool.
- `unchanged`: the input sub-shape is present in the result as the very same TopoDS shape (same `TShape` and location).

An input sub-shape can appear in several lists (for example `unchanged` and `generated`). Sub-shapes absent from all lists have no reported relation.

### SweepInfo (extrude, revolve)

```ts
interface SweepInfo {
  bottom: number[];                          // faces of FirstShape (the profile itself)
  top: number[];                             // faces of LastShape (the translated/rotated copy)
  sides: { edge: number; faces: number[] }[]; // per profile edge index, the generated side faces
}
```

`bottom` and `top` are empty for a full revolution. For extrude, `bottom` is the profile face itself, which also appears in `history.faces.unchanged`.

## Queries

```ts
topology(shape: ShapeId): Result<{ faces: FaceInfo[]; edges: EdgeInfo[]; vertices: VertexInfo[] }>;
properties(shape: ShapeId): Result<Properties>;
mesh(shape: ShapeId, linearDeflection: number, angularDeflection: number): Result<Mesh>;
check(shape: ShapeId): Result<{ valid: boolean; shapeType; solids: number }>;
distance(a: ShapeId, subA: { kind; index } | null, b: ShapeId, subB: { kind; index } | null):
  Result<{ value: number; pointA: [x, y, z]; pointB: [x, y, z]; solutions: number }>;
```

```ts
interface FaceInfo {
  index: number;
  surface: 'plane' | 'cylinder' | 'cone' | 'sphere' | 'torus' | 'bspline' | 'other';
  area: number;
  centroid: [x, y, z];
  normal: [x, y, z];     // outward, at the middle of the UV range (constant for planes)
  bbox: [xmin, ymin, zmin, xmax, ymax, zmax];
  reversed: boolean;     // face orientation relative to its surface
  axis?: { origin: [x, y, z]; dir: [x, y, z] }; // cylinder, cone, sphere (center), torus
  radius?: number;       // cylinder, sphere; cone reference radius; torus major radius
}
interface EdgeInfo {
  index: number;
  curve: 'line' | 'circle' | 'ellipse' | 'bspline' | 'other';
  length: number;
  midpoint: [x, y, z];   // point at the middle of the parameter range
  faces: number[];       // adjacent faces, unique (a seam edge lists its face once)
  degenerated: boolean;
  start?: [x, y, z];     // at the first and last curve parameter; absent when degenerated
  end?: [x, y, z];
  axis?: { origin: [x, y, z]; dir: [x, y, z] }; // circles: center and normal
  radius?: number;       // circles
}
interface VertexInfo { index: number; point: [x, y, z] }

interface Properties {
  shapeType: 'compound' | 'compsolid' | 'solid' | 'shell' | 'face' | 'wire' | 'edge' | 'vertex' | 'shape';
  volume: number;
  area: number;
  centroid: [x, y, z];   // volume centroid, or surface centroid if the volume is zero
  bbox: [xmin, ymin, zmin, xmax, ymax, zmax]; // BRepBndLib::AddOptimal, not enlarged by tolerances
  valid: boolean;        // BRepCheck_Analyzer
  solids: number; shells: number; faces: number; edges: number; vertices: number;
}

interface Mesh {
  positions: Float32Array;     // xyz per vertex, vertices are not shared between faces
  normals: Float32Array;       // unit normal per vertex
  indices: Uint32Array;        // triangles, counter-clockwise seen from outside
  faceRanges: Uint32Array;     // [start, count] into indices, one pair per face index
  edgePositions: Float32Array; // xyz per polyline point
  edgeRanges: Uint32Array;     // [start, count] in points, one pair per edge index
}
```

`check()` runs only `BRepCheck_Analyzer`; use it instead of `properties()` when only validity is needed. `distance()` runs `BRepExtrema_DistShapeShape` between two shapes or sub-shapes (`{ kind: 'face' | 'edge' | 'vertex', index }`, `null` for the whole shape) and returns the first solution's points.

`mesh()` drops any previous triangulation of the shape and runs `BRepMesh_IncrementalMesh` with the given linear (mm) and angular (radians) deflection. Edge polylines come from the face triangulation, so their points coincide with mesh vertices. A degenerated edge has `count = 0`.

## Exchange

```ts
exportStep(shapes: ShapeId[]): Result<{ bytes: Uint8Array }>;
importStep(bytes: Uint8Array | ArrayBuffer): Result<{ shape: ShapeId }>;
exportBrep(shape: ShapeId): Result<{ bytes: Uint8Array }>;
importBrep(bytes: Uint8Array | ArrayBuffer): Result<{ shape: ShapeId }>;
```

- STEP goes through in-memory streams (`STEPControl_Writer::WriteStream`, `STEPControl_Reader::ReadStream`), AP214, millimetres. Several shapes are written as several roots; on import several roots come back as one compound.
- `exportBrep` writes the binary BRep format (`BinTools`, without triangulation). `importBrep` reads binary or text BRep. A BRep round trip is exact and keeps the sub-shape order, so it is suitable for caching results with their history indices.

## Error codes

| Code                  | Meaning                                                     |
| --------------------- | ----------------------------------------------------------- |
| `INVALID_ARGUMENT`    | Wrong type, non-finite number, out of range value or index  |
| `INVALID_HANDLE`      | Unknown or released `ShapeId`                               |
| `INVALID_PROFILE`     | Profile loops are open, self-intersecting or inconsistent   |
| `INVALID_RESULT`      | Fillet/chamfer produced a shape that fails `BRepCheck`      |
| `EMPTY_RESULT`        | The operation produced a null shape                         |
| `EXTRUDE_FAILED`, `REVOLVE_FAILED`, `BOOLEAN_FAILED`, `FILLET_FAILED`, `CHAMFER_FAILED`, `TRANSFORM_FAILED`, `DISTANCE_FAILED`, `MESH_FAILED` | The OCCT algorithm reported failure |
| `STEP_READ_FAILED`, `STEP_WRITE_FAILED`, `BREP_READ_FAILED` | Exchange failures                   |
| `OCCT_EXCEPTION`      | An OCCT `Standard_Failure` was thrown (type and message included) |
| `STD_EXCEPTION`, `OUT_OF_MEMORY`, `UNKNOWN_EXCEPTION` | Other C++ exceptions                        |
| `WASM_TRAP`, `UNCAUGHT_EXCEPTION`, `JS_ERROR` | Raised by the JS loader, see Traps above              |
