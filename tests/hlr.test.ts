// Hidden line removal for drawings: project() on the reference models.
import { beforeAll, describe, expect, it } from 'vitest';
import type { OcctKernel, ProjectedCurve } from '../dist/index.js';
import { loadKernel } from './kernel.js';
import { buildR1, buildR2, buildR3, ok } from './models.mjs';

let k: OcctKernel;
beforeAll(async () => {
  k = await loadKernel();
});

const FRONT = { direction: [0, 1, 0] as [number, number, number], up: [0, 0, 1] as [number, number, number] };
const TOP = { direction: [0, 0, -1] as [number, number, number], up: [0, 1, 0] as [number, number, number] };
const ISO = { direction: [-1, 1, -1] as [number, number, number], up: [0, 0, 1] as [number, number, number] };

function length(c: ProjectedCurve): number {
  const d = (p: [number, number], q: [number, number]) => Math.hypot(q[0] - p[0], q[1] - p[1]);
  switch (c.type) {
    case 'line':
      return d(c.a, c.b);
    case 'circle':
      return 2 * Math.PI * c.radius;
    case 'arc': {
      const ang = (p: [number, number]) => Math.atan2(p[1] - c.center[1], p[0] - c.center[0]);
      const sweep = (from: number, to: number) => (((to - from) % (2 * Math.PI)) + 2 * Math.PI) % (2 * Math.PI);
      const ccw = sweep(ang(c.a), ang(c.m)) < sweep(ang(c.a), ang(c.b));
      const s = sweep(ang(c.a), ang(c.b));
      return c.radius * (ccw ? s : 2 * Math.PI - s);
    }
    case 'polyline': {
      let s = 0;
      for (let i = 2; i < c.points.length; i += 2)
        s += Math.hypot(c.points[i] - c.points[i - 2], c.points[i + 1] - c.points[i - 1]);
      return s;
    }
  }
}

const total = (cs: ProjectedCurve[]) => cs.reduce((s, c) => s + length(c), 0);

describe('project', () => {
  it('draws the front of R1 from its front edges', () => {
    const { shape } = buildR1(k);
    const r = ok(k.project(shape, { ...FRONT, hidden: true }));
    expect(r.xDir.map((v) => Math.round(v * 1e9) / 1e9)).toEqual([1, 0, 0]);
    const visible = r.curves.filter((c) => c.visible);
    expect(visible.every((c) => c.type === 'line' && c.kind === 'sharp')).toBe(true);
    expect(total(visible)).toBeCloseTo(2 * (40 + 20), 6);
    const topo = ok(k.topology(shape));
    for (const c of visible) {
      expect(c.edge).toBeGreaterThanOrEqual(0);
      expect(topo.edges[c.edge].midpoint[1]).toBeCloseTo(0, 9); // the front face, y = 0
    }
    const xs = visible.flatMap((c) => (c.type === 'line' ? [c.a[0], c.b[0]] : []));
    const ys = visible.flatMap((c) => (c.type === 'line' ? [c.a[1], c.b[1]] : []));
    expect([Math.min(...xs), Math.max(...xs)].map((v) => Math.round(v * 1e6) / 1e6)).toEqual([0, 40]);
    expect([Math.min(...ys), Math.max(...ys)].map((v) => Math.round(v * 1e6) / 1e6)).toEqual([0, 20]);
  });

  it('shows nine edges of a box in an isometric view and hides three', () => {
    const r = ok(k.project(buildR1(k).shape, { ...ISO, hidden: true }));
    const edges = (visible: boolean) => new Set(r.curves.filter((c) => c.visible === visible).map((c) => c.edge));
    expect(edges(true).size).toBe(9);
    expect(edges(false).size).toBe(3);
    expect([...edges(true)].some((e) => edges(false).has(e))).toBe(false);
  });

  it('keeps circles as circles and draws the holes hidden from the front', () => {
    const { shape } = buildR2(k);
    const top = ok(k.project(shape, TOP));
    const front = ok(k.project(shape, { ...FRONT, hidden: true }));
    const circles = top.curves.filter((c) => c.type === 'circle');
    expect(circles.length).toBeGreaterThanOrEqual(4);
    for (const c of circles) expect(c.type === 'circle' && c.radius).toBeCloseTo(3, 9);
    expect(front.curves.some((c) => c.type === 'polyline')).toBe(false); // circles seen edge on are lines
    const outline = top.curves.filter((c) => c.type === 'line');
    expect(total(outline)).toBeCloseTo(2 * (100 + 60), 6);

    const silhouettes = front.curves.filter((c) => !c.visible && c.kind === 'outline');
    // Each hole shows two hidden silhouette lines from its wall (one of them
    // is the wall's seam); the holes behind each other in this view share them.
    const topo = ok(k.topology(shape));
    const xs = new Set<number>();
    for (const c of silhouettes) {
      expect(c.edge).toBe(-1);
      expect(topo.faces[c.face].surface).toBe('cylinder');
      if (c.type !== 'line') throw new Error(`silhouette is a ${c.type}`);
      expect(c.a[0]).toBeCloseTo(c.b[0], 9);
      xs.add(Math.round(c.a[0] * 1e6) / 1e6);
    }
    expect([...xs].sort((a, b) => a - b)).toEqual([7, 13, 87, 93]);
  });

  it('marks the tangent edges of a fillet as smooth and stays fast', () => {
    const { shape } = buildR3(k);
    const plain = ok(k.project(shape, { ...ISO, hidden: true }));
    expect(plain.curves.some((c) => c.kind === 'smooth')).toBe(false);
    const r = ok(k.project(shape, { ...ISO, hidden: true, smooth: true }));
    const smooth = r.curves.filter((c) => c.kind === 'smooth');
    expect(smooth.length).toBeGreaterThan(0);
    expect(smooth.every((c) => c.edge >= 0)).toBe(true);
    expect(r.ms).toBeLessThan(1000);
  });

  it('hides one shape behind another and tells them apart', () => {
    const front = ok(k.makeBox(10, 10, 10)).shape;
    const back = ok(k.makeBox(30, 30, 30)).shape; // from y = 0 to 30, behind and around
    const moved = ok(k.transform(back, [1, 0, 0, -10, 0, 1, 0, 20, 0, 0, 1, -10])).shape;
    const r = ok(k.project([front, moved], { ...FRONT, hidden: true }));
    const of = (shape: number, visible: boolean) => r.curves.filter((c) => c.shape === shape && c.visible === visible);
    expect(total(of(0, true))).toBeCloseTo(40, 6); // the small box in front, all visible
    expect(of(1, true).length).toBeGreaterThan(0);
    expect(r.curves.every((c) => c.shape === 0 || c.shape === 1)).toBe(true);
    // A single id still works.
    expect(ok(k.project(front, FRONT)).curves.every((c) => c.shape === 0)).toBe(true);
  });

  it('refuses a view without a direction or with up along it', () => {
    const { shape } = buildR1(k);
    expect(k.project(shape, { direction: [0, 0, 1], up: [0, 0, 2] })).toMatchObject({
      ok: false,
      error: { code: 'INVALID_ARGUMENT' },
    });
    expect(k.project(shape, undefined as never)).toMatchObject({ ok: false, error: { code: 'INVALID_ARGUMENT' } });
  });
});
