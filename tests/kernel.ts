// Loads the built module from dist/ (or OCCT_WASM_DIST) for the Node tests.
import { existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import type { OcctKernel, OcctKernelOptions } from '../dist/index.js';

export const distDir = resolve(process.env.OCCT_WASM_DIST ?? resolve(import.meta.dirname, '../dist'));

export async function loadKernel(options?: OcctKernelOptions): Promise<OcctKernel> {
  const entry = resolve(distDir, 'index.mjs');
  if (!existsSync(entry)) {
    throw new Error(`${entry} not found: build the module first (pnpm build or pnpm build:local)`);
  }
  const mod = (await import(pathToFileURL(entry).href)) as typeof import('../dist/index.js');
  return mod.createOcctKernel(options);
}

export function relErr(actual: number, expected: number): number {
  return Math.abs(actual - expected) / Math.max(Math.abs(expected), 1e-300);
}

export function dot(a: ArrayLike<number>, b: ArrayLike<number>): number {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
