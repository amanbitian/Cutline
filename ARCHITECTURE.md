# Cutline Architecture Blueprint

> **How to read this document.** It is the target design. Sections open with a short **Status** block saying what exists in the repository today, verified by the native test suite (751 tests). Where the text below a status block describes something not listed as built, it is design intent, not implementation. [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md) is the authoritative capability table, [GAP_ANALYSIS.md](GAP_ANALYSIS.md) the audit, and [ROADMAP.md](ROADMAP.md) the plan.

## 1. Purpose and definition

Cutline is a cross-platform professional NLE built as a set of cooperating engines and isolated services rather than one UI process that owns everything. Its target is reliable, non-destructive editing from lightweight social content through long-form, multicamera, high-resolution, HDR, and collaborative productions.

> **Product definition:** A non-destructive, frame-accurate, GPU-native professional NLE with editing, color, audio, graphics, captioning, collaboration, AI, and delivery capabilities; hardware-agnostic across AMD, NVIDIA, Intel, Apple Silicon, and CPU-only systems; with optional vendor fast paths rather than vendor dependencies.

The core performance rule is:

```text
decode once â†’ keep surfaces local â†’ process once â†’ reuse/cache aggressively â†’ copy only when unavoidable
```

## 2. Architectural goals and non-goals

| Goal | Architectural consequence |
|---|---|
| Real-time response under load | Deadline-aware scheduler, adaptive quality, prioritized I/O, prefetch, strict memory budgets |
| Correct final output | Exact rational time, sample-clocked audio, deterministic render graph, original media at export |
| Broad hardware support | Capability model, portable API boundaries, CPU fallback, compatibility rules |
| Reliable projects | Transactional database, immutable media identity, journal, snapshots, schema migrations |
| Resilient extensibility | Process-isolated plug-ins/workers, versioned SDKs, codec and AI provider boundaries |
| Collaboration | Change sets, optimistic synchronization, entity locks, conflict policies, audit history |
| Testability | Pure timeline compiler where possible, golden-frame tests, fixture media, reproducible render jobs |

Non-goals are reverse engineering proprietary project files, mimicking another editorâ€™s UI, bundling legally restricted codecs without explicit licensing, or making a cloud AI vendor a hard dependency.

### Requirement traceability and completion standard

`feature-parity.yaml` maps every capability in the product baseline to a priority, current status, and a testable acceptance statement. It is the release source of truth. Architecture text describes target design; it does not by itself constitute implementation. Before a status can change to `implemented`, the project must have:

1. A working runtime pathâ€”not only a UI, mock, schema, or API declaration.
2. Input validation, bounded failure behavior, and user-visible diagnostics.
3. Automated tests appropriate to the risk: unit/integration for commands and project state; fixture/golden/deadline tests for media processing.
4. A documented compatibility and licensing position for all codecs, plug-ins, and external providers involved.

## 3. System map

### Current working vertical slice

**Status.** The native C++20 core runs the pipeline end to end, in one process, with every public interface shaped so it could later be split across the process boundaries below:

```text
ingest (probe â†’ fingerprint â†’ journal) â†’ command bus â†’ SQLite project store (changeset undo)
   â†’ sequence snapshot â†’ timeline compiler â†’ PlaybackPlan
        â”œâ”€ decode (FFmpeg / synthetic) â†’ software compositor â”€â”
        â””â”€ decode â†’ audio mixer â†’ audio clock / WASAPI sink    â”œâ†’ playback engine / export worker â†’ encode + mux
```

Built: project store, command bus, timeline compiler, media provider interface with FFmpeg and synthetic implementations, software compositor, audio mixer and clock, WASAPI device sink, playback engine, ingest, export, and the Qt Quick desktop application. It includes docking and seven workspaces, custom timeline and monitor items, transport, inspector, jobs, LUT browser, speed-ramp editor, multicam, export queue, local transcript and caption editing, asynchronous video scopes and a realtime audio mixer; its framework-independent models and the application itself (offscreen; 31 application cases) are tested with Qt 6.8.3. An independent review on 2026-10-06 found correctness defects in several engine areas; [REMEDIATION.md](REMEDIATION.md) tracks them. Not built: the GPU shaders beyond the common subset, platform GPU executors beyond D3D11, separate worker processes, collaboration, generative AI, and plug-in hosting. `cutline_demo` and the Qt application are the verified front ends in this build.

The browser prototype in the repository root (`index.html`, `app.js`, `mcp-server.js`) is a superseded UX reference. Its pixel-coordinate timeline, `requestAnimationFrame` playback, CSS-filter grading and toast export do not satisfy the audio-clock, colour, codec or export-worker requirements and share no code with the native core.

Code map for the boxes below: **Project service** = `core/project`, `core/commands`, `core/db`; **Timeline compiler** = `timeline/`; **Media engine** = `media/`; **Render/audio graph** = `render/`, `audio/`; **Playback and export** = `playback/`, `exporter/`.

```text
                                CUTLINE APPLICATION
 â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
 â”‚ UI shell: workspaces Â· panels Â· timeline Â· monitors Â· inspector Â· commands  â”‚
 â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
                                     â”‚ validated editor commands
 â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
 â”‚ Project service           â”‚ Timeline compilerâ”‚ Collaboration service           â”‚
 â”‚ DB Â· journal Â· snapshots  â”‚ sequences â†’ plan â”‚ changesets Â· locks Â· comments  â”‚
 â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
                â”‚                      â”‚                        â”‚
      â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â–¼â”€â”€â”€â”€â”€â”€â”€â”€â”€â”  â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â–¼â”€â”€â”€â”€â”€â”€â”€â”€â”€â”  â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â–¼â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
      â”‚ Media engine      â”‚  â”‚ Render/audio graphâ”‚  â”‚ Background services â”‚
      â”‚ probe Â· demux Â·   â”‚  â”‚ color Â· effects Â· â”‚  â”‚ index Â· AI Â· proxy  â”‚
      â”‚ decode Â· time map â”‚  â”‚ composite Â· mix   â”‚  â”‚ waveform Â· export   â”‚
      â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
                â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
                                â–¼             â–¼
                  Resource/job scheduler Â· cache Â· hardware manager
                                â”‚             â”‚
                         CPU Â· GPU Â· NPU Â· decoder Â· encoder Â· disk
```

