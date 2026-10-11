import { defineConfig, devices } from '@playwright/test';

const port = 4510;

export default defineConfig({
  testDir: 'tests/browser',
  timeout: 120_000,
  reporter: [['list']],
  use: { baseURL: `http://127.0.0.1:${port}` },
  webServer: {
    command: 'node scripts/serve.mjs',
    url: `http://127.0.0.1:${port}/package.json`,
    reuseExistingServer: !process.env.CI,
    env: { PORT: String(port) },
  },
  projects: [
    { name: 'chromium', use: { ...devices['Desktop Chrome'] } },
    { name: 'firefox', use: { ...devices['Desktop Firefox'] } },
    { name: 'webkit', use: { ...devices['Desktop Safari'] } },
  ],
});
