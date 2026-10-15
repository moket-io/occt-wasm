// Golden fixtures for the 0.5.0 calls: shell (thickSolid), offset, draft,
// wires, sweep, loft and STEP import part by part. Expected values come from
// formulas; the tolerance is 1e-6 relative for volumes.
import { beforeAll, describe, expect, it } from 'vitest';
import type { OcctKernel, Segment } from '../dist/index.js';
import { loadKernel, relErr } from './kernel.js';
import { buildR1, buildR3, circleLoop, ok, rectLoop, XY } from './models.mjs';

let k: OcctKernel;
beforeAll(async () => {
  k = await loadKernel();
});

const code = (r: { ok: boolean; error?: { code: string } }) => (r.ok ? 'OK' : r.error?.code);
const faceAtZ = (shape: number, z: number) =>
  ok(k.topology(shape)).faces.find((f) => f.surface === 'plane' && Math.abs(f.centroid[2] - z) < 1e-9 && f.normal[2] > 0.99)!
    .index;
/** The plane x = 0 seen from -y: sketch x is model x, sketch y is model z. */
const XZ = { origin: [0, 0, 0], normal: [0, -1, 0], xDir: [1, 0, 0] };

describe('thickSolid (shell)', () => {
  it('hollows R1 with the top open and maps the walls to the faces they came from', () => {
    const { shape } = buildR1(k);
    const top = faceAtZ(shape, 20);
    const shell = ok(k.thickSolid(shape, [top], -2));
    const p = ok(k.properties(shell.shape));
    expect(p.valid).toBe(true);
    expect(p.solids).toBe(1);
    expect(relErr(p.volume, 24000 - 36 * 26 * 18)).toBeLessThan(1e-6);
    [0, 0, 0, 40, 30, 20].forEach((v, i) => expect(p.bbox[i]).toBeCloseTo(v, 6));
    // 5 outer walls, 5 inner walls and the rim.
    expect(p.faces).toBe(11);
    // The removed face becomes the rim; every kept face stays as the outer
    // wall and generates its inner wall.
    const rim = shell.history.faces.modified.find((m) => m.from === top)!.to;
    expect(rim).toHaveLength(1);
    expect(ok(k.topology(shell.shape)).faces[rim[0]].area).toBeCloseTo(40 * 30 - 36 * 26, 6);
    for (const f of [0, 1, 2, 3, 4, 5].filter((f) => f !== top)) {
      expect(shell.history.faces.unchanged.some((u) => u.from === f)).toBe(true);
      expect(shell.history.faces.generated.filter((g) => g.from === f && g.kind === 'face')).toHaveLength(1);
    }
  });

  it('closes a cavity when no face is removed', () => {
    const inside = ok(k.thickSolid(buildR1(k).shape, [], -2));
    const p = ok(k.properties(inside.shape));
    expect(p).toMatchObject({ valid: true, solids: 1, shells: 2, faces: 12 });
    expect(relErr(p.volume, 24000 - 36 * 26 * 16)).toBeLessThan(1e-6);
    // Like the walls of an open shell: each face stays and generates its offset copy.
    expect(inside.history.faces.unchanged).toHaveLength(6);
    expect(inside.history.faces.generated).toHaveLength(6);
    // Outside: the part itself is the cavity.
    const outside = ok(k.properties(ok(k.thickSolid(buildR1(k).shape, [], 1)).shape));
    const grown = 24000 + 5200 + Math.PI * (40 + 30 + 20) + (4 / 3) * Math.PI;
    expect(relErr(outside.volume, grown - 24000)).toBeLessThan(1e-6);
  });

  it('shells a filleted part thinner than its fillets and fails at or above the radius', () => {
    const { shape } = buildR3(k);
    const bottom = ok(k.topology(shape)).faces.find((f) => f.surface === 'plane' && f.normal[2] < -0.99)!.index;
    const thin = ok(k.thickSolid(shape, [bottom], -1));
    expect(ok(k.properties(thin.shape)).valid).toBe(true);
    const thick = k.thickSolid(shape, [bottom], -4);
    expect(code(thick)).toBe('SHELL_FAILED');
    // The module keeps working.
    expect(ok(k.properties(buildR1(k).shape)).volume).toBeCloseTo(24000, 6);
  });
});

describe('offsetShape', () => {
  it('grows R1 by 1 with rounded edges and corners', () => {
    const grown = ok(k.offsetShape(buildR1(k).shape, 1));
    const p = ok(k.properties(grown.shape));
    expect(p.valid).toBe(true);
    // Steiner formula: V + S r + (pi/4) * (sum of edge lengths) r^2 + 4/3 pi r^3.
    const volume = 24000 + 5200 + Math.PI * (40 + 30 + 20) + (4 / 3) * Math.PI;
    expect(relErr(p.volume, volume)).toBeLessThan(1e-6);
    expect(p.bbox[0]).toBeCloseTo(-1, 6);
    expect(p.bbox[5]).toBeCloseTo(21, 6);
    expect(grown.history.faces.generated.map((g) => g.from).sort()).toEqual([0, 1, 2, 3, 4, 5]);
  });
});