### Process boundaries

| Process/service | Responsibility | Failure policy |
|---|---|---|
| `Cutline.exe` | UI, command dispatch, read model, playback coordination | Preserve project and display recoverable failures |
| `RenderWorker` | Preview render, background renders, export queue | Restartable; a failed job does not close the editor |
| `MediaIndexer` | Probe, checksum, thumbnail, waveform, proxy scheduling | Restartable and idempotent |
| `AIWorker` | Transcription, embeddings, vision, audio analysis, provider calls | Runs jobs with progress/cancel/checkpoint boundaries |
| `PluginHost` | OFX/VST3/native extension isolation | Quarantine failed plug-in; save diagnostic data |
| `CollaborationService` | Sync, locks, comments, review versions | Offline queue; never overwrites local work silently |

An MVP may host these in a single development process, but all public APIs must behave as if they are remote/asynchronous. That keeps the later split achievable without rewrites.

## 4. Command and state model

All state changes arrive through a versioned command bus. UI gestures, shortcuts, scripts, AI suggestions, undo/redo, collaboration changes, and imported interchange documents go through the same validation path.

```text
intent â†’ validate permissions & invariants â†’ transaction â†’ persist command â†’ update read model â†’ publish events
```

Example command envelope:

```ts
type CommandEnvelope = {
  id: string;
  projectId: string;
  authorId: string;
  baseRevision: number;
  timestamp: string;
  type: string;
  payload: unknown;
  idempotencyKey: string;
};

type MoveClip = {
  type: 'timeline.moveClip';
  payload: { clipId: string; targetTrackId: string; start: RationalTime };
};
```

Required commands include create/update/delete media and bins, create sequence/track/clip, insert/overwrite/lift/extract, split, all trim families, set clip/effect/keyframe parameters, marker/caption/comment commands, attach proxy, render/export job commands, and project migration commands.

**Status.** Implemented: 65 typed commands behind a descriptor table that ties each command type to its payload, label and wire name; stateless validation on the envelope plus state-dependent validation in the store (overlap, media duration, locks, nesting and bin cycles); optimistic revision checks; idempotent replay; and an append-only journal.

**Undo is row-level changesets, not inverse commands or preimages.** While a command runs, SQLite's session extension records the exact rows it inserted, deleted or updated, and undo applies the inverted changeset. This was chosen over the two obvious alternatives for reasons that held up in practice: hand-written inverses for 38 commands would each be a place for undo to be subtly wrong, and whole-database preimages (the original design here) cost two full project copies per command. A changeset costs bytes proportional to the edit â€” a 21-command session is about 4 KB â€” and it correctly restores rows removed by foreign-key and trigger cascades, which a payload-only inverse silently loses. Replay disables the cascades (`SQLITE_CHANGESETAPPLY_FKNOACTION` plus a trigger-suppression flag), because the changeset already records every cascaded row and running the cascade again would delete them twice.

The undo stack is in memory and does not survive reopening a project. The changesets are also persisted on each journal row for crash replay, but no replay path reads them yet.

Remote changes should be represented separately from local undo stacks so a user cannot accidentally undo another editorâ€™s confirmed work.

## 5. Project format and persistence

Each project is a portable directory, not one opaque binary.

```text
project.vproj/
â”œâ”€â”€ project.json                 # format version, identity, settings, manifest
â”œâ”€â”€ project.db                   # transactional state / normalized records
â”œâ”€â”€ journal/                     # ordered commands after latest snapshot
â”œâ”€â”€ snapshots/                   # compact recovery checkpoints
â”œâ”€â”€ cache_manifest/              # cache keys and validity, not the cache payload itself
â”œâ”€â”€ attachments/                 # optional project-local graphics/documents
â””â”€â”€ interchange/                 # imported/exported OTIO, XML, EDL, AAF artifacts
```

### Current implementation

**Status.** `ProjectStore` is the only writer to a project. It creates and opens portable SQLite-WAL packages (`project.db`, `project.json`, `journal/`, `snapshots/`), applies each command inside one transaction, records it with its changeset, writes a journal file per revision, takes full snapshots on a configurable interval, and validates integrity on demand. Access is serialised by a mutex and the handle is opened `FULLMUTEX`, since the store is shared by the UI, workers and any future automation bridge. Every statement is prepared with bound parameters; none is built by concatenating user text.

**Schema v3** adds what v2 had no room for: effects with keyframed parameters, transitions, nested sequences and adjustment clips (a clip references exactly one of media, sequence or nothing, enforced by a `CHECK`), structured per-stream media metadata including colour primaries, transfer, matrix and range, an audio channel model, media duration and relink state, and markers on sequences, clips or media. A v2 project that cannot be migrated in place is refused with an explanation rather than half-converted.

Two details that are easy to get wrong:

- **Rational times are stored twice.** The exact numerator/denominator pair is authoritative. A derived integer `_ticks` column on a 254016000000/s timebase exists only so timeline ranges can be indexed and range-queried in SQL, which a fraction cannot be. One function writes both so they cannot drift.
- **Effects and markers reference their owner polymorphically** (`owner_kind` + `owner_id`) so one parameter/keyframe table serves clips, tracks, sequences and transitions. SQLite cannot enforce such a reference, so cascade is supplied by triggers and `ValidateDatabase()` checks for orphans. This is the schema's one soft spot and it has already produced a bug (undo colliding with the cascade).

The browser prototype's JSON/NDJSON persistence (`mcp-server.js`, `project-state.json`) is not the persistence layer described here and is not connected to the native store.

