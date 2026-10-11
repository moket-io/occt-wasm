// Verifies that versions.json, scripts/config.sh, the Dockerfile, package.json
// and the OCCT submodule pointer agree. Run in CI.
import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('..', import.meta.url));
const read = (p) => readFileSync(root + p, 'utf8');

const config = Object.fromEntries(
  [...(read('scripts/config-occt.sh') + read('scripts/config.sh')).matchAll(/^([A-Z_]+)="([^"]*)"/gm)].map((m) => [m[1], m[2]]),
);
const versions = JSON.parse(read('versions.json'));
const pkg = JSON.parse(read('package.json'));
const dockerfile = read('docker/Dockerfile');

const errors = [];
const expect = (name, actual, wanted) => {
  if (actual !== wanted) errors.push(`${name}: ${JSON.stringify(actual)} != ${JSON.stringify(wanted)}`);
};

expect('facade version (package.json)', pkg.version, config.FACADE_VERSION);
expect('facade version (versions.json)', versions.facade, config.FACADE_VERSION);
expect('occt tag', versions.occt.tag, config.OCCT_TAG);
expect('occt commit', versions.occt.commit, config.OCCT_SHA);
expect('occt toolkits', versions.occt.toolkits.join(' '), config.OCCT_TOOLKITS);
expect('emsdk version', versions.emsdk.version, config.EMSDK_VERSION);
expect('occt flags', versions.flags.occtCxx, config.OCCT_CXX_FLAGS);
expect('facade flags', versions.flags.facadeCxx, config.FACADE_CXX_FLAGS);
expect('link flags', versions.flags.link, config.FACADE_LINK_FLAGS);

const image = `${versions.emsdk.image}@${versions.emsdk.digest}`;
if (!dockerfile.includes(`ARG EMSDK_IMAGE=${image}`)) errors.push(`Dockerfile EMSDK_IMAGE is not ${image}`);

try {
  const tree = execFileSync('git', ['ls-files', '--stage', 'third_party/occt'], { cwd: root, encoding: 'utf8' });
  const sha = tree.split(/\s+/)[1];
  expect('third_party/occt submodule', sha, config.OCCT_SHA);
} catch {
  errors.push('cannot read the third_party/occt submodule pointer');
}

if (errors.length) {
  console.error('Version mismatch:\n  ' + errors.join('\n  '));
  process.exit(1);
}
console.log('versions consistent');
