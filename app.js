let assets = [
  { id: 'media-lake', name: 'Lake aerial', meta: 'MOV · 4K', time: '00:18', thumb: 'thumb-lake', type: 'video' },
  { id: 'media-maya', name: 'Maya portrait', meta: 'BRAW · 6K', time: '00:27', thumb: 'thumb-person', type: 'video' },
  { id: 'media-forest', name: 'Forest walk', meta: 'MOV · 4K', time: '00:12', thumb: 'thumb-forest', type: 'video' },
  { id: 'media-coast', name: 'Coast detail', meta: 'MOV · 4K', time: '00:08', thumb: 'thumb-wave', type: 'video' }
];
let tracks = [
  { id: 'V3', kind: 'video', clips: [{ id: 'clip-title', name: 'NEW SEASON title', left: 45, width: 225, class: 'title', colorGrade: {} }] },
  { id: 'V2', kind: 'video', clips: [{ id: 'clip-maya', mediaId: 'media-maya', name: 'Maya portrait', left: 210, width: 205, class: 'video alt', colorGrade: {} }] },
  { id: 'V1', kind: 'video', clips: [{ id: 'clip-lake', mediaId: 'media-lake', name: 'Lake aerial', left: 0, width: 220, class: 'video', colorGrade: { exposure: 0, contrast: 100, saturation: 100, temperature: 0 } }, { id: 'clip-forest', mediaId: 'media-forest', name: 'Forest walk', left: 420, width: 165, class: 'video', colorGrade: {} }, { id: 'clip-coast', mediaId: 'media-coast', name: 'Coast detail', left: 590, width: 190, class: 'video alt', colorGrade: {} }] },
  { id: 'A1', kind: 'audio', clips: [{ id: 'clip-lake-audio', name: 'Lake aerial audio', left: 0, width: 220, class: 'audio' }, { id: 'clip-forest-audio', name: 'Forest walk audio', left: 420, width: 360, class: 'audio' }] },
  { id: 'A2', kind: 'audio', clips: [{ id: 'clip-music', name: 'Liminal — final mix', left: 45, width: 735, class: 'audio' }] }
];
const assetGrid = document.querySelector('#assetGrid');
const tracksEl = document.querySelector('#tracks');
const labelsEl = document.querySelector('#trackLabels');
const toast = document.querySelector('#toast');
const preview = document.querySelector('#mediaPreview');
const monitor = document.querySelector('#monitor');
const gradeWash = document.querySelector('#gradeWash');
const fps = 24;
const localMediaUrls = new Map();
let selectedAsset = 0;
let selectedClipId = 'clip-lake';
let playing = false;
let currentFrame = 12 * fps + 8;
let animationFrame;
let toastTimeout;
let serverRevision = 0;

function createId(prefix) { return `${prefix}-${crypto.randomUUID ? crypto.randomUUID() : `${Date.now()}-${Math.random().toString(16).slice(2)}`}`; }
function selectedClip() { for (const track of tracks) { const clip = track.clips.find(item => item.id === selectedClipId); if (clip) return { track, clip }; } return null; }
function formatTime(frame) { const seconds = Math.floor(frame / fps), f = frame % fps; return [Math.floor(seconds / 3600), Math.floor(seconds / 60) % 60, seconds % 60, f].map(value => String(value).padStart(2, '0')).join(':'); }
function timelineEnd() { return Math.max(0, ...tracks.flatMap(track => track.clips.map(clip => clip.left + clip.width))); }
function showToast(message) { toast.textContent = message; toast.classList.add('show'); clearTimeout(toastTimeout); toastTimeout = setTimeout(() => toast.classList.remove('show'), 3000); }

function applyProjectState(project) {
  if (!project || !Array.isArray(project.media) || !Array.isArray(project.tracks)) return;
  assets = project.media; tracks = project.tracks; serverRevision = project.revision || serverRevision;
  selectedAsset = Math.min(selectedAsset, Math.max(0, assets.length - 1));
  if (!selectedClip()) selectedClipId = tracks.flatMap(track => track.clips)[0]?.id || null;
  renderAssets(); renderTimeline(); syncInspector();
  document.querySelector('#jobCount').textContent = String(project.exportJobs?.filter(job => job.status === 'queued').length || 0);
}