Use SQLite (WAL mode) or an equivalent embedded transactional database for the local project service. Every project schema has a `format_version`, migration identifier, and reversible backup/snapshot path. On open: validate â†’ backup â†’ migrate transactionally â†’ reopen through the current read model. If migration fails, preserve the original project untouched.

### Core records

| Record | Key fields |
|---|---|
| `Media` | ID, immutable fingerprint, paths, probe metadata, time map, color metadata, proxies, relink candidates |
| `Bin` | ID, parent, display properties, ordered membership |
| `Sequence` | ID, display format, frame rate, canvas, color pipeline, tracks |
| `Track` | ID, sequence, kind, order, mute/solo/lock/routing |
| `Clip` | ID, media/sequence reference, source range, timeline range, rate map, effects, link group |
| `Effect` / `Keyframe` | provider/version, parameters, animation, masks, enabled state |
| `Caption` / `Marker` | time range, content/style, author, review metadata |
| `AudioRouting` | buses, sends, plug-ins, automation |
| `Analysis` | transcript, speaker words, scenes, masks, embeddings, model/provider/version |
| `RenderCache` | content-addressed key, quality policy, dependency hash, location, expiry |
| `ChangeSet` / `Comment` | author, revision, entity scope, status, conflict metadata |

## 6. Canonical time and synchronization

Do not use `float seconds` as a project source of truth. Represent timeline times as normalized 64-bit rational values and protect every multiplication/addition from overflow.

```cpp
struct RationalTime {
  int64_t numerator;
  int64_t denominator; // always positive and reduced
};
```

The time layer must correctly represent 23.976/24000:1001, 24, 25, 29.97/30000:1001, drop-frame timecode, 30, 48, 50, 59.94/60000:1001, 60, and arbitrary source timestamps. Convert to presentation strings only at UI/API boundaries.

Audio is the master playback clock. Audio hardware consumes sample frames at a known rate; video scheduler and playhead derive presentation time from that clock. Internal processing targets 32-bit float, 48/96 kHz, sample-accurate boundaries, and real-time-safe callback code with no allocation, locks, disk I/O, or network calls.

**Status.** Implemented in `core/time`: normalised 64-bit rationals with checked arithmetic that throws on overflow rather than wrapping, frame conversion with explicit rounding modes, and 29.97/59.94 drop-frame parse and format. Conversions to ticks, frames and samples multiply and divide in an exact 128-bit intermediate and narrow once, so they throw only when the result genuinely does not fit; an earlier version multiplied the numerator by the tick rate in 64 bits and overflowed on ordinary hour-long timelines. The integer tick timebase (254016000000/s = 2â¶Â·3â´Â·7Â²Â·10â¶) is divisible by every broadcast frame rate and audio sample rate in use, including the 1001-based ones, and this is proven by test rather than assumed. `FormatTimecode` snaps to the nearest frame by default, since a display must be able to show a time between frames; pass `RoundingMode::Exact` to assert alignment.

Audio is the playback clock in the code as in the design: `AudioClock` is advanced only by samples the device accepted, and `PlaybackEngine::position()` subtracts the sink's output latency so picture is rendered for what is audible rather than what is queued. Export locks audio to video by deriving each frame's sample count from absolute time instead of accumulating: 48 kHz against 30000/1001 is 1601.6 samples per frame, and rounding each frame independently loses about a sample every three frames, a frame of desynchronisation every few minutes. A 1000-frame export at that rate produces exactly 1,601,600 samples. Not yet built: drift resync when the device rate differs from nominal, and xrun/device-change recovery.

### VFR rule

Variable-frame-rate media uses an indexed timestamp map:

```text
source packet/frame PTS â†’ source timestamp map â†’ source-to-sequence transform â†’ canonical timeline time
```

Never assume `frame index Ã— nominal duration`. Timeline playback/editing asks for source frames by timestamp/range; a chosen interpolation policy determines output sampling.

## 7. Media ingest and identity

Import is a durable job pipeline:

```text
discover â†’ probe â†’ checksum/fingerprint â†’ copy or link â†’ color/HDR interpretation
â†’ proxy decision â†’ thumbnails/waveforms â†’ media index â†’ optional AI analysis â†’ ready
```

Probe gathers container, codec, profile, pixel format, dimensions, rotation, nominal rate, VFR/CFR evidence, duration, source timecode, color primaries/transfer/matrix/range, HDR metadata, audio streams/channels/layout/rate, camera metadata, and file health information.

`MediaID` must not derive from filename or path. Use a stored content/fingerprint hash based on a versioned combination of sampled/full file hashing, byte size, duration, essential stream metadata, and optional camera/source identifiers. Maintain path aliases separately so media can be relinked after a drive or folder change. A full verification pass validates source content during copy/archive workflows.

**Status.** Implemented: probe into typed `media_streams` rows (codec, geometry, pixel aspect, exact frame rate, cadence, bit depth, field order, colour primaries/transfer/matrix/range, sample rate, channel layout); a versioned sampled SHA-256 fingerprint over the file size plus head, middle and tail spans (small files are hashed whole, and the size goes in first so files sharing sampled spans but differing in length cannot collide); duplicate detection by fingerprint; relink by fingerprint; and import recorded as two journalled, undoable commands. Media that cannot be opened is remembered as offline and renders as a gap, rather than being retried on every frame.

Built for proxies: in-process background generation, cancellation/progress, persisted association, source-fingerprint stale detection, missing-file status, monitor preference with original fallback, and an originals-only export clone. Still not built: proxy UI, thumbnails, waveforms, index, path aliases, image-sequence detection, checksum verification during copy, and the background indexer process.

Proxies are first-class mappings from source media to an approved proxy representation. A proxy has status, settings, checksum, generated time mapping, and an explicit original/proxy selection policy. Export defaults to originals and must warn before output if originals are offline.

All required editor services run in-process or as bundled local workers. No edit, save, proxy, analysis, recovery, or export path may require a network connection; [OFFLINE_OPERATION.md](OFFLINE_OPERATION.md) records the boundary.

