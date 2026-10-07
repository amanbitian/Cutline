#!/usr/bin/env node
'use strict';

/*
 * Cutline local MCP + HTTP bridge.
 *
 * Standard input/output are reserved for newline-delimited JSON-RPC messages.
 * The optional HTTP server lets the browser prototype and an MCP client work
 * against the same persisted project document without exposing it publicly.
 */
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const http = require('node:http');
const path = require('node:path');
const readline = require('node:readline');
const crypto = require('node:crypto');
const { FRAME_RATES, displayUnitsToTime } = require('./core/rational-time');

const root = __dirname;
const statePath = path.join(root, 'project-state.json');
const journalPath = path.join(root, 'project-journal.ndjson');
const snapshotsPath = path.join(root, 'snapshots');
const portArg = process.argv.indexOf('--http-port');
const httpPort = Number(portArg >= 0 ? process.argv[portArg + 1] : process.env.CUTLINE_HTTP_PORT || 4173);
const MAX_BODY_BYTES = 1024 * 1024;
const SAFE_NUMBER = value => Number.isFinite(value) && Math.abs(value) <= 1000000;
const DISPLAY_UNITS_PER_SECOND = 10;

function fail(message, code = -32602) {
  const error = new Error(message);
  error.code = code;
  throw error;
}

function clipTiming(project, clip) {
  const rate = project.project?.sequence?.frameRate || FRAME_RATES['23.976'];
  return {
    startTime: displayUnitsToTime(Number(clip.left || 0), rate, DISPLAY_UNITS_PER_SECOND),
    durationTime: displayUnitsToTime(Number(clip.width || 0), rate, DISPLAY_UNITS_PER_SECOND)
  };
}

function normalizeProject(project) {
  project.schemaVersion = Math.max(Number(project.schemaVersion || 1), 2);
  project.undoStack = Array.isArray(project.undoStack) ? project.undoStack : [];
  project.redoStack = Array.isArray(project.redoStack) ? project.redoStack : [];
  for (const track of project.tracks || []) for (const clip of track.clips || []) Object.assign(clip, clipTiming(project, clip));
  return project;
}

function state() {
  return normalizeProject(JSON.parse(fs.readFileSync(statePath, 'utf8')));
}

function projectSnapshot(project) {
  const copy = JSON.parse(JSON.stringify(project));
  delete copy.journal;
  delete copy.undoStack;
  delete copy.redoStack;
  return copy;
}

function writeSnapshot(project, label = 'autosave') {
  fs.mkdirSync(snapshotsPath, { recursive: true });
  const name = `${label}-r${String(project.revision).padStart(8, '0')}.json`;
  fs.writeFileSync(path.join(snapshotsPath, name), `${JSON.stringify(projectSnapshot(project), null, 2)}\n`, 'utf8');
  return name;
}

function writeState(next, command, before, { preserveHistory = false, forceSnapshot = false } = {}) {
  normalizeProject(next);
  if (!preserveHistory && before) {
    next.undoStack = [...(before.undoStack || []), projectSnapshot(before)].slice(-100);
    next.redoStack = [];
  }
  next.undoStack = next.undoStack || [];
  next.redoStack = next.redoStack || [];
  next.revision = (next.revision || 0) + 1;
  const entry = { id: crypto.randomUUID(), revision: next.revision, timestamp: new Date().toISOString(), ...command };
  next.journal = [...(next.journal || []), entry].slice(-500);
  const temporaryPath = `${statePath}.${process.pid}.tmp`;
  fs.writeFileSync(temporaryPath, `${JSON.stringify(next, null, 2)}\n`, 'utf8');
  fs.renameSync(temporaryPath, statePath);
  fs.appendFileSync(journalPath, `${JSON.stringify(entry)}\n`, 'utf8');
  if (forceSnapshot || next.revision % 25 === 0) entry.snapshot = writeSnapshot(next, forceSnapshot ? 'manual' : 'autosave');
  return next;
}

function findClip(project, clipId) {
  for (const track of project.tracks) {
    const clip = track.clips.find(item => item.id === clipId);
    if (clip) return { track, clip };
  }
  fail(`Unknown clip: ${clipId}`, -32004);
}

function compactProject(project) {
  return {
    revision: project.revision,
    project: project.project,
    media: project.media,
    tracks: project.tracks,
    markers: project.markers,
    captions: project.captions,
    exportJobs: project.exportJobs
  };
}

