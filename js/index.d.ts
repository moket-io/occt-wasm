// Hand-written types for @moket/occt-wasm. The Emscripten-generated
// declarations (occt-wasm.generated.d.ts) are kept only as a reference.
//
// SPDX-License-Identifier: LGPL-2.1-only

/** Integer handle of a shape in the module arena. Ids are never reused. */
export type ShapeId = number;

export type Vec2 = readonly [number, number] | ArrayLike<number>;
export type Vec3 = readonly [number, number, number] | ArrayLike<number>;
/** [xmin, ymin, zmin, xmax, ymax, zmax] */
export type BBox = [number, number, number, number, number, number];

export type ErrorCode =
  | 'INVALID_ARGUMENT'
  | 'INVALID_HANDLE'
  | 'INVALID_PROFILE'
  | 'INVALID_RESULT'
  | 'EMPTY_RESULT'
  | 'EXTRUDE_FAILED'
  | 'REVOLVE_FAILED'
  | 'BOOLEAN_FAILED'
  | 'FILLET_FAILED'
  | 'CHAMFER_FAILED'
  | 'TRANSFORM_FAILED'
  | 'DISTANCE_FAILED'
  | 'MESH_FAILED'
  | 'STEP_READ_FAILED'
  | 'STEP_WRITE_FAILED'
  | 'BREP_READ_FAILED'
  | 'OCCT_EXCEPTION'
  | 'STD_EXCEPTION'
  | 'OUT_OF_MEMORY'
  | 'UNKNOWN_EXCEPTION'
  | 'WASM_TRAP'
  | 'UNCAUGHT_EXCEPTION'
  | 'JS_ERROR';

export interface KernelError {
  code: ErrorCode;
  message: string;
}

export interface Failure {
  ok: false;
  error: KernelError;
}

export type Result<T> = ({ ok: true } & T) | Failure;

// ---------------------------------------------------------------------------
// Construction

/** Sketch plane. xDir is projected to be orthogonal to normal; yDir = normal x xDir. */
export interface Plane {
  origin: Vec3;
  normal: Vec3;
  xDir: Vec3;
}

export interface LineSegment {
  kind: 'line';
  a: Vec2;
  b: Vec2;
}

export interface ArcSegment {
  kind: 'arc';
  center: Vec2;
  radius: number;
  start: Vec2;
  end: Vec2;
  /** Counter-clockwise around the plane normal. Default true. */
  ccw?: boolean;
}

export interface CircleSegment {
  kind: 'circle';
  center: Vec2;
  radius: number;
}

export type Segment = LineSegment | ArcSegment | CircleSegment;

/**
 * Closed loop: each segment ends where the next one starts (gap <= 1e-6 mm),
 * the last one ends where the first one starts. A circle is a loop by itself.
 */
export type Loop = readonly Segment[];

export type ShapeResult = Result<{ shape: ShapeId }>;

export type ProfileFaceResult = Result<{
  shape: ShapeId;
  /**
   * segmentEdges[loop][segment] = 0-based edge index of the face
   * (TopExp::MapShapes order), the same index extrude/revolve history uses.
   */
  segmentEdges: number[][];
}>;

// ---------------------------------------------------------------------------
// History

export type SubShapeKind = 'face' | 'edge' | 'vertex';

/** Input sub-shape `from` became result sub-shapes `to` (same kind). */
export interface ModifiedEntry {
  /** Argument index of the input shape (boolean: 0 = a, 1 = b; others: 0). */
  input: number;
  from: number;
  to: number[];
}

/** Input sub-shape `from` generated result sub-shapes `to` of kind `kind`. */
export interface GeneratedEntry {
  input: number;
  from: number;
  kind: SubShapeKind;
  to: number[];
}

export interface DeletedEntry {
  input: number;
  from: number;
}

/** Input sub-shape `from` is present in the result as the same TopoDS shape at index `to`. */
export interface UnchangedEntry {
  input: number;
  from: number;
  to: number;
}

export interface KindHistory {
  modified: ModifiedEntry[];
  generated: GeneratedEntry[];
  deleted: DeletedEntry[];
  unchanged: UnchangedEntry[];
}

/**
 * Keyed by the kind of the INPUT sub-shape. Indices are 0-based positions in
 * TopExp::MapShapes order of the input shape (`from`) and of the result (`to`),
 * i.e. the same indices topology() reports.
 */
export interface History {
  faces: KindHistory;
  edges: KindHistory;
  vertices: KindHistory;
}

/** Prism/revolution structure, result face indices. */
export interface SweepInfo {
  /** Faces of the start cap (FirstShape). Empty for full revolutions. */
  bottom: number[];
  /** Faces of the end cap (LastShape). */
  top: number[];
  /** For every edge of the input profile, the side faces it generated. */
  sides: { edge: number; faces: number[] }[];
}

export type OpResult = Result<{ shape: ShapeId; history: History }>;
export type SweepResult = Result<{ shape: ShapeId; history: History; sweep: SweepInfo }>;