## 8. Timeline compiler

The editorâ€™s visible timeline is not the render plan. The timeline compiler resolves sequence nesting, active ranges, source time transforms, transition overlaps, adjustment layers, keyframes, captions, effects, masks, graphics, audio routing, and multicam angles into an execution graph for a specific time interval and quality policy.

It must support:

- video, audio, subtitle/caption, and metadata tracks; arbitrary practical track counts
- linked and unlinked AV clips; sync locks; groups; compound/nested sequences
- insert, overwrite, lift, extract, ripple, roll, slip, slide, rate stretch, reverse, freeze, and time remapping
- constant/variable speed curves, frame sampling, blending, and optical-flow policies
- transitions, effect stacks, blend modes, track mattes, adjustment layers, masks, and keyframes
- source monitor in/out, three-point edits, multicam source and live angle switching
- timecode/waveform sync, markers, captions, review comments, and sequence-level metadata

Compiler output is immutable for a render request and includes dependency hashes so cached results remain correct.

**Status.** `TimelineCompiler` is a pure function over an immutable `Sequence` snapshot. It performs no I/O and is shared by monitor and export. Alongside ordering, mute/solo, transitions, nesting and adjustment layers, it now samples keyframed `time_remap` curves for eased ramps, hold freezes and reverse segments. `AudioMixer` samples the same curve per output sample. `timeline::multicam` performs offline timecode, marker or audio-correlation sync, produces angle-monitor requests, records live cuts and flattens them into ordinary clips. `LoadSequenceGraph` still builds a fixed-query snapshot and records its source revision.

Not built: dependency hashes on compiler output, track mattes, and incremental snapshot update. The snapshot is rebuilt whole after each edit, which is O(project) per edit and will not scale to feature-length timelines.

## 9. Render graph and color pipeline

Each render node declares its input/output format, CPU/GPU availability, temporal radius, region-of-interest support, cacheability, deterministic status, supported precision, HDR behavior, preferred device, and fusion compatibility.

```text
media decode â†’ input transform â†’ effects/masks/tracking â†’ grade â†’ composite/graphics
â†’ output transform â†’ display or encoder surface
```

The compiler performs node fusion, dead-node elimination, constant folding, ROI rendering, quality-driven resolution reduction, shader specialization, texture reuse, buffer aliasing, async compute scheduling, decode/render overlap, copy/render overlap, and pipeline caching.

**Status.** The CPU renderer remains the reference and fallback. A Direct3D 11 compositor executes the common editing subset on Windows, while a backend-neutral immutable graph compiler lowers sampled `PlaybackPlan` sources, effects, transitions, sequence effects, media generations, colour policy and quality policy; validates references and cycles; removes unreachable nodes; bypasses disabled unary nodes; fuses safe transform/opacity/colour/LUT/mask chains; and computes dependency hashes so unchanged branches keep reusable content addresses. The compiled graph has no executor yet. `render::Compositor` remains a fixed float pipeline with premultiplied layers, transitions, adjustment layers and sequence effects. `BlendModes` supplies 12 straight-colour modes composed correctly in premultiplied space. `MeshWarp` supplies a bilinear control grid and rolling-shutter scan-line correction. `OpticalFlow` supplies deterministic block motion with confidence, forward/backward occlusion checks and blend fallback; fractional source times can select frame blending or flow synthesis. `Graphics` defines versioned `.cutgraphic` documents for text, rectangles, ellipses and resolver-backed images and `.cuttemplate` packages with exposed controls; packages are cached and render as generators. Existing LUT, keying, grading, scopes, tracking, stabilisation, noise reduction and captions share this path. Unsupported effects and recoverable asset failures are reported rather than hidden.

It is deliberately the CPU reference: a GPU backend must reproduce it, and the golden-frame tests (PPM, per-channel tolerance, difference maps written on failure) are what would verify that. At 1080p it measures about 37 ms for one track with three effects in Release, so it is an export and test renderer rather than an interactive one. Measured cost is dominated by effects (17 effects â‰ˆ 134 ms against â‰ˆ 18 ms for one), which is why per-node caching in a real graph is the largest algorithmic win available.

Built: a Direct3D 11 compositor that renders the common subset of a playback plan on the GPU (tested against this CPU reference) and reads hardware-decoded pictures in place. The same shader pass also runs the colour tools and 3D/1D LUTs, with constants derived by `DescribeColorOp` (`render/ColorOps.h`) beside the software code (so the two share defaults and clamps), and the software compositor spreads its per-pixel passes over every core through `core/util/ParallelRows.h` (the result is bit-identical to one thread). Not built: graph lowering/execution, ROI and per-node intermediate caching, GPU filters/keys/blend modes/masks, and other graphics APIs. The CPU reference now includes render-version-gated working/display transforms, LUTs, PQ/HLG transfer handling, tone/gamut mapping and waveform/parade/vectorscope/histogram measurements. Scope measurement has a bounded, latest-frame asynchronous CPU worker and a Qt display for all four modes; GPU aggregation remains open. OpenColorIO, an ACES output transform, 10-bit end-to-end output and HDR monitoring also remain open.

Use one effect graph for preview and export. Only the quality policy changes:

| Policy | Preview | Final export |
|---|---|---|
| Resolution | adaptive full to 1/8 | full output size |
| Source | proxy/cached frame permitted | originals unless explicitly overridden |
| Temporal work | bypassed/approximate when needed | maximum configured precision |
| Sampling | low sample count / fast interpolation | configured final samples / best interpolation |
| Cache | aggressive playback cache | validated cache or freshly rendered original path |

Color management should use explicit transforms: input color space/device transform â†’ scene-linear/wide-gamut working space â†’ effects/grade/composite â†’ output device transform. Support SDR, wide gamut, PQ, HLG, LUTs, 8/10/12-bit integer and FP16/FP32 workflows according to node capabilities. Provide luma/RGB/YC waveforms, RGB/YUV parade, vectorscope, histogram, HDR waveform/nits, false color, zebra, and gamut warning scopes.