function executeCommand(command) {
  if (!command || typeof command.type !== 'string') fail('Command requires a string type.');
  const args = command.arguments || {};
  const project = state();
  if (Number.isInteger(command.baseRevision) && command.baseRevision !== project.revision) fail(`Project changed from revision ${command.baseRevision} to ${project.revision}; refresh and retry.`, -32009);
  const before = JSON.parse(JSON.stringify(project));
  let summary;

  if (command.type === 'project.undo' || command.type === 'project.redo') {
    const source = command.type === 'project.undo' ? project.undoStack || [] : project.redoStack || [];
    if (!source.length) fail(`Nothing available to ${command.type === 'project.undo' ? 'undo' : 'redo'}.`, -32005);
    const restored = source[source.length - 1];
    const current = projectSnapshot(project);
    const opposite = command.type === 'project.undo' ? project.redoStack || [] : project.undoStack || [];
    const next = { ...restored, journal: project.journal || [], undoStack: command.type === 'project.undo' ? source.slice(0, -1) : [...opposite, current].slice(-100), redoStack: command.type === 'project.undo' ? [...opposite, current].slice(-100) : source.slice(0, -1) };
    summary = command.type === 'project.undo' ? 'Undid the previous command.' : 'Redid the next command.';
    return { project: writeState(next, { type: command.type, arguments: {} }, null, { preserveHistory: true }), summary };
  }

  switch (command.type) {
    case 'timeline.move_clip': {
      if (typeof args.clipId !== 'string' || typeof args.targetTrackId !== 'string' || !SAFE_NUMBER(args.left)) fail('clipId, targetTrackId, and finite left are required.');
      const { track, clip } = findClip(project, args.clipId);
      const target = project.tracks.find(item => item.id === args.targetTrackId);
      if (!target) fail(`Unknown track: ${args.targetTrackId}`, -32004);
      if (track.kind !== target.kind) fail('A video clip cannot be moved to an audio track, or vice versa.');
      track.clips = track.clips.filter(item => item.id !== clip.id);
      clip.left = Math.max(0, Math.round(args.left));
      target.clips.push(clip);
      summary = `Moved ${clip.name} to ${target.id}.`;
      break;
    }
    case 'timeline.trim_clip': {
      if (typeof args.clipId !== 'string' || !SAFE_NUMBER(args.left) || !SAFE_NUMBER(args.width) || args.width < 8) fail('clipId, non-negative left, and width of at least 8 are required.');
      const { clip } = findClip(project, args.clipId);
      clip.left = Math.max(0, Math.round(args.left));
      clip.width = Math.round(args.width);
      summary = `Trimmed ${clip.name}.`;
      break;
    }
    case 'timeline.add_track': {
      const kind = args.kind === 'audio' ? 'audio' : args.kind === 'video' ? 'video' : fail('kind must be video or audio.');
      const prefix = kind === 'video' ? 'V' : 'A';
      const number = project.tracks.filter(track => track.kind === kind).length + 1;
      const id = args.id || `${prefix}${number}`;
      if (!/^[AV][0-9]+$/.test(id) || project.tracks.some(track => track.id === id)) fail('Track id must be a unique value like V4 or A3.');
      project.tracks.unshift({ id, kind, clips: [] });
      summary = `Added ${kind} track ${id}.`;
      break;
    }
    case 'media.register': {
      const media = args.media;
      if (!media || typeof media !== 'object' || typeof media.id !== 'string' || typeof media.name !== 'string') fail('media requires an id and name.');
      if (project.media.some(item => item.id === media.id)) fail(`Media already exists: ${media.id}`);
      project.media.unshift({ id: media.id, name: media.name.slice(0, 260), meta: String(media.meta || 'Local media').slice(0, 120), time: String(media.time || 'LOCAL').slice(0, 30), thumb: String(media.thumb || 'thumb-wave'), type: String(media.type || 'video') });
      summary = `Registered ${media.name}.`;
      break;
    }
    case 'timeline.add_clip': {
      if (typeof args.mediaId !== 'string' || typeof args.trackId !== 'string' || !SAFE_NUMBER(args.left) || !SAFE_NUMBER(args.width) || args.width < 8) fail('mediaId, trackId, non-negative left, and width of at least 8 are required.');
      const media = project.media.find(item => item.id === args.mediaId); const track = project.tracks.find(item => item.id === args.trackId);
      if (!media) fail(`Unknown media: ${args.mediaId}`, -32004); if (!track) fail(`Unknown track: ${args.trackId}`, -32004);
      const isAudio = String(media.type || '').startsWith('audio/');
      if ((track.kind === 'audio') !== isAudio) fail('Media type and target track are not compatible.');
      const clip = { id: crypto.randomUUID(), mediaId: media.id, name: media.name, left: Math.max(0, Math.round(args.left)), width: Math.round(args.width), class: isAudio ? 'audio' : 'video', colorGrade: {} };
      track.clips.push(clip); summary = `Added ${media.name} to ${track.id}.`; break;
    }
    case 'timeline.split_clip': {
      if (typeof args.clipId !== 'string' || !SAFE_NUMBER(args.at)) fail('clipId and split point are required.');
      const { track, clip } = findClip(project, args.clipId); const at = Math.round(args.at);
      if (at <= clip.left || at >= clip.left + clip.width) fail('Split point must fall inside the selected clip.');
      const second = { ...clip, id: crypto.randomUUID(), left: at, width: clip.left + clip.width - at, name: `${clip.name} (part 2)` };
      clip.width = at - clip.left; track.clips.push(second); summary = `Split ${clip.name}.`; break;
    }
    case 'timeline.ripple_delete': {
      if (typeof args.clipId !== 'string') fail('clipId is required.');
      const { track, clip } = findClip(project, args.clipId); const end = clip.left + clip.width;
      track.clips = track.clips.filter(item => item.id !== clip.id).map(item => item.left >= end ? { ...item, left: item.left - clip.width } : item);
      summary = `Ripple deleted ${clip.name}.`; break;
    }
    case 'color.set_grade': {
      if (typeof args.clipId !== 'string' || !args.grade || typeof args.grade !== 'object') fail('clipId and grade object are required.');
      const permitted = ['exposure', 'contrast', 'saturation', 'temperature', 'tint', 'highlights', 'shadows', 'lut'];
      const grade = {};
      for (const [key, value] of Object.entries(args.grade)) {
        if (!permitted.includes(key)) fail(`Unsupported grade property: ${key}`);
        if (key === 'lut' ? typeof value !== 'string' : !SAFE_NUMBER(value)) fail(`Invalid grade value for ${key}.`);
        grade[key] = value;
      }
      const { clip } = findClip(project, args.clipId);
      clip.colorGrade = { ...(clip.colorGrade || {}), ...grade };
      summary = `Updated color grade for ${clip.name}.`;
      break;
    }
    case 'timeline.set_transform': {
      if (typeof args.clipId !== 'string' || !args.transform || typeof args.transform !== 'object') fail('clipId and transform object are required.');
      const permitted = ['x', 'y', 'scale', 'rotation', 'opacity'];
      const transform = {};
      for (const [key, value] of Object.entries(args.transform)) {
        if (!permitted.includes(key) || !SAFE_NUMBER(value)) fail(`Invalid transform property: ${key}`);
        transform[key] = value;
      }
      const { clip } = findClip(project, args.clipId);
      clip.transform = { ...(clip.transform || {}), ...transform };
      summary = `Updated transform for ${clip.name}.`;
      break;
    }
    case 'marker.add': {
      if (!SAFE_NUMBER(args.frame)) fail('A finite frame value is required.');
      const marker = { id: crypto.randomUUID(), frame: Math.max(0, Math.round(args.frame)), name: String(args.name || 'Marker').slice(0, 160), color: String(args.color || 'green') };
      project.markers.push(marker);
      summary = `Added marker at frame ${marker.frame}.`;
      break;
    }
    case 'caption.add': {
      if (!SAFE_NUMBER(args.startFrame) || !SAFE_NUMBER(args.endFrame) || args.endFrame <= args.startFrame || typeof args.text !== 'string') fail('startFrame, endFrame, and text are required.');
      project.captions.push({ id: crypto.randomUUID(), startFrame: Math.round(args.startFrame), endFrame: Math.round(args.endFrame), text: args.text.slice(0, 2000), style: args.style || 'Default' });
      summary = 'Added caption.';
      break;
    }
    case 'export.queue': {
      const job = { id: crypto.randomUUID(), status: 'queued', preset: String(args.preset || 'H.264 4K'), destination: String(args.destination || 'exports/'), createdAt: new Date().toISOString() };
      project.exportJobs.push(job);
      summary = `Queued export using ${job.preset}.`;
      break;
    }
    case 'project.snapshot': {
      summary = 'Created a project recovery snapshot.';
      break;
    }
    default:
      fail(`Unsupported command type: ${command.type}`, -32601);
  }

  return { project: writeState(project, { type: command.type, arguments: args }, before, { forceSnapshot: command.type === 'project.snapshot' }), summary };
}