async function refreshProject({ quiet = false } = {}) {
  try {
    const response = await fetch('/api/project', { cache: 'no-store' });
    if (!response.ok) throw new Error('Project bridge is unavailable.');
    const project = await response.json();
    if (project.revision !== serverRevision) applyProjectState(project);
    return true;
  } catch {
    if (!quiet) showToast('Start mcp-server.js to persist edits and connect an LLM.');
    return false;
  }
}

async function dispatchCommand(type, args, localFallback) {
  try {
    const response = await fetch('/api/commands', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ type, arguments: args, baseRevision: serverRevision || undefined }) });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || 'Command was rejected.');
    applyProjectState(result.project); showToast(result.summary); return true;
  } catch (error) {
    if (localFallback) localFallback(); else showToast(error.message || 'Command failed.');
    return false;
  }
}

function renderAssets() {
  assetGrid.innerHTML = assets.map((asset, index) => `<article class="asset ${index === selectedAsset ? 'selected' : ''}" data-index="${index}">
    <div class="asset-thumb ${asset.thumb || 'thumb-wave'}"><span class="duration">${asset.time || 'NEW'}</span></div><p>${asset.name}</p><small>${asset.meta || 'Imported media'}</small>
  </article>`).join('');
  document.querySelectorAll('.asset').forEach(element => {
    element.addEventListener('click', () => selectAsset(Number(element.dataset.index)));
    element.addEventListener('dblclick', () => insertSelectedAsset());
  });
}

function renderTimeline() {
  labelsEl.innerHTML = tracks.map(track => `<div class="track-label"><strong>${track.id}</strong><span class="track-switches">${track.kind === 'video' ? '◉ ◫' : 'M S'}</span></div>`).join('');
  tracksEl.innerHTML = tracks.map(track => `<div class="track" data-track="${track.id}">${track.clips.map(clip =>
    `<div class="clip ${clip.class} ${clip.id === selectedClipId ? 'selected' : ''}" style="left:${clip.left}px;width:${clip.width}px" draggable="true" data-clip-id="${clip.id}"><span>${clip.name}</span></div>`).join('')}</div>`).join('');
  initTimelineInteractions();
}

function initTimelineInteractions() {
  document.querySelectorAll('.clip').forEach(element => {
    element.addEventListener('click', () => { selectedClipId = element.dataset.clipId; renderTimeline(); syncInspector(); });
    element.addEventListener('dragstart', event => { event.dataTransfer.setData('text/plain', element.dataset.clipId); element.classList.add('dragging'); });
    element.addEventListener('dragend', () => element.classList.remove('dragging'));
  });
  document.querySelectorAll('.track').forEach(trackElement => {
    trackElement.addEventListener('dragover', event => event.preventDefault());
    trackElement.addEventListener('drop', event => {
      event.preventDefault(); const clipId = event.dataTransfer.getData('text/plain'); const targetTrackId = trackElement.dataset.track;
      const source = tracks.find(track => track.clips.some(clip => clip.id === clipId)); const clip = source?.clips.find(item => item.id === clipId); const target = tracks.find(track => track.id === targetTrackId);
      if (!clip || !target || source.kind !== target.kind) return showToast('Video and audio clips stay on compatible tracks.');
      const left = Math.max(0, event.offsetX - 30);
      dispatchCommand('timeline.move_clip', { clipId, targetTrackId, left }, () => { source.clips = source.clips.filter(item => item.id !== clipId); clip.left = left; target.clips.push(clip); renderTimeline(); });
    });
  });
}

function selectAsset(index) {
  selectedAsset = index; const asset = assets[index];
  document.querySelector('#inspectorTitle').textContent = asset?.name || 'No selection'; renderAssets();
  const source = localMediaUrls.get(asset?.id);
  if (source) { preview.src = source; preview.hidden = false; monitor.classList.add('has-media'); preview.load(); showToast(`${asset.name} loaded in the monitor.`); }
  else if (asset) showToast('Re-import this local file to preview it after re-opening the project.');
}

function insertSelectedAsset() {
  const asset = assets[selectedAsset]; const isAudio = String(asset?.type || '').startsWith('audio/'); const track = isAudio ? (tracks.find(item => item.id === 'A1') || tracks.find(item => item.kind === 'audio')) : (tracks.find(item => item.id === 'V1') || tracks.find(item => item.kind === 'video'));
  if (!asset || !track) return; const width = 180; const left = timelineEnd() + 10;
  dispatchCommand('timeline.add_clip', { mediaId: asset.id, trackId: track.id, left, width }, () => { track.clips.push({ id: createId('clip'), mediaId: asset.id, name: asset.name, left, width, class: 'video', colorGrade: {} }); renderTimeline(); });
}