## 10. Hardware and codec capability layer

No product layer may branch on a GPU brand. It asks a capability registry for a device suited to a workload:

```cpp
DeviceHandle bestDevice(Workload workload, Codec codec, PixelFormat format, QualityPolicy quality);
```

The registry discovers CPU instruction capabilities (AVX2/AVX-512/NEON where applicable), GPUs/VRAM/driver versions, compute APIs, decoder/encoder support, NPU/AI providers, displays, storage, and external I/O. It applies signed/versioned compatibility rules and returns a scored decision with a human-readable reason.

| Platform | Primary render path | Media/AI providers | Required fallback |
|---|---|---|---|
| Windows | Direct3D 12 | OS codecs, D3D12 video, NVDEC/NVENC, AMF, Quick Sync/oneVPL, Windows ML | software decode/encode + CPU effects |
| Linux | Vulkan | VAAPI, Vulkan Video, optional vendor paths, ONNX/OpenVINO/ROCm-capable providers | software + CPU |
| macOS | Metal | VideoToolbox, Core ML | software + CPU |
| CPU-only | CPU renderer | software media and ONNX CPU | fully usable reduced-performance path |

**Status.** The `CodecProvider` boundary exists as `media::Source` and `media::SourceProvider` (read) and `media::Writer` and `media::WriterProvider` (write), with a registry for each. Nothing above them names FFmpeg. Two source providers exist â€” FFmpeg software decode and a deterministic synthetic source â€” and one writer, FFmpeg. `render::gpu` now defines backend/vendor descriptors, workload capability masks, deterministic device scoring, CPU fallback, opaque external surfaces and completion fences. Tests cover AMD, NVIDIA, Intel and Apple descriptors without vendor-specific product logic. Operating-system adapter discovery, device creation, shader execution and hardware codecs are not implemented; Windows is the only target built, and the audio sink falls back to an offline sink when no device backend is present, so the editor still opens on a machine with no sound card.

FFmpeg should be a modular demux/parser/software fallback, not the only media strategy. Vendor and operating-system codec providers belong behind a `CodecProvider` boundary. Every binary distribution must have a reviewed licensing manifest; restricted, patented, GPL, or commercial codecs must remain optional and explicitly licensed.

## 11. Resource, cache, and workload scheduling

`ResourceManager` owns RAM, each GPUâ€™s VRAM, cache disk quota, decoder/encoder session capacity, I/O throughput, and thermal/power signals where available. It allocates budgets by work class, emits pressure events, and evicts only recreatable assets. Decode surfaces remain GPU-local when later nodes share the device; copies between adapters must be measured and justified.

Cache is content-addressed, versioned, and partitioned by artifact type:

```text
packets/ decoded_frames/ gpu_frames/ thumbnails/ waveforms/ proxies/
rendered_previews/ optical_flow/ stabilization/ masks/ transcripts/ embeddings/
shader_pipelines/ analysis/ export_intermediates/
```

Cache keys include media fingerprint, source range, edit/effect dependency hashes, provider/version, device/driver constraints where needed, color pipeline, and quality policy. The cache manager maintains a manifest, LRU + cost-aware eviction, integrity checks, and user-visible clearing by category.

**Status.** `PlaybackEngine` holds a byte-budgeted LRU raw-frame cache (default 256 MB) and a bounded decode pool (two workers, eight frames ahead, at most 64 queued jobs and eight retained sources per worker by default). Every worker owns independent `Source` instances; seek/edit generations cancel obsolete queued work and suppress stale results. The cache key is media id plus requested source time, so duplicate clips share decodes. Render and optical-flow caches participate in one pressure-aware, cost-weighted RAM/disk quota manager, with a VRAM tier ready for device resources. A bounded priority scheduler defines audio, visible-frame, presentation, interactive, read-ahead, scopes, export, cache, proxy and analysis classes; it supports deduplication, stale-generation cancellation, lower-priority replacement and reserved interactive capacity. Existing decode, presenter, scope, analysis, proxy and export workers have not migrated to it, so composition remains synchronous and serial per engine. There is no GPU backend. `Compositor` remains move-only and one instance belongs to one render thread.

Playback scheduler priority:

```text
P0 audio deadline
P1 current display frame
P2 next 3â€“8 presentation frames
P3 GPU composite needed for those frames
P4 decode/I/O prefetch
P5 thumbnails
P6 waveforms
P7 stabilization/optical-flow analysis
P8 proxy generation
P9 semantic and AI indexing
```

On sustained misses, progressively reduce preview resolution full â†’ 1/2 â†’ 1/4 â†’ 1/8, first avoiding skips in the audio clock. When paused, schedule a full-quality refinement. Interactive scrubbing may request a nearest frame at reduced resolution and omit expensive temporal nodes. Export runs independently and must not starve live audio/playback.

## 12. Audio engine

The audio graph resolves clip gain/pan/time transform, clip plug-ins, track mixer, sends, buses, side-chain inputs, master effects, metering, and device output. It supports sample-accurate automation, fades/crossfades, channel mappings, loudness measurements, dialogue/music classification hooks, and VST3 through a hosted plug-in boundary. Audio analysis and speech cleanup execute outside the real-time callback and deliver render-ready artifacts.

**Status.** The audio graph routes clips through track/bus effects, sends and the master. `AudioMixer` renders from the sequence snapshot because a block spans clip edges, transitions and keyframes: effects, automation, crossfade weights and `time_remap` source positions are evaluated per sample. Constant retiming can preserve pitch with cached WSOLA chains. Curved ramps, freezes and reverse segments currently use 4-point cubic varispeed and explicitly count a fallback when maintain-pitch was requested. A remapped block scans its curve for source extrema, then performs one contiguous resolver read, avoiding a decoder call per output sample. The realtime WASAPI callback remains allocation-free and lock-free.

