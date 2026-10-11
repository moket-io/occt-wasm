// API surface: queries, mesh, other operations, exchange, errors, memory.
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { beforeAll, describe, expect, it } from 'vitest';
import type { OcctKernel } from '../dist/index.js';
import { distDir, dot, loadKernel, relErr } from './kernel.js';
import { buildR1, buildR2, buildR3, edgesAtZ, ok, rectLoop, XY } from './models.mjs';

let k: OcctKernel;
beforeAll(async () => {
  k = await loadKernel();
});

describe('module', () => {
  it('reports versions matching versions.json', () => {
    const versions = JSON.parse(readFileSync(resolve(distDir, 'versions.json'), 'utf8'));
    expect(k.version()).toEqual({
      occt: '8.0.1',
      facade: versions.facade,
      emscripten: versions.emsdk.version,
    });
  });

  it('loads with an explicit wasmUrl and with wasmBinary', async () => {
    const wasm = resolve(distDir, 'occt-wasm.wasm');
    const viaUrl = await loadKernel({ wasmUrl: pathToFileURL(wasm).href });
    expect(ok(viaUrl.makeBox(1, 2, 3)).shape).toBe(1);
    const viaBinary = await loadKernel({ wasmBinary: readFileSync(wasm) });
    expect(ok(viaBinary.properties(ok(viaBinary.makeBox(1, 2, 3)).shape)).volume).toBeCloseTo(6, 12);
  });
});

describe('topology', () => {
  it('describes R1 faces, edges and vertices', () => {
    const { shape } = buildR1(k);
    const t = ok(k.topology(shape));
    expect(t.faces).toHaveLength(6);
    expect(t.edges).toHaveLength(12);
    expect(t.vertices).toHaveLength(8);
    expect(t.faces.every((f) => f.surface === 'plane')).toBe(true);
    expect(t.faces.reduce((s, f) => s + f.area, 0)).toBeCloseTo(5200, 9);
    for (const f of t.faces) {
      // Outward normals: the centroid moved along the normal leaves the box.
      const p = f.centroid.map((v, i) => v + f.normal[i]);
      const inside = p[0] > 0 && p[0] < 40 && p[1] > 0 && p[1] < 30 && p[2] > 0 && p[2] < 20;
      expect(inside).toBe(false);
      expect(f.bbox).toHaveLength(6);
    }
    for (const e of t.edges) {
      expect(e.curve).toBe('line');
      expect(e.faces).toHaveLength(2);
      expect([40, 30, 20]).toContain(Math.round(e.length));
    }
    t.faces.forEach((f, i) => expect(f.index).toBe(i));
  });

  it('classifies curved geometry', () => {
    const t = ok(k.topology(buildR2(k).shape));
    const circles = t.edges.filter((e) => e.curve === 'circle');
    expect(circles).toHaveLength(8);
    for (const c of circles) expect(c.length).toBeCloseTo(6 * Math.PI, 9);
    const seams = t.edges.filter((e) => e.faces.length === 1);
    expect(seams).toHaveLength(4);
  });
});

describe('mesh', () => {
  it('returns consistent typed arrays with per-face and per-edge ranges', () => {
    const { shape } = buildR3(k);
    const props = ok(k.properties(shape));
    const m = ok(k.mesh(shape, 0.05, 0.3));
    expect(m.positions).toBeInstanceOf(Float32Array);
    expect(m.normals).toBeInstanceOf(Float32Array);
    expect(m.indices).toBeInstanceOf(Uint32Array);
    expect(m.normals.length).toBe(m.positions.length);
    expect(m.faceRanges.length).toBe(2 * props.faces);
    expect(m.edgeRanges.length).toBe(2 * props.edges);

    const nVerts = m.positions.length / 3;
    expect(Math.max(...m.indices)).toBeLessThan(nVerts);
    let covered = 0;
    for (let f = 0; f < props.faces; f++) {
      expect(m.faceRanges[2 * f]).toBe(covered);
      expect(m.faceRanges[2 * f + 1]).toBeGreaterThan(0);
      covered += m.faceRanges[2 * f + 1];
    }
    expect(covered).toBe(m.indices.length);
    for (let e = 0; e < props.edges; e++) expect(m.edgeRanges[2 * e + 1]).toBeGreaterThanOrEqual(2);

    // Triangle area matches the B-rep area and winding agrees with normals.
    let area = 0;
    let agree = 0;
    const P = (i: number) => [m.positions[3 * i], m.positions[3 * i + 1], m.positions[3 * i + 2]];
    for (let t = 0; t < m.indices.length; t += 3) {
      const [a, b, c] = [P(m.indices[t]), P(m.indices[t + 1]), P(m.indices[t + 2])];
      const u = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
      const v = [c[0] - a[0], c[1] - a[1], c[2] - a[2]];
      const n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]];
      area += Math.hypot(n[0], n[1], n[2]) / 2;
      const i = m.indices[t];
      if (dot(n, [m.normals[3 * i], m.normals[3 * i + 1], m.normals[3 * i + 2]]) > 0) agree++;
    }
    expect(relErr(area, props.area)).toBeLessThan(5e-3);
    expect(agree).toBe(m.indices.length / 3);
    for (let i = 0; i < nVerts; i++) {
      expect(Math.hypot(m.normals[3 * i], m.normals[3 * i + 1], m.normals[3 * i + 2])).toBeCloseTo(1, 5);
    }
  });

  it('gets finer with a smaller deflection', () => {
    const { shape } = buildR2(k);
    const coarse = ok(k.mesh(shape, 0.5, 0.5));
    const fine = ok(k.mesh(shape, 0.01, 0.1));
    expect(fine.indices.length).toBeGreaterThan(coarse.indices.length);
  });
});