const tools = [
  { name: 'cutline_get_project', description: 'Read the current Cutline project, sequence, media, timeline, markers, captions, and export queue.', annotations: { readOnlyHint: true }, inputSchema: { type: 'object', properties: {} } },
  { name: 'cutline_get_timeline', description: 'Read the sequence tracks and clips, including clip IDs required by editing and color tools.', annotations: { readOnlyHint: true }, inputSchema: { type: 'object', properties: {} } },
  { name: 'cutline_undo', description: 'Undo the most recent journaled project command.', inputSchema: { type: 'object', properties: {} } },
  { name: 'cutline_redo', description: 'Redo the next available project command.', inputSchema: { type: 'object', properties: {} } },
  { name: 'cutline_create_snapshot', description: 'Write a named recovery checkpoint for the current project state.', inputSchema: { type: 'object', properties: {} } },
  { name: 'cutline_move_clip', description: 'Move one existing clip to a compatible track at a timeline pixel position. This is a validated, journaled edit.', inputSchema: { type: 'object', required: ['clipId', 'targetTrackId', 'left'], properties: { clipId: { type: 'string' }, targetTrackId: { type: 'string' }, left: { type: 'number', minimum: 0 } } } },
  { name: 'cutline_trim_clip', description: 'Set a clip’s timeline position and visual duration in the prototype. This is a validated, journaled edit.', inputSchema: { type: 'object', required: ['clipId', 'left', 'width'], properties: { clipId: { type: 'string' }, left: { type: 'number', minimum: 0 }, width: { type: 'number', minimum: 8 } } } },
  { name: 'cutline_add_track', description: 'Add a new audio or video track.', inputSchema: { type: 'object', required: ['kind'], properties: { kind: { type: 'string', enum: ['video', 'audio'] }, id: { type: 'string', description: 'Optional unique ID such as V4 or A3.' } } } },
  { name: 'cutline_add_clip', description: 'Add registered media to a compatible timeline track.', inputSchema: { type: 'object', required: ['mediaId', 'trackId', 'left', 'width'], properties: { mediaId: { type: 'string' }, trackId: { type: 'string' }, left: { type: 'number', minimum: 0 }, width: { type: 'number', minimum: 8 } } } },
  { name: 'cutline_split_clip', description: 'Split a clip at a timeline position strictly inside its bounds.', inputSchema: { type: 'object', required: ['clipId', 'at'], properties: { clipId: { type: 'string' }, at: { type: 'number', minimum: 0 } } } },
  { name: 'cutline_ripple_delete', description: 'Delete a clip and close the gap on its track.', inputSchema: { type: 'object', required: ['clipId'], properties: { clipId: { type: 'string' } } } },
  { name: 'cutline_set_color_grade', description: 'Apply non-destructive prototype color-grade parameters to a clip. Values are saved in the command journal and project state.', inputSchema: { type: 'object', required: ['clipId', 'grade'], properties: { clipId: { type: 'string' }, grade: { type: 'object', properties: { exposure: { type: 'number' }, contrast: { type: 'number' }, saturation: { type: 'number' }, temperature: { type: 'number' }, tint: { type: 'number' }, highlights: { type: 'number' }, shadows: { type: 'number' }, lut: { type: 'string' } } } } } },
  { name: 'cutline_set_transform', description: 'Apply non-destructive transform parameters to a clip.', inputSchema: { type: 'object', required: ['clipId', 'transform'], properties: { clipId: { type: 'string' }, transform: { type: 'object', properties: { x: { type: 'number' }, y: { type: 'number' }, scale: { type: 'number' }, rotation: { type: 'number' }, opacity: { type: 'number' } } } } } },
  { name: 'cutline_add_marker', description: 'Add a named timeline marker at a frame.', inputSchema: { type: 'object', required: ['frame'], properties: { frame: { type: 'number', minimum: 0 }, name: { type: 'string' }, color: { type: 'string' } } } },
  { name: 'cutline_add_caption', description: 'Add a caption to the project at a frame range.', inputSchema: { type: 'object', required: ['startFrame', 'endFrame', 'text'], properties: { startFrame: { type: 'number', minimum: 0 }, endFrame: { type: 'number', minimum: 1 }, text: { type: 'string' }, style: { type: 'string' } } } },
  { name: 'cutline_queue_export', description: 'Create a queued export job. This prototype records the job; it does not encode media yet.', inputSchema: { type: 'object', properties: { preset: { type: 'string' }, destination: { type: 'string' } } } }
];

