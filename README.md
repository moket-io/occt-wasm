# occt-wasm

[Open CASCADE Technology](https://github.com/Open-Cascade-SAS/OCCT) (OCCT) 8.0.1 compiled to WebAssembly, with a thin C++ facade exposed through embind. It is the geometry kernel of [Moket](https://moket.io), a browser-native parametric CAD, and runs in browsers, Web Workers and Node.

The module is distributed under the LGPL 2.1 with the Open CASCADE exception. It is a separate repository and a separately loaded file on purpose: applications load `occt-wasm.wasm` at runtime and users can replace it with their own build (see [Rebuilding and replacing the module](#rebuilding-and-replacing-the-module)).

## What is inside

- **OCCT 8.0.1** (tag `V8_0_1`, commit `b8f597c677811d1f9f4d8a97f5ae2825c0353a42`), static libraries for modeling, booleans, fillets, offsets, features, meshing, shape healing, hidden line removal and STEP.
- **Facade** (`facade/`), about 2150 lines of C++ with no product logic: a shape arena addressed by integer handles, construction, extrude/revolve/boolean/fillet/chamfer/transform with full topological history, topology queries with axes of analytic faces and circles, validity checks, minimum distances, hidden line removal for drawings, tessellation, mass properties, STEP (also of assemblies, with XCAF names) and BRep exchange. Every entry point catches all exceptions and returns `{ ok, error?: { code, message }, ... }`.
- **Loader** (`js/`), an ESM `createOcctKernel()` with hand-written TypeScript types.

## Usage

```ts
import { createOcctKernel } from '@moket/occt-wasm';
// With a bundler, pass the hashed asset URL of the wasm file:
// import wasmUrl from '@moket/occt-wasm/occt-wasm.wasm?url';

const k = await createOcctKernel({ wasmUrl });

const XY = { origin: [0, 0, 0], normal: [0, 0, 1], xDir: [1, 0, 0] };
const rect = [
  { kind: 'line', a: [0, 0], b: [40, 0] },
  { kind: 'line', a: [40, 0], b: [40, 30] },
  { kind: 'line', a: [40, 30], b: [0, 30] },
  { kind: 'line', a: [0, 30], b: [0, 0] },
];
const profile = k.makeProfileFace(XY, [rect]);
if (!profile.ok) throw new Error(profile.error.message);

const box = k.extrude(profile.shape, [0, 0, 1], 20);           // + history, sweep info
const top = k.topology(box.shape).edges.filter((e) => e.midpoint[2] === 20).map((e) => e.index);
const r3 = k.fillet(box.shape, top, 3);                         // + history
const mesh = k.mesh(r3.shape, 0.05, 0.3);                       // typed arrays, transferable
const step = k.exportStep([r3.shape]);                          // Uint8Array
k.releaseAll();
```

Run it in a dedicated module Worker (`new Worker(url, { type: 'module' })`): operations are synchronous and can take long on complex models.

## API overview

Full reference with exact types and semantics: [API.md](API.md). Types: [`js/index.d.ts`](js/index.d.ts).

| Group        | Calls                                                                                   |
| ------------ | --------------------------------------------------------------------------------------- |
| Module       | `version()`, `heapStats()`, `release(id)`, `releaseAll()`                               |
| Construction | `makeBox`, `makeCylinder`, `makeProfileFace(plane, loops)` (lines, arcs, circles, holes) |
| Operations   | `extrude`, `revolve`, `boolean('fuse' \| 'cut' \| 'common')`, `fillet`, `chamfer`, `transform` |
| Queries      | `topology`, `properties`, `check`, `distance`, `mesh`                                   |
| Drawings     | `project` (exact hidden line removal with the source edge of every curve)               |
| Exchange     | `exportStep`, `importStep`, `exportBrep`, `importBrep`, `exportStepAssembly` (XCAF products and instances) |

Operations return a **history**: for every input sub-shape (face, edge, vertex, by index in `TopExp::MapShapes` order) which result sub-shapes it was modified into, which it generated (with their kind), whether it was deleted, or whether it is present unchanged. Extrude and revolve also report the bottom, top and per-profile-edge side faces, and `makeProfileFace` maps every sketch segment to its edge index. Moket builds topological naming on top of this in TypeScript.

## Versions

| Component  | Version                                                                                |
| ---------- | -------------------------------------------------------------------------------------- |
| OCCT       | 8.0.1, `V8_0_1` = `b8f597c677811d1f9f4d8a97f5ae2825c0353a42` (submodule `third_party/occt`) |
| Emscripten | 6.0.12, image `emscripten/emsdk:6.0.12@sha256:77638e6c215a65f25a704c0c831d5c2463a8c0468a58c6cb82489dbe621dfa05` |
| Facade     | 0.4.0                                                                                  |

All pins and compiler flags are in [`versions.json`](versions.json) (shipped in `dist/`) and [`scripts/config-occt.sh`](scripts/config-occt.sh) / [`scripts/config.sh`](scripts/config.sh); CI checks that they agree.

Build flags, in short: wasm exceptions (`-fwasm-exceptions`), SIMD (`-msimd128`), `-O3`, no LTO, no threads, 32-bit memory growing from 64 MB up to 4 GB, 8 MB stack. OCCT is compiled with `-UOCC_CONVERT_SIGNALS`: OCCT signal handling relies on `setjmp/longjmp`, which together with wasm exceptions produces an invalid module once STEP and shape healing are linked.

## Sizes and timings

Measured on an Apple M4 (10 cores), macOS, Node 24.

| File             | Raw      | gzip -9 | brotli -11 |
| ---------------- | -------- | ------- | ---------- |
| `occt-wasm.wasm` | 15.57 MB | 5.22 MB | 3.64 MB    |
| `occt-wasm.mjs`  | 91 KB    | 27 KB   | 23 KB      |

CI fails if the brotli-compressed wasm exceeds 4.5 MB.

Node 24 (`node scripts/bench.mjs`, median of 15 runs):

| Step                                          | Time    |
| --------------------------------------------- | ------- |
| Cold start (compile, instantiate, first box)  | 24 ms   |
| R1 sketch + extrude + mesh                    | 1.0 ms  |
| R2 plate with four holes                      | 0.7 ms  |
| R3 (R1 + fillet R3 on four edges)             | 9.3 ms  |
| R3 mesh (0.05 mm, 0.3 rad)                    | 1.2 ms  |
| R3 STEP export                                | 2.4 ms  |

Browser smoke test (module Worker, first run in a fresh page, Playwright headless):

| Browser            | Load + instantiate | R3 build | R3 mesh | STEP export |
| ------------------ | ------------------ | -------- | ------- | ----------- |
| Chromium           | 75 ms              | 76 ms    | 13 ms   | 34 ms       |
| Firefox            | 232 ms             | 16 ms    | 5 ms    | 5 ms        |
| WebKit             | 92 ms              | 64 ms    | 16 ms   | 19 ms       |

Chromium and WebKit tier up lazily, so their first calls run in the baseline compiler; Firefox compiles the whole module up front. The 64 MB initial heap was not exceeded by any test.

Build times:

| Build                                   | Time      |
| --------------------------------------- | --------- |
| OCCT libraries, local emsdk, `-j10`     | 7 min 19 s |
| Facade compile + link, local            | 38 s      |
| Full Docker build, Docker Desktop VM with 2 CPUs (OCCT stage 19 min 12 s) | 20 min 31 s |
| Docker rebuild with cached OCCT layer   | 60 s      |
| CI, cold (GitHub `ubuntu-24.04`, 2 vCPU, OCCT layer not cached) | 93 min |

## Building

### Docker (reference)

```sh
git clone --recurse-submodules --shallow-submodules https://github.com/moket-io/occt-wasm.git
cd occt-wasm
scripts/build.sh            # docker buildx, writes dist/
```

`docker/Dockerfile` has three stages: `occt-libs` (static OCCT, cached as long as the OCCT sources, `scripts/config-occt.sh`, `scripts/build-occt.sh` and the emsdk image digest do not change), `facade` (compile and link) and `dist` (export only). CI and releases use this build. The OCCT stage is kept in the GitHub Actions cache; a change to its inputs took about 90 minutes on the 2 vCPU runner the repository had while it was private (public repositories get 4 vCPU runners), any other change a few minutes.

### Local

```sh
scripts/build-local.sh      # installs emsdk 6.0.12 into .cache/ if needed
```

Uses an activated emsdk of the pinned version if one is on `PATH`, otherwise installs it into `.cache/emsdk`. OCCT is built once into `.cache/occt-install` and rebuilt only when its inputs change. Local and Docker builds use the same scripts and flags and map source paths with `-ffile-prefix-map`. On macOS the local module is byte-identical to the Docker one except for a single `f64` constant that clang folds with the host math library (1 ulp apart), and the JS glue is identical. Docker builds are byte-identical across runs and across host architectures: the module built by CI on linux/amd64 has the same SHA-256 as the one built in Docker Desktop on linux/arm64. Releases are always built in Docker.

### Tests

```sh
pnpm install
pnpm test           # Vitest, Node 24, against dist/
pnpm test:browser   # Playwright: module Worker in Chromium, Firefox and WebKit
pnpm typecheck      # tests type-checked against dist/index.d.ts
```

The Node suite covers golden fixtures for the Moket reference models (R1 box, R2 plate with holes, R3 fillet, R6 STEP round trip of R1-R3), the history of extrude, fillet and booleans, topology, mesh consistency, revolve, arcs, chamfer, BRep round trip, error paths (an impossible fillet returns `ok: false` and the module keeps working) and memory release. Golden values are in [`tests/fixtures/golden.json`](tests/fixtures/golden.json) and are checked against analytic formulas; the tolerance is 1e-6 relative for volumes and 1e-6 mm for bounding boxes.

## Rebuilding and replacing the module

The LGPL requires that you can modify the library and use your modified version with the application (LGPL 2.1, section 6). This module is designed for that:

1. **Get the source.** This repository at the release tag contains the facade, the build scripts and the pinned OCCT submodule. Every GitHub Release also carries `occt-wasm-<version>-src.tar.gz` with the complete corresponding source, OCCT included.
2. **Modify and rebuild.** Change OCCT (in `third_party/occt`) or the facade, then run `scripts/build.sh` (or `scripts/build-local.sh`). The result is `dist/occt-wasm.wasm` and `dist/occt-wasm.mjs`.
3. **Replace.** Applications load the module as a separate file at runtime; nothing from OCCT is bundled into application code. To use your build, put your `occt-wasm.wasm` and `occt-wasm.mjs` in place of the shipped ones (for a web app, in the directory the app serves them from), or point the loader at them with `createOcctKernel({ wasmUrl })`. Keep the facade API compatible (same function names and result shapes, see API.md) so the application keeps working.

`dist/SHA256SUMS` lists the checksums of the official files so you can tell which module is in use.

## License

- Facade, loader and scripts: LGPL-2.1-only ([LICENSE](LICENSE)).
- Open CASCADE Technology: LGPL 2.1 with the [Open CASCADE exception 1.0](OCCT_LGPL_EXCEPTION.txt).
- See [NOTICE](NOTICE). This software uses Open CASCADE Technology, Copyright (c) 1999-2026 OPEN CASCADE SAS.
