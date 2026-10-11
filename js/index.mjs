// ESM loader for the occt-wasm module. Works in browsers, Web Workers and Node.
//
// SPDX-License-Identifier: LGPL-2.1-only
import createOcctModule from './occt-wasm.mjs';

const METHODS = [
  'version',
  'heapStats',
  'release',
  'releaseAll',
  'makeBox',
  'makeCylinder',
  'makeProfileFace',
  'extrude',
  'revolve',
  'boolean',
  'fillet',
  'chamfer',
  'topology',
  'properties',
  'mesh',
  'exportStep',
  'importStep',
  'exportBrep',
  'importBrep',
];

function errorResult(code, message) {
  return { ok: false, error: { code, message } };
}

/**
 * Instantiates the OCCT module and returns the kernel API.
 * @param {import('./index').OcctKernelOptions} [options]
 * @returns {Promise<import('./index').OcctKernel>}
 */
export async function createOcctKernel(options = {}) {
  const moduleArgs = {};
  if (options.wasmUrl !== undefined) {
    const url = String(options.wasmUrl);
    moduleArgs.locateFile = (path, prefix) => (path.endsWith('.wasm') ? url : prefix + path);
  } else if (options.locateFile) {
    moduleArgs.locateFile = options.locateFile;
  }
  if (options.wasmBinary !== undefined) moduleArgs.wasmBinary = options.wasmBinary;
  if (options.print) moduleArgs.print = options.print;
  if (options.printErr) moduleArgs.printErr = options.printErr;

  const module = await createOcctModule(moduleArgs);

  const kernel = {
    module,
    trapped: false,
  };

  for (const name of METHODS) {
    const fn = module[name];
    kernel[name] = (...args) => {
      try {
        return fn(...args);
      } catch (e) {
        // The facade catches every C++ exception, so anything reaching here
        // is a wasm trap (abort, out of bounds access, stack overflow) or a
        // binding error (wrong argument count). After a trap the module
        // state is undefined: drop the kernel and create a new one.
        if (typeof WebAssembly !== 'undefined' && e instanceof WebAssembly.RuntimeError) {
          kernel.trapped = true;
          return errorResult('WASM_TRAP', String(e.message));
        }
        if (typeof WebAssembly !== 'undefined' && WebAssembly.Exception && e instanceof WebAssembly.Exception) {
          kernel.trapped = true;
          return errorResult('UNCAUGHT_EXCEPTION', 'Uncaught C++ exception');
        }
        return errorResult('JS_ERROR', String(e && e.message ? e.message : e));
      }
    };
  }

  return kernel;
}

export default createOcctKernel;
