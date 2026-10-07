'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { fingerprintFile } = require('../core/media-identity');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'cutline-fingerprint-'));
const media = path.join(directory, 'fixture.bin');
try {
  fs.writeFileSync(media, Buffer.from('cutline-media-fixture'));
  const first = fingerprintFile(media); const second = fingerprintFile(media);
  assert.equal(first.value, second.value); assert.equal(first.byteSize, 21);
} finally { fs.rmSync(directory, { recursive: true, force: true }); }
