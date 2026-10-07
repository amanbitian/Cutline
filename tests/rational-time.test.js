'use strict';
const assert = require('node:assert/strict');
const time = require('../core/rational-time');

const ntsc = time.FRAME_RATES['23.976'];
assert.deepEqual(time.fromFrames(24000, ntsc), { numerator: '1001', denominator: '1' });
assert.deepEqual(time.add({ numerator: '1', denominator: '3' }, { numerator: '1', denominator: '6' }), { numerator: '1', denominator: '2' });
assert.deepEqual(time.subtract({ numerator: '1', denominator: '2' }, { numerator: '1', denominator: '3' }), { numerator: '1', denominator: '6' });
assert.equal(time.compare({ numerator: '1000', denominator: '1001' }, { numerator: '1', denominator: '1' }), -1);
assert.equal(time.toFrameCount(time.fromFrames(123456, ntsc), ntsc).toString(), '123456');
assert.equal(time.formatTimecode(time.fromFrames(24, time.FRAME_RATES['24']), time.FRAME_RATES['24']), '00:00:01:00');
assert.deepEqual(time.displayUnitsToTime(10, time.FRAME_RATES['24']), { numerator: '1', denominator: '1' });
