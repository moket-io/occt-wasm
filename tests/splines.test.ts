// Golden fixtures for B-spline profile segments (0.9.0): sketch splines and
// text outlines. Expected areas come from the curves themselves: the integral
// of x dy over a polynomial curve, by Gauss-Legendre quadrature per span,
// which is exact for the degrees used here.
import { beforeAll, describe, expect, it } from 'vitest';
import type { BSplineSegment, OcctKernel, Segment, Vec2 } from '../dist/index.js';
import { loadKernel, relErr } from './kernel.js';
import { circleLoop, ok, XY } from './models.mjs';

let k: OcctKernel;
beforeAll(async () => {
  k = await loadKernel();
});

const code = (r: { ok: boolean; error?: { code: string } }) => (r.ok ? 'OK' : r.error?.code);
/** The plane x = 0 seen from -y: sketch x is model x, sketch y is model z. */
const XZ = { origin: [0, 0, 0], normal: [0, -1, 0], xDir: [1, 0, 0] };

const line = (a: Vec2, b: Vec2): Segment => ({ kind: 'line', a, b });
const bezier = (...poles: Vec2[]): BSplineSegment => {
  const n = poles.length;
  return { kind: 'bspline', poles, degree: n - 1, knots: [0, 1], multiplicities: [n, n] };
};

/** Point and derivative of a Bezier curve (de Casteljau on the poles and their differences). */
function bezierAt(poles: readonly Vec2[], t: number): { p: Vec2; d: Vec2 } {
  const point = (pts: readonly Vec2[]): Vec2 => {
    let q = pts.map((p) => [p[0], p[1]] as Vec2);
    while (q.length > 1) q = q.slice(1).map((p, i) => [q[i][0] + (p[0] - q[i][0]) * t, q[i][1] + (p[1] - q[i][1]) * t]);
    return q[0];
  };
  const n = poles.length - 1;
  const diffs = poles.slice(1).map((p, i) => [n * (p[0] - poles[i][0]), n * (p[1] - poles[i][1])] as Vec2);
  return { p: point(poles), d: point(diffs) };
}

// 10-point Gauss-Legendre on [0, 1]: exact for polynomials up to degree 19.
const GAUSS: [number, number][] = [
  [0.1488743389816312, 0.2955242247147529],
  [0.4333953941292472, 0.2692667193099963],
  [0.6794095682990244, 0.219086362515982],
  [0.8650633666889845, 0.1494513491505806],
  [0.9739065285171717, 0.0666713443086881],
].flatMap(([x, w]) => [
  [(1 - x) / 2, w / 2],
  [(1 + x) / 2, w / 2],
]);

/** Integral of x dy along a Bezier curve, split into `parts` pieces. */
function xdy(poles: readonly Vec2[], parts = 64): number {
  let sum = 0;
  for (let i = 0; i < parts; i++) {
    for (const [s, w] of GAUSS) {
      const { p, d } = bezierAt(poles, (i + s) / parts);
      sum += (w / parts) * p[0] * d[1];
    }
  }
  return sum;
}

/** Arc length of a Bezier curve (not polynomial: fine Gauss panels). */
function arcLength(poles: readonly Vec2[], parts = 256): number {
  let sum = 0;
  for (let i = 0; i < parts; i++) {
    for (const [s, w] of GAUSS) {
      const { d } = bezierAt(poles, (i + s) / parts);
      sum += (w / parts) * Math.hypot(d[0], d[1]);
    }
  }
  return sum;
}

