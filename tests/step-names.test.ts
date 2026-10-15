// Golden fixtures for named faces in STEP (0.10.0): a cosmetic thread is a
// cylindrical face with a name, written as the name of its ADVANCED_FACE and
// read back by importStepParts on the same face, next to the colours.
import { beforeAll, describe, expect, it } from 'vitest';
import type { OcctKernel, StepPart } from '../dist/index.js';
import { loadKernel, relErr } from './kernel.js';
import { ok } from './models.mjs';

let k: OcctKernel;
beforeAll(async () => {
  k = await loadKernel();
});

const code = (r: { ok: boolean; error?: { code: string } }) => (r.ok ? 'OK' : r.error?.code);
const near = (a: readonly number[] | undefined, b: readonly number[]) =>
  expect(a?.every((v, i) => Math.abs(v - b[i]) < 2e-3)).toBe(true);
const red: [number, number, number] = [0.8, 0.1, 0.1];
const blue: [number, number, number] = [0.1, 0.2, 0.9];
const I = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0];

const THREAD = 'Thread M6x1 - 6g, L12';

/** A bolt shank, radius 3, length 12, and its faces: side, top and bottom. */
function shank() {
  const shape = ok(k.makeCylinder([0, 0, 0], [0, 0, 1], 3, 12)).shape;
  const faces = ok(k.topology(shape)).faces;
  const side = faces.findIndex((f) => f.surface === 'cylinder');
  const top = faces.findIndex((f) => f.surface === 'plane' && f.centroid[2] > 6);
  const bottom = faces.findIndex((f) => f.surface === 'plane' && f.centroid[2] < 6);
  expect([side, top, bottom].sort()).toEqual([0, 1, 2]);
  return { shape, side, top, bottom };
}

/** The face a name points to, by the returned shape's topology. */
function namedFace(part: StepPart | undefined, name: string) {
  const entry = part?.faceNames?.find((f) => f.name === name);
  expect(entry).toBeDefined();
  return ok(k.topology(part!.shape)).faces[entry!.face];
}