function syncInspector() {
  const selection = selectedClip(); const clip = selection?.clip; if (!clip) return;
  document.querySelector('#inspectorTitle').textContent = clip.name;
  const grade = { exposure: 0, contrast: 100, saturation: 100, temperature: 0, ...(clip.colorGrade || {}) };
  for (const key of ['Exposure', 'Contrast', 'Saturation', 'Temperature']) { const lower = key.toLowerCase(); document.querySelector(`#grade${key}`).value = grade[lower]; document.querySelector(`#grade${key}Value`).textContent = key === 'Exposure' ? Number(grade[lower]).toFixed(2) : grade[lower]; }
  applyPreviewGrade(grade);
}

function applyPreviewGrade(grade) {
  const exposure = Number(grade.exposure || 0); const contrast = Number(grade.contrast ?? 100); const saturation = Number(grade.saturation ?? 100); const temperature = Number(grade.temperature || 0);
  preview.style.filter = `brightness(${Math.pow(2, exposure).toFixed(3)}) contrast(${contrast}%) saturate(${saturation}%)`;
  const alpha = Math.abs(temperature) / 850;
  gradeWash.style.background = temperature >= 0 ? `rgba(255, ${Math.max(80, 190 - temperature)}, ${Math.max(40, 160 - temperature)}, ${alpha})` : `rgba(${Math.max(40, 160 + temperature)}, ${Math.max(100, 195 + temperature)}, 255, ${alpha})`;
}

function updateGradeFromControls() {
  const selection = selectedClip(); if (!selection) return;
  const grade = { exposure: Number(document.querySelector('#gradeExposure').value), contrast: Number(document.querySelector('#gradeContrast').value), saturation: Number(document.querySelector('#gradeSaturation').value), temperature: Number(document.querySelector('#gradeTemperature').value) };
  for (const [key, value] of Object.entries(grade)) document.querySelector(`#grade${key[0].toUpperCase()}${key.slice(1)}Value`).textContent = key === 'exposure' ? value.toFixed(2) : value;
  applyPreviewGrade(grade);
  dispatchCommand('color.set_grade', { clipId: selection.clip.id, grade }, () => { selection.clip.colorGrade = { ...(selection.clip.colorGrade || {}), ...grade }; });
}

function updateTransport() { document.querySelector('#timeReadout').textContent = formatTime(currentFrame); document.querySelector('#playhead').style.left = `${105 + (currentFrame / fps) * 10}px`; }
function tick() { if (!playing) return; currentFrame = (currentFrame + 1) % Math.max(fps, Math.ceil(timelineEnd() / 10) * fps); updateTransport(); animationFrame = requestAnimationFrame(tick); }
function togglePlay() { if (!preview.hidden && preview.src) { if (preview.paused) preview.play().catch(() => showToast('Your browser could not play this source.')); else preview.pause(); return; } playing = !playing; document.querySelector('#playButton').textContent = playing ? '❚❚' : '▶'; if (playing) tick(); else cancelAnimationFrame(animationFrame); }

preview.addEventListener('play', () => { playing = true; document.querySelector('#playButton').textContent = '❚❚'; });
preview.addEventListener('pause', () => { playing = false; document.querySelector('#playButton').textContent = '▶'; });
preview.addEventListener('timeupdate', () => { currentFrame = Math.round(preview.currentTime * fps); updateTransport(); });

