// Prints raw/gzip/brotli sizes of dist artifacts and enforces the size budget.
// Usage: node scripts/size-report.mjs [--budget-br-mb 4.5] [--json out.json]
import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { brotliCompressSync, constants, gzipSync } from 'node:zlib';

const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : fallback;
};
const budgetBr = Number(opt('--budget-br-mb', '4.5')) * 1e6;
const dist = fileURLToPath(new URL('../dist/', import.meta.url));

const report = {};
for (const file of ['occt-wasm.wasm', 'occt-wasm.mjs', 'index.mjs']) {
  const buf = readFileSync(dist + file);
  report[file] = {
    raw: buf.length,
    gzip: gzipSync(buf, { level: 9 }).length,
    brotli: brotliCompressSync(buf, {
      params: { [constants.BROTLI_PARAM_QUALITY]: 11, [constants.BROTLI_PARAM_SIZE_HINT]: buf.length },
    }).length,
  };
}

const mb = (n) => (n / 1e6).toFixed(2) + ' MB';
console.log('file              raw         gzip        brotli');
for (const [file, s] of Object.entries(report)) {
  console.log(`${file.padEnd(16)}  ${mb(s.raw).padEnd(10)}  ${mb(s.gzip).padEnd(10)}  ${mb(s.brotli)}`);
}
const json = opt('--json');
if (json) writeFileSync(json, JSON.stringify(report, null, 2));

const br = report['occt-wasm.wasm'].brotli;
if (br > budgetBr) {
  console.error(`Size budget exceeded: brotli wasm ${mb(br)} > ${mb(budgetBr)}`);
  process.exit(1);
}
console.log(`Size budget ok: brotli wasm ${mb(br)} <= ${mb(budgetBr)}`);
