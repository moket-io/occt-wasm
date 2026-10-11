// Module Worker: loads the kernel from dist/ with an explicit wasmUrl (the way
// a host app passes its hashed asset URL) and builds R3.
import { createOcctKernel } from '../../dist/index.mjs';
import { buildR3, ok } from '../models.mjs';

try {
  const t0 = performance.now();
  const k = await createOcctKernel({ wasmUrl: new URL('../../dist/occt-wasm.wasm', import.meta.url) });
  const t1 = performance.now();
  const { shape } = buildR3(k);
  const t2 = performance.now();
  const props = ok(k.properties(shape));
  const mesh = ok(k.mesh(shape, 0.05, 0.3));
  const t3 = performance.now();
  const step = ok(k.exportStep([shape]));
  const t4 = performance.now();
  const back = ok(k.properties(ok(k.importStep(step.bytes)).shape));
  // Error path inside the browser too.
  const failed = k.fillet(ok(k.makeBox(10, 10, 10)).shape, [0], 20);
  self.postMessage({
    ok: true,
    version: k.version(),
    volume: props.volume,
    valid: props.valid,
    faces: props.faces,
    triangles: mesh.indices.length / 3,
    meshIsTyped: mesh.positions instanceof Float32Array,
    stepVolume: back.volume,
    failedCode: failed.ok ? null : failed.error.code,
    trapped: k.trapped,
    timings: {
      initMs: t1 - t0,
      r3Ms: t2 - t1,
      meshMs: t3 - t2,
      stepExportMs: t4 - t3,
    },
    heap: k.heapStats(),
  });
} catch (e) {
  self.postMessage({ ok: false, error: String(e && e.stack ? e.stack : e) });
}
