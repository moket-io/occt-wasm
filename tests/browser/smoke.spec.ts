import { expect, test } from '@playwright/test';
import { R3_VOLUME } from '../models.mjs';

test('builds R3 in a module Worker', async ({ page, browserName }) => {
  await page.goto('/tests/browser/index.html');
  const result = await page.evaluate(() => (window as unknown as { runSmoke(): Promise<any> }).runSmoke());
  expect(result.ok, result.error).toBe(true);
  expect(result.version.occt).toBe('8.0.1');
  expect(result.valid).toBe(true);
  expect(result.faces).toBe(10);
  expect(Math.abs(result.volume - R3_VOLUME) / R3_VOLUME).toBeLessThan(1e-6);
  expect(Math.abs(result.stepVolume - R3_VOLUME) / R3_VOLUME).toBeLessThan(1e-6);
  expect(result.meshIsTyped).toBe(true);
  expect(result.triangles).toBeGreaterThan(0);
  expect(result.failedCode).not.toBeNull();
  expect(result.trapped).toBe(false);
  console.log(`[${browserName}] ${JSON.stringify(result.timings)} heap=${result.heap.heapBytes}`);
});
