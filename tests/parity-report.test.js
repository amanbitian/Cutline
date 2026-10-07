'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const root = path.resolve(__dirname, '..');
const requiredDomains = [
  'accessibility', 'ai', 'audio', 'camera-formats', 'captions', 'codecs',
  'collaboration', 'color', 'editorial', 'effects', 'export', 'graphics',
  'hardware', 'interchange', 'media', 'monitors', 'performance', 'plugins',
  'review', 'timeline', 'transitions', 'trimming', 'vr'
];

const result = spawnSync(process.execPath, [path.join(root, 'scripts', 'generate-parity-report.js')], {
  cwd: root,
  encoding: 'utf8'
});
assert.equal(result.status, 0, result.stderr || result.stdout);

const report = JSON.parse(fs.readFileSync(path.join(root, 'premiere-parity-report.json'), 'utf8'));
assert.deepEqual(report.domains.map(domain => domain.domain), requiredDomains);
assert.ok(report.total_features >= 200, 'Expected an atomic manifest, not a short broad checklist.');
assert.equal(report.capabilities.length, report.total_features);
assert.equal('overall_percentage' in report, false);
assert.match(report.weighting, /No overall percentage/);
for (const domain of report.domains) {
  assert.ok(domain.total > 0, `${domain.domain} must not be empty.`);
  assert.equal(Object.values(domain.counts).reduce((sum, value) => sum + value, 0), domain.total);
}
for (const capability of report.capabilities) {
  assert.ok(capability.id && capability.name && capability.reference_version);
  assert.ok(Array.isArray(capability.acceptance) && capability.acceptance.length > 0);
}
console.log(`Passed parity manifest aggregation test (${report.total_features} features).`);