Not built: plug-in delay compensation, VST3 hosting, waveform generation, dialogue/music/SFX roles, variable-rate pitch-preserving time stretch, automation write modes and device-change recovery. Latency compensation must be designed before plug-in hosting.

## 13. AI and automation

AI is a set of asynchronous providersâ€”not a privileged write path.

| Service family | Capabilities |
|---|---|
| Speech | transcription, diarization, captions, translation, filler/silence detection, text editing |
| Audio | speech enhancement, noise reduction, event indexing, remix-to-length, generation providers |
| Vision | scene detection, object detection/segmentation, mask tracking, auto reframe |
| Semantic | clip/transcript/audio embeddings and search |
| Generative | video/audio/image generation and extension via provider contracts |

Every result is stored with model/provider/version, source media fingerprint, source range, confidence, and user-review status. AI requests produce proposed `CommandEnvelope` objects such as `RippleDelete` ranges; users can preview, accept, modify, or reject them. A `GenerativeProvider` supports local, cloud, and enterprise/private implementations without exposing provider details to the timeline model.

The same command API powers keyboard bindings, macros, Python/JavaScript automation, accessible UI controls, and AI. That makes edits auditable and undoable.

## 14. Plug-ins and interchange

| Extension point | Contract |
|---|---|
| Video effects/transitions | Native SDK and OFX where appropriate; declare render metadata and sandbox process |
| Audio | VST3 host with scanning, crash quarantine, latency reporting, and automation mapping |
| Import/export | Versioned codec/interchange provider interfaces |
| Panels | Web/QML-style panel API with scoped project capabilities |
| AI | Provider SDK with explicit data/consent boundaries |
| Automation | Python/JavaScript command API, never raw database access |

Interchange adapters should normalize to the internal timeline model and retain unrepresentable metadata as warnings, not silently drop it. Priorities: OpenTimelineIO, EDL, Final Cut XML, AAF/OMF where licensing and implementation permit, caption sidecar/embed/burn-in workflows, and common image-sequence/media formats.

## 15. Collaboration

### Local production mode

Many small project packages point to common shared media on managed storage. Use project/bin/sequence locks, user-visible ownership, media verification, and production-level dependency links. This model favors very large jobs and shared high-bandwidth storage.

### Cloud project mode

Clients create optimistic change sets against a revision. The service validates them, stores history, detects overlapping edits, and synchronizes deltas. Sequence and bin locks protect high-conflict ranges; comments and review versions are append-only collaboration records. Offline clients queue change sets and reconcile explicitlyâ€”never auto-discard local work.

Conflict policy must be field/entity-specific: label changes may merge; simultaneous incompatible changes to the same clip timing require a clear user resolution UI. Every resolved conflict is audited.

## 16. Security, privacy, and licensing

- Treat local media and metadata as customer data. Make cloud analysis/generative uploads explicit, scoped, and revocable.
- Keep credentials in the operating system credential store; never in project files or command journals.
- Verify plug-in signatures/identity where available; expose an unsafe plug-in warning and isolated host diagnostics.
- Store hashes for integrity and relinking; provide a clear policy for full versus sampled hashing.
- Ship a Software Bill of Materials and codec/license manifest. Review dependencies and binary configuration per target platform.
- Sanitize media metadata and plug-in IPC input; enforce resource/time limits on untrusted decoders and plug-ins.

## 17. Quality, observability, and release gates

The project requires fixture media for CFR/VFR, mixed frame rates, long duration, multichannel audio, HDR, alpha, image sequences, corrupt/truncated media, offline/relinked media, and codec/provider fallbacks.

| Test layer | Release gate |
|---|---|
| Unit | Rational time, timeline edit invariants, migration, cache key, command inversion |
| Integration | Import/probe/relink, project recovery, render graph compile, worker IPC, provider fallback |
| Golden render | Hash/tolerance comparison of known frames across supported backends and quality modes |
| Playback | Audio drop-out and frame-present deadlines under controlled load |
| Soak | Long session, repeated save/reopen, cache pressure, worker/plugin crash recovery |
| Compatibility | GPU/driver rules, codec support, HDR display path, CPU-only capability |
| Security/license | SBOM review, dependency scan, codec configuration audit |

**Status.** 751 native tests across 12 suites run under CTest, with the core library compiling clean at `/W4`. Coverage includes time-remap video/audio mapping, optical flow, rolling-shutter/mesh warp, blend modes, graphics/templates, multicam, local speech/transcript editing, realtime audio meters, playback-plan lowering, immutable render-graph compilation, device selection, priority scheduling and UI-domain models in addition to store, media, playback, export, effects and golden-frame checks. Generated fixture media and export round trips remain the independent end-to-end evidence. Tests whose premise does not apply report themselves as skipped rather than passing silently.

Absent: A/V sync measurement over a long run, soak and leak tests, audio dropout and frame-deadline tests under load, a codec compatibility matrix, GPU/driver compatibility rules, and a performance regression gate. `scripts/bench.bat` measures per-frame cost in Release but nothing fails a build on a regression.

`scripts/cross-check-parity.js` ties the manifests below to the suite: a capability may not claim `implemented` unless it names tests that exist.

Maintain `premiere_parity.yaml` as a product-planning manifest, not a claim of identical behavior:

```yaml
feature: object_mask
reference_version: 26.5
priority: P2
status: planned
acceptance: "Track a reviewed object mask through an edited source range."
```

## 18. Capability baseline

This is the functional scope manifest. It is a planning baseline, not a promise that every item ships in the initial release. Each entry should receive a priority, owner, acceptance criteria, test fixture, and release status in the parity manifest.

