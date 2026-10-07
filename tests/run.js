'use strict';
const { spawnSync } = require('node:child_process');
const path = require('node:path');
const tests = ['rational-time.test.js', 'job-scheduler.test.js', 'media-identity.test.js', 'parity-report.test.js'];
for (const test of tests) {
  const result = spawnSync(process.execPath, [path.join(__dirname, test)], { stdio: 'inherit' });
  if (result.status !== 0) process.exit(result.status || 1);
}
console.log(`Passed ${tests.length} core tests.`);