describe('draft', () => {
  it('tapers the four sides of R1 by 5 degrees about the bottom', () => {
    const { shape } = buildR1(k);
    const sides = ok(k.topology(shape))
      .faces.filter((f) => Math.abs(f.normal[2]) < 1e-9)
      .map((f) => f.index);
    expect(sides).toHaveLength(4);
    const angle = (5 * Math.PI) / 180;
    const drafted = ok(k.draft(shape, sides, [0, 0, 1], angle, { origin: [0, 0, 0], normal: [0, 0, 1] }));
    const p = ok(k.properties(drafted.shape));
    expect(p.valid).toBe(true);
    expect(p.faces).toBe(6);
    // Bottom 40 x 30 kept, every side moves in by t at the top: a prismatoid.
    const t = 20 * Math.tan(angle);
    const volume = (20 / 6) * (40 * 30 + (40 - 2 * t) * (30 - 2 * t) + 4 * (40 - t) * (30 - t));
    expect(relErr(p.volume, volume)).toBeLessThan(1e-6);
    expect(p.bbox[0]).toBeCloseTo(0, 6);
    // Tapered faces are new faces generated from the sides; top and bottom are trimmed.
    expect(drafted.history.faces.generated.map((g) => g.from).sort()).toEqual([...sides].sort());
    expect(drafted.history.faces.modified).toHaveLength(2);
  });

  it('names the face that cannot be tapered', () => {
    const { shape } = buildR1(k);
    const top = faceAtZ(shape, 20);
    const r = k.draft(shape, [top], [0, 0, 1], 0.1, { origin: [0, 0, 0], normal: [0, 0, 1] });
    expect(code(r)).toBe('DRAFT_FAILED');
    if (!r.ok) expect(r.error.index).toBe(top);
  });
});

describe('wires', () => {
  const path: Segment[] = [
    // Out of order and one of them reversed: makeWire joins them end to end.
    { kind: 'line', a: [40, 40], b: [10, 40] },
    { kind: 'line', a: [0, 0], b: [0, 30] },
    { kind: 'arc', center: [10, 30], radius: 10, start: [0, 30], end: [10, 40], ccw: false },
  ];

  it('joins sketch segments in any order into one open wire', () => {
    const w = ok(k.makeWire(XZ, path));
    expect(w.closed).toBe(false);
    expect([...w.segmentEdges].sort()).toEqual([0, 1, 2]);
    const length = ok(k.topology(w.shape)).edges.reduce((s, e) => s + e.length, 0);
    expect(length).toBeCloseTo(60 + 5 * Math.PI, 9);
  });

  it('names the gap of a broken path', () => {
    const r = k.makeWire(XZ, [path[1], { kind: 'line', a: [0, 31], b: [0, 50] }]);
    expect(code(r)).toBe('PATH_NOT_CONNECTED');
    if (!r.ok) expect(r.error.message).toMatch(/\(0, 0, 3[01]\)/);
  });

  it('joins body edges into a closed wire', () => {
    const { shape } = buildR1(k);
    const top = ok(k.topology(shape))
      .edges.filter((e) => Math.abs(e.midpoint[2] - 20) < 1e-9)
      .map((e) => e.index);
    const w = ok(k.joinEdges([{ shape, edges: top }]));
    expect(w.closed).toBe(true);
    expect(w.edges[0]).toHaveLength(4);
    expect(new Set(w.edges[0]).size).toBe(4);
  });
});