| Area | Capability set |
|---|---|
| Media organization | Bins, metadata, labels, smart search, markers, relink, duplicate detection, offline status |
| Import and ingest | Drag/drop, camera cards, image sequences, link/copy, checksum, transcode, color detection, verification |
| Proxy workflow | Create, attach, relink, validate, toggle, and per-sequence/proxy quality policies |
| Source editing | Source monitor, in/out, subclips, three-point edit, match frame, source patching |
| Timeline | Practical unlimited AV tracks, linked AV, nesting, compounds, adjustment layers, track targeting, sync/track locks |
| Core edits | Insert, overwrite, lift, extract, split, duplicate, paste attributes, grouped and multicam edits |
| Trimming | Ripple, roll, slip, slide, dynamic trim, snapping, J/K/L transport, keyboard remapping |
| Speed and time | Rate stretch, reverse, frame hold, speed ramps, remapping, sampling, blending, optical flow |
| Synchronization | Audio waveform sync, source timecode sync, clap/auto sync, multicam alignment |
| Effects and motion | GPU effects, transitions, keyframes, Bezier/ease/hold, transform, crop, opacity, blend modes, mattes |
| Masks and tracking | Ellipse/rectangle/pen masks, feather/expansion, object masks, planar/object tracking, stabilization |
| Compositing and graphics | Alpha, blend modes, track mattes, shapes, vector/image layers, titles, typography, templates |
| Color | Input/output transforms, grading tools, LUTs, secondaries, HDR PQ/HLG, wide gamut, professional scopes |
| Audio | Clip/track mixer, buses, sends, VST3, EQ, compression, gate, reverb, automation, loudness, ducking |
| Speech and captions | Speech-to-text, transcript edits, paper edit, caption authoring, styles, translation, single-word captions, exports |
| Media intelligence | Semantic search, visual/audio tags, scene detection, face/object labels, source transcripts, duplicate suggestions |
| Assisted/generative workflows | Command-based AI edits, subject-aware reframe, speech cleanup, music remix, provider-neutral generated media/extend APIs |
| Export | H.264/H.265/AV1 and other licensed/available codecs, image sequences, presets, queue, metadata, captions sidecar/embed/burn-in, QC |
| Interchange | OTIO, EDL, Final Cut XML, AAF/OMF where supported, project archive/collect/consolidate |
| Collaboration | Local productions, shared media, cloud projects, locks, change sets, comments, review versions, conflict resolution |
| Extensibility | OFX, VST3, importer/exporter SDKs, panels, Python/JavaScript automation, AI provider SDK |
| Workspace | Customizable workspaces, keyboard remapping, multi-monitor, external monitoring, accessibility, autosave/recovery |

## 19. Delivery roadmap and acceptance criteria

### P0 â€” trusted editorial core

**Status.** The engine half is done and verified; the interaction half is not. Done: project DB/journal/snapshots, rational time, media probe and fingerprint, CPU software decode, audio output, sequence and AV tracks, core edits, markers, undo/redo, composite and mix, and export (H.264 and lossless, verified by round trip). Not done: source and program *monitors* as UI, three-point editing, source patching, track targeting, J/K/L transport, the ruler, playhead, selection and snapping, ripple trim, autosave and crash-recovery replay. Of the 55 P0 capabilities, 29 are implemented with named tests, 7 partial and 19 not started; nineteen of the twenty-six unfinished are interaction rather than engine, so the application shell is the next dependency. See [ROADMAP.md](ROADMAP.md).

Build project DB/journal/snapshots, rational time, media probe/fingerprint, a CPU software decode path, basic audio output, source/program monitors, sequence and AV tracks, core edits, markers, undo/redo, autosave/recovery, and basic H.264 export. Acceptance: an editor can cut a short mixed-media sequence, save/reopen it exactly, recover after forced interruption, and produce a synchronized export.

### P1 â€” performance and finishing

Build D3D12/Metal/Vulkan render abstraction, hardware capability discovery, cache/resource manager, proxies, preview policies, render worker, color transform system, core GPU effects/transitions, scopes, mixer, captions, waveform generation, and detailed export presets. Acceptance: a 4K timeline with representative effects remains usable with adaptive preview and matches a final render at export quality.

### P2 â€” intelligence, extensibility, interchange

Build transcription/captions, semantic media search, paper edit, scene detection, auto reframe, OTIO/XML/EDL adapters, VST3/OFX hosting, and scripting. Acceptance: background analysis never blocks the timeline; AI suggestions are reviewable commands; imported/exported interchange surfaces clear warnings for lossy fields.

### P3 â€” scale and collaboration

Build production manifests/shared media tooling, cloud change sets, locks, comments, review versions, conflict UI, queue management, and operator diagnostics. Acceptance: two users can make non-conflicting edits, observe remote updates, resolve a conflict explicitly, and recover after one client disconnects.

### P4 â€” advanced workflow

Build production-grade tracking/object masks/stabilization/optical flow, generative provider routing, advanced HDR/external I/O, multi-GPU policies, and performance auto-tuning. Acceptance: provider failure falls back safely; hardware selection is explainable; golden-frame and deadline metrics remain within published thresholds.

## 20. Decisions

**Made, and reflected in the code:**

1. **Platform delivery: Windows first; GPU architecture: cross-platform and vendor-neutral.** The first backend is Direct3D 12 and must run through standard drivers on AMD, NVIDIA and Intel. Metal is the macOS backend for Apple Silicon and supported AMD Macs; Vulkan is the Linux/portable backend. The CPU renderer remains the semantic reference and fallback. Projects and effect definitions contain no backend or vendor identity.
2. **Native stack: a typed C++ core with a declarative UI.** Timing and render authority is native; the browser prototype is a reference only. Qt Quick is wired as an optional placeholder host. Whether a 60 fps custom timeline with 100+ tracks is viable in QML is unrecorded â€” it is, but only as a custom `QQuickItem` with a batched renderer, not delegates.
3. **FFmpeg is optional and LGPL.** The core builds and every non-decode test runs without it. The SDK is auto-detected at `.tools/ffmpeg` and an LGPL shared build is used, because a GPL build would impose GPL on the application. A reviewed licensing manifest is still required before any distribution.
4. **SQLite is vendored** (3.48 amalgamation, session extension enabled) so the build is reproducible with no package manager.
5. **Undo is changeset-based** rather than inverse-command or preimage-based (section 4).