/** Point and derivative of a non-rational B-spline at u (de Boor on the poles and on the derivative's poles). */
function splineAt(s: BSplineSegment, u: number): { p: Vec2; d: Vec2 } {
  const flat = s.knots.flatMap((t, i) => Array<number>(s.multiplicities[i]).fill(t));
  const deBoor = (pts: readonly Vec2[], knots: number[], deg: number): Vec2 => {
    let span = deg;
    while (span < pts.length - 1 && u >= knots[span + 1]) span++;
    const d = pts.slice(span - deg, span + 1).map((q) => [q[0], q[1]] as [number, number]);
    for (let r = 1; r <= deg; r++) {
      for (let j = deg; j >= r; j--) {
        const i = span - deg + j;
        const a = (u - knots[i]) / (knots[i + deg + 1 - r] - knots[i]);
        d[j] = [(1 - a) * d[j - 1][0] + a * d[j][0], (1 - a) * d[j - 1][1] + a * d[j][1]];
      }
    }
    return d[deg];
  };
  const p = s.degree;
  const diffs = s.poles.slice(1).map((q, i) => {
    const f = p / (flat[i + p + 1] - flat[i + 1]);
    return [f * (q[0] - s.poles[i][0]), f * (q[1] - s.poles[i][1])] as Vec2;
  });
  return { p: deBoor(s.poles, flat, p), d: deBoor(diffs, flat.slice(1, -1), p - 1) };
}

/** Integral of x dy along a non-rational B-spline, Gauss per knot span (exact). */
function splineXdy(s: BSplineSegment): number {
  let sum = 0;
  for (let i = 0; i + 1 < s.knots.length; i++) {
    const [u0, u1] = [s.knots[i], s.knots[i + 1]];
    for (const [t, w] of GAUSS) {
      const { p, d } = splineAt(s, u0 + (u1 - u0) * t);
      sum += w * (u1 - u0) * p[0] * d[1];
    }
  }
  return sum;
}

/** Enclosed area of a closed loop of lines and Beziers: |sum of x dy|. */
function loopArea(loop: readonly Segment[]): number {
  let sum = 0;
  for (const s of loop) {
    if (s.kind === 'line') sum += ((s.a[0] + s.b[0]) / 2) * (s.b[1] - s.a[1]);
    else if (s.kind === 'bspline') sum += xdy(s.poles);
    else throw new Error('lines and Beziers only');
  }
  return Math.abs(sum);
}

/** A rounded square of four quadratic Beziers through the side midpoints, corners as control points. */
function roundedSquare(r: number, ccw = true): Segment[] {
  const loop = [
    bezier([r, 0], [r, r], [0, r]),
    bezier([0, r], [-r, r], [-r, 0]),
    bezier([-r, 0], [-r, -r], [0, -r]),
    bezier([0, -r], [r, -r], [r, 0]),
  ];
  return ccw ? loop : loop.reverse().map((s) => ({ ...s, poles: [...s.poles].reverse() }));
}

const ARCH: Vec2[] = [
  [0, 0],
  [10, 30],
  [30, 30],
  [40, 0],
];

