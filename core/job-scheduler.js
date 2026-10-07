'use strict';

const PRIORITIES = Object.freeze({ AUDIO_DEADLINE: 0, CURRENT_FRAME: 1, PLAYBACK_PREFETCH: 2, COMPOSITE: 3, DECODE: 4, THUMBNAIL: 5, WAVEFORM: 6, ANALYSIS: 7, PROXY: 8, AI_INDEX: 9 });

class JobScheduler {
  constructor() { this.jobs = new Map(); this.sequence = 0; }
  submit({ id, priority, run, kind = 'generic' }) {
    if (typeof id !== 'string' || !id || this.jobs.has(id)) throw new Error('Job id must be unique.');
    if (!Number.isInteger(priority) || priority < 0 || priority > 9 || typeof run !== 'function') throw new TypeError('Invalid job.');
    this.jobs.set(id, { id, priority, run, kind, sequence: this.sequence++, state: 'queued', cancelled: false }); return id;
  }
  cancel(id) { const job = this.jobs.get(id); if (!job || job.state !== 'queued') return false; job.cancelled = true; job.state = 'cancelled'; return true; }
  next() { return [...this.jobs.values()].filter(job => job.state === 'queued' && !job.cancelled).sort((a, b) => a.priority - b.priority || a.sequence - b.sequence)[0] || null; }
  async runOne() { const job = this.next(); if (!job) return null; job.state = 'running'; try { const result = await job.run(); job.state = 'completed'; job.result = result; return job; } catch (error) { job.state = 'failed'; job.error = String(error?.message || error); return job; } }
  snapshot() { return [...this.jobs.values()].map(({ run, ...job }) => ({ ...job })); }
}

module.exports = { PRIORITIES, JobScheduler };
