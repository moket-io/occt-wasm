// Moket reference models built through the public kernel API. Plain ESM so
// that both Vitest (Node) and the browser smoke test (Worker) can import it.

export const XY = { origin: [0, 0, 0], normal: [0, 0, 1], xDir: [1, 0, 0] };

/** Throws if the result is a failure, otherwise returns it. */
export function ok(result) {
  if (!result || result.ok !== true) {
    const e = result && result.error ? `${result.error.code}: ${result.error.message}` : String(result);
    throw new Error(e);
  }
  return result;
}

/** Counter-clockwise rectangle [x0, x0 + w] x [y0, y0 + h]. */
export function rectLoop(x0, y0, w, h) {
  const p = [
    [x0, y0],
    [x0 + w, y0],
    [x0 + w, y0 + h],
    [x0, y0 + h],
  ];
  return p.map((a, i) => ({ kind: 'line', a, b: p[(i + 1) % 4] }));
}

export function circleLoop(cx, cy, r) {
  return [{ kind: 'circle', center: [cx, cy], radius: r }];
}

/** R1: 40 x 30 rectangle sketch extruded 20. */
export function buildR1(k) {
  const profile = ok(k.makeProfileFace(XY, [rectLoop(0, 0, 40, 30)]));
  const extrude = ok(k.extrude(profile.shape, [0, 0, 1], 20));
  return { profile, extrude, shape: extrude.shape };
}

/** R2: plate 100 x 60 x 10 with four through holes d6 centered 10 mm from the edges. */
export function buildR2(k) {
  const holes = [
    [10, 10],
    [90, 10],
    [90, 50],
    [10, 50],
  ].map(([x, y]) => circleLoop(x, y, 3));
  const profile = ok(k.makeProfileFace(XY, [rectLoop(0, 0, 100, 60), ...holes]));
  const extrude = ok(k.extrude(profile.shape, [0, 0, 1], 10));
  return { profile, extrude, shape: extrude.shape };
}

/** Indices of straight edges whose midpoint lies at height z. */
export function edgesAtZ(k, shape, z, tol = 1e-9) {
  const topo = ok(k.topology(shape));
  return topo.edges.filter((e) => e.curve === 'line' && Math.abs(e.midpoint[2] - z) < tol).map((e) => e.index);
}

/** R3: R1 with a fillet R3 on the four top edges. */
export function buildR3(k) {
  const r1 = buildR1(k);
  const topEdges = edgesAtZ(k, r1.shape, 20);
  const fillet = ok(k.fillet(r1.shape, topEdges, 3));
  return { r1, topEdges, fillet, shape: fillet.shape };
}

// Analytic expectations.
export const R1_VOLUME = 40 * 30 * 20;
export const R2_VOLUME = 60000 - 360 * Math.PI;
/**
 * R3: the fillet removes a constant cross-section A = (1 - pi/4) r^2 swept
 * along the rectangle through its centroid (mitered corners), the centroid
 * sits d = r (10 - 3 pi) / (12 - 3 pi) inside each top edge.
 */
export const R3_VOLUME = (() => {
  const r = 3;
  const area = (1 - Math.PI / 4) * r * r;
  const d = (r * (10 - 3 * Math.PI)) / (12 - 3 * Math.PI);
  const path = 2 * (40 - 2 * d) + 2 * (30 - 2 * d);
  return R1_VOLUME - area * path;
})();
