// Node timings for the README: cold start, reference models, mesh, STEP.
// Usage: node scripts/bench.mjs [distDir]
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const dist = resolve(process.argv[2] ?? new URL('../dist', import.meta.url).pathname);
const { createOcctKernel } = await import(pathToFileURL(resolve(dist, 'index.mjs')).href);
const models = await import(new URL('../tests/models.mjs', import.meta.url).href);
const { ok, buildR1, buildR2, buildR3 } = models;

const time = (fn) => {
  const t = performance.now();
  const r = fn();
  return [performance.now() - t, r];
};
const median = (xs) => [...xs].sort((a, b) => a - b)[Math.floor(xs.length / 2)];

let t = performance.now();
const k = await createOcctKernel();
const [firstBox] = time(() => ok(k.makeBox(1, 1, 1)));
const coldStart = performance.now() - t;

const runs = 15;
const out = { coldStartMs: coldStart, firstBoxMs: firstBox };
const bench = (name, fn) => {
  const xs = [];
  const ys = [];
  for (let i = 0; i < runs; i++) {
    xs.push(time(fn)[0]);
    k.releaseAll();
  }
  out[name] = median(xs);
};
bench('r1BuildMeshMs', () => ok(k.mesh(buildR1(k).shape, 0.1, 0.5)));
bench('r2BuildMs', () => buildR2(k));
bench('r3BuildMs', () => buildR3(k));
bench('r3MeshMs', () => {
  const { shape } = buildR3(k);
  const t0 = performance.now();
  ok(k.mesh(shape, 0.05, 0.3));
  return performance.now() - t0;
});
{
  const xs = [];
  const ys = [];
  for (let i = 0; i < runs; i++) {
    const { shape } = buildR3(k);
    xs.push(time(() => ok(k.exportStep([shape])))[0]);
    ys.push(time(() => ok(k.mesh(shape, 0.05, 0.3)))[0]);
    k.releaseAll();
  }
  out.r3StepExportMs = median(xs);
  out.r3MeshMs = median(ys);
}
out.heapMB = k.heapStats().heapBytes / 1048576;
for (const [key, v] of Object.entries(out)) out[key] = Math.round(v * 10) / 10;
console.log(JSON.stringify(out, null, 2));