document.querySelector('#playButton').addEventListener('click', togglePlay);
document.querySelector('#markerButton').addEventListener('click', () => dispatchCommand('marker.add', { frame: currentFrame, name: `Marker ${formatTime(currentFrame)}` }, () => showToast(`Marker added at ${formatTime(currentFrame)} (local only).`)));
document.querySelector('#snapButton').addEventListener('click', event => { event.currentTarget.classList.toggle('active'); showToast(`Timeline snapping ${event.currentTarget.classList.contains('active') ? 'on' : 'off'}`); });
document.querySelector('#addTrackButton').addEventListener('click', () => { const id = `V${tracks.filter(track => track.kind === 'video').length + 1}`; dispatchCommand('timeline.add_track', { kind: 'video', id }, () => { tracks.unshift({ id, kind:'video', clips:[] }); renderTimeline(); }); });
document.querySelector('#splitButton').addEventListener('click', () => { const selection = selectedClip(); if (!selection) return showToast('Select a clip to split.'); dispatchCommand('timeline.split_clip', { clipId: selection.clip.id, at: selection.clip.left + Math.floor(selection.clip.width / 2) }); });
document.querySelector('#rippleButton').addEventListener('click', () => { const selection = selectedClip(); if (!selection) return showToast('Select a clip to ripple delete.'); dispatchCommand('timeline.ripple_delete', { clipId: selection.clip.id }); });
document.querySelector('#exportButton').addEventListener('click', () => dispatchCommand('export.queue', { preset: 'H.264 4K', destination: 'exports/' }, () => { document.querySelector('#jobCount').textContent = '1'; showToast('Export queued — encoder worker is the next native milestone.'); }));
document.querySelector('#fileImport').addEventListener('change', event => { [...event.target.files].forEach((file, index) => { const id = createId('media'); const localUrl = URL.createObjectURL(file); localMediaUrls.set(id, localUrl); const media = { id, name: file.name, meta: `${file.type.split('/')[0]?.toUpperCase() || 'MEDIA'} · Local`, time: 'LOCAL', thumb: 'thumb-wave', type: file.type || 'video' }; if (index === 0 && file.type.startsWith('video/')) { preview.src = localUrl; preview.hidden = false; monitor.classList.add('has-media'); preview.load(); } dispatchCommand('media.register', { media }, () => { assets.unshift(media); renderAssets(); }); }); if (event.target.files.length) showToast('Media added. Double-click an asset to place it on V1.'); });
for (const key of ['Exposure', 'Contrast', 'Saturation', 'Temperature']) document.querySelector(`#grade${key}`).addEventListener('input', updateGradeFromControls);
document.querySelector('#resetGradeButton').addEventListener('click', () => { const selection = selectedClip(); if (!selection) return; const grade = { exposure: 0, contrast: 100, saturation: 100, temperature: 0 }; dispatchCommand('color.set_grade', { clipId: selection.clip.id, grade }, () => { selection.clip.colorGrade = grade; syncInspector(); }); });
document.querySelectorAll('.nav-item[data-panel]').forEach(button => button.addEventListener('click', () => { document.querySelectorAll('.nav-item').forEach(item => item.classList.remove('active')); button.classList.add('active'); const labels = {media:'Media browser', effects:'Effects library', text:'Titles & graphics', captions:'Captions', ai:'Media Intelligence', jobs:'Render queue', settings:'Hardware & system'}; showToast(labels[button.dataset.panel]); }));
document.querySelector('#performanceChip').addEventListener('click', () => { const modes = ['Interactive', 'Playback', 'Quality Preview', 'Final Render']; const next = modes[(modes.findIndex(mode => document.querySelector('#performanceChip').textContent.includes(mode)) + 1) % modes.length]; document.querySelector('#performanceChip').innerHTML = `<span></span> ${next}`; showToast(`${next} workload profile selected`); });
document.addEventListener('keydown', event => { if ((event.ctrlKey || event.metaKey) && event.code === 'KeyZ' && document.activeElement.tagName !== 'INPUT') { event.preventDefault(); dispatchCommand(event.shiftKey ? 'project.redo' : 'project.undo', {}); return; } if ((event.ctrlKey || event.metaKey) && event.code === 'KeyY' && document.activeElement.tagName !== 'INPUT') { event.preventDefault(); dispatchCommand('project.redo', {}); return; } if (event.code === 'Space' && document.activeElement.tagName !== 'INPUT') { event.preventDefault(); togglePlay(); } if ((event.ctrlKey || event.metaKey) && event.code === 'KeyK') { event.preventDefault(); document.querySelector('#commandPalette').hidden = false; document.querySelector('#commandPalette input').focus(); } if (event.code === 'Escape') document.querySelector('#commandPalette').hidden = true; });
document.querySelector('#commandPalette input').addEventListener('keydown', event => { if (event.key === 'Enter') { showToast(`Command staged: ${event.target.value}`); event.target.value = ''; document.querySelector('#commandPalette').hidden = true; } });

renderAssets(); updateTransport();
refreshProject({ quiet: true }).then(connected => { if (connected) showToast('Connected to local project bridge. Double-click media to add it to V1.'); });
setInterval(() => refreshProject({ quiet: true }), 1200);