describe('sweep', () => {
  it('sweeps a circle along a straight line into a cylinder', () => {
    const profile = ok(k.makeProfileFace(XY, [circleLoop(0, 0, 2)]));
    const spine = ok(k.makeWire(XZ, [{ kind: 'line', a: [0, 0], b: [0, 50] }]));
    for (const orientation of ['perpendicular', 'parallel'] as const) {
      const s = ok(k.sweep(profile.shape, spine.shape, { orientation }));
      const p = ok(k.properties(s.shape));
      expect(p.valid).toBe(true);
      expect(relErr(p.volume, Math.PI * 4 * 50)).toBeLessThan(1e-6);
      expect(s.sweep.bottom).toHaveLength(1);
      expect(s.sweep.top).toHaveLength(1);
      expect(s.sweep.sides[0].faces).toHaveLength(1);
    }
  });

  it('sweeps a circle along a line, an arc and a line (Pappus)', () => {
    const profile = ok(k.makeProfileFace(XY, [circleLoop(0, 0, 2)]));
    const spine = ok(
      k.makeWire(XZ, [
        { kind: 'line', a: [0, 0], b: [0, 30] },
        { kind: 'arc', center: [10, 30], radius: 10, start: [0, 30], end: [10, 40], ccw: false },
        { kind: 'line', a: [10, 40], b: [40, 40] },
      ]),
    );
    const s = ok(k.sweep(profile.shape, spine.shape));
    const p = ok(k.properties(s.shape));
    expect(p.valid).toBe(true);
    expect(relErr(p.volume, Math.PI * 4 * (60 + 5 * Math.PI))).toBeLessThan(1e-5);
    expect(p.bbox[3]).toBeCloseTo(40, 4);
    expect(p.bbox[5]).toBeCloseTo(42, 4);
  });

  it('sweeps along a path with a sharp corner with a mitred joint', () => {
    const disc = ok(k.makeProfileFace(XY, [circleLoop(0, 0, 3)])).shape;
    // Up 30, then along X 30: the mitre keeps pi r^2 times the centerline length.
    const corner = ok(
      k.makeWire(XZ, [
        { kind: 'line', a: [0, 0], b: [0, 30] },
        { kind: 'line', a: [0, 30], b: [30, 30] },
      ]),
    ).shape;
    const s = ok(k.sweep(disc, corner));
    const p = ok(k.properties(s.shape));
    expect(p.valid).toBe(true);
    expect(relErr(p.volume, Math.PI * 9 * 60)).toBeLessThan(1e-6);
    expect(p.bbox[3]).toBeCloseTo(30, 6);
    expect(p.bbox[5]).toBeCloseTo(33, 6);
    expect([s.sweep.bottom.length, s.sweep.top.length]).toEqual([1, 1]);
    expect(s.sweep.sides[0].faces).toHaveLength(2);

    // A profile with holes sweeps only along smooth paths.
    const ring = ok(k.makeProfileFace(XY, [circleLoop(0, 0, 3), circleLoop(0, 0, 2)])).shape;
    const r = k.sweep(ring, corner);
    expect(code(r)).toBe('SWEEP_FAILED');
  });

  it('rejects a spine that is not a wire', () => {
    const profile = ok(k.makeProfileFace(XY, [circleLoop(0, 0, 2)])).shape;
    expect(code(k.sweep(profile, buildR1(k).shape))).toBe('INVALID_ARGUMENT');
    const spine = ok(k.makeWire(XZ, [{ kind: 'line', a: [0, 0], b: [0, 5] }])).shape;
    expect(code(k.sweep(profile, spine, { orientation: 'sideways' as never }))).toBe('INVALID_ARGUMENT');
  });
});

describe('loft', () => {
  const at = (z: number) => ({ origin: [0, 0, z], normal: [0, 0, 1], xDir: [1, 0, 0] });

  it('lofts a square into a smaller one (frustum of a pyramid)', () => {
    const a = ok(k.makeProfileFace(at(0), [rectLoop(-10, -10, 20, 20)])).shape;
    const b = ok(k.makeProfileFace(at(30), [rectLoop(-5, -5, 10, 10)])).shape;
    for (const ruled of [true, false]) {
      const l = ok(k.loft([a, b], { ruled }));
      const p = ok(k.properties(l.shape));
      expect(p.valid).toBe(true);
      expect(p.faces).toBe(6);
      expect(relErr(p.volume, (30 / 3) * (400 + 100 + 200))).toBeLessThan(1e-6);
      expect(l.loft.first).toHaveLength(1);
      expect(l.loft.last).toHaveLength(1);
      expect(l.loft.sections).toHaveLength(2);
      expect(l.loft.sections[0].every((s) => s.faces.length === 1)).toBe(true);
    }
  });

  it('lofts circles into a cone frustum and a square into a point', () => {
    const c1 = ok(k.makeProfileFace(at(0), [circleLoop(0, 0, 10)])).shape;
    const c2 = ok(k.makeProfileFace(at(30), [circleLoop(0, 0, 5)])).shape;
    const cone = ok(k.properties(ok(k.loft([c1, c2], { ruled: true })).shape));
    expect(relErr(cone.volume, Math.PI * 10 * (100 + 50 + 25))).toBeLessThan(1e-6);

    const base = ok(k.makeProfileFace(at(0), [rectLoop(0, 0, 20, 20)])).shape;
    const tip = ok(k.makeVertex([10, 10, 30])).shape;
    const pyramid = ok(k.properties(ok(k.loft([base, tip], { ruled: true })).shape));
    expect(pyramid.valid).toBe(true);
    expect(relErr(pyramid.volume, (400 * 30) / 3)).toBeLessThan(1e-6);
  });

  it('lofts from the top face of a body', () => {
    const { shape } = buildR1(k);
    const top = ok(k.subShape(shape, 'face', faceAtZ(shape, 20))).shape;
    expect(ok(k.properties(top)).shapeType).toBe('face');
    const b = ok(k.makeProfileFace(at(50), [rectLoop(10, 5, 20, 20)])).shape;
    const p = ok(k.properties(ok(k.loft([top, b], { ruled: true })).shape));
    expect(p.valid).toBe(true);
    expect(p.bbox[2]).toBeCloseTo(20, 6);
    expect(p.bbox[5]).toBeCloseTo(50, 6);
    expect(code(k.subShape(shape, 'face', 6))).toBe('INVALID_ARGUMENT');
    expect(code(k.subShape(shape, 'thing' as never, 0))).toBe('INVALID_ARGUMENT');
  });

  it('gives a shell when asked and refuses holes', () => {
    const a = ok(k.makeProfileFace(at(0), [rectLoop(0, 0, 20, 20)])).shape;
    const b = ok(k.makeProfileFace(at(10), [rectLoop(0, 0, 20, 20)])).shape;
    const open = ok(k.properties(ok(k.loft([a, b], { solid: false })).shape));
    expect(open.solids).toBe(0);
    expect(open.faces).toBe(4);
    const holed = ok(k.makeProfileFace(at(0), [rectLoop(0, 0, 20, 20), circleLoop(10, 10, 3)])).shape;
    const r = k.loft([holed, b]);
    expect(code(r)).toBe('LOFT_FAILED');
    if (!r.ok) expect(r.error.index).toBe(0);
    expect(code(k.loft([a]))).toBe('INVALID_ARGUMENT');
  });
});