export type BooleanOp = 'fuse' | 'cut' | 'common';

// ---------------------------------------------------------------------------
// Queries

export type SurfaceType = 'plane' | 'cylinder' | 'cone' | 'sphere' | 'torus' | 'bspline' | 'other';
export type CurveType = 'line' | 'circle' | 'ellipse' | 'bspline' | 'other';

export interface FaceInfo {
  index: number;
  surface: SurfaceType;
  area: number;
  centroid: [number, number, number];
  /** Outward normal at the middle of the face parameter range. */
  normal: [number, number, number];
  bbox: BBox;
  /** Face orientation is REVERSED relative to its surface. */
  reversed: boolean;
  /** Axis of cylinders, cones, spheres (center, pole direction) and tori. */
  axis?: Axis;
  /** Cylinder and sphere radius, cone reference radius, torus major radius. */
  radius?: number;
}

export interface Axis {
  origin: [number, number, number];
  dir: [number, number, number];
}

export interface EdgeInfo {
  index: number;
  curve: CurveType;
  length: number;
  midpoint: [number, number, number];
  /** Indices of adjacent faces (unique). */
  faces: number[];
  degenerated: boolean;
  /** Points at the first and last curve parameter (absent for degenerated edges). */
  start?: [number, number, number];
  end?: [number, number, number];
  /** Circle center and normal. */
  axis?: Axis;
  /** Circle radius. */
  radius?: number;
}

export interface VertexInfo {
  index: number;
  point: [number, number, number];
}

export type TopologyResult = Result<{
  faces: FaceInfo[];
  edges: EdgeInfo[];
  vertices: VertexInfo[];
}>;

export type ShapeType =
  | 'compound'
  | 'compsolid'
  | 'solid'
  | 'shell'
  | 'face'
  | 'wire'
  | 'edge'
  | 'vertex'
  | 'shape';

export type PropertiesResult = Result<{
  shapeType: ShapeType;
  volume: number;
  area: number;
  /** Volume centroid, or surface centroid when the shape has no volume. */
  centroid: [number, number, number];
  /** Optimal bounding box (BRepBndLib::AddOptimal, not enlarged by tolerances). */
  bbox: BBox;
  /** BRepCheck_Analyzer verdict. */
  valid: boolean;
  solids: number;
  shells: number;
  faces: number;
  edges: number;
  vertices: number;
}>;

export type MeshResult = Result<{
  /** xyz per vertex. Vertices are not shared between faces. */
  positions: Float32Array;
  normals: Float32Array;
  /** Triangles, counter-clockwise seen from outside. */
  indices: Uint32Array;
  /** Pairs [start, count] into `indices`, one pair per face index. */
  faceRanges: Uint32Array;
  /** xyz per polyline point. */
  edgePositions: Float32Array;
  /** Pairs [start, count] in points into `edgePositions`, one pair per edge index. */
  edgeRanges: Uint32Array;
}>;

export type BytesResult = Result<{ bytes: Uint8Array }>;

export type CheckResult = Result<{
  /** BRepCheck_Analyzer verdict. */
  valid: boolean;
  shapeType: ShapeType;
  solids: number;
}>;

/** A face, edge or vertex of a shape by its topology() index. */
export interface SubShapeRef {
  kind: SubShapeKind;
  index: number;
}

export type DistanceResult = Result<{
  /** Minimum distance. */
  value: number;
  /** Closest points on a and b (the first solution). */
  pointA: [number, number, number];
  pointB: [number, number, number];
  /** Number of solutions; parallel faces have several. */
  solutions: number;
}>;

/** A view for hidden line removal. Orthographic. */
export interface ProjectView {
  /** Direction the viewer looks in, e.g. [0, 1, 0] for a front view. */
  direction: Vec3;
  /** Up on the drawing; must not be parallel to direction. */
  up: Vec3;
  /** Also return the hidden parts. Default false. */
  hidden?: boolean;
  /** Also return smooth (tangent) edges between faces. Default false. */
  smooth?: boolean;
  /** Chordal deviation of polylines for curves other than lines and circles, mm. Default 0.01. */
  deflection?: number;
}

interface ProjectedCurveBase {
  visible: boolean;
  /** sharp edge, smooth (tangent) edge, or outline (silhouette of a curved face). */
  kind: 'sharp' | 'smooth' | 'outline';
  /** Position of the source shape in the shapes passed to project(). */
  shape: number;
  /** topology() index of the source edge; -1 for outlines. */
  edge: number;
  /** topology() index of the face an outline belongs to; -1 otherwise. */
  face: number;
}

/** View coordinates: x to the right, y up, in model units. */
export type ProjectedCurve = ProjectedCurveBase &
  (
    | { type: 'line'; a: [number, number]; b: [number, number] }
    | { type: 'circle'; center: [number, number]; radius: number }
    | {
        type: 'arc';
        center: [number, number];
        radius: number;
        /** Start, a point in between and end. */
        a: [number, number];
        m: [number, number];
        b: [number, number];
      }
    | { type: 'polyline'; points: number[] }
  );