describe('STEP face names', () => {
  it('writes a thread name on a face and reads it back with the colours', () => {
    const { shape, side, top, bottom } = shank();
    const bytes = ok(
      k.exportStep([shape], {
        name: 'Shank',
        faceNames: [
          { shape: 0, faces: [side], name: THREAD },
          { shape: 0, faces: [bottom], name: 'Datum A' },
        ],
        faceColors: [
          { shape: 0, faces: [top], color: red },
          { shape: 0, faces: [bottom], color: blue },
        ],
      }),
    ).bytes;

    // The name is the face's own name in the STEP text, written once.
    const text = new TextDecoder().decode(bytes);
    expect(text).toMatch(/ADVANCED_FACE\('Thread M6x1 - 6g, L12'/);
    expect(text).toMatch(/ADVANCED_FACE\('Datum A'/);
    expect(text.split(THREAD)).toHaveLength(2);
    expect(text.match(/ADVANCED_FACE\(''/g)).toHaveLength(1);

    const parts = ok(k.importStepParts(bytes)).parts;
    expect(parts).toHaveLength(1);
    const [part] = parts;
    expect(part.product).toBe('Shank');
    expect(part.faceNames).toHaveLength(2);
    const thread = namedFace(part, THREAD);
    expect(thread.surface).toBe('cylinder');
    expect(thread.radius).toBeCloseTo(3, 9);
    expect(relErr(thread.area, 2 * Math.PI * 3 * 12)).toBeLessThan(1e-9);
    const datum = namedFace(part, 'Datum A');
    expect(datum.surface).toBe('plane');
    expect(datum.centroid[2]).toBeCloseTo(0, 9);

    // Colours still read, the named bottom face keeps its colour too.
    const faces = ok(k.topology(part.shape)).faces;
    const colour = (z: number) =>
      part.faceColors?.find((f) => Math.abs(faces[f.face].centroid[2] - z) < 1e-6 && faces[f.face].surface === 'plane')
        ?.color;
    expect(part.faceColors).toHaveLength(2);
    near(colour(12), red);
    near(colour(0), blue);

    // Names alone are options too; without names nothing comes back.
    const onlyNames = ok(k.exportStep([shape], { faceNames: [{ shape: 0, faces: [side], name: THREAD }] })).bytes;
    const alone = ok(k.importStepParts(onlyNames)).parts[0];
    expect(namedFace(alone, THREAD).surface).toBe('cylinder');
    expect(alone.faceColors).toBeUndefined();
    expect(ok(k.importStepParts(ok(k.exportStep([shape], { name: 'Plain' })).bytes)).parts[0].faceNames).toBeUndefined();
    expect(ok(k.importStepParts(ok(k.exportStep([shape])).bytes)).parts[0].faceNames).toBeUndefined();
  });

  it('keeps spaces, quotes, backslashes and non-ASCII names', () => {
    const { shape, side, top, bottom } = shank();
    const names = ['Резьба M6', `Bolt's "head"`, 'C:\\threads\\M6 x 1'];
    const bytes = ok(
      k.exportStep([shape], {
        faceNames: [
          { shape: 0, faces: [side], name: names[0] },
          { shape: 0, faces: [top], name: names[1] },
          { shape: 0, faces: [bottom], name: names[2] },
        ],
      }),
    ).bytes;
    const text = new TextDecoder().decode(bytes);
    // UTF-8 as it is, apostrophes and backslashes doubled as STEP strings want.
    expect(text).toContain("'Резьба M6'");
    expect(text).toContain(`'Bolt''s "head"'`);
    expect(text).toContain("'C:\\\\threads\\\\M6 x 1'");

    const part = ok(k.importStepParts(bytes)).parts[0];
    expect(part.faceNames?.map((f) => f.name).sort()).toEqual([...names].sort());
    expect(namedFace(part, names[0]).surface).toBe('cylinder');
    expect(namedFace(part, names[1]).centroid[2]).toBeCloseTo(12, 9);
    expect(namedFace(part, names[2]).centroid[2]).toBeCloseTo(0, 9);
  });

  it('names a face of a part on every instance of an assembly', () => {
    const { shape, side, top } = shank();
    const plate = ok(k.makeBox(40, 30, 5)).shape;
    const moved = [1, 0, 0, 20, 0, 1, 0, 10, 0, 0, 1, 5];
    // A quarter turn about X, so the shank lies along Y.
    const turned = [1, 0, 0, 30, 0, 0, 1, 0, 0, -1, 0, 20];
    const bytes = ok(
      k.exportStepAssembly({
        name: 'Fixture',
        parts: [
          { name: 'Plate', shapes: [plate] },
          {
            name: 'Shank M6',
            shapes: [shape],
            faceNames: [{ shape: 0, faces: [side], name: THREAD }],
            faceColors: [{ shape: 0, faces: [top], color: red }],
          },
        ],
        instances: [
          { part: 0, name: 'Plate:1', matrix: I },
          { part: 1, name: 'Shank M6:1', matrix: I },
          { part: 1, name: 'Shank M6:2', matrix: moved },
          { part: 1, name: 'Shank M6:3', matrix: turned },
        ],
      }),
    ).bytes;
    const text = new TextDecoder().decode(bytes);
    // The part is written once, so is its name.
    expect(text.split(THREAD)).toHaveLength(2);
    // Instance names stay on the occurrences, not on the part's solid.
    expect(text).not.toMatch(/MANIFOLD_SOLID_BREP\('Shank M6:/);

    for (const local of [false, true]) {
      const parts = ok(k.importStepParts(bytes, { local })).parts;
      expect(parts.map((p) => p.name).sort()).toEqual(['Plate:1', 'Shank M6:1', 'Shank M6:2', 'Shank M6:3']);
      expect(parts.find((p) => p.name === 'Plate:1')?.faceNames).toBeUndefined();
      for (const p of parts.filter((q) => q.product === 'Shank M6')) {
        expect(p.faceNames).toHaveLength(1);
        const thread = namedFace(p, THREAD);
        expect(thread.surface).toBe('cylinder');
        expect(relErr(thread.area, 2 * Math.PI * 3 * 12)).toBeLessThan(1e-9);
        expect(p.faceColors).toHaveLength(1);
        near(p.faceColors?.[0].color, red);
      }
      if (!local) {
        // The turned instance's thread axis lies along Y.
        const axis = namedFace(
          parts.find((p) => p.name === 'Shank M6:3'),
          THREAD,
        ).axis;
        expect(Math.abs(axis?.dir[1] ?? 0)).toBeCloseTo(1, 9);
      }
    }
  });

  it('rejects bad face names', () => {
    const { shape, side } = shank();
    const named = (entry: object) => k.exportStep([shape], { faceNames: [entry as never] });
    expect(code(named({ shape: 1, faces: [side], name: THREAD }))).toBe('INVALID_ARGUMENT');
    expect(code(named({ shape: -1, faces: [side], name: THREAD }))).toBe('INVALID_ARGUMENT');
    expect(code(named({ shape: 0, faces: [3], name: THREAD }))).toBe('INVALID_ARGUMENT');
    expect(code(named({ shape: 0, faces: [-1], name: THREAD }))).toBe('INVALID_ARGUMENT');
    expect(code(named({ shape: 0, faces: [side], name: '' }))).toBe('INVALID_ARGUMENT');
    expect(code(named({ shape: 0, faces: [side], name: 5 }))).toBe('INVALID_ARGUMENT');
    expect(code(named({ shape: 0, faces: [side] }))).toBe('INVALID_ARGUMENT');
    expect(code(k.exportStep([shape], { faceNames: 'thread' as never }))).toBe('INVALID_ARGUMENT');
    expect(
      code(
        k.exportStepAssembly({
          parts: [{ shapes: [shape], faceNames: [{ shape: 0, faces: [7], name: THREAD }] }],
          instances: [{ part: 0, matrix: I }],
        }),
      ),
    ).toBe('INVALID_ARGUMENT');
    // The module is still usable.
    expect(code(named({ shape: 0, faces: [side], name: THREAD }))).toBe('OK');
  });
});