describe('bspline profile segments', () => {
  it('extrudes a cubic arch closed by a line: volume = 10 x the exact area', () => {
    const area = loopArea([bezier(...ARCH), line([40, 0], [0, 0])]);
    // Closed form: y = 90 t (1 - t), x = 30 t + 30 t^2 - 20 t^3, so the
    // integral of y dx over [0, 1] is 90 * (15 + 10 - 30 + 12) = 630.
    expect(relErr(area, 630)).toBeLessThan(1e-12);

    const profile = ok(k.makeProfileFace(XY, [[bezier(...ARCH), line([40, 0], [0, 0])]]));
    const topo = ok(k.topology(profile.shape));
    expect(topo.edges).toHaveLength(2);
    expect(topo.edges.map((e) => e.curve).sort()).toEqual(['bspline', 'line']);
    expect(topo.edges[profile.segmentEdges[0][0]].curve).toBe('bspline');
    expect(topo.edges[profile.segmentEdges[0][1]].curve).toBe('line');
    expect(relErr(topo.faces[0].area, area)).toBeLessThan(1e-9);
    // The face normal is the plane normal whatever the loop direction.
    expect(topo.faces[0].normal[2]).toBeCloseTo(1, 12);

    const solid = ok(k.extrude(profile.shape, [0, 0, 1], 10));
    const p = ok(k.properties(solid.shape));
    expect(p).toMatchObject({ valid: true, solids: 1, faces: 4 });
    expect(relErr(p.volume, 10 * area)).toBeLessThan(1e-9);
    // Top of the arch: y(1/2) = 22.5.
    expect(p.bbox[4]).toBeCloseTo(22.5, 5);
    expect(p.bbox[5]).toBeCloseTo(10, 6);
    // One side face per segment, the spline's a B-spline-swept surface.
    expect(solid.sweep.sides.map((s) => s.faces.length)).toEqual([1, 1]);

    // The same loop the other way round gives the same solid.
    const reversed = ok(k.makeProfileFace(XY, [[line([0, 0], [40, 0]), bezier(...[...ARCH].reverse())]]));
    const q = ok(k.properties(ok(k.extrude(reversed.shape, [0, 0, 1], 10)).shape));
    expect(q.valid).toBe(true);
    expect(relErr(q.volume, 10 * area)).toBeLessThan(1e-9);
  });

  it('extrudes a letter-like profile of quadratic Beziers with a hole', () => {
    const outer = roundedSquare(10);
    const hole = roundedSquare(5); // counter-clockwise like the outer loop: flipped by the kernel
    const outerArea = loopArea(outer);
    const holeArea = loopArea(hole);
    // A parabolic segment is 2/3 of its control triangle: diamond 2 r^2 + 4 * 2/3 * r^2 / 2.
    expect(relErr(outerArea, 1000 / 3)).toBeLessThan(1e-12);
    expect(relErr(holeArea, 250 / 3)).toBeLessThan(1e-12);

    const profile = ok(k.makeProfileFace(XY, [outer, hole]));
    expect(profile.segmentEdges.map((l) => l.length)).toEqual([4, 4]);
    expect(new Set(profile.segmentEdges.flat()).size).toBe(8);
    const face = ok(k.topology(profile.shape)).faces[0];
    expect(relErr(face.area, outerArea - holeArea)).toBeLessThan(1e-9);

    const height = 3;
    const solid = ok(k.extrude(profile.shape, [0, 0, 1], height));
    const p = ok(k.properties(solid.shape));
    // Top, bottom, four outer and four inner walls.
    expect(p).toMatchObject({ valid: true, solids: 1, shells: 1, faces: 10 });
    expect(relErr(p.volume, (outerArea - holeArea) * height)).toBeLessThan(1e-9);
    // The hole is a hole: a pin through its middle does not touch the solid.
    const pin = ok(k.extrude(ok(k.makeProfileFace({ ...XY, origin: [0, 0, -1] }, [circleLoop(0, 0, 1)])).shape, [0, 0, 1], 5));
    const cut = ok(k.boolean('cut', solid.shape, pin.shape));
    expect(relErr(ok(k.properties(cut.shape)).volume, p.volume)).toBeLessThan(1e-9);

    // A glyph given clockwise, the hole clockwise too.
    const cw = ok(k.makeProfileFace(XY, [roundedSquare(10, false), roundedSquare(5, false)]));
    const q = ok(k.properties(ok(k.extrude(cw.shape, [0, 0, 1], height)).shape));
    expect(q.valid).toBe(true);
    expect(relErr(q.volume, (outerArea - holeArea) * height)).toBeLessThan(1e-9);
  });

  it('builds a loop of one closed spline with several knot spans', () => {
    // The same rounded square as one quadratic B-spline: interior knots of
    // multiplicity 2 make it pass through the side midpoints, with corners
    // there. Mass properties integrate such faces span by span.
    const r = 10;
    const spline: BSplineSegment = {
      kind: 'bspline',
      degree: 2,
      poles: [
        [r, 0],
        [r, r],
        [0, r],
        [-r, r],
        [-r, 0],
        [-r, -r],
        [0, -r],
        [r, -r],
        [r, 0],
      ],
      knots: [0, 1, 2, 3, 4],
      multiplicities: [3, 2, 2, 2, 3],
    };
    const profile = ok(k.makeProfileFace(XY, [[spline]]));
    expect(profile.segmentEdges).toEqual([[0]]);
    const p = ok(k.properties(ok(k.extrude(profile.shape, [0, 0, 1], 2)).shape));
    expect(p.valid).toBe(true);
    [-10, -10, 0, 10, 10, 2].forEach((v, i) => expect(p.bbox[i]).toBeCloseTo(v, 6));
    expect(relErr(ok(k.topology(profile.shape)).faces[0].area, 1000 / 3)).toBeLessThan(1e-9);
    expect(relErr(p.volume, (2 * 1000) / 3)).toBeLessThan(1e-9);
  });

  it('extrudes a smooth cubic spline of several spans: volume = height x the exact area', () => {
    const spline: BSplineSegment = {
      kind: 'bspline',
      degree: 3,
      poles: [
        [0, 0],
        [5, 20],
        [15, 30],
        [25, 30],
        [35, 20],
        [40, 0],
      ],
      knots: [0, 1, 2, 3],
      multiplicities: [4, 1, 1, 4],
    };
    const area = Math.abs(splineXdy(spline));
    const profile = ok(k.makeProfileFace(XY, [[spline, line([40, 0], [0, 0])]]));
    const p = ok(k.properties(ok(k.extrude(profile.shape, [0, 0, 1], 10)).shape));
    expect(p.valid).toBe(true);
    expect(relErr(ok(k.topology(profile.shape)).faces[0].area, area)).toBeLessThan(1e-9);
    expect(relErr(p.volume, 10 * area)).toBeLessThan(1e-9);
  });

  it('builds an exact quarter circle from a rational spline', () => {
    const quarter: BSplineSegment = {
      kind: 'bspline',
      degree: 2,
      poles: [
        [10, 0],
        [10, 10],
        [0, 10],
      ],
      weights: [1, Math.SQRT1_2, 1],
      knots: [0, 1],
      multiplicities: [3, 3],
    };
    const profile = ok(k.makeProfileFace(XY, [[quarter, line([0, 10], [0, 0]), line([0, 0], [10, 0])]]));
    const topo = ok(k.topology(profile.shape));
    const arc = topo.edges[profile.segmentEdges[0][0]];
    expect(arc.curve).toBe('bspline');
    expect(relErr(arc.length, 5 * Math.PI)).toBeLessThan(1e-6);
    // On the circle: the midpoint is at radius 10.
    expect(Math.hypot(arc.midpoint[0], arc.midpoint[1])).toBeCloseTo(10, 9);

    const p = ok(k.properties(ok(k.extrude(profile.shape, [0, 0, 1], 1)).shape));
    expect(p.valid).toBe(true);
    expect(relErr(p.volume, (Math.PI * 100) / 4)).toBeLessThan(1e-9);
  });

  it('revolves a spline profile', () => {
    // The arch on XZ, shifted off the axis by 10, a full turn about z (Pappus).
    const shifted = ARCH.map((q) => [q[0] + 10, q[1]] as Vec2);
    const loop = [bezier(...shifted), line([50, 0], [10, 0])];
    const profile = ok(k.makeProfileFace(XZ, [loop]));
    const p = ok(k.properties(ok(k.revolve(profile.shape, [0, 0, 0], [0, 0, 1], 2 * Math.PI)).shape));
    expect(p.valid).toBe(true);
    // V = 2 pi * integral of x dA = 2 pi * |integral of x^2/2 dy| over the boundary.
    let moment = 0;
    for (let i = 0; i < 64; i++) {
      for (const [s, w] of GAUSS) {
        const { p: q, d } = bezierAt(shifted, (i + s) / 64);
        moment += (w / 64) * ((q[0] * q[0]) / 2) * d[1];
      }
    }
    expect(relErr(p.volume, 2 * Math.PI * Math.abs(moment))).toBeLessThan(1e-7);
  });
});

