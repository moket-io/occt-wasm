// Golden fixtures for modeled threads (0.10.1): a groove swept along a helix
// and cut from a shaft (external) or from the wall of a bore (internal).
// The removed volume has a closed form: a helical sweep of a section in an
// axial plane removes, per turn, the section's area times the path of its
// centroid (Pappus), so a slab of the material `turns` pitches high loses
// area inside the material x 2 pi x centroid radius x turns.
import { beforeAll, describe, expect, it } from 'vitest';
import type { OcctKernel } from '../dist/index.js';
import { loadKernel, relErr } from './kernel.js';
import { ok } from './models.mjs';

let k: OcctKernel;
beforeAll(async () => {
  k = await loadKernel();
});

const P = 1; // pitch, M6 x 1
const H = 20; // height of the material, z in [0, H]
const Z0 = -1.5; // the groove starts below the material and ends above it
const LENGTH = H + 3;

/** A symmetric trapezoid in (r, z): width wa at radius ra, wb at rb, from r0 to r1. */
function trapezoid(r0: number, r1: number, ra: number, wa: number, rb: number, wb: number) {
  const width = (r: number) => wa + ((r - ra) * (wb - wa)) / (rb - ra);
  const points: [number, number][] = [
    [r0, -width(r0) / 2],
    [r1, -width(r1) / 2],
    [r1, width(r1) / 2],
    [r0, width(r0) / 2],
  ];
  /** Area x 2 pi x centroid radius x H / P of the part between lo and hi. */
  const removed = (lo: number, hi: number) => {
    const a = width(lo);
    const b = width(hi);
    const area = ((a + b) / 2) * (hi - lo);
    const centroid = lo + ((hi - lo) * (a + 2 * b)) / (3 * (a + b));
    return area * 2 * Math.PI * centroid * (H / P);
  };
  return { points, removed };
}

// External M6 x 1: from the minor radius out past the major one.
const external = trapezoid(2.3865655, 3.1, 2.3865655, 0.1666668, 3.1, 0.990470);
// Internal M6 x 1: 0.75 P wide at D1/2, P/8 at r = 3, from inside the bore out to r = 3.
const internal = trapezoid(2.4, 3, 4.917 / 2, 0.75 * P, 3, P / 8);

/** The groove: the section in the axial plane at angle `start`, swept along a helix of radius 3. */
function groove(points: [number, number][], start: number, leftHanded: boolean) {
  const xDir: [number, number, number] = [Math.cos(start), Math.sin(start), 0];
  const plane = { origin: [0, 0, Z0], xDir, normal: [-xDir[1], xDir[0], 0] };
  const loop = points.map((a, i) => ({ kind: 'line' as const, a, b: points[(i + 1) % points.length] }));
  const profile = ok(k.makeProfileFace(plane, [loop])).shape;
  const spine = ok(k.helix([0, 0, Z0], [0, 0, 1], 3, P, LENGTH, { xDir, leftHanded })).shape;
  const swept = ok(k.sweep(profile, spine, { orientation: 'binormal', binormal: [0, 0, 1] })).shape;
  expect(ok(k.check(swept)).valid).toBe(true);
  return swept;
}

const volume = (shape: number) => ok(k.properties(shape)).volume;

/** Cuts the groove and checks the removed volume, validity and that common is the removed part. */
function expectCut(body: number, tool: number, expected: number) {
  const cut = ok(k.boolean('cut', body, tool)).shape;
  expect(ok(k.check(cut)).valid).toBe(true);
  expect(relErr(volume(body) - volume(cut), expected)).toBeLessThan(1e-4);
  const common = ok(k.boolean('common', body, tool)).shape;
  expect(relErr(volume(common), expected)).toBeLessThan(1e-4);
}

const cases = [
  { hand: 'right', left: false, start: 0 },
  { hand: 'right', left: false, start: 0.7 },
  { hand: 'left', left: true, start: 0 },
  { hand: 'left', left: true, start: 0.7 },
];

describe('modeled threads', () => {
  it('builds the helix one edge per turn', () => {
    expect(ok(k.topology(ok(k.helix([0, 0, Z0], [0, 0, 1], 3, P, LENGTH)).shape)).edges).toHaveLength(23);
    // Not a whole number of turns: equal pieces of less than a turn.
    const t = ok(k.topology(ok(k.helix([0, 0, 0], [0, 0, 1], 3, P, 2.5)).shape));
    expect(t.edges).toHaveLength(3);
    const turn = Math.hypot(2 * Math.PI * 3, P);
    for (const e of t.edges) expect(relErr(e.length, (2.5 * turn) / 3)).toBeLessThan(1e-6);
    expect(ok(k.topology(ok(k.helix([0, 0, 0], [0, 0, 1], 3, P, 0.25)).shape)).edges).toHaveLength(1);
  });

  for (const c of cases) {
    it(`cuts an external groove from a shaft (${c.hand}-handed, start ${c.start})`, () => {
      const shaft = ok(k.makeCylinder([0, 0, 0], [0, 0, 1], 3, H)).shape;
      expectCut(shaft, groove(external.points, c.start, c.left), external.removed(external.points[0][0], 3));
    });

    it(`cuts an internal groove from a bore (${c.hand}-handed, start ${c.start})`, () => {
      const box = ok(k.transform(ok(k.makeBox(10, 10, H)).shape, [1, 0, 0, -5, 0, 1, 0, -5, 0, 0, 1, 0])).shape;
      const bore = ok(k.makeCylinder([0, 0, -1], [0, 0, 1], 2.5, H + 2)).shape;
      const block = ok(k.boolean('cut', box, bore)).shape;
      expectCut(block, groove(internal.points, c.start, c.left), internal.removed(2.5, 3));
    });
  }
});