export type ProjectResult = Result<{
  curves: ProjectedCurve[];
  /** The view's x axis in model space (up x towards the viewer). */
  xDir: [number, number, number];
  /** Time spent in OCCT, ms. */
  ms: number;
}>;

export interface VersionInfo {
  occt: string;
  facade: string;
  emscripten: string;
}

export interface HeapStats {
  /** Size of the wasm linear memory. */
  heapBytes: number;
  /** Bytes currently allocated by malloc. */
  usedBytes: number;
  liveShapes: number;
}

// ---------------------------------------------------------------------------
// Kernel

export interface OcctKernel {
  /** The raw Emscripten module. */
  readonly module: unknown;
  /** True after a wasm trap; the instance must then be discarded. */
  readonly trapped: boolean;

  version(): VersionInfo;
  heapStats(): HeapStats;
  release(id: ShapeId): Result<{}>;
  releaseAll(): Result<{ released: number }>;

  makeBox(dx: number, dy: number, dz: number): ShapeResult;
  makeCylinder(axisOrigin: Vec3, axisDir: Vec3, radius: number, height: number): ShapeResult;
  makeProfileFace(plane: Plane, loops: readonly Loop[]): ProfileFaceResult;

  /** Prism of a face (or wire, edge, shell) along dir * distance. */
  extrude(shape: ShapeId, dir: Vec3, distance: number): SweepResult;
  /** Revolution around the axis by angle (radians, |angle| in (0, 2*pi]). */
  revolve(shape: ShapeId, axisOrigin: Vec3, axisDir: Vec3, angle: number): SweepResult;
  boolean(op: BooleanOp, a: ShapeId, b: ShapeId): OpResult;
  /** Constant-radius fillet; edges are 0-based indices from topology(shape). */
  fillet(shape: ShapeId, edges: ArrayLike<number>, radius: number): OpResult;
  /** Symmetric chamfer; edges are 0-based indices from topology(shape). */
  chamfer(shape: ShapeId, edges: ArrayLike<number>, distance: number): OpResult;
  /**
   * Transformed copy. `matrix` is a row-major 3x4 rigid motion or reflection
   * [r11 r12 r13 tx r21 r22 r23 ty r31 r32 r33 tz]. History maps every
   * sub-shape to its copy.
   */
  transform(shape: ShapeId, matrix: ArrayLike<number>): OpResult;

  topology(shape: ShapeId): TopologyResult;
  properties(shape: ShapeId): PropertiesResult;
  /** angularDeflection in radians. */
  mesh(shape: ShapeId, linearDeflection: number, angularDeflection: number): MeshResult;
  /** Validity only, without mass properties: much cheaper than properties(). */
  check(shape: ShapeId): CheckResult;
  /** Minimum distance between shapes or their sub-shapes (null = the whole shape). */
  distance(a: ShapeId, subA: SubShapeRef | null, b: ShapeId, subB: SubShapeRef | null): DistanceResult;
  /** Exact hidden line removal (HLRBRep_Algo) onto the view plane; the shapes hide each other. */
  project(shapes: ShapeId | readonly ShapeId[], view: ProjectView): ProjectResult;

  exportStep(shapes: ArrayLike<ShapeId>): BytesResult;
  importStep(bytes: Uint8Array | ArrayBuffer): ShapeResult;
  /** Binary BRep (BinTools) for caching. */
  exportBrep(shape: ShapeId): BytesResult;
  /** Binary (BinTools) or text BRep. */
  importBrep(bytes: Uint8Array | ArrayBuffer): ShapeResult;
  /**
   * STEP of an assembly through XCAF: one product per part (a compound when
   * a part has several shapes), placed by named instances under one root.
   */
  exportStepAssembly(assembly: StepAssembly): BytesResult;
}

export interface StepAssembly {
  /** Name of the root product. */
  name?: string;
  parts: ReadonlyArray<{ name?: string; shapes: ArrayLike<ShapeId> }>;
  /**
   * `part` indexes `parts`; `matrix` is a row-major 3x4 rigid motion
   * [r11 r12 r13 tx r21 r22 r23 ty r31 r32 r33 tz].
   */
  instances: ReadonlyArray<{ part: number; name?: string; matrix: ArrayLike<number> }>;
}

export interface OcctKernelOptions {
  /** URL of occt-wasm.wasm, e.g. a hashed asset URL from the host bundler. */
  wasmUrl?: string | URL;
  /** Full control over file resolution (ignored when wasmUrl is set). */
  locateFile?: (path: string, prefix: string) => string;
  /** Pre-fetched module bytes; skips fetching. */
  wasmBinary?: ArrayBuffer | Uint8Array;
  print?: (text: string) => void;
  printErr?: (text: string) => void;
}

export function createOcctKernel(options?: OcctKernelOptions): Promise<OcctKernel>;
export default createOcctKernel;