describe('bspline paths', () => {
  const curve: Vec2[] = [
    [0, 0],
    [0, 15],
    [15, 30],
    [30, 30],
  ];

  it('makes an open wire of a spline and a line and sweeps a circle along it', () => {
    // In order from the profile, so that the sweep starts at the origin.
    const path: Segment[] = [bezier(...curve), line([30, 30], [40, 30])];
    const w = ok(k.makeWire(XZ, path));
    expect(w.closed).toBe(false);
    const edges = ok(k.topology(w.shape)).edges;
    expect(edges[w.segmentEdges[0]].curve).toBe('bspline');
    expect(edges[w.segmentEdges[1]].curve).toBe('line');
    const length = arcLength(curve) + 10;
    expect(relErr(edges.reduce((s, e) => s + e.length, 0), length)).toBeLessThan(1e-6);

    // The tube's volume is the section times the path length (the section's
    // centroid is on the path, the bend radius stays above the tube radius).
    const disc = ok(k.makeProfileFace(XY, [circleLoop(0, 0, 2)])).shape;
    const s = ok(k.sweep(disc, w.shape));
    const p = ok(k.properties(s.shape));
    expect(p.valid).toBe(true);
    expect(relErr(p.volume, Math.PI * 4 * length)).toBeLessThan(1e-4);
    expect(p.bbox[3]).toBeCloseTo(40, 4);
    expect(p.bbox[5]).toBeCloseTo(32, 3);
  });
});

