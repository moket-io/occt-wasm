// ESM loader for the occt-wasm module. Works in browsers, Web Workers and Node.
//
// SPDX-License-Identifier: LGPL-2.1-only
import createOcctModule from './occt-wasm.mjs';

const METHODS = [
  'version',
  'heapStats',
  'release',
  'releaseAll',
  'makeBox',
  'makeCylinder',
  'makeVertex',
  'helix',
  'subShape',
  'makeProfileFace',
  'makeWire',
  'joinEdges',
  'extrude',
  'revolve',
  'boolean',
  'fillet',
  'chamfer',
  'transform',
  'thickSolid',
  'offsetShape',
  'draft',
  'sweep',
  'loft',
  'topology',
  'properties',
  'mesh',
  'check',
  'distance',
  'project',
  'exportStep',
  'importStep',
  'importStepParts',
  'exportBrep',
  'importBrep',
  'exportStepAssembly',
];

// Bindings take every argument; these pad trailing optional ones.
const ARITY = {
  helix: 6,
  thickSolid: 4,
  offsetShape: 3,
  sweep: 3,
  loft: 2,
  exportStep: 2,
  importStepParts: 2,
};

const SURFACES = ['plane', 'cylinder', 'cone', 'sphere', 'torus', 'bspline', 'other'];
const CURVES = ['line', 'circle', 'ellipse', 'bspline', 'other'];
const KINDS = ['face', 'edge', 'vertex'];

// The facade packs large results into typed arrays (crossing into JS one
// value at a time is what costs); these turn them back into the documented
// objects.

const vec = (a, i) => [a[i], a[i + 1], a[i + 2]];

function unpackTopology(r) {
  const F = r.packedFaces;
  const faces = [];
  for (let i = 0, o = 0; o < F.length; i++, o += 23) {
    const f = {
      index: i,
      surface: SURFACES[F[o]],
      area: F[o + 1],
      centroid: vec(F, o + 2),
      normal: vec(F, o + 5),
      bbox: [F[o + 8], F[o + 9], F[o + 10], F[o + 11], F[o + 12], F[o + 13]],
      reversed: F[o + 14] === 1,
    };
    if (F[o + 15] === 1) {
      f.axis = { origin: vec(F, o + 16), dir: vec(F, o + 19) };
      f.radius = F[o + 22];
    }
    faces.push(f);
  }
  const E = r.packedEdges;
  const A = r.packedEdgeFaces;
  const edges = [];
  for (let i = 0, o = 0, a = 0; o < E.length; i++, o += 20) {
    const degenerated = E[o] === 1;
    const e = { index: i, degenerated, curve: CURVES[E[o + 1]], length: E[o + 2], midpoint: vec(E, o + 3) };
    if (!degenerated) {
      e.start = vec(E, o + 6);
      e.end = vec(E, o + 9);
      if (E[o + 12] === 1) {
        e.axis = { origin: vec(E, o + 13), dir: vec(E, o + 16) };
        e.radius = E[o + 19];
      }
    }
    const n = A[a];
    e.faces = Array.from(A.subarray(a + 1, a + 1 + n));
    a += 1 + n;
    edges.push(e);
  }
  const V = r.packedVertices;
  const vertices = [];
  for (let i = 0; i * 3 < V.length; i++) vertices.push({ index: i, point: vec(V, i * 3) });
  return { ok: true, faces, edges, vertices };
}

function unpackKind(h) {
  const modified = [];
  const M = h.modified;
  for (let o = 0; o < M.length; ) {
    const n = M[o + 2];
    modified.push({ input: M[o], from: M[o + 1], to: Array.from(M.subarray(o + 3, o + 3 + n)) });
    o += 3 + n;
  }
  const generated = [];
  const G = h.generated;
  for (let o = 0; o < G.length; ) {
    const n = G[o + 3];
    generated.push({ input: G[o], from: G[o + 1], kind: KINDS[G[o + 2]], to: Array.from(G.subarray(o + 4, o + 4 + n)) });
    o += 4 + n;
  }
  const deleted = [];
  const D = h.deleted;
  for (let o = 0; o < D.length; o += 2) deleted.push({ input: D[o], from: D[o + 1] });
  const unchanged = [];
  const U = h.unchanged;
  for (let o = 0; o < U.length; o += 3) unchanged.push({ input: U[o], from: U[o + 1], to: U[o + 2] });
  return { modified, generated, deleted, unchanged };
}

function unpack(name, r) {
  if (!r || r.ok !== true) return r;
  if (name === 'topology') return unpackTopology(r);
  if (r.history && r.history.packed) {
    const h = r.history;
    r.history = { faces: unpackKind(h.faces), edges: unpackKind(h.edges), vertices: unpackKind(h.vertices) };
  }
  return r;
}

function errorResult(code, message) {
  return { ok: false, error: { code, message } };
}

/**
 * Instantiates the OCCT module and returns the kernel API.
 * @param {import('./index').OcctKernelOptions} [options]
 * @returns {Promise<import('./index').OcctKernel>}
 */
export async function createOcctKernel(options = {}) {
  const moduleArgs = {};
  if (options.wasmUrl !== undefined) {
    const url = String(options.wasmUrl);
    moduleArgs.locateFile = (path, prefix) => (path.endsWith('.wasm') ? url : prefix + path);
  } else if (options.locateFile) {
    moduleArgs.locateFile = options.locateFile;
  }
  if (options.wasmBinary !== undefined) moduleArgs.wasmBinary = options.wasmBinary;
  if (options.print) moduleArgs.print = options.print;
  if (options.printErr) moduleArgs.printErr = options.printErr;

  const module = await createOcctModule(moduleArgs);

  const kernel = {
    module,
    trapped: false,
  };

  for (const name of METHODS) {
    const fn = module[name];
    const arity = ARITY[name] ?? 0;
    kernel[name] = (...args) => {
      while (args.length < arity) args.push(undefined);
      try {
        return unpack(name, fn(...args));
      } catch (e) {
        // The facade catches every C++ exception, so anything reaching here
        // is a wasm trap (abort, out of bounds access, stack overflow) or a
        // binding error (wrong argument count). After a trap the module
        // state is undefined: drop the kernel and create a new one.
        if (typeof WebAssembly !== 'undefined' && e instanceof WebAssembly.RuntimeError) {
          kernel.trapped = true;
          return errorResult('WASM_TRAP', String(e.message));
        }
        if (typeof WebAssembly !== 'undefined' && WebAssembly.Exception && e instanceof WebAssembly.Exception) {
          kernel.trapped = true;
          return errorResult('UNCAUGHT_EXCEPTION', 'Uncaught C++ exception');
        }
        return errorResult('JS_ERROR', String(e && e.message ? e.message : e));
      }
    };
  }

  return kernel;
}

export default createOcctKernel;