describe('other operations', () => {
  it('revolves a profile into a ring and reports caps of a partial revolution', () => {
    const xz = { origin: [0, 0, 0], normal: [0, -1, 0], xDir: [1, 0, 0] };
    const profile = ok(k.makeProfileFace(xz, [rectLoop(10, 0, 5, 4)]));
    const full = ok(k.revolve(profile.shape, [0, 0, 0], [0, 0, 1], 2 * Math.PI));
    const props = ok(k.properties(full.shape));
    expect(props.valid).toBe(true);
    expect(relErr(props.volume, Math.PI * (15 * 15 - 10 * 10) * 4)).toBeLessThan(1e-6);

    const half = ok(k.revolve(profile.shape, [0, 0, 0], [0, 0, 1], Math.PI));
    expect(half.sweep.bottom).toHaveLength(1);
    expect(half.sweep.top).toHaveLength(1);
    expect(half.sweep.sides).toHaveLength(4);
    expect(relErr(ok(k.properties(half.shape)).volume, (Math.PI * (225 - 100) * 4) / 2)).toBeLessThan(1e-6);
  });

  it('builds profiles with arcs (slot) in any plane', () => {
    const plane = { origin: [0, 0, 5], normal: [0, 0, 1], xDir: [1, 1, 0] };
    const slot = [
      { kind: 'line', a: [0, -2], b: [10, -2] },
      { kind: 'arc', center: [10, 0], radius: 2, start: [10, -2], end: [10, 2], ccw: true },
      { kind: 'line', a: [10, 2], b: [0, 2] },
      { kind: 'arc', center: [0, 0], radius: 2, start: [0, 2], end: [0, -2], ccw: true },
    ] as const;
    const profile = ok(k.makeProfileFace(plane, [slot]));
    expect(ok(k.properties(profile.shape)).area).toBeCloseTo(40 + 4 * Math.PI, 9);
    const solid = ok(k.extrude(profile.shape, [0, 0, 1], 3));
    const props = ok(k.properties(solid.shape));
    expect(props.valid).toBe(true);
    expect(props.volume).toBeCloseTo((40 + 4 * Math.PI) * 3, 6);
    expect(props.bbox[2]).toBeCloseTo(5, 9);
    // A clockwise outer loop is reoriented, not rejected.
    const cw = [...rectLoop(0, 0, 4, 4)].reverse().map((s) => ({ kind: 'line' as const, a: s.b, b: s.a }));
    expect(ok(k.properties(ok(k.makeProfileFace(XY, [cw])).shape)).area).toBeCloseTo(16, 12);
  });

  it('chamfers an edge', () => {
    const { shape } = buildR1(k);
    const edge = edgesAtZ(k, shape, 20)[0];
    const length = ok(k.topology(shape)).edges[edge].length;
    const ch = ok(k.chamfer(shape, [edge], 2));
    const props = ok(k.properties(ch.shape));
    expect(props.valid).toBe(true);
    expect(props.faces).toBe(7);
    expect(props.volume).toBeCloseTo(24000 - 2 * length, 6);
    expect(ch.history.edges.generated.some((g) => g.from === edge && g.kind === 'face')).toBe(true);
  });
});

describe('BRep exchange', () => {
  it('round-trips R3 through binary BRep exactly', () => {
    const { shape } = buildR3(k);
    const before = ok(k.properties(shape));
    const bytes = ok(k.exportBrep(shape)).bytes;
    const back = ok(k.importBrep(bytes));
    const after = ok(k.properties(back.shape));
    expect(after.volume).toBe(before.volume);
    expect([after.faces, after.edges, after.vertices]).toEqual([before.faces, before.edges, before.vertices]);
    // Same topology order: history indices stay meaningful after a cache hit.
    expect(ok(k.topology(back.shape)).faces.map((f) => f.surface)).toEqual(
      ok(k.topology(shape)).faces.map((f) => f.surface),
    );
    // ArrayBuffer input works too.
    expect(ok(k.importBrep(bytes.slice().buffer)).shape).toBeGreaterThan(0);
  });
});

