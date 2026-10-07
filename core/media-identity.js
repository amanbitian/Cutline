'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');

const DEFAULT_SAMPLE_BYTES = 1024 * 1024;

function fingerprintFile(filePath, { sampleBytes = DEFAULT_SAMPLE_BYTES } = {}) {
  if (!Number.isSafeInteger(sampleBytes) || sampleBytes <= 0) throw new TypeError('sampleBytes must be a positive integer.');
  const stat = fs.statSync(filePath);
  if (!stat.isFile()) throw new TypeError('Media identity requires a regular file.');
  const hash = crypto.createHash('sha256');
  const descriptor = fs.openSync(filePath, 'r');
  try {
    const first = Buffer.alloc(Math.min(sampleBytes, stat.size));
    fs.readSync(descriptor, first, 0, first.length, 0); hash.update(first);
    if (stat.size > sampleBytes) {
      const last = Buffer.alloc(Math.min(sampleBytes, stat.size));
      fs.readSync(descriptor, last, 0, last.length, Math.max(0, stat.size - last.length)); hash.update(last);
    }
  } finally { fs.closeSync(descriptor); }
  hash.update(String(stat.size)); hash.update(String(Math.trunc(stat.mtimeMs)));
  return Object.freeze({ algorithm: 'sha256-first-last-v1', value: hash.digest('hex'), byteSize: stat.size });
}

module.exports = { fingerprintFile };