describe('bspline validation', () => {
  const base = bezier(...ARCH);
  const cases: [string, Partial<BSplineSegment> & Record<string, unknown>, RegExp][] = [
    ['degree 0', { degree: 0 }, /degree must be 1 to 8/],
    ['degree 9', { degree: 9 }, /degree must be 1 to 8/],
    ['fractional degree', { degree: 2.5 }, /degree must be an integer/],
    ['too few poles', { poles: ARCH.slice(0, 3) }, /at least degree \+ 1 poles/],
    ['one knot', { knots: [0], multiplicities: [4] }, /at least 2 knots/],
    ['knots and multiplicities of different lengths', { multiplicities: [4, 4, 1] }, /one multiplicity per knot/],
    ['decreasing knots', { knots: [1, 0] }, /strictly increasing/],
    ['repeated knots', { knots: [0, 0] }, /strictly increasing/],
    ['unclamped ends', { poles: [...ARCH, [50, 10]], multiplicities: [5, 4] }, /degree \+ 1 at both ends/],
    [
      'interior multiplicity above the degree',
      { poles: [...ARCH, [50, 10], [60, 0], [70, 0], [80, 0]], knots: [0, 0.5, 1], multiplicities: [4, 4, 4] },
      /1 to degree inside/,
    ],
    ['multiplicities not adding up', { poles: [...ARCH, [50, 10]] }, /add up to poles \+ degree \+ 1/],
    ['weights of the wrong count', { weights: [1, 1, 1] }, /one weight per pole/],
    ['zero weight', { weights: [1, 0, 1, 1] }, /weights must be positive/],
    ['negative weight', { weights: [1, -1, 1, 1] }, /weights must be positive/],
    ['non-finite pole', { poles: [[0, 0], [10, Infinity], [30, 30], [40, 0]] }, /must be finite/],
    ['poles not an array', { poles: 3 as never }, /poles must be an array/],
    ['missing knots', { knots: undefined }, /knots must be an array/],
  ];

  for (const [name, change, message] of cases) {
    it(`refuses ${name} naming the segment`, () => {
      const bad = { ...base, ...change } as Segment;
      const r = k.makeProfileFace(XY, [[line([40, 0], [0, 0]), bad]]);
      expect(code(r)).toBe('INVALID_ARGUMENT');
      if (!r.ok) {
        expect(r.error.message).toMatch(/^loop 0 segment 1: /);
        expect(r.error.message).toMatch(message);
      }
      const w = k.makeWire(XZ, [line([40, 0], [50, 0]), bad]);
      expect(code(w)).toBe('INVALID_ARGUMENT');
      if (!w.ok) expect(w.error.message).toMatch(/^segment 1: /);
    });
  }

  it('refuses a spline whose poles all coincide and one that does not meet its neighbour', () => {
    const point = bezier([5, 5], [5, 5], [5, 5]);
    expect(code(k.makeProfileFace(XY, [[point]]))).toBe('INVALID_PROFILE');
    const open = k.makeProfileFace(XY, [[bezier(...ARCH), line([40, 0], [1, 0])]]);
    expect(code(open)).toBe('INVALID_PROFILE');
    // The module keeps working.
    expect(ok(k.makeProfileFace(XY, [[bezier(...ARCH), line([40, 0], [0, 0])]])).segmentEdges[0]).toHaveLength(2);
  });
});