**Still open:**

1. The exact cross-API surface-sharing contract: D3D12 with Media Foundation/D3D11VA, Metal with VideoToolbox, and Vulkan with VAAPI/Vulkan Video, including when an explicit copy is safer than interop.
2. Licensing and distribution policy for proprietary codecs, OFX/VST3, and any cloud AI provider.
3. Local-first versus hosted collaboration timeline and data residency requirements.
4. P0 file formats/codecs, minimum hardware profile, supported resolution/HDR limit, and the specific acceptance media set.
5. The level of project portability required in the first release and which interchange formats are contractual.
6. Whether render semantics are versioned per sequence (strongly recommended before colour management; see section 9).

Resolving these turns the blueprint into an actionable backlog without compromising the core architecture.

## 21. Preview, replacement, monitoring, and advanced-workflow boundaries

This section records required design boundaries for later phases. It is architecture only; none of these items changes the implementation status of the current P0 core.

### Preview rendering and Render and Replace

Preview is a quality policy over the same immutable timeline/render request used for final export. A policy may select proxy versus full-resolution media, preview resolution, temporal-effect quality, cache use, and a present deadline, but it must not change editorial timing, effect order, color-transform intent, or command semantics. The render scheduler records which policy produced an image so a low-quality preview can never be mistaken for a final render.

`RenderAndReplace` is a command-driven, reversible operation. The command freezes the selected sequence range, dependency hashes, render policy, and output specification; an isolated worker renders a derived media asset; a successful command replaces the selected timeline instance with that asset while retaining the original component and all provenance. Undo restores the original component. A failed/cancelled worker leaves the sequence unchanged. The derived asset is cacheable only when media fingerprints, settings, providers, and dependency hashes match.

### Linked and external compositions

Nested sequences, linked compositions, and future external composition adapters are dependency nodes, not opaque media files. Their adapter contracts expose a versioned input model, declared time mapping, render request, output metadata, availability state, and invalidation key. A project can choose a frozen render, a live linked dependency, or an offline placeholder; the UI must reveal that state. A live adapter cannot mutate the Cutline project database directly. Missing adapters or crashed external applications resolve to a recoverable diagnostic/placeholder, never silently change timing.

### Metadata and operational logging

Media metadata is typed, namespaced, and provenance-bearing: raw probe data, user-entered metadata, imported sidecar fields, analysis fields, and derived fields remain distinguishable. Mutation history belongs in the durable command journal; operational logs are separate append-only diagnostics carrying timestamps, component/version, correlation ID, severity, safe device/provider data, and redaction policy. Diagnostics must not store credentials or full customer media paths unless the user explicitly exports a support bundle.

### Monitoring and presentation

Source, program, reference, fullscreen, and future external-monitor outputs receive the same composited frame plus an explicit presentation transform. Monitor overlays (timecode, safe margins, dropped-frame state, proxy state, and diagnostics) are UI-layer data and never become source pixels unless an export request asks for them. External monitoring requires a device provider boundary with color transform, video mode, latency, genlock/reference state where supported, and hot-plug/device-loss notifications. Color-critical display selection and calibration are recorded with the review/export session rather than silently inherited from an arbitrary GUI display.

### AI provenance and review

AI analysis, LLM-generated command proposals, and generative assets are first-class provenance records. Each result identifies provider, model/version, parameters, consent/data-boundary state, input media fingerprints/ranges, output hashes, timestamps, confidence where meaningful, and review/accept/reject state. AI may propose normal command envelopes but never receives a raw database write path. Generated output is a derived media asset with a durable relationship to prompt/inputs/policy, and can be removed or replaced without corrupting timeline history.

Review is versioned and append-only. A review asset identifies the rendered sequence revision, range, output settings, color/presentation policy, and media hashes. Comments and drawing annotations reference an exact timeline time/frame plus reviewer identity and target revision; approvals record scope and decision. Sharing, notifications, identity, retention, and cloud storage remain outside P0 and need explicit deployment policy.

### Immersive video, professional I/O, and recovery

Immersive media must preserve projection/stereo/spatial-audio metadata through probe, timeline, monitor, and export. Reframing is a non-destructive time-varying view transform; it must not rewrite original media. These workflows are deferred until the conventional media pipeline has fixture coverage.

Professional I/O and hardware providers use capability discovery and explicit lifecycle states (`available`, `busy`, `lost`, `recovering`, `unsupported`). Device reset, GPU loss, driver failure, or audio-device change must stop unsafe work, preserve the last durable project revision, invalidate device-local caches, report the exact failed work, and offer bounded reinitialization/fallback. The editor must remain usable with an explicit CPU/software path when a supported fallback exists; it must not silently claim output/presentation correctness after a device failure.

### Atomic parity manifests

`feature-parity.yaml` stays as the broad compatibility roadmap for existing consumers. Detailed manifests in `parity/*.yaml` represent one capability per record and inherit a common contract: ID, name, priority, evidence-based status, implementation notes, measurable acceptance, unit/integration/fixture test placeholders, platform and hardware policy, known gaps, and licensing notes. The generator validates every record and emits JSON/Markdown domain counts without an overall percentage. Status transitions require evidence: `partial` needs a tested runtime component, `implemented` needs the declared acceptance coverage, and `validated` needs the relevant compatibility, failure, and performance evidence. This is intentionally stricter than a UI or schema checklist, and `scripts/cross-check-parity.js` enforces it mechanically: a record may not claim `implemented` without naming unit tests that exist in `tests/native`. The first run of that check moved 43 capabilities to `implemented` with evidence (the manifests had recorded none) and downgraded several that had been recorded optimistically â€” the effect "graph" is a fixed pipeline, transition alignment is recorded but never used to place anything, linked A/V is stored but nothing links or unlinks, and crash recovery persists changesets that nothing replays.