function toolCommand(name, args) {
  const map = {
    cutline_undo: 'project.undo', cutline_redo: 'project.redo', cutline_create_snapshot: 'project.snapshot',
    cutline_move_clip: 'timeline.move_clip', cutline_trim_clip: 'timeline.trim_clip', cutline_add_track: 'timeline.add_track', cutline_add_clip: 'timeline.add_clip', cutline_split_clip: 'timeline.split_clip', cutline_ripple_delete: 'timeline.ripple_delete',
    cutline_set_color_grade: 'color.set_grade', cutline_set_transform: 'timeline.set_transform', cutline_add_marker: 'marker.add',
    cutline_add_caption: 'caption.add', cutline_queue_export: 'export.queue'
  };
  return map[name] ? executeCommand({ type: map[name], arguments: args }) : null;
}

function respond(id, result, error) {
  process.stdout.write(`${JSON.stringify(error ? { jsonrpc: '2.0', id, error } : { jsonrpc: '2.0', id, result })}\n`);
}

async function handleMcp(request) {
  const id = request.id;
  try {
    if (request.method === 'initialize') return respond(id, { protocolVersion: '2025-06-18', capabilities: { tools: {} }, serverInfo: { name: 'cutline-local', version: '0.1.0' } });
    if (request.method === 'notifications/initialized') return;
    if (request.method === 'ping') return respond(id, {});
    if (request.method === 'tools/list') return respond(id, { tools });
    if (request.method === 'tools/call') {
      const { name, arguments: args = {} } = request.params || {};
      let result;
      if (name === 'cutline_get_project') result = compactProject(state());
      else if (name === 'cutline_get_timeline') { const project = state(); result = { revision: project.revision, sequence: project.project.sequence, tracks: project.tracks }; }
      else result = toolCommand(name, args);
      if (!result) fail(`Unknown tool: ${name}`, -32601);
      const output = result.project ? { summary: result.summary, revision: result.project.revision, project: compactProject(result.project) } : result;
      return respond(id, { content: [{ type: 'text', text: JSON.stringify(output, null, 2) }], structuredContent: output });
    }
    return respond(id, null, { code: -32601, message: `Method not found: ${request.method}` });
  } catch (error) {
    return respond(id, null, { code: error.code || -32000, message: error.message || 'Unexpected server error' });
  }
}

