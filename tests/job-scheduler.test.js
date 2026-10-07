'use strict';
const assert = require('node:assert/strict');
const { JobScheduler, PRIORITIES } = require('../core/job-scheduler');

(async () => {
  const scheduler = new JobScheduler(); const events = [];
  scheduler.submit({ id: 'proxy', priority: PRIORITIES.PROXY, run: async () => events.push('proxy') });
  scheduler.submit({ id: 'audio', priority: PRIORITIES.AUDIO_DEADLINE, run: async () => events.push('audio') });
  scheduler.submit({ id: 'frame', priority: PRIORITIES.CURRENT_FRAME, run: async () => events.push('frame') });
  await scheduler.runOne(); await scheduler.runOne(); await scheduler.runOne();
  assert.deepEqual(events, ['audio', 'frame', 'proxy']);
})().catch(error => { console.error(error); process.exitCode = 1; });
