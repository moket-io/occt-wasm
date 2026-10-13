// 0.6.0: queries on a result reuse what is known about the faces and edges it
// shares with its input. Every answer must equal the one for a fresh copy of
// the same shape (a BRep round trip shares nothing).
import { beforeAll, describe, expect, it } from 'vitest';
import type { OcctKernel, ShapeId } from '../dist/index.js';
import { loadKernel } from './kernel.js';
import { buildR2, buildR3, circleLoop, ok, XY } from './models.mjs';

let k: OcctKernel;
beforeAll(async () => {
  k = await loadKernel();
});

const fresh = (shape: ShapeId) => ok(k.importBrep(ok(k.exportBrep(shape)).bytes)).shape;

/** R2 with a filleted pocket and a hole: queried after every step. */
function plate() {
  let shape = buildR2(k).shape;
  ok(k.topology(shape));
  ok(k.properties(shape));
  ok(k.mesh(shape, 0.05, 0.3));
  const pocket = ok(k.extrude(ok(k.makeProfileFace({ ...XY, origin: [0, 0, 6] }, [circleLoop(50, 30, 12)])).shape, [0, 0, 1], 5));
  shape = ok(k.boolean('cut', shape, pocket.shape)).shape;
  ok(k.topology(shape));
  const rim = ok(k.topology(shape)).edges.filter((e) => e.curve === 'circle' && Math.abs(e.midpoint[2] - 10) < 1e-9 && Math.abs((e.radius ?? 0) - 12) < 1e-9);
  shape = ok(k.fillet(shape, rim.map((e) => e.index), 1)).shape;
  return shape;
}

function expectClose(a: unknown, b: unknown, path = ''): void {
  if (typeof a === 'number' && typeof b === 'number') {
    expect(Math.abs(a - b), path).toBeLessThanOrEqual(1e-9 * Math.max(1, Math.abs(b)));
    return;
  }
  if (Array.isArray(a) && Array.isArray(b)) {
    expect(a.length, path).toBe(b.length);
    a.forEach((v, i) => expectClose(v, b[i], `${path}[${i}]`));
    return;
  }
  if (a && b && typeof a === 'object' && typeof b === 'object') {
    expect(Object.keys(a), path).toEqual(Object.keys(b));
    for (const key of Object.keys(a)) expectClose((a as never)[key], (b as never)[key], `${path}.${key}`);
    return;
  }
  expect(a, path).toEqual(b);
}

describe('incremental queries', () => {
  it('describes a result like a fresh copy of it', () => {
    const shape = plate();
    expectClose(ok(k.topology(shape)), ok(k.topology(fresh(shape))));
  });

  it('measures a result like a fresh copy of it', () => {
    const shape = plate();
    const a = ok(k.properties(shape));
    const b = ok(k.properties(fresh(shape)));
    expect(a.valid).toBe(true);
    expect(Math.abs(a.volume - b.volume) / b.volume).toBeLessThan(1e-9);
    expect(Math.abs(a.area - b.area) / b.area).toBeLessThan(1e-9);
    a.centroid.forEach((v, i) => expect(v).toBeCloseTo(b.centroid[i], 7));
    a.bbox.forEach((v, i) => expect(v).toBeCloseTo(b.bbox[i], 7));
    expect({ ...a, volume: 0, area: 0, centroid: 0, bbox: 0 }).toEqual({ ...b, volume: 0, area: 0, centroid: 0, bbox: 0 });
    expect(ok(k.properties(buildR3(k).shape)).volume).toBeGreaterThan(0);
  });

  it('meshes only the faces an operation changed', () => {
    const r2 = buildR2(k).shape;
    const before = ok(k.mesh(r2, 0.05, 0.3));
    const cut = ok(k.boolean('cut', r2, ok(k.makeCylinder([50, 30, -1], [0, 0, 1], 5, 20)).shape));
    const after = ok(k.mesh(cut.shape, 0.05, 0.3));
    const triangles = (m: typeof before, face: number) => m.faceRanges[2 * face + 1];
    // A hole wall of R2 the cut did not touch keeps its triangulation.
    for (const u of cut.history.faces.unchanged.filter((e) => e.input === 0))
      expect(triangles(after, u.to)).toBe(triangles(before, u.from));
    // The same mesh as a fresh copy meshed from scratch, face by face.
    const copy = ok(k.mesh(fresh(cut.shape), 0.05, 0.3));
    expect(after.faceRanges.length).toBe(copy.faceRanges.length);
    expect(after.edgeRanges.length).toBe(copy.edgeRanges.length);
    // Other deflections mesh everything again.
    const coarse = ok(k.mesh(cut.shape, 0.5, 0.5));
    expect(coarse.indices.length).toBeLessThan(after.indices.length);
  });

  it('still finds invalid shapes', () => {
    const box = ok(k.makeBox(40, 30, 20)).shape;
    expect(ok(k.check(box)).valid).toBe(true);
    const bytes = ok(k.exportBrep(box)).bytes;
    // Move one vertex off its edges: some copy of a 40 in the file is one.
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let invalid = 0;
    for (let i = 0; i + 8 <= bytes.length; i++)
      for (const little of [true, false]) {
        if (view.getFloat64(i, little) !== 40) continue;
        const moved = bytes.slice();
        new DataView(moved.buffer).setFloat64(i, 41, little);
        const r = k.importBrep(moved);
        if (r.ok && !ok(k.check(r.shape)).valid) {
          invalid++;
          expect(ok(k.properties(r.shape)).valid).toBe(false);
        }
      }
    expect(invalid).toBeGreaterThan(0);
  });

  it('forgets faces no shape holds any more', () => {
    k.releaseAll();
    expect(k.heapStats().cachedSubShapes).toBe(0);
    for (let i = 0; i < 400; i++) {
      const box = ok(k.makeBox(10, 10, 10)).shape;
      const hole = ok(k.makeCylinder([5, 5, -1], [0, 0, 1], 2, 12)).shape;
      const cut = ok(k.boolean('cut', box, hole)).shape;
      ok(k.topology(cut));
      ok(k.check(cut));
      for (const id of [box, hole, cut]) ok(k.release(id));
    }
    expect(k.heapStats().liveShapes).toBe(0);
    expect(k.heapStats().cachedSubShapes).toBeLessThan(8192);
    k.releaseAll();
    expect(k.heapStats().cachedSubShapes).toBe(0);
  });
});
