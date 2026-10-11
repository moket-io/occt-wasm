// Golden fixtures for the Moket reference models (docs/specs/kernel-spike.md).
import { beforeAll, describe, expect, it } from 'vitest';
import type { OcctKernel, PropertiesResult, ShapeId } from '../dist/index.js';
import golden from './fixtures/golden.json' with { type: 'json' };
import { loadKernel, relErr } from './kernel.js';
import { buildR1, buildR2, buildR3, ok, R1_VOLUME, R2_VOLUME, R3_VOLUME } from './models.mjs';

type Golden = (typeof golden)['R1'];
type Props = Extract<PropertiesResult, { ok: true }>;

const VOLUME_TOL = 1e-6; // relative
const BBOX_TOL = 1e-6; // mm

let k: OcctKernel;
beforeAll(async () => {
  k = await loadKernel();
});

function expectMatches(props: Props, g: Golden) {
  expect(props.valid).toBe(true);
  expect(relErr(props.volume, g.volume)).toBeLessThan(VOLUME_TOL);
  props.bbox.forEach((v, i) => expect(Math.abs(v - g.bbox[i])).toBeLessThan(BBOX_TOL));
  expect(props.solids).toBe(g.solids);
  expect(props.faces).toBe(g.faces);
  expect(props.edges).toBe(g.edges);
  expect(props.vertices).toBe(g.vertices);
}

describe('R1 box 40 x 30 x 20 from a rectangle sketch + extrude', () => {
  it('matches the golden fixture', () => {
    expect(golden.R1.volume).toBe(R1_VOLUME);
    const { shape } = buildR1(k);
    const props = ok(k.properties(shape));
    expect(props.shapeType).toBe('solid');
    expectMatches(props, golden.R1);
    expect(relErr(props.area, 2 * (40 * 30 + 40 * 20 + 30 * 20))).toBeLessThan(1e-9);
    props.centroid.forEach((v, i) => expect(Math.abs(v - [20, 15, 10][i])).toBeLessThan(1e-9));
  });
});

describe('R2 plate 100 x 60 x 10 with four through holes d6', () => {
  it('matches the golden fixture', () => {
    expect(relErr(golden.R2.volume, R2_VOLUME)).toBeLessThan(1e-12);
    const { shape } = buildR2(k);
    const props = ok(k.properties(shape));
    expectMatches(props, golden.R2);
    const topo = ok(k.topology(shape));
    expect(topo.faces.filter((f) => f.surface === 'cylinder')).toHaveLength(4);
    expect(topo.faces.filter((f) => f.surface === 'plane')).toHaveLength(6);
  });

  it('gives the same plate when the holes are cut with booleans', () => {
    let plate = ok(k.makeBox(100, 60, 10)).shape;
    for (const [x, y] of [
      [10, 10],
      [90, 10],
      [90, 50],
      [10, 50],
    ]) {
      const tool = ok(k.makeCylinder([x, y, -1], [0, 0, 1], 3, 12)).shape;
      plate = ok(k.boolean('cut', plate, tool)).shape;
    }
    const props = ok(k.properties(plate));
    expectMatches(props, golden.R2);
  });
});

describe('R3 R1 + fillet R3 on the four top edges', () => {
  it('matches the golden fixture: 6 planes + 4 fillet cylinders, no corner blends', () => {
    expect(relErr(golden.R3.volume, R3_VOLUME)).toBeLessThan(1e-12);
    const { shape, topEdges } = buildR3(k);
    expect(topEdges).toHaveLength(4);
    const props = ok(k.properties(shape));
    expect(props.shapeType).toBe('solid');
    expectMatches(props, golden.R3);
    const topo = ok(k.topology(shape));
    const bySurface: Record<string, number> = {};
    for (const f of topo.faces) bySurface[f.surface] = (bySurface[f.surface] ?? 0) + 1;
    expect(bySurface).toEqual(golden.R3.surfaces);
  });
});

describe('R6 STEP round-trip of R1-R3', () => {
  const cases: [string, (k: OcctKernel) => { shape: ShapeId }, Golden][] = [
    ['R1', buildR1, golden.R1],
    ['R2', buildR2, golden.R2],
    ['R3', buildR3, golden.R3],
  ];

  for (const [name, build, g] of cases) {
    it(`${name}: re-imported volume, bbox and topology counts match`, () => {
      const { shape } = build(k);
      const before = ok(k.properties(shape));
      const step = ok(k.exportStep([shape]));
      expect(step.bytes).toBeInstanceOf(Uint8Array);
      const text = new TextDecoder().decode(step.bytes.subarray(0, 64));
      expect(text.startsWith('ISO-10303-21;')).toBe(true);

      const back = ok(k.importStep(step.bytes));
      const after = ok(k.properties(back.shape));
      expectMatches(after, g);
      expect(relErr(after.volume, before.volume)).toBeLessThan(VOLUME_TOL);
      after.bbox.forEach((v, i) => expect(Math.abs(v - before.bbox[i])).toBeLessThan(BBOX_TOL));
      expect([after.faces, after.edges, after.vertices]).toEqual([before.faces, before.edges, before.vertices]);
    });
  }

  it('exports several shapes into one file', () => {
    const a = buildR1(k).shape;
    const b = ok(k.makeBox(5, 5, 5)).shape;
    const step = ok(k.exportStep([a, b]));
    const back = ok(k.importStep(step.bytes));
    const props = ok(k.properties(back.shape));
    expect(props.solids).toBe(2);
    expect(relErr(props.volume, 24000 + 125)).toBeLessThan(VOLUME_TOL);
  });
});