function sendJson(response, status, body) {
  response.writeHead(status, { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store' });
  response.end(JSON.stringify(body));
}

function getBody(request) {
  return new Promise((resolve, reject) => {
    let size = 0; let data = '';
    request.on('data', chunk => { size += chunk.length; if (size > MAX_BODY_BYTES) { reject(new Error('Request body too large.')); request.destroy(); } else data += chunk; });
    request.on('end', () => { try { resolve(data ? JSON.parse(data) : {}); } catch { reject(new Error('Invalid JSON body.')); } });
    request.on('error', reject);
  });
}

const contentTypes = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8', '.json': 'application/json; charset=utf-8' };
const server = http.createServer(async (request, response) => {
  try {
    const url = new URL(request.url, `http://${request.headers.host || '127.0.0.1'}`);
    if (request.method === 'GET' && url.pathname === '/api/project') return sendJson(response, 200, compactProject(state()));
    if (request.method === 'POST' && url.pathname === '/api/commands') {
      const result = executeCommand(await getBody(request));
      return sendJson(response, 200, { summary: result.summary, project: compactProject(result.project) });
    }
    if (request.method !== 'GET' && request.method !== 'HEAD') return sendJson(response, 405, { error: 'Method not allowed.' });
    const requested = url.pathname === '/' ? 'index.html' : decodeURIComponent(url.pathname).replace(/^[/\\]+/, '');
    const file = path.resolve(root, requested);
    if (!file.startsWith(`${root}${path.sep}`) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) return sendJson(response, 404, { error: 'Not found.' });
    response.writeHead(200, { 'content-type': contentTypes[path.extname(file)] || 'application/octet-stream', 'cache-control': 'no-store' });
    if (request.method === 'HEAD') return response.end();
    fs.createReadStream(file).pipe(response);
  } catch (error) {
    sendJson(response, 400, { error: error.message || 'Bad request.' });
  }
});

server.listen(httpPort, '127.0.0.1', () => process.stderr.write(`Cutline HTTP preview: http://127.0.0.1:${httpPort}\n`));
const input = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
input.on('line', line => { if (!line.trim()) return; try { handleMcp(JSON.parse(line)); } catch { respond(null, null, { code: -32700, message: 'Parse error.' }); } });
input.on('close', () => server.close());