describe('importStepParts', () => {
  const I = (x: number, y: number, z: number) => [1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z];

  it('reads an assembly part by part with names, paths, colors and placements', () => {
    const plate = ok(k.makeBox(40, 30, 5)).shape;
    const pin = ok(k.makeCylinder([0, 0, 0], [0, 0, 1], 2, 10)).shape;
    const bytes = ok(
      k.exportStepAssembly({
        name: 'Fixture',
        parts: [
          { name: 'Plate', shapes: [plate], color: [0.8, 0.2, 0.1] },
          { name: 'Pin Ø4', shapes: [pin] },
        ],
        instances: [
          { part: 0, name: 'Plate:1', matrix: I(0, 0, 0) },
          { part: 1, name: 'Pin Ø4:1', matrix: I(10, 10, 5) },
          { part: 1, name: 'Pin Ø4:2', matrix: I(30, 20, 5) },
        ],
      }),
    ).bytes;
    const r = ok(k.importStepParts(bytes));
    expect(r).toMatchObject({ solids: 3, open: 0, healed: 0 });
    expect(r.units).toEqual(['millimetre']);
    const byName = Object.fromEntries(r.parts.map((p) => [p.name, p]));
    expect(Object.keys(byName).sort()).toEqual(['Pin Ø4:1', 'Pin Ø4:2', 'Plate:1']);
    expect(byName['Plate:1']).toMatchObject({ type: 'solid', valid: true, product: 'Plate', path: ['Fixture'] });
    expect(byName['Pin Ø4:2'].product).toBe('Pin Ø4');
    const color = byName['Plate:1'].color!;
    [0.8, 0.2, 0.1].forEach((c, i) => expect(color[i]).toBeCloseTo(c, 2));
    expect(byName['Pin Ø4:1'].color).toBeUndefined();
    const box = ok(k.properties(byName['Pin Ø4:2'].shape)).bbox;
    expect(box[0]).toBeCloseTo(28, 6);
    expect(box[5]).toBeCloseTo(15, 6);
  });

  it('converts inches to millimeters', () => {
    const text = new TextDecoder().decode(ok(k.exportStep([ok(k.makeBox(1, 2, 3)).shape])).bytes);
    const mm = /#(\d+)\s*=\s*\(\s*LENGTH_UNIT\(\)\s*NAMED_UNIT\(\*\)\s*SI_UNIT\(\.MILLI\.,\.METRE\.\)\s*\)/.exec(text)!;
    expect(mm).not.toBeNull();
    const inch = text
      .replace(/GLOBAL_UNIT_ASSIGNED_CONTEXT\s*\(\(#(\d+),/, (s, id) => (id === mm[1] ? s.replace(`#${id}`, '#900001') : s))
      .replace(
        /ENDSEC;\s*END-ISO-10303-21;/,
        `#900001 = ( CONVERSION_BASED_UNIT('INCH',#900002) LENGTH_UNIT() NAMED_UNIT(#900003) );\n` +
          `#900002 = LENGTH_MEASURE_WITH_UNIT(LENGTH_MEASURE(25.4),#${mm[1]});\n` +
          `#900003 = DIMENSIONAL_EXPONENTS(1.,0.,0.,0.,0.,0.,0.);\nENDSEC;\nEND-ISO-10303-21;`,
      );
    expect(inch).toContain('#900001,');
    const r = ok(k.importStepParts(new TextEncoder().encode(inch)));
    expect(r.units).toEqual(['inch']);
    const p = ok(k.properties(r.parts[0].shape));
    expect(relErr(p.volume, 6 * 25.4 ** 3)).toBeLessThan(1e-6);
    expect(p.bbox[5]).toBeCloseTo(76.2, 6);
  });

  it('sews a closed surface model into a solid unless healing is off', () => {
    const text = new TextDecoder().decode(ok(k.exportStep([buildR3(k).shape])).bytes);
    const surfaces = text
      .replace(/MANIFOLD_SOLID_BREP\('([^']*)',(#\d+)\)/, "SHELL_BASED_SURFACE_MODEL('$1',($2))")
      .replace(/ADVANCED_BREP_SHAPE_REPRESENTATION/g, 'MANIFOLD_SURFACE_SHAPE_REPRESENTATION');
    expect(surfaces).toContain('SHELL_BASED_SURFACE_MODEL');
    const bytes = new TextEncoder().encode(surfaces);

    const raw = ok(k.importStepParts(bytes, { heal: false }));
    expect(raw).toMatchObject({ solids: 0, open: 1, healed: 0 });
    expect(raw.parts[0].type).toBe('shell');

    const healed = ok(k.importStepParts(bytes));
    expect(healed).toMatchObject({ solids: 1, open: 0, healed: 1 });
    expect(healed.parts[0]).toMatchObject({ type: 'solid', healed: true, valid: true });
    expect(relErr(ok(k.properties(healed.parts[0].shape)).volume, ok(k.properties(buildR3(k).shape)).volume)).toBeLessThan(
      1e-6,
    );
  });

  it('reports an open face apart and rejects garbage', () => {
    const face = ok(k.makeProfileFace(XY, [rectLoop(0, 0, 10, 10)])).shape;
    const r = ok(k.importStepParts(ok(k.exportStep([face])).bytes));
    expect(r.solids).toBe(0);
    expect(r.open).toBe(1);
    expect(r.parts[0].type).not.toBe('solid');
    expect(code(k.importStepParts(new TextEncoder().encode('not a step file')))).toBe('STEP_READ_FAILED');
  });
});

describe('STEP assembly structure (0.8.0)', () => {
  type M = number[];
  const I: M = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0];
  const T = (x: number, y: number, z: number): M => [1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z];
  /** a after b: the placement of b inside a, both row-major 3x4. */
  const mul = (a: M, b: M): M => {
    const r: M = [];
    for (let i = 0; i < 3; i++) {
      for (let j = 0; j < 4; j++) {
        let v = 0;
        for (let l = 0; l < 3; l++) v += a[i * 4 + l] * b[l * 4 + j];
        r.push(j === 3 ? v + a[i * 4 + 3] : v);
      }
    }
    return r;
  };
  const close = (a: readonly number[] | undefined, b: readonly number[], tol: number) => {
    expect(a).toHaveLength(b.length);
    a!.forEach((v, i) => expect(Math.abs(v - b[i])).toBeLessThan(tol));
  };
  const near = (a: readonly number[] | undefined, b: readonly number[]) =>
    expect(a !== undefined && a.every((v, i) => Math.abs(v - b[i]) < 2e-3)).toBe(true);
  const red: [number, number, number] = [0.8, 0.1, 0.1];
  const blue: [number, number, number] = [0.1, 0.2, 0.9];
  const green: [number, number, number] = [0.1, 0.7, 0.2];

  it('round-trips a sub-assembly placed twice, part by part in product coordinates', () => {
    const base = ok(k.makeBox(40, 30, 5)).shape;
    const pin = ok(k.makeCylinder([0, 0, 0], [0, 0, 1], 2, 10)).shape;
    const washer = ok(k.makeBox(6, 4, 1)).shape;
    const pinFace = ok(k.topology(pin)).faces[1];
    const pinAt = T(1, 2, 3);
    const washerAt = [0, -1, 0, 5, 1, 0, 0, 0, 0, 0, 1, 0]; // a quarter turn about Z
    const sub1 = T(10, 10, 5);
    const sub2 = [1, 0, 0, 30, 0, 0, -1, 20, 0, 1, 0, 5]; // a quarter turn about X
    const bytes = ok(
      k.exportStepAssembly({
        name: 'Top',
        parts: [
          { name: 'Base', shapes: [base] },
          { name: 'Pin', shapes: [pin], faceColors: [{ shape: 0, faces: [1], color: red }] },
          { name: 'Washer', shapes: [washer] },
        ],
        assemblies: [
          {
            name: 'Sub',
            instances: [
              { part: 1, name: 'Pin:1', matrix: pinAt },
              { part: 2, name: 'Washer:1', matrix: washerAt, color: blue },
            ],
          },
        ],
        instances: [
          { part: 0, name: 'Base:1', matrix: I },
          { assembly: 0, name: 'Sub:1', matrix: sub1 },
          { assembly: 0, name: 'Sub:2', matrix: sub2, color: green },
        ],
      }),
    ).bytes;
    const text = new TextDecoder().decode(bytes);
    // Root, Sub, Base, Pin, Washer; Base:1, Sub:1, Sub:2, Pin:1, Washer:1.
    expect(text.match(/=\s*PRODUCT\(/g)).toHaveLength(5);
    expect(text.match(/NEXT_ASSEMBLY_USAGE_OCCURRENCE/g)).toHaveLength(5);

    const placed = ok(k.importStepParts(bytes));
    const local = ok(k.importStepParts(bytes, { local: true }));
    expect(placed).toMatchObject({ solids: 5, open: 0, healed: 0 });
    expect(local).toMatchObject({ solids: 5, open: 0, healed: 0 });
    const key = (p: { instances: { name: string }[] }) => p.instances.map((i) => i.name).join('/');
    const keys = ['Base:1', 'Sub:1/Pin:1', 'Sub:1/Washer:1', 'Sub:2/Pin:1', 'Sub:2/Washer:1'];
    const P = Object.fromEntries(placed.parts.map((p) => [key(p), p]));
    const L = Object.fromEntries(local.parts.map((p) => [key(p), p]));
    expect(Object.keys(P).sort()).toEqual(keys);
    expect(Object.keys(L).sort()).toEqual(keys);

    // Names along the chains, three levels: the root, Sub, the part.
    expect(L['Base:1'].instances.map((i) => i.productName)).toEqual(['Base']);
    expect(L['Sub:2/Pin:1'].instances.map((i) => i.productName)).toEqual(['Sub', 'Pin']);
    expect(L['Sub:1/Washer:1'].instances.map((i) => i.productName)).toEqual(['Sub', 'Washer']);
    expect(L['Sub:2/Washer:1']).toMatchObject({ name: 'Washer:1', product: 'Washer', path: ['Top', 'Sub:2'] });
    for (const p of local.parts) {
      expect(p.root.name).toBe('Top');
      expect(p.root.id).toBe(local.parts[0].root.id);
      expect(p.productId).toBe(p.instances.at(-1)!.product);
      expect(p.productId).not.toBe(p.root.id);
    }
    // Both instances of Sub place the same product; each part's product is shared by its instances.
    const sub = L['Sub:1/Pin:1'].instances[0];
    expect(L['Sub:2/Washer:1'].instances[0].product).toBe(sub.product);
    expect(L['Sub:1/Washer:1'].instances[0].id).toBe(sub.id);
    expect(L['Sub:2/Pin:1'].instances[0].id).not.toBe(sub.id);
    expect(L['Sub:2/Pin:1'].productId).toBe(L['Sub:1/Pin:1'].productId);
    expect(L['Sub:2/Washer:1'].productId).toBe(L['Sub:1/Washer:1'].productId);
    expect(new Set([sub.product, L['Base:1'].productId, L['Sub:1/Pin:1'].productId, L['Sub:1/Washer:1'].productId]).size).toBe(4);

    // Placements: the chain composed is `matrix`, which is what was written.
    const expected: Record<string, M> = {
      'Base:1': I,
      'Sub:1/Pin:1': mul(sub1, pinAt),
      'Sub:1/Washer:1': mul(sub1, washerAt),
      'Sub:2/Pin:1': mul(sub2, pinAt),
      'Sub:2/Washer:1': mul(sub2, washerAt),
    };
    const volumes: Record<string, number> = {
      Base: ok(k.properties(base)).volume,
      Pin: ok(k.properties(pin)).volume,
      Washer: ok(k.properties(washer)).volume,
    };
    for (const name of keys) {
      const l = L[name];
      const p = P[name];
      close(l.instances.map((i) => i.matrix).reduce(mul), l.matrix, 1e-9);
      close(l.matrix, expected[name], 1e-9);
      close(p.matrix, l.matrix, 1e-12);
      expect(p.productId).toBe(l.productId);
      expect(p.instances.map((i) => i.id)).toEqual(l.instances.map((i) => i.id));
      // `matrix` moves the local shape onto the placed one.
      const moved = ok(k.properties(ok(k.transform(l.shape, l.matrix)).shape));
      const there = ok(k.properties(p.shape));
      close(moved.bbox, there.bbox, 1e-6);
      close(moved.centroid, there.centroid, 1e-6);
      expect(relErr(ok(k.properties(l.shape)).volume, volumes[l.product])).toBeLessThan(1e-6);
    }
    // Local shapes are where their product has them.
    close(ok(k.properties(L['Sub:2/Washer:1'].shape)).bbox, [0, 0, 0, 6, 4, 1], 1e-6);
    close(ok(k.properties(P['Sub:1/Washer:1'].shape)).bbox, [11, 10, 5, 15, 16, 6], 1e-6);

    // Colours in both modes: the washer's instance colour, the pin's face
    // colour, and Sub:2's instance colour on the pin under it.
    for (const parts of [P, L]) {
      near(parts['Sub:1/Washer:1'].color, blue);
      near(parts['Sub:2/Washer:1'].color, blue);
      expect(parts['Sub:1/Pin:1'].color).toBeUndefined();
      near(parts['Sub:2/Pin:1'].color, green);
      expect(parts['Base:1'].color).toBeUndefined();
      for (const pinName of ['Sub:1/Pin:1', 'Sub:2/Pin:1']) {
        const faces = parts[pinName].faceColors!;
        expect(faces).toHaveLength(1);
        near(faces[0].color, red);
        expect(ok(k.topology(parts[pinName].shape)).faces[faces[0].face].area).toBeCloseTo(pinFace.area, 6);
      }
    }
  });

  it('places assemblies listed after the one that uses them', () => {
    const box = ok(k.makeBox(2, 3, 4)).shape;
    const bytes = ok(
      k.exportStepAssembly({
        name: 'Root',
        parts: [{ name: 'Box', shapes: [box] }],
        assemblies: [
          { name: 'Outer', instances: [{ assembly: 1, name: 'Inner:1', matrix: T(0, 10, 0) }] },
          { name: 'Inner', instances: [{ part: 0, name: 'Box:1', matrix: T(0, 0, 100) }] },
        ],
        instances: [{ assembly: 0, name: 'Outer:1', matrix: T(1000, 0, 0) }],
      }),
    ).bytes;
    const parts = ok(k.importStepParts(bytes, { local: true })).parts;
    expect(parts).toHaveLength(1);
    expect(parts[0].instances.map((i) => i.name)).toEqual(['Outer:1', 'Inner:1', 'Box:1']);
    expect(parts[0].instances.map((i) => i.productName)).toEqual(['Outer', 'Inner', 'Box']);
    close(parts[0].matrix, T(1000, 10, 100), 1e-9);
    close(ok(k.properties(parts[0].shape)).bbox, [0, 0, 0, 2, 3, 4], 1e-6);
  });

  it('gives flat assemblies chains of one and free shapes none', () => {
    const plate = ok(k.makeBox(40, 30, 5)).shape;
    const pin = ok(k.makeCylinder([0, 0, 0], [0, 0, 1], 2, 10)).shape;
    const bytes = ok(
      k.exportStepAssembly({
        name: 'Fixture',
        parts: [
          { name: 'Plate', shapes: [plate] },
          { name: 'Pin', shapes: [pin] },
        ],
        instances: [
          { part: 0, name: 'Plate:1', matrix: I },
          { part: 1, name: 'Pin:1', matrix: T(10, 10, 5) },
          { part: 1, name: 'Pin:2', matrix: T(30, 20, 5) },
        ],
      }),
    ).bytes;
    for (const local of [false, true]) {
      const parts = ok(k.importStepParts(bytes, { local })).parts;
      expect(parts).toHaveLength(3);
      for (const p of parts) {
        expect(p.instances).toHaveLength(1);
        expect(p.root.name).toBe('Fixture');
        expect(p.instances[0].product).toBe(p.productId);
        close(p.instances[0].matrix, p.matrix, 1e-12);
      }
      const pins = parts.filter((p) => p.product === 'Pin');
      expect(pins.map((p) => p.instances[0].name).sort()).toEqual(['Pin:1', 'Pin:2']);
      expect(pins[0].productId).toBe(pins[1].productId);
      const one = pins.find((p) => p.instances[0].name === 'Pin:1')!;
      close(one.matrix, T(10, 10, 5), 1e-9);
      const box = ok(k.properties(one.shape)).bbox;
      close(box, local ? [-2, -2, 0, 2, 2, 10] : [8, 8, 5, 12, 12, 15], 1e-6);
    }
    // Default import: as before, in place.
    const placed = ok(k.importStepParts(bytes)).parts.find((p) => p.name === 'Pin:1')!;
    close(ok(k.properties(placed.shape)).bbox, [8, 8, 5, 12, 12, 15], 1e-6);

    const free = ok(k.importStepParts(ok(k.exportStep([ok(k.makeBox(1, 2, 3)).shape], { name: 'Block' })).bytes)).parts;
    expect(free).toHaveLength(1);
    expect(free[0].instances).toEqual([]);
    expect(free[0].root).toEqual({ id: free[0].productId, name: 'Block' });
    close(free[0].matrix, I, 1e-12);
  });

  it('rejects bad nested assemblies', () => {
    const box = ok(k.makeBox(1, 1, 1)).shape;
    const parts = [{ shapes: [box] }];
    const leaf = { name: 'Leaf', instances: [{ part: 0, matrix: I }] };
    const bad = (assembly: Parameters<OcctKernel['exportStepAssembly']>[0]) => code(k.exportStepAssembly(assembly));
    // Both, neither, out of range.
    expect(bad({ parts, assemblies: [leaf], instances: [{ part: 0, assembly: 0, matrix: I }] })).toBe('INVALID_ARGUMENT');
    expect(bad({ parts, assemblies: [leaf], instances: [{ matrix: I }] })).toBe('INVALID_ARGUMENT');
    expect(bad({ parts, assemblies: [leaf], instances: [{ assembly: 1, matrix: I }] })).toBe('INVALID_ARGUMENT');
    expect(bad({ parts, instances: [{ assembly: 0, matrix: I }] })).toBe('INVALID_ARGUMENT');
    expect(bad({ parts, assemblies: [{ instances: [{ part: 1, matrix: I }] }], instances: [{ assembly: 0, matrix: I }] })).toBe(
      'INVALID_ARGUMENT',
    );
    expect(bad({ parts, assemblies: [{ instances: [{ matrix: I }] }], instances: [{ assembly: 0, matrix: I }] })).toBe(
      'INVALID_ARGUMENT',
    );
    // Cycles, direct and through another assembly.
    const self = k.exportStepAssembly({
      parts,
      assemblies: [{ instances: [{ part: 0, matrix: I }, { assembly: 0, matrix: T(5, 0, 0) }] }],
      instances: [{ assembly: 0, matrix: I }],
    });
    expect(code(self)).toBe('INVALID_ARGUMENT');
    if (!self.ok) expect(self.error.message).toBe('assemblies contain themselves');
    expect(
      bad({
        parts,
        assemblies: [{ instances: [{ assembly: 1, matrix: I }] }, { instances: [{ assembly: 0, matrix: I }, { part: 0, matrix: I }] }],
        instances: [{ part: 0, matrix: I }],
      }),
    ).toBe('INVALID_ARGUMENT');
    // An empty assembly, and one that is not an array.
    expect(bad({ parts, assemblies: [leaf, { instances: [] }], instances: [{ assembly: 0, matrix: I }] })).toBe('INVALID_ARGUMENT');
    expect(bad({ parts, assemblies: {} as never, instances: [{ part: 0, matrix: I }] })).toBe('INVALID_ARGUMENT');
    // A valid nested input still writes.
    expect(bad({ parts, assemblies: [leaf], instances: [{ assembly: 0, matrix: I }] })).toBe('OK');
  });
});

describe('helix', () => {
  const R = 10;
  const pitch = 4;
  const turns = 5;
  const length = turns * Math.hypot(2 * Math.PI * R, pitch);

  it('builds a helix of the right length that ends above its start', () => {
    const h = ok(k.helix([0, 0, 0], [0, 0, 1], R, pitch, turns * pitch));
    const t = ok(k.topology(h.shape));
    // One edge per turn (0.10.1), equal pieces, joined end to end.
    expect(t.edges).toHaveLength(turns);
    for (const e of t.edges) expect(relErr(e.length, length / turns)).toBeLessThan(1e-6);
    expect(t.vertices).toHaveLength(turns + 1);
    const ends = [...t.vertices.map((v) => v.point)].sort((p, q) => p[2] - q[2]);
    const [a, b] = [ends[0], ends[ends.length - 1]];
    expect(a[0]).toBeCloseTo(R, 6);
    expect(a[1]).toBeCloseTo(0, 6);
    expect(a[2]).toBeCloseTo(0, 6);
    expect(b[0]).toBeCloseTo(R, 6);
    expect(b[1]).toBeCloseTo(0, 6);
    expect(b[2]).toBeCloseTo(turns * pitch, 6);
    // Right-handed about +Z: a quarter turn in, it is on +Y.
    const quarter = ok(k.helix([0, 0, 0], [0, 0, 1], R, pitch, pitch / 4));
    const end = ok(k.topology(quarter.shape)).vertices.map((v) => v.point).find((p) => p[2] > 0.5);
    expect(end?.[1]).toBeCloseTo(R, 6);
    const left = ok(k.helix([0, 0, 0], [0, 0, 1], R, pitch, pitch / 4, { leftHanded: true }));
    const leftEnd = ok(k.topology(left.shape)).vertices.map((v) => v.point).find((p) => p[2] > 0.5);
    expect(leftEnd?.[1]).toBeCloseTo(-R, 6);
  });

  it('widens on a cone and starts on xDir', () => {
    const taper = (10 * Math.PI) / 180;
    const height = 20;
    const h = ok(k.helix([0, 0, 0], [0, 0, 1], R, pitch, height, { taper, xDir: [0, 1, 0] }));
    const points = ok(k.topology(h.shape)).vertices.map((v) => v.point);
    const top = points.reduce((p, q) => (q[2] > p[2] ? q : p));
    const bottom = points.reduce((p, q) => (q[2] < p[2] ? q : p));
    expect(top[2]).toBeCloseTo(height, 6);
    expect(bottom[1]).toBeCloseTo(R, 6);
    expect(Math.hypot(top[0], top[1])).toBeCloseTo(R + height * Math.tan(taper), 6);
  });

  it('sweeps a coil with the binormal along the axis (volume = area x length)', () => {
    const a = 1;
    const tangent = [0, 2 * Math.PI * R, pitch].map((v) => v / Math.hypot(2 * Math.PI * R, pitch));
    const plane = { origin: [R, 0, 0], normal: tangent, xDir: [1, 0, 0] };
    const profile = ok(k.makeProfileFace(plane, [circleLoop(0, 0, a)]));
    const spine = ok(k.helix([0, 0, 0], [0, 0, 1], R, pitch, turns * pitch));
    const s = ok(k.sweep(profile.shape, spine.shape, { orientation: 'binormal', binormal: [0, 0, 1] }));
    const p = ok(k.properties(s.shape));
    expect(p.valid).toBe(true);
    expect(relErr(p.volume, Math.PI * a * a * length)).toBeLessThan(1e-4);
    expect(p.bbox[5]).toBeCloseTo(turns * pitch + a * Math.cos(Math.atan2(pitch, 2 * Math.PI * R)), 3);
  });

  it('rejects bad helices', () => {
    const code = (r: { ok: boolean; error?: { code: string } }) => (r.ok ? 'OK' : r.error?.code);
    expect(code(k.helix([0, 0, 0], [0, 0, 1], 0, 1, 1))).toBe('INVALID_ARGUMENT');
    expect(code(k.helix([0, 0, 0], [0, 0, 1], 1, 1, 1, { taper: 2 }))).toBe('INVALID_ARGUMENT');
    expect(code(k.helix([0, 0, 0], [0, 0, 1], 1, 1, 1, { xDir: [0, 0, 1] }))).toBe('INVALID_ARGUMENT');
    expect(code(k.helix([0, 0, 0], [0, 0, 1], 1, 1, 10, { taper: -0.5 }))).toBe('INVALID_ARGUMENT');
  });
});