describe('errors', () => {
  it('fillet radius too large returns ok:false and the module stays usable', () => {
    const { shape } = buildR1(k);
    const res = k.fillet(shape, edgesAtZ(k, shape, 20), 25);
    expect(res.ok).toBe(false);
    if (!res.ok) expect(['FILLET_FAILED', 'INVALID_RESULT', 'OCCT_EXCEPTION']).toContain(res.error.code);
    expect(k.trapped).toBe(false);
    // Still works afterwards.
    const r3 = buildR3(k);
    expect(ok(k.properties(r3.shape)).valid).toBe(true);
  });

  it('rejects bad arguments with structured errors', () => {
    const code = (r: { ok: boolean; error?: { code: string } }) => (r.ok ? 'OK' : r.error!.code);
    expect(code(k.makeBox(0, 1, 1))).toBe('INVALID_ARGUMENT');
    expect(code(k.makeBox(NaN, 1, 1))).toBe('INVALID_ARGUMENT');
    expect(code(k.makeCylinder([0, 0, 0], [0, 0, 0], 1, 1))).toBe('INVALID_ARGUMENT');
    expect(code(k.extrude(123456, [0, 0, 1], 1))).toBe('INVALID_HANDLE');
    expect(code(k.boolean('xor' as never, 1, 1))).toBe('INVALID_ARGUMENT');
    const box = ok(k.makeBox(1, 1, 1)).shape;
    expect(code(k.fillet(box, [99], 0.1))).toBe('INVALID_ARGUMENT');
    expect(code(k.fillet(box, [], 0.1))).toBe('INVALID_ARGUMENT');
    expect(code(k.mesh(box, 0, 0.5))).toBe('INVALID_ARGUMENT');
    expect(code(k.importStep(new TextEncoder().encode('not a step file')))).toBe('STEP_READ_FAILED');
    expect(code(k.importStep('nope' as never))).toBe('INVALID_ARGUMENT');
    expect(code(k.importBrep(new Uint8Array([1, 2, 3])))).not.toBe('OK');
    // Open loop and unknown segment kind.
    const open = [
      { kind: 'line', a: [0, 0], b: [1, 0] },
      { kind: 'line', a: [1, 0], b: [1, 1] },
    ] as const;
    expect(code(k.makeProfileFace(XY, [open]))).toBe('INVALID_PROFILE');
    expect(code(k.makeProfileFace(XY, [[{ kind: 'spline' } as never]]))).toBe('INVALID_ARGUMENT');
    // Self-intersecting (bow tie) loop.
    const bowTie = [
      { kind: 'line', a: [0, 0], b: [1, 1] },
      { kind: 'line', a: [1, 1], b: [1, 0] },
      { kind: 'line', a: [1, 0], b: [0, 1] },
      { kind: 'line', a: [0, 1], b: [0, 0] },
    ] as const;
    expect(code(k.makeProfileFace(XY, [bowTie]))).toBe('INVALID_PROFILE');
    expect(code(k.makeBox(1, 1, 1))).toBe('OK');
  });
});

describe('memory', () => {
  it('releases shapes and reports heap stats', () => {
    ok(k.releaseAll());
    expect(k.heapStats().liveShapes).toBe(0);
    const a = ok(k.makeBox(1, 1, 1)).shape;
    const b = ok(k.makeBox(2, 2, 2)).shape;
    expect(b).toBeGreaterThan(a);
    const stats = k.heapStats();
    expect(stats.liveShapes).toBe(2);
    expect(stats.heapBytes).toBeGreaterThanOrEqual(64 * 1024 * 1024);
    expect(stats.usedBytes).toBeGreaterThan(0);
    ok(k.release(a));
    expect(k.release(a).ok).toBe(false);
    expect(k.properties(a).ok).toBe(false);
    expect(k.heapStats().liveShapes).toBe(1);
    expect(ok(k.releaseAll()).released).toBe(1);
  });

  it('does not leak across repeated R3 builds', () => {
    ok(k.releaseAll());
    for (let i = 0; i < 5; i++) {
      buildR3(k);
      ok(k.releaseAll());
    }
    const baseline = k.heapStats().usedBytes;
    for (let i = 0; i < 20; i++) {
      const { shape } = buildR3(k);
      ok(k.mesh(shape, 0.1, 0.5));
      ok(k.exportStep([shape]));
      ok(k.releaseAll());
    }
    expect(k.heapStats().usedBytes - baseline).toBeLessThan(2 * 1024 * 1024);
  });
});
