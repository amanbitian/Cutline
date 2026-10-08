# Cutline Architecture

> **How to read this document.** It has two parts.
>
> **Part I, "As built"** describes the software that exists in this repository, module by module, written from the source code on 2026-10-08. Every number in it (commands, effects, tests, panels) is checked by `scripts/check-docs.js` or was counted from the code on that date. If you want to know *how something works today*, read Part I.
>
> **Part II, "Target design"** is the long-term blueprint: where the product is meant to go, with a short **Status** note on each section saying how far the code has got. If Part II describes something Part I does not, it is design intent, not implementation.
>
> Other documents: [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md) is the capability-by-capability truth table with measurements; [GAP_ANALYSIS.md](GAP_ANALYSIS.md) is the audit of what is missing; [TODO.md](TODO.md) is the work queue; [IMPLEMENTATION_GUIDE.md](IMPLEMENTATION_GUIDE.md) tells an engineer how to build each missing piece; [ROADMAP.md](ROADMAP.md) is the plan; [README.md](README.md) is the entry point.

---

# Part I: As built

## 1. What Cutline is today

Cutline is a Windows-first, offline, non-destructive video editor written in C++20 with a Qt Quick front end.

| Layer | What exists | Size (non-test C++/QML lines, approximate) |
|---|---|---|
| **Project core** | exact rational time, SQLite project package (schema v13), a typed command bus of 70 commands, row-level changeset undo, journal, snapshots, crash-recovery engine, project collection | `core/` 10,400 |
| **Media** | provider interfaces for reading and writing, FFmpeg and synthetic providers, probing, fingerprinting, ingest, verified copy, proxies | `media/` 4,400 |
| **Timeline** | immutable sequence snapshot, loader, compiler to a per-instant playback plan, multicam | `timeline/` 1,600 |
| **Render** | CPU reference compositor, Direct3D 11 compositor, colour management, grading, keys, masks, tracking, stabilisation, warps, optical flow, noise reduction, graphics, captions, transitions, scopes, caches | `render/` 11,700 |
| **Audio** | per-sample mixer, DSP (EQ, dynamics, denoise, dereverb), WSOLA time stretch, buses and sends, loudness, WASAPI output and capture | `audio/` 3,700 |
| **Playback and export** | engine with decode pool, read-ahead and caches; export worker, presets, queue, validation | `playback/` 1,400, `exporter/` 1,600 |
| **Exchange** | OpenTimelineIO, CMX 3600 EDL, Final Cut 7 XML, captions (SRT/WebVTT), offline speech-to-text | `interchange/` 3,300, `captions/` 700, `speech/` 1,000 |
| **UI logic** | framework-independent editing rules, layout, shortcuts, preferences, monitor logic, jobs | `ui/` 9,600 |
| **Application** | `Session` and the Qt Quick items and QML | `app/` 15,300 |
| **Tests** | 757 native test cases in 12 suites and 32 application test cases, golden frames | `tests/` 28,400 |

**What a person can do in the application:** create or open a project, import and ingest media (verified copy, proxies), cut on a multi-track timeline with the selection, ripple, roll, slip, slide, razor, track-select, hand and zoom tools, insert/overwrite/lift/extract/split/link, copy and paste, add effects and keyframes, draw masks and track them, grade with wheels/curves/LUTs/scopes, retime with a speed-ramp editor, put transitions on cuts, design titles from templates, cut multicam, edit by transcript, author captions, mix audio with buses, sends, automation, roles and loudness, record a voice-over, and export through a persistent queue of 21 presets.

**What it cannot yet do** is listed in section 17; the largest gaps are a source monitor, source patching and track targeting, bins/relink/search in the project panel, nested and multiple sequences in the interface, waveforms and thumbnails on clips, interchange and project-manager dialogs, a recovery prompt, third-party plug-in hosting, AAF, and offline analysis features beyond transcription.

**Platform.** Windows 10/11 only. The audio sink (WASAPI), the GPU compositor (Direct3D 11), the text rasteriser (GDI) and the build scripts (`.bat`) are Windows-specific. The core, compositor, mixer, store and most tests compile and run without them.

---

## 2. Repository map and layering

```text
video_editor/
├── core/                  Foundation. Depends on nothing else in the repository.
│   ├── time/              RationalTime (exact rationals), FrameRate, drop-frame timecode, tick timebase
│   ├── anim/              Keyframes and curves: Value, AnimatedValue (hold/linear/bezier/ease)
│   ├── model/             Vocabulary: TrackKind, SourceKind, MarkerKind, ... and RenderVersion
│   ├── commands/          CommandType, 70 payload structs, Descriptors table, Validate, labels, journal JSON
│   ├── db/                Sql.h (RAII over SQLite), ChangeSet.h (session-extension recorder, invert/apply)
│   ├── project/           ProjectStore (the only writer), Schema (create, migrate), Consolidate (collect a project)
│   ├── jobs/              PriorityScheduler (bounded, classes, deduplication, generation cancel)
│   ├── resource/          QuotaManager (RAM/disk/VRAM budgets with pressure levels)
│   └── util/              Json (write), JsonParse (read), XmlParse, Sha256, Base64, ParallelRows (row thread pool)
├── effects/               EffectRegistry (51 built-in descriptors), GraphicsDocument, MaskDocument (versioned data)
├── media/                 VideoFrame/AudioBuffer, Source and Writer interfaces + registries,
│   │                      SyntheticSource, TimestampMap, Ingest, IngestWorkflow, ProxyWorkflow, DeviceFrame
│   └── ffmpeg/            FFmpegSource (demux, decode, seek, probe, D3D11VA), FFmpegWriter (encode, mux)
├── timeline/              Sequence (snapshot), SequenceLoader, TimelineCompiler (PlaybackPlan), Multicam
├── render/                Compositor (CPU reference) and everything it calls; D3D11Compositor (GPU); caches; scopes
├── audio/                 AudioMixer, Dsp, TimeStretch, StretchCache, AudioClock, AudioSink + WasapiSink, AudioCapture
├── playback/              PlaybackEngine, DecodePool
├── exporter/              ExportWorker, ExportPresets, ExportQueue, ExportValidation
├── interchange/           Interchange (neutral model, FromProject, ApplyToProject), Otio, Edl, FcpXml
├── captions/              Caption model, SRT/WebVTT, sidecars, layout
├── speech/                Whisper (child-process adapter), Transcript (word-timed, sidecar)
├── ui/                    Pure UI logic (no Qt): EditPlanner, TimelineView, Monitor, Transport, Jobs, Layout,
│                          Preferences, Shortcuts, Inspector, LutLibrary, LookPack, Ramp, Multicam, MaskEditor,
│                          TextEdit, Transitions, GraphicsDesigner, AudioWorkflow
├── app/                   Qt Quick application: Session (+ 9 split files), items (Monitor, Timeline, Multicam,
│   │                      Scope), DockController, an image provider (LUT previews); main.cpp; demo_main.cpp; bench_main.cpp
│   └── qml/               Main.qml, Theme.qml, dialogs, 17 panels
├── tests/native/          12 suites + TestHarness.h, GoldenImage.h; tests/app/app_tests.cpp; tests/golden/*.ppm
├── parity/ + feature-parity.yaml + premiere-parity-report.*   capability manifests and generated report
├── scripts/               build/test/bench/run batch files; check-docs.js; cross-check-parity.js; generate-parity-report.js
├── perf/                  raw benchmark outputs referenced from PERFORMANCE_LOG.md
├── third_party/sqlite3/   vendored SQLite amalgamation (session extension enabled)
├── .tools/ (not committed) ffmpeg, qt, whisper, vcpkg, vst3sdk (fetched, unused)
└── index.html, app.js, styles.css, mcp-server.js, core/*.js   the superseded browser prototype
```

### 2.1 Dependency rules

```text
 app/ ──► ui/ ──► playback/ ──► render/ ─┐
   │        │         │          audio/ ─┤
   │        │         ▼                  ▼
   │        └────► timeline/ ───────► media/ ──► core/
   │                  ▲                  ▲
   └──► exporter/ ────┴──────────────────┘          interchange/, captions/, speech/, effects/ sit beside timeline/
```

- **`core/` depends on nothing in the repository.** It does not know about media, rendering or the UI.
- **`media/` never names FFmpeg above `media/ffmpeg/`.** Everything above uses `media::Source` and `media::Writer`; providers register themselves (`media::RegisterAllProviders()`).
- **`ui/` contains no Qt.** Editing rules, layout, shortcuts and preferences are plain C++ that the native suite tests without a window. `app/` is a thin skin over them.
- **Only `ProjectStore` writes the database.** Everyone else reads (the timeline loader, `Session`'s read models) under `ProjectStore::mutex()`.
- **Preview and export share the render code.** The monitor and the exporter both call `PlaybackEngine::RenderFrame` / `RenderAudioAt`, which call the same compiler, compositor and mixer.

---

## 3. Runtime architecture

Everything runs in one process today (`Cutline.exe`), with interfaces shaped so they can be split later.

### 3.1 Threads

| Thread(s) | Owner | Purpose | Talks to others through |
|---|---|---|---|
| GUI thread | Qt | QML, `Session`, painting, a 16 ms `QTimer` tick (`Session::OnTick`) | queued signals |
| Frame presenter worker (1) | `ui::FramePresenter` (`ui/Monitor.cpp`) | renders monitor frames; latest request wins; a playback mode lets a finished frame show while the next waits | one pending request, generation counter |
| Decode workers (2 by default) | `playback::DecodePool` | read-ahead of raw frames, each with its own `Source` instances | bounded queue, generation, byte-bounded frame cache |
| Read-ahead primer (1) | `PlaybackEngine` | compiles the next frames off the caller and publishes the jobs | one pending request |
| Audio producer + device (2) | `audio::WasapiSink` | producer renders mixed blocks; the device callback only copies from a **lock-free ring** and advances the clock | ring buffer, atomics |
| Scope worker (1) | `render::AsyncScopes` | downsamples the latest frame and measures waveform/parade/vectorscope/histogram | latest-frame slot |
| Optical-flow analysis (1) | `render::FlowCache` | single-flight analysis of source-frame pairs | content-addressed cache |
| Row workers (≈ hardware threads) | `core/util/ParallelRows.h` | splits per-pixel passes of the software compositor by rows; the caller also works | shared counter; a busy pool makes a second caller run its own rows |
| Priority scheduler workers | `core::jobs::PriorityScheduler` | ordered background work with classes (audio, visible frame, presentation, interactive, read-ahead, scopes, export, cache, proxy, analysis); **not yet adopted by the workers above** | queue |
| Job runner threads | `ui::JobRunner` | user-visible jobs: import, proxy, tracking, stabilisation, optical flow, transcription, loudness, pre-render | `JobContext` progress/cancel; completion queued to the GUI thread |
| Export queue worker (1) | `exporter::ExportQueue` | runs queued exports one at a time | persisted state |
| Export render worker (1 per export) | `FramePipeline` in `exporter/ExportWorker.cpp` | renders picture and sound for frame *n+1* while the caller encodes frame *n*; one packet in flight | mutex + condition variable |
| Capture thread (1) | `audio::AudioCapture` | WASAPI input to a WAV file | file |

The **rule for new code**: never block the GUI thread on decode, composition, disk or the network; never allocate or lock in the audio device callback.

### 3.2 The edit loop (one gesture)

```text
mouse/key ─► TimelineItem / Session::handleKey            (app/)
          ─► ui::PlanXxx(EditContext, ...) -> EditPlan   (ui/: pure rules; or a refusal with a reason)
          ─► Session::Apply(plan)                        (builds CommandEnvelopes with Session::Envelope)
          ─► ProjectStore::ExecuteGroup(envelopes, label)
                 1. Validate(envelope)                    stateless checks (core/commands/Command.cpp)
                 2. BEGIN; start the ChangeSet recorder
                 3. Apply(sqlite3*, envelope, payload)    state-dependent checks + SQL (ProjectStore.cpp)
                 4. check invariants (overlap, transitions, locks, cycles)
                 5. save the changeset + journal row; COMMIT
                 6. after commit, outside the lock: write the journal file; maybe write a snapshot
          ─► Session::Reload()
                 timeline::LoadSequenceGraph(store, sequence_id) -> immutable SequenceGraph
                 PlaybackEngine::UpdateSequence(graph, invalidate_media)    (keeps decoders unless media changed)
                 refresh read models (media list, inspector, history, markers, transcript, captions, mixer)
          ─► Session::RequestFrame()                      -> FramePresenter -> PlaybackEngine::RenderFrame
```

A group of commands is **one undo step**; if any command in it fails the ones before it are taken back and the failure is reported (`ExecuteGroup`).

### 3.3 Playback

```text
transport (ui::Transport) ──tick──► Session::OnTick
   audio clock leads:   PlaybackEngine::position()  = AudioClock samples − sink latency
   picture follows:     RenderFrame(position)       = Compile(plan) -> resolve frames -> Compositor/D3D11Compositor
   no audio device:     the wall clock drives the transport (and a failed device mid-play falls back the same way)

RenderFrame(t):
   plan = TimelineCompiler.Compile(graph, t)                       pure function
   for each source request: frame = cache | read-ahead | Source.ReadVideo | Source.ReadDeviceVideo (GPU)
   pictures = D3D11Compositor.Compose(plan)  if Supports(plan)  else  Compositor.Compose(plan)   (reason counted)
   RenderCache lookup by content address before composing; store after
```

### 3.4 Export

```text
ExportQueue (persistent, one job at a time) -> executor in Session (app/SessionExport.cpp)
   -> PlaybackEngine::ExportClone(format)         originals only, sequence size, software compositor, own caches
   -> exporter::Export(engine, request, progress)
         resolve request from the sequence; open media::Writer (encoder chain from the preset, first that works)
         FramePipeline worker: for frame n: RenderFrame + RenderAudioAt(sample range derived from absolute time)
         caller: WriteVideo / WriteAudio; progress/cancel; publish the file only when complete
   -> ExportValidation: reopen the file with the normal reader and compare size, rate, codec, duration, first/last picture
```

Audio is locked to video by deriving each frame's sample count from absolute time (`SampleAt`), so 48 kHz against 30000/1001 produces exactly 1,601,600 samples per 1000 frames.

### 3.5 Ingest and proxies

```text
File > Import: IngestFile (probe + sampled SHA-256 fingerprint) -> one ImportMedia command (media row + streams)
File > Ingest: IngestWorkflow plan -> CopyVerified (hash while reading, write name.part, read back and hash, rename if equal)
               -> ImportMedia -> optional proxy job
Proxy job: ProxyWorkflow (cancellable) -> ProRes 422 file under <project>/proxies -> AttachProxy (stores the source fingerprint)
Playback: a proxy is used only while its source_fingerprint still matches the media fingerprint; export never uses proxies
```

### 3.6 Recovery (engine)

```text
InspectPackage(path):  does the database open and validate?  which snapshots validate?  how far does the journal reach?
RecoverPackage(path):  healthy -> open;  else rebuild from the snapshot that reaches furthest, replay journal records idempotently,
                       move what was replaced into <package>/quarantine, open the result
```

The application does **not** call these yet (see TODO `RECOVERY-001`, guide WP-B13).

---

## 4. Project format and persistence

### 4.1 Package layout

```text
<name>.cutline/
├── project.db            SQLite (WAL) — the authoritative project
├── project.json          { formatVersion, projectId }
├── journal/              one JSON file per committed revision (command, payload, changeset) — for recovery
├── snapshots/            full database copies at an interval (default every 250 revisions, keep 8)
├── quarantine/           (only after a recovery) what the recovery replaced
├── exports/queue.json    export queue (written atomically, survives a restart)
├── proxies/              generated proxy media
├── cache/flow/           optical-flow analysis cache (disk tier)
├── transcripts/          one JSON per media (word-timed transcript, keyed by fingerprint)
└── recordings/           voice-over takes (float WAV)
```

Only `project.db` is the project; everything else can be rebuilt or is a derived artefact.

### 4.2 Schema v13 (25 tables)

| Table | Holds |
|---|---|
| `project_meta`, `project_settings` | schema version, revision counter, identity; defaults |
| `bins` | tree of bins (parent, order, name) |
| `media`, `media_streams`, `media_proxies` | imported media (path, fingerprint, duration, start timecode, missing flag, probe JSON, `bin_id`), one row per stream (codec, geometry, exact rate, cadence, bit depth, colour primaries/transfer/matrix/range, audio layout), proxy association |
| `sequences` | frame rate, size, pixel aspect, sample rate, channel layout, working/display colour space, **render version**, drop-frame, field order |
| `tracks`, `track_sends` | video/audio tracks (order, lock, mute, solo, gain, pan, layout), bus flag and output bus, sends |
| `clips` | a clip references exactly one of media, nested sequence, or nothing (adjustment/graphic) — enforced by a `CHECK`; source range, timeline start, rate, reversed, maintain pitch, **audio role**, link group, enabled |
| `transitions` | joins two clips (or one edge) on a track: kind, alignment, start, duration |
| `effects`, `effect_parameters`, `keyframes`, `effect_masks` | polymorphic owner (clip, track, sequence, transition), order, enabled, `preset_name`, parameter constants/keyframed curves, effect-owned mask documents |
| `markers` | on sequence, clip or media: range, label, kind, colour, metadata |
| `caption_tracks`, `captions` | timed text per sequence with track style |
| `multicam_groups`, `multicam_angles`, `multicam_switches` | groups, angles with sync offsets, recorded cuts |
| `graphics`, `graphic_templates` | project-held title documents and installed template packages (immutable versions) |
| `tracking_data` | stored point/planar tracks keyed to media fingerprint and range |
| `command_journal` | revision, command id, author, time, type, payload JSON, changeset BLOB, idempotency key |

Two details that are easy to get wrong:
- **Rational times are stored twice**: the exact numerator/denominator pair is authoritative; a derived integer `_ticks` column (254,016,000,000 ticks per second) exists only so ranges can be indexed in SQL. One function writes both.
- **Effects and markers reference their owner polymorphically** (`owner_kind` + `owner_id`); SQLite cannot enforce that, so cascade is supplied by triggers and checked by `ValidateDatabase()`. It is the schema's one soft spot.

### 4.3 Migrations and render versions

`Schema.cpp::MigrateSchema` runs a chain of `if (from_version <= N)` blocks, each idempotent (`CREATE TABLE IF NOT EXISTS`, guarded `ALTER TABLE`). A project from a *newer* build is refused. `sequences.render_version` (`core/model/RenderVersion.h`) records which rendering rules a sequence was made under (v1 legacy, v2 equal-power crossfades, v3 colour-managed); renderers ask for named decisions (`RenderSemantics::For(version)`); upgrading a sequence is an undoable edit. **Anything that changes output for existing content must add a version.**

### 4.4 Undo, journal, snapshots

- **Undo is row-level changesets.** While a command runs, SQLite's session extension records the exact rows inserted, deleted or updated; undo applies the inverted changeset (with `SQLITE_CHANGESETAPPLY_FKNOACTION` plus a trigger-suppression flag so cascades are not run twice). A 21-command session costs about 4 KB. The tables the recorder watches are listed in `RecordedTables()` (`core/db/ChangeSet.cpp`); **a new table must be added there**.
- **The undo stack is in memory** (default depth 256 steps) and does not survive reopening; the changesets are also persisted in `command_journal` and in the journal files and are what recovery replays.
- **Post-commit persistence cannot fail an edit.** Writing the journal file or a snapshot happens after the commit and outside the store lock; failures become warnings (`CommandResult::warnings`, `PersistenceWarnings()`).
- **Idempotency and concurrency:** each command carries `base_revision` (rejected if stale) and an `idempotency_key` (a retried command is recognised and not applied twice).

---

## 5. The command bus

A command is `CommandEnvelope { command_id, project_id, author_id, base_revision, timestamp_utc, type, payload, idempotency_key }`. `Descriptors()` in `core/commands/Command.cpp` ties each `CommandType` to its payload type (by `std::variant` index), a wire name, and an undo label; `TypeMatchesPayload` refuses a mismatch.

| Group | Commands (70 in total) |
|---|---|
| Project (2) | `CreateProject`, `RenameProject` |
| Bins (4) | `CreateBin`, `RenameBin`, `DeleteBin`, `MoveBin` |
| Media (6) | `ImportMedia`, `RemoveMedia`, `RelinkMedia`, `SetMediaStreams`, `AttachProxy`, `DetachProxy` |
| Sequences (3) | `CreateSequence`, `UpdateSequenceSettings`, `DeleteSequence` |
| Tracks (5) | `AddVideoTrack`, `AddAudioTrack`, `RemoveTrack`, `SetTrackState`, `SetTrackRouting` |
| Clips (13) | `InsertClip`, `DeleteClip`, `RippleDeleteClip`, `MoveClip`, `SplitClip`, `TrimClip`, `SetClipEnabled`, `SetClipSpeed`, `LinkClips`, `UnlinkClips`, `SetSpeedRamp`, `ClearSpeedRamp`, `SetClipAudioRole` |
| Multicam (7) | `CreateMulticamGroup`, `SetMulticamSync`, `RecordMulticamSwitch`, `RemoveMulticamSwitch`, `RenameMulticamAngle`, `DeleteMulticamGroup`, `FlattenMulticamGroup` |
| Graphics (6) | `CreateGraphic`, `UpdateGraphic`, `DeleteGraphic`, `InstallGraphicTemplate`, `RemoveGraphicTemplate`, `AddGraphicClip` |
| Analysis (2) | `SaveTrackingData`, `DeleteTrackingData` |
| Captions (6) | `AddCaptionTrack`, `RemoveCaptionTrack`, `UpdateCaptionTrack`, `AddCaptions`, `UpdateCaption`, `RemoveCaptions` |
| Transitions (3) | `AddTransition`, `RemoveTransition`, `SetTransitionTiming` |
| Effects (10) | `AddEffect`, `RemoveEffect`, `SetEffectEnabled`, `ReorderEffect`, `AddMask`, `UpdateMask`, `RemoveMask`, `SetParameterConstant`, `SetKeyframe`, `RemoveKeyframe` |
| Markers (3) | `AddMarker`, `RemoveMarker`, `UpdateMarker` |

Validation is layered: `Validate()` (stateless: ids, ranges, payload/type agreement) → `ProjectStore::Apply` (state: existence, overlap, media duration, nesting and bin cycles, **track locks**, transition rules written once in SQL and applied after every edit that can disturb one) → `ValidateDatabase()` (the whole-project invariants, run on open and on demand). Nine commands are never issued by the application today (`RenameProject`, `RenameBin`, `DeleteBin`, `MoveBin`, `DeleteSequence`, `RemoveGraphicTemplate`, `DeleteTrackingData`, `RemoveMarker`, `UpdateMarker`) and `CreateBin` and `CreateSequence` are issued only at project creation or by the demo; see TODO "Interfaces still missing for finished engines".

**Known defect:** the application stamps every command `2026-01-01T00:00:00Z` (TODO `JOURNAL-TIME-001`, guide WP-A1).

---

## 6. Time

`time::RationalTime` is a normalised 64-bit rational with checked arithmetic (overflow throws, never wraps). Conversions to ticks, frames and samples use an exact 128-bit intermediate and narrow once. `FrameRate` is a rational too. The tick timebase 254,016,000,000/s is divisible by every broadcast frame rate (including 1001-based) and every audio sample rate in use (proven by test). Drop-frame timecode (29.97/59.94) parses and formats; `FormatTimecode` snaps to the nearest frame unless `RoundingMode::Exact` is given. **`double` seconds appear only at the QML boundary.** Variable-frame-rate sources are resolved through a `TimestampMap` built from real packet timestamps (never `index × nominal duration`).

---

## 7. Media layer

- **Interfaces** (`media/Source.h`, `media/Writer.h`): `Source` offers `probe()`, `ReadVideo(time)`, `ReadDeviceVideo(time)` (GPU, optional), `ReadAudio(time, rate, channels, frames)` (zero-padded, sample-exact at any start), `timestamps()`. One thread may read video while another reads audio of the same `Source`; two threads in one read method are not supported (the decode pool gives every worker its own instances). `SourceRegistry` and `WriterRegistry` choose a provider by path/settings.
- **FFmpeg provider** (`media/ffmpeg/`): separate demuxer and decoder per stream (so alternating audio and video reads cannot discard each other's packets); source time zero is the earliest timestamp in the container; audio is resampled with an exact index for coarse time bases (Matroska); 8-bit sources decode to `Rgba8`, deeper to `Rgba16`; YUV→RGB conversion is delegated to swscale with explicit matrix and range; optional Direct3D 11 video acceleration returns NV12/P010 textures (`media::DeviceFrame`). Encoder side: options per codec, container options, hardware encoder chains.
- **Synthetic provider** (`media/SyntheticSource.h`): deterministic generated video and audio addressed by `synthetic:` URIs — frames that encode their own index, closed-form tones — what makes retime, reverse and VFR behaviour testable without media files.
- **Pixel formats**: `Rgba8`, `Rgba16`, `RgbaF32` (`media/VideoFrame.h`, 64-byte-aligned rows); audio is planar `float` (`media/AudioBuffer.h`).
- **Ingest** (`media/Ingest.h`, `IngestWorkflow`): probe, sampled SHA-256 fingerprint (size + head/middle/tail; small files whole), duplicate detection, one `ImportMedia` command; `CopyVerified` copy with read-back hash; proxy choices (automatic by an offline policy, Full HD, HD, light, none).
- **Proxies** (`media/ProxyWorkflow.h`): cancellable background generation, association keyed to the source fingerprint, ready/stale/missing status, automatic fallback to the original, originals-only export.
- **Not built:** thumbnails, waveform peak files, path aliases, image-sequence detection, camera RAW providers, packet-level access (for smart render).

---

## 8. Timeline model and compiler

- **`timeline::Sequence`** (`timeline/Sequence.h`): immutable snapshot — tracks (clips sorted by start with cached tick bounds, transitions, track effects, routing), sequence effects, caption tracks, colour spaces, render version, source revision. **`SequenceGraph`**: the root plus every sequence it nests (index 0 is the root). **`LoadSequenceGraph`** builds it in a fixed number of queries regardless of clip count; it is rebuilt whole after each edit (O(project); see `PERF-SNAPSHOT-001`).
- **`TimelineCompiler::Compile(graph, t, options)`** is a **pure function** (no I/O, no database): for the instant `t` it returns a `PlaybackPlan` — `video` and `audio` `SourceRequest`s in composite order (source time already reversed/retimed, sampled effect values at clip-local time, depth for nesting), active `TransitionMix`es with progress, sequence-level effects, and captions to burn in. It honours mute/solo through `TrackContributes` (shared with the mixer), follows nested sequences to a depth bound, applies speed-remap curves (ramps, holds, reverse segments), and returns read-ahead boundaries.
- **Multicam** (`timeline/Multicam.h`): sync by audio cross-correlation (first 90 s at 8 kHz), timecode, markers or by hand; angle-monitor requests; live cut recording; flattening into ordinary linked clips.

---

## 9. Render

### 9.1 Compositor (CPU reference)

`render::Compositor::Compose(plan, resolver, statistics)` draws one layer per track bottom to top in **premultiplied float RGBA** (`render/Layer.h`, with dirty rectangles), mixes a transition into a single layer, applies adjustment clips to everything already composited, applies sequence effects, converts working→display colour, draws captions, then flattens to the output format. A single plain layer takes a fast path that skips the canvas. All per-pixel passes use `ParallelRows` and are bit-identical to one thread. The CPU compositor **defines** correct output; the golden-frame tests and the GPU parity tests pin it.

### 9.2 Effects (51 registered)

`effects/EffectRegistry` describes every built-in once (id, name, category, medium, parameters with dimension, default, range, unit, keyframeability, asset kind, CPU/GPU availability). Commands, the compositor, the mixer and the inspector all read it.

| Category | Video effects (41) |
|---|---|
| Transform | Opacity, Motion, Transform, Crop |
| Color | Basic Color (`grade`, and the legacy duplicate `lumetri`), Color Wheels, Curves, Hue Curves, Color Adjust, HSL Secondary, Channel Mixer, Black & White, Tint, Creative LUT, Input Colour Space |
| Blur & Sharpen | Box Blur, Gaussian Blur, Directional Blur, Sharpen, Unsharp Mask |
| Stylize | Vignette, Glow, Drop Shadow, Posterize |
| Distort | Lens Correction, Wide-Angle Correction, Stabilizer, Rolling-Shutter Repair, Mesh Warp, Wave Warp, Bulge |
| Keying | Chroma Key, Luma Key |
| Restoration | Noise Reduction |
| Time | Time Remap, Frame Interpolation |
| Composite | Blend Mode (12 modes) |
| Generate | Solid Color, Title / Graphic, Motion Graphics Template |

Audio effects (10): Volume, Gain, Pan, Equalizer, Compressor, Limiter, Noise Gate, Auto Duck, Noise Reduction, Reverb Reduction.

An unknown effect type is **kept and serialised** and reported by the compositor/mixer rather than dropped.

### 9.3 Subsystems

| Subsystem | Files | Notes |
|---|---|---|
| Colour management | `render/ColorManagement.*`, `ColorOps.h`, `ColorEffects.cpp` | spaces: Rec.709, sRGB, P3-D65, Rec.2020, PQ, HLG, linear variants, ACEScg/AP0/ACEScc/ACEScct, ARRI LogC3, S-Log3, V-Log, Log3G10; input→working→display; tone and gamut mapping; version-gated (v3). **No OpenColorIO, no ACES RRT/ODT, no 10-bit end to end, no HDR monitoring.** |
| LUTs | `render/CubeLut.*`, `ui/LutLibrary`, `ui/LookPack` | `.cube` 3D and 1D, trilinear (no tetrahedral), domain bounds, a pack of nine generated looks |
| Grading | `ColorEffects.cpp` | wheels, tone/hue curves (five-point), white balance, vibrance, shadows/highlights, HSL secondary; all also on the GPU |
| Keys, masks | `Filters.cpp`, `effects/MaskDocument`, `ui/MaskEditor` | chroma/luma key with spill and cleanup; rectangle/ellipse/Bézier masks with feather, expansion, opacity, invert, add/subtract/intersect, keyframes |
| Tracking, stabilisation | `Tracking.*`, `TrackingData.*`, `Stabilizer.*`, `RollingShutter.*`, `MeshWarp.*` | point and planar trackers, RANSAC similarity stabilisation, rolling-shutter measurement and repair |
| Time and flow | `OpticalFlow.*`, `FlowCache.*`, `NoiseReduction.*` | block optical flow with confidence/occlusion, content-addressed analysis cache, temporal+spatial noise reduction |
| Graphics, text, captions | `Graphics.*`, `TextRaster.*` (GDI), `CaptionRender.*` | one rasteriser for titles and captions so monitor and export agree |
| Transitions | `Transitions.*` | 23 kinds: dissolves (cross, additive, film), dips (black, white), wipes (4 directions, 2 barn doors, clock), irises (box, circle), pushes (4), slides (4), zoom dissolve |
| Scopes | `Scopes.*` | luma waveform, RGB parade, vectorscope, RGB/luma histogram; asynchronous worker |
| Render cache | `RenderCache.*` | frames addressed by a hash of everything they are made from; memory + checksummed disk tiers; invalidation by address |
| Resource budgets | `core/resource/QuotaManager.h` | one pressure-aware quota over render frames and optical-flow entries (RAM/disk; a VRAM tier reserved); pressure levels reduce budgets to 75% and 50% |
| Render graph | `RenderGraph*`, `RenderGraphBuilder` | immutable, backend-neutral nodes with dependency hashes, dead-node elimination, safe fusion — **built and tested but not executed by anything** |

### 9.4 GPU compositor (Direct3D 11)

`render/D3D11Compositor.h` runs **compute shaders** (one HLSL source string) for: layers with the motion transform, crop, opacity and primary grade; all colour tools (up to 16 operations per clip); 3D and 1D LUTs (up to 8 per clip, ≤ 129 points per side); solid generators; adjustment clips; cross dissolves; sequence effects — premultiplied source-over in half float, from 8/16/32-bit pictures or from **hardware-decoded NV12/P010 planes read in place**. `Supports(plan)` refuses anything else with a reason (filters, keys, blend modes, masks, graphics, captions, colour conversion between spaces, optical flow, other transitions); the engine then composes that whole frame in software and counts it. A device error switches the engine to software for good. Targets are allocated lazily (about 190 MiB at 4K in the common case). The finished picture is **read back to memory** for the monitor. Exports never use it (`ExportClone`). Verified on one AMD adapter.

---

## 10. Audio

- **Mixer** (`audio/AudioMixer.h`): renders from the sequence snapshot **per sample**, so clip edges, crossfades, automation and speed-remap curves land exactly and any block size tiles to the same samples. Track/bus graph with sends (pre/post fader), routing loops refused, solo through buses, per-track meters (peak/RMS/LUFS).
- **DSP** (`audio/Dsp.*`): processors are functions of a bounded neighbourhood (`memory`, `lookahead`), so output does not depend on how the render was cut: EQ (3-band), compressor, limiter, gate, side-chain duck, spectral denoise, dereverb; BS.1770/EBU R128 loudness (`MeasureLoudness`, streaming `LoudnessMeter`) with true peak. Stateful effects are applied on a 4096-sample grid of cells.
- **Time stretch** (`audio/TimeStretch.h`, `StretchCache`): WSOLA pitch preservation along constant or curved retiming within 0.25–4×; holds, reverses and extremes fall back to varispeed and are counted.
- **Clock and output**: `AudioClock` advances only by samples the device accepted; `WasapiSink` (shared mode) with a lock-free ring; an `OfflineSink` for export and tests; `AudioCapture` for voice-over. Underruns are counted.
- **Workflow** (`ui/AudioWorkflow`): roles (dialogue, music, effects, ambience) with starting effect chains, loudness targets (EBU R128, ATSC A/85, streaming −14, podcast −16), "match this sound" / "match the programme", automation modes (read/write/touch/latch).
- **Not built:** plug-in hosting and delay compensation, reverb/delay/modulation/pitch effects, input monitoring, control surfaces, waveform drawing.

---

## 11. The playback engine

`playback::PlaybackEngine` owns the mutable state around the pure pieces: the current immutable `SequenceGraph` (swapped atomically by `UpdateSequence`; renders in progress finish on the snapshot they started with), open decoders (shared where safe), a byte-bounded LRU raw-frame cache keyed by media id and source time (default 256 MB), the bounded `DecodePool` (2 workers, 8 frames ahead, ≤ 64 queued jobs, ≤ 8 retained sources per worker), the render cache, the GPU compositor, nested-sequence composition, proxy selection with automatic fallback, offline media remembered rather than retried, and statistics. Seeks and edits bump a generation so stale decode results and audio blocks are discarded. Hardware-decoded media does not schedule duplicate software read-ahead. `ExportClone` and `OriginalQualityClone` create independent engines that ignore proxies and the GPU.

---

## 12. Export

`exporter/ExportPresets.h` — **21 presets** resolved to the encoders that work on this machine: H.264 high/balanced, HEVC 8 and 10-bit, AV1, VP9/Opus, MPEG-2 4:2:2 MXF, ProRes (Proxy, LT, 422, HQ, 4444), DNxHR (SQ, HQ, HQX, 444), FFV1 10-bit, WAV 24-bit, FLAC, AAC, MP3. A preset names a chain (graphics-card encoders first when preferred, then software); each is test-opened (`media::TestEncoder`) and the first that works is used with the skipped ones reported. `ExportQueue` persists to `<project>/exports/queue.json`; jobs run one at a time, can be paused, cancelled, retried, reordered; a job that was running when the application died returns as *interrupted* with its partial file removed. `ExportWorker` writes to a unique temporary and publishes only a finished file (an existing file is never touched by a cancelled or failed export). `ExportValidation` reopens the finished file and compares it with what was asked.

**Not built:** export at a size other than the sequence's (refused), image sequences, stills, GIF, smart render, render-and-replace, watch folders, HDR metadata, certified broadcast wrappers.

---

## 13. Interchange, captions, speech

- **Interchange** (`interchange/`): a neutral `Timeline` model; `FromProject(store, sequence_id, report)` reads a sequence (and everything it nests) into it, `ApplyToProject` writes one into the project as ordinary commands (one command at a time, so an import is not yet one undo step). Writers/readers for **OpenTimelineIO**, **CMX 3600 EDL** (dissolves, speed, locators, drop frame) and **Final Cut 7 XML** (`xmeml`). Everything that cannot be represented is listed in a `Report` (`Note`/`Warning`/`Error`), never silently dropped. **No application menu uses any of this yet.** AAF/OMF are not started.
- **Captions** (`captions/`): cues and track styles as project data, SubRip and WebVTT import/export (cue errors skipped with line numbers), sidecar files, layout, and burn-in through the shared rasteriser.
- **Speech** (`speech/`): `Whisper` runs whisper.cpp's command-line tool as a cancellable child process on a 16 kHz mono WAV and reads its JSON; `Transcript` holds words with times in media time, speakers and corrections, kept as a sidecar keyed by the media fingerprint; `ui::TextEdit` maps words onto every clip that shows the media and plans ripple/lift removals, filler and pause removal, and caption creation. Engine and model are not bundled.

---

## 14. UI architecture

### 14.1 Logic in `ui/` (no Qt)

| Module | Responsibility |
|---|---|
| `EditPlanner` | every editing rule as a function returning an `EditPlan`: move, trim (normal/ripple), roll, slip, slide, split, delete (ripple), lift, extract, remove ranges, place (insert/overwrite), insert edit from a source range, three-point resolution, enable/link/unlink, speed |
| `TimelineView` | viewport (time↔pixels), ruler, track rows and clip boxes (virtualised for hundreds of tracks), hit testing, selection model, snapping |
| `Monitor` | render size by quality and window, picture placement and zoom (`FrameRect`), overlays (safe margins, centre, thirds), `FramePresenter` worker |
| `Transport` | play, J/K/L shuttle (to 8×), step, loop, driven by a caller-supplied clock |
| `Layout` | docking tree (tabs, splits, floating panels), panel registry (17 panels), 8 built-in workspaces, JSON round trip |
| `Shortcuts` | 109 registered application commands, chord parsing, keymap with conflict handling, search |
| `Preferences` | 35 typed, validated, observable settings, atomic save |
| `Inspector` | per-clip effect list, parameter values at the playhead, add/remove/reorder/enable, keyframe toggle, reset |
| `Jobs` | job tracker and runner with progress and cancel |
| `Ramp`, `Multicam`, `MaskEditor`, `TextEdit`, `Transitions`, `GraphicsDesigner`, `AudioWorkflow`, `LutLibrary`, `LookPack` | the models behind each feature's interface |

### 14.2 The application (`app/`)

- **`Session`** is the single object QML talks to (context properties `session` and `appSession`). Its implementation is split by subject: `Session.cpp` (project lifecycle, editing, selection, transport, inspector, command dispatch `trigger`, key handling), `SessionAudio`, `SessionCaptions`, `SessionExport`, `SessionGraphics`, `SessionIngest`, `SessionMasks`, `SessionMulticam`, `SessionTranscript`, `SessionTransitions`. It exposes read models as `Q_PROPERTY` lists/maps and actions as `Q_INVOKABLE` methods; edits go through `Apply(plan)` / `Run(type, payload)`.
- **Qt Quick items**: `TimelineItem` (custom `QQuickPaintedItem`: ruler, headers, clips, gestures, ghost clips, snap line, speed-ramp handles), `MonitorItem` (picture, overlays, mask handles, timecode), `MulticamItem` (angle monitor), `ScopeItem`, `DockController`; image provider for LUT previews.
- **QML**: `Main.qml` (menus, workspace bar, command palette, full-screen window), `Theme.qml` (singleton palette), dialogs (Preferences, Shortcuts, Export, Ingest, Transition, Speed, Ramp, Colour settings, Multicam set-up, Command palette), `DockArea`/`PanelGroup` (docking, tab strip, a `Loader` that maps a panel id to a file), and 17 panels: Project, Program Monitor, Timeline, Effect Controls, Effects, History, Background Jobs, Scopes, Audio Mixer, Essential Sound, Markers (**placeholder**), Multicam, Export Queue, Transcript, Captions, Titles, Graphic Designer.
- **Workspaces**: Editing, Assembly, Color, Audio, Multicam, Text, Graphics, Effects (`Alt+Shift+1..8`).
- **Keyboard**: keys not consumed by a text field reach `Session::handleKey`, which looks the chord up in the keymap and runs `trigger(command_id)`. Transport/mark commands act on the program monitor (there is no focus routing to a source monitor yet).
- **Headless testing**: `main.cpp` has `--screenshot`, `--do trigger:|select:|seek:|effect:|tool:|workspace:`, `--demo`, `--open`, `--config`, `--no-audio`, `--delay`.

### 14.3 Adding to the UI

The step-by-step recipes (a command, a planner, a panel, a preference, an effect) are in [IMPLEMENTATION_GUIDE.md](IMPLEMENTATION_GUIDE.md), section 3.

---

## 15. Build, test and tooling

- **Build**: CMake 3.24+, Ninja, MSVC (C++20, `/W4`, core compiles clean). Targets: `cutline_core` (static library), `Cutline` (Qt application), `cutline_demo`, `cutline_bench`, 12 native test executables, `cutline_app_tests` (Qt). Options: `CUTLINE_BUILD_NATIVE_APP`, `CUTLINE_BUILD_TESTS`, `CUTLINE_ENABLE_FFMPEG`, `CUTLINE_USE_VENDORED_SQLITE`, `CUTLINE_FFMPEG_ROOT`. Fixtures (`cmake/Fixtures.cmake`) are generated at configure time with the bundled `ffmpeg`.
- **Dependencies**: SQLite 3.48 (vendored, session extension), FFmpeg shared LGPL (optional, `.tools/ffmpeg`), Qt 6.8.3 (optional, `.tools/qt`), whisper.cpp (optional, `.tools/whisper`), Windows system libraries (D3D11, DXGI, WASAPI, GDI).
- **Test layers**: native suites (`core` 48, `store` 129, `timeline` 78, `media` 69, `render` 151, `audio` 81, `playback` 50, `export` 36, `interchange` 31, `ui` 62, `gpu` 18, `speech` 7 — 760) using a small harness (`CUTLINE_TEST`, `CHECK`, `CHECK_EQ`, `CHECK_THROWS`, `SKIP_UNLESS`, `SKIP_INAPPLICABLE`; `CUTLINE_ONLY`, `CUTLINE_TRACE`, `CUTLINE_STRICT`); golden frames (`tests/golden/*.ppm`, `CUTLINE_UPDATE_GOLDEN=1`); the application suite (32 test functions) drives the real `TimelineItem`, `MonitorItem` and `Session` on an offscreen window. Tests whose premise does not hold report **skip**, never a silent pass.
- **Documentation and parity checks**: `scripts/check-docs.js` (test counts, command/effect/application-test counts, links, source paths, mojibake, stray control characters), `scripts/cross-check-parity.js` (a capability may claim `implemented` only if it names tests that exist; `--execute` also requires them to have passed in `build/native`), `scripts/generate-parity-report.js`.
- **Benchmarks**: `cutline_bench` modes `--gpu`, `--color`, `--flow`, `--range`, `--playback`; raw output in `perf/`, summaries in [PERFORMANCE_LOG.md](PERFORMANCE_LOG.md).

---

## 16. Measured behaviour (reference machine, Release)

AMD Radeon AI PRO R9700 (Direct3D 11), 32 hardware threads. One 1080p layer with three effects: software 36.9 ms, GPU 2.7 ms including read-back (0.26 ms on the card); four layers 153.7 ms vs 4.5 ms; software one-effect frame 7–14 ms after row parallelism (22–142 ms before); 4K hardware-decode + GPU compose with read-ahead enabled 10.5 ms median (p95 10.9 ms); CPU effects at 4K: blur 54 ms, sharpen 50 ms, lens correction 50 ms, Gaussian 402 ms, glow 446 ms, noise reduction 3,254 ms. A 1080p optical-flow pair takes 4.9 s to analyse. Details and caveats (one machine, small fixtures, warm caches) are in `PERFORMANCE_LOG.md` and `perf/`.

---

## 17. Known gaps (as built)

The authoritative list is [TODO.md](TODO.md); the summary:

1. **Editing surface:** no source monitor, source marks, patching, targeting, four-point/replace edit, match frame, subclips; the project panel is a flat list (no bins/relink/search/labels/thumbnails); nested and extra sequences cannot be created or opened; Markers panel is a placeholder; no waveforms/thumbnails/cache bar/caption lane on the timeline; no effect copy/paste or presets; no interchange or project-manager dialogs; no recovery prompt; no autosave (a preference exists and does nothing).
2. **Render:** only a subset runs on the GPU; no track mattes; no executor for the render graph; no shared texture with the monitor; no OpenColorIO/ACES output/10-bit/HDR monitoring.
3. **Audio:** no plug-in hosting, no reverb/delay/modulation/pitch effects.
4. **Delivery:** no export resize, image sequences, smart render, render-and-replace, watch folders.
5. **Exchange and AI:** no AAF/OMF; no scene detection, semantic search, auto reframe, translation, speech enhancement.
6. **Platform and scale:** Windows only; one GPU vendor verified; no collaboration, review, VR, camera RAW, accessibility work, CI or soak tests.
7. **Defects:** hard-coded command timestamps; ten dead preferences; a duplicated "Basic Color" effect.

---

# Part II: Target design

*The sections below are the long-term blueprint. Each opens with a short **Status** note pointing at the as-built sections above.*

## T1. Purpose and definition

Cutline is a cross-platform professional NLE built as a set of cooperating engines and isolated services rather than one UI process that owns everything. Its target is reliable, non-destructive editing from lightweight social content through long-form, multicamera, high-resolution, HDR, and collaborative productions.

> **Product definition:** A non-destructive, frame-accurate, GPU-native professional NLE with editing, color, audio, graphics, captioning, collaboration, AI, and delivery capabilities; hardware-agnostic across AMD, NVIDIA, Intel, Apple Silicon, and CPU-only systems; with optional vendor fast paths rather than vendor dependencies.

The core performance rule is:

```text
decode once → keep surfaces local → process once → reuse/cache aggressively → copy only when unavoidable
```

## T2. Architectural goals and non-goals

| Goal | Architectural consequence |
|---|---|
| Real-time response under load | Deadline-aware scheduler, adaptive quality, prioritized I/O, prefetch, strict memory budgets |
| Correct final output | Exact rational time, sample-clocked audio, deterministic render graph, original media at export |
| Broad hardware support | Capability model, portable API boundaries, CPU fallback, compatibility rules |
| Reliable projects | Transactional database, immutable media identity, journal, snapshots, schema migrations |
| Resilient extensibility | Process-isolated plug-ins/workers, versioned SDKs, codec and AI provider boundaries |
| Collaboration | Change sets, optimistic synchronization, entity locks, conflict policies, audit history |
| Testability | Pure timeline compiler where possible, golden-frame tests, fixture media, reproducible render jobs |

Non-goals are reverse engineering proprietary project files, mimicking another editor’s UI, bundling legally restricted codecs without explicit licensing, or making a cloud AI vendor a hard dependency.

### Requirement traceability and completion standard

`feature-parity.yaml` maps every capability in the product baseline to a priority, current status, and a testable acceptance statement. It is the release source of truth. Architecture text describes target design; it does not by itself constitute implementation. Before a status can change to `implemented`, the project must have:

1. A working runtime path—not only a UI, mock, schema, or API declaration.
2. Input validation, bounded failure behavior, and user-visible diagnostics.
3. Automated tests appropriate to the risk: unit/integration for commands and project state; fixture/golden/deadline tests for media processing.
4. A documented compatibility and licensing position for all codecs, plug-ins, and external providers involved.

## T3. System map

### Current working vertical slice

**Status.** The pipeline described below runs end to end in one process today; Part I, sections 1 to 3, describes it as built (threads, the edit loop, playback, export, ingest, recovery), and section 14 the application. The browser prototype in the repository root (`index.html`, `app.js`, `mcp-server.js`) is a superseded UX reference that shares no code with the native core.

```text
ingest (probe → fingerprint → journal) → command bus → SQLite project store (changeset undo)
   → sequence snapshot → timeline compiler → PlaybackPlan
        ├─ decode (FFmpeg / synthetic) → compositor (CPU reference, Direct3D 11 subset) ─┐
        └─ decode → audio mixer → audio clock / WASAPI sink                              ├→ playback engine / export worker → encode + mux
```

Code map for the boxes below: **Project service** = `core/project`, `core/commands`, `core/db`; **Timeline compiler** = `timeline/`; **Media engine** = `media/`; **Render/audio graph** = `render/`, `audio/`; **Playback and export** = `playback/`, `exporter/`.

```text
                                CUTLINE APPLICATION
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │ UI shell: workspaces · panels · timeline · monitors · inspector · commands  │
 └───────────────────────────────────┬──────────────────────────────────────────┘
                                     │ validated editor commands
 ┌───────────────────────────┬───────┴─────────┬─────────────────────────────────┐
 │ Project service           │ Timeline compiler│ Collaboration service           │
 │ DB · journal · snapshots  │ sequences → plan │ changesets · locks · comments  │
 └──────────────┬────────────┴────────┬────────┴───────────────┬─────────────────┘
                │                      │                        │
      ┌─────────▼─────────┐  ┌─────────▼─────────┐  ┌──────────▼──────────┐
      │ Media engine      │  │ Render/audio graph│  │ Background services │
      │ probe · demux ·   │  │ color · effects · │  │ index · AI · proxy  │
      │ decode · time map │  │ composite · mix   │  │ waveform · export   │
      └─────────┬─────────┘  └─────────┬─────────┘  └──────────┬──────────┘
                └───────────────┬──────┴──────┬────────────────┘
                                ▼             ▼
                  Resource/job scheduler · cache · hardware manager
                                │             │
                         CPU · GPU · NPU · decoder · encoder · disk
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

## T4. Command and state model

All state changes arrive through a versioned command bus. UI gestures, shortcuts, scripts, AI suggestions, undo/redo, collaboration changes, and imported interchange documents go through the same validation path.

```text
intent → validate permissions & invariants → transaction → persist command → update read model → publish events
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

**Status.** Implemented: 70 typed commands behind a descriptor table that ties each command type to its payload, label and wire name; stateless validation on the envelope plus state-dependent validation in the store (overlap, media duration, locks, nesting and bin cycles); optimistic revision checks; idempotent replay; and an append-only journal.

**Undo is row-level changesets, not inverse commands or preimages.** While a command runs, SQLite's session extension records the exact rows it inserted, deleted or updated, and undo applies the inverted changeset. This was chosen over the two obvious alternatives for reasons that held up in practice: hand-written inverses for 38 commands would each be a place for undo to be subtly wrong, and whole-database preimages (the original design here) cost two full project copies per command. A changeset costs bytes proportional to the edit — a 21-command session is about 4 KB — and it correctly restores rows removed by foreign-key and trigger cascades, which a payload-only inverse silently loses. Replay disables the cascades (`SQLITE_CHANGESETAPPLY_FKNOACTION` plus a trigger-suppression flag), because the changeset already records every cascaded row and running the cascade again would delete them twice.

The undo stack is in memory and does not survive reopening a project. The changesets are also persisted on each journal row and in a journal file per revision, and `ProjectStore::RecoverPackage` replays them idempotently from a snapshot (Part I, section 3.6); the application does not yet offer recovery on open.

Remote changes should be represented separately from local undo stacks so a user cannot accidentally undo another editor’s confirmed work.

## T5. Project format and persistence

Each project is a portable directory, not one opaque binary.

```text
<name>.cutline/              (target layout; the layout as built is in Part I, section 4.1)
├── project.json                 # format version, identity, settings, manifest
├── project.db                   # transactional state / normalized records
├── journal/                     # ordered commands after latest snapshot
├── snapshots/                   # compact recovery checkpoints
├── cache_manifest/              # cache keys and validity, not the cache payload itself
├── attachments/                 # optional project-local graphics/documents
└── interchange/                 # imported/exported OTIO, XML, EDL, AAF artifacts
```

### Current implementation

**Status.** `ProjectStore` is the only writer to a project. It creates and opens portable SQLite-WAL packages (`project.db`, `project.json`, `journal/`, `snapshots/`), applies each command inside one transaction, records it with its changeset, writes a journal file per revision, takes full snapshots on a configurable interval, and validates integrity on demand. Access is serialised by a mutex and the handle is opened `FULLMUTEX`, since the store is shared by the UI, workers and any future automation bridge. Every statement is prepared with bound parameters; none is built by concatenating user text.

**Schema v13** (25 tables, listed in Part I, section 4.2) holds effects with keyframed parameters and masks, transitions, nested sequences and adjustment clips (a clip references exactly one of media, sequence or nothing, enforced by a `CHECK`), structured per-stream media metadata including colour primaries, transfer, matrix and range, audio routing, tracking data, captions, proxies, multicam groups, project graphics and templates, clip audio roles, markers, and a per-sequence render version. A project from a newer build is refused with an explanation rather than half-opened.

Two details that are easy to get wrong:

- **Rational times are stored twice.** The exact numerator/denominator pair is authoritative. A derived integer `_ticks` column on a 254016000000/s timebase exists only so timeline ranges can be indexed and range-queried in SQL, which a fraction cannot be. One function writes both so they cannot drift.
- **Effects and markers reference their owner polymorphically** (`owner_kind` + `owner_id`) so one parameter/keyframe table serves clips, tracks, sequences and transitions. SQLite cannot enforce such a reference, so cascade is supplied by triggers and `ValidateDatabase()` checks for orphans. This is the schema's one soft spot and it has already produced a bug (undo colliding with the cascade).

The browser prototype's JSON/NDJSON persistence (`mcp-server.js`, `project-state.json`) is not the persistence layer described here and is not connected to the native store.

Use SQLite (WAL mode) or an equivalent embedded transactional database for the local project service. Every project schema has a `format_version`, migration identifier, and reversible backup/snapshot path. On open: validate → backup → migrate transactionally → reopen through the current read model. If migration fails, preserve the original project untouched.

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

## T6. Canonical time and synchronization

Do not use `float seconds` as a project source of truth. Represent timeline times as normalized 64-bit rational values and protect every multiplication/addition from overflow.

```cpp
struct RationalTime {
  int64_t numerator;
  int64_t denominator; // always positive and reduced
};
```

The time layer must correctly represent 23.976/24000:1001, 24, 25, 29.97/30000:1001, drop-frame timecode, 30, 48, 50, 59.94/60000:1001, 60, and arbitrary source timestamps. Convert to presentation strings only at UI/API boundaries.

Audio is the master playback clock. Audio hardware consumes sample frames at a known rate; video scheduler and playhead derive presentation time from that clock. Internal processing targets 32-bit float, 48/96 kHz, sample-accurate boundaries, and real-time-safe callback code with no allocation, locks, disk I/O, or network calls.

**Status.** Implemented in `core/time`: normalised 64-bit rationals with checked arithmetic that throws on overflow rather than wrapping, frame conversion with explicit rounding modes, and 29.97/59.94 drop-frame parse and format. Conversions to ticks, frames and samples multiply and divide in an exact 128-bit intermediate and narrow once, so they throw only when the result genuinely does not fit; an earlier version multiplied the numerator by the tick rate in 64 bits and overflowed on ordinary hour-long timelines. The integer tick timebase (254016000000/s = 2⁶·3⁴·7²·10⁶) is divisible by every broadcast frame rate and audio sample rate in use, including the 1001-based ones, and this is proven by test rather than assumed. `FormatTimecode` snaps to the nearest frame by default, since a display must be able to show a time between frames; pass `RoundingMode::Exact` to assert alignment.

Audio is the playback clock in the code as in the design: `AudioClock` is advanced only by samples the device accepted, and `PlaybackEngine::position()` subtracts the sink's output latency so picture is rendered for what is audible rather than what is queued. Export locks audio to video by deriving each frame's sample count from absolute time instead of accumulating: 48 kHz against 30000/1001 is 1601.6 samples per frame, and rounding each frame independently loses about a sample every three frames, a frame of desynchronisation every few minutes. A 1000-frame export at that rate produces exactly 1,601,600 samples. If the audio device fails during playback the session stops the audio clock, says so, and lets the wall clock finish the play. Not yet built: drift resync when the device rate differs from nominal, and automatic recovery when the device changes.

### VFR rule

Variable-frame-rate media uses an indexed timestamp map:

```text
source packet/frame PTS → source timestamp map → source-to-sequence transform → canonical timeline time
```

Never assume `frame index × nominal duration`. Timeline playback/editing asks for source frames by timestamp/range; a chosen interpolation policy determines output sampling.

## T7. Media ingest and identity

Import is a durable job pipeline:

```text
discover → probe → checksum/fingerprint → copy or link → color/HDR interpretation
→ proxy decision → thumbnails/waveforms → media index → optional AI analysis → ready
```

Probe gathers container, codec, profile, pixel format, dimensions, rotation, nominal rate, VFR/CFR evidence, duration, source timecode, color primaries/transfer/matrix/range, HDR metadata, audio streams/channels/layout/rate, camera metadata, and file health information.

`MediaID` must not derive from filename or path. Use a stored content/fingerprint hash based on a versioned combination of sampled/full file hashing, byte size, duration, essential stream metadata, and optional camera/source identifiers. Maintain path aliases separately so media can be relinked after a drive or folder change. A full verification pass validates source content during copy/archive workflows.

**Status.** Implemented: probe into typed `media_streams` rows (codec, geometry, pixel aspect, exact frame rate, cadence, bit depth, field order, colour primaries/transfer/matrix/range, sample rate, channel layout); a versioned sampled SHA-256 fingerprint over the file size plus head, middle and tail spans (small files are hashed whole, and the size goes in first so files sharing sampled spans but differing in length cannot collide); duplicate detection by fingerprint; relink by fingerprint; and import recorded as one journalled, undoable command (the media row and its streams together). Media that cannot be opened is remembered as offline and renders as a gap, rather than being retried on every frame.

Built for proxies: in-process background generation, cancellation/progress, persisted association, source-fingerprint stale detection, missing-file status, monitor preference with original fallback, and an originals-only export clone. The Project panel has a per-item proxy menu with progress and a Proxies switch, and File > Ingest Media copies with a hash of the source and a read-back hash of the copy (`media::CopyVerified`). Still not built: thumbnails, waveforms, a media index, path aliases, image-sequence detection, and the background indexer process.

Proxies are first-class mappings from source media to an approved proxy representation. A proxy has status, settings, checksum, generated time mapping, and an explicit original/proxy selection policy. Export defaults to originals and must warn before output if originals are offline.

All required editor services run in-process or as bundled local workers. No edit, save, proxy, analysis, recovery, or export path may require a network connection; [OFFLINE_OPERATION.md](OFFLINE_OPERATION.md) records the boundary.

## T8. Timeline compiler

The editor’s visible timeline is not the render plan. The timeline compiler resolves sequence nesting, active ranges, source time transforms, transition overlaps, adjustment layers, keyframes, captions, effects, masks, graphics, audio routing, and multicam angles into an execution graph for a specific time interval and quality policy.

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

## T9. Render graph and color pipeline

Each render node declares its input/output format, CPU/GPU availability, temporal radius, region-of-interest support, cacheability, deterministic status, supported precision, HDR behavior, preferred device, and fusion compatibility.

```text
media decode → input transform → effects/masks/tracking → grade → composite/graphics
→ output transform → display or encoder surface
```

The compiler performs node fusion, dead-node elimination, constant folding, ROI rendering, quality-driven resolution reduction, shader specialization, texture reuse, buffer aliasing, async compute scheduling, decode/render overlap, copy/render overlap, and pipeline caching.

**Status.** Part I, section 9, describes the renderer as built. In short: the CPU `Compositor` is the reference and the export path; a Direct3D 11 compute-shader compositor renders the common subset on the card (layers, motion, crop, opacity, all colour tools and LUTs, generators, adjustment clips, dissolves, sequence effects, hardware-decoded pictures read in place) and is checked picture by picture against the CPU one; everything else falls back, whole frame, to the CPU. The backend-neutral `RenderGraph` (immutable nodes, dependency hashes, dead-node elimination, safe fusion) is built and tested but **nothing executes it**. Not built: graph execution, region-of-interest and per-node intermediate caching, GPU filters/keys/blend modes/masks, a shared texture with the monitor, other graphics APIs, OpenColorIO, an ACES output transform, 10-bit end to end, HDR monitoring.

At 1080p the CPU compositor measures 7–14 ms for one effect on a 32-thread machine after row parallelism (22–142 ms before), and about 37 ms for one track with three effects in the original single-threaded measurement; the card does the same frames in 3–5 ms including read-back.

Use one effect graph for preview and export. Only the quality policy changes:

| Policy | Preview | Final export |
|---|---|---|
| Resolution | adaptive full to 1/8 | full output size |
| Source | proxy/cached frame permitted | originals unless explicitly overridden |
| Temporal work | bypassed/approximate when needed | maximum configured precision |
| Sampling | low sample count / fast interpolation | configured final samples / best interpolation |
| Cache | aggressive playback cache | validated cache or freshly rendered original path |

Color management should use explicit transforms: input color space/device transform → scene-linear/wide-gamut working space → effects/grade/composite → output device transform. Support SDR, wide gamut, PQ, HLG, LUTs, 8/10/12-bit integer and FP16/FP32 workflows according to node capabilities. Provide luma/RGB/YC waveforms, RGB/YUV parade, vectorscope, histogram, HDR waveform/nits, false color, zebra, and gamut warning scopes.

## T10. Hardware and codec capability layer

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

**Status.** The `CodecProvider` boundary exists as `media::Source`/`SourceProvider` (read) and `media::Writer`/`WriterProvider` (write) with a registry for each; nothing above them names FFmpeg. Two source providers exist (FFmpeg and a deterministic synthetic source) and one writer (FFmpeg). `render::gpu` defines backend/vendor descriptors, workload masks, deterministic scoring, CPU fallback, opaque external surfaces and fences, tested with AMD, NVIDIA, Intel and Apple descriptors without vendor-specific product logic. **Windows is the only target built**: Direct3D 11 adapter discovery and device creation work (verified on one AMD adapter), Direct3D 11 video acceleration decodes H.264-class streams onto the compositor's device, and AMD AMF hardware encoders are verified for H.264, HEVC (8/10-bit) and AV1; NVENC and Quick Sync are probed and skipped when they will not start but have never run in this project. Direct3D 12, Metal and Vulkan do not exist. The audio sink falls back to an offline sink when no device exists, so the editor opens on a machine with no sound card.

FFmpeg should be a modular demux/parser/software fallback, not the only media strategy. Vendor and operating-system codec providers belong behind a `CodecProvider` boundary. Every binary distribution must have a reviewed licensing manifest; restricted, patented, GPL, or commercial codecs must remain optional and explicitly licensed.

## T11. Resource, cache, and workload scheduling

`ResourceManager` owns RAM, each GPU’s VRAM, cache disk quota, decoder/encoder session capacity, I/O throughput, and thermal/power signals where available. It allocates budgets by work class, emits pressure events, and evicts only recreatable assets. Decode surfaces remain GPU-local when later nodes share the device; copies between adapters must be measured and justified.

Cache is content-addressed, versioned, and partitioned by artifact type:

```text
packets/ decoded_frames/ gpu_frames/ thumbnails/ waveforms/ proxies/
rendered_previews/ optical_flow/ stabilization/ masks/ transcripts/ embeddings/
shader_pipelines/ analysis/ export_intermediates/
```

Cache keys include media fingerprint, source range, edit/effect dependency hashes, provider/version, device/driver constraints where needed, color pipeline, and quality policy. The cache manager maintains a manifest, LRU + cost-aware eviction, integrity checks, and user-visible clearing by category.

**Status.** Part I, sections 3.1, 9.3 and 11. `PlaybackEngine` holds a byte-budgeted LRU raw-frame cache (default 256 MB) and a bounded decode pool (two workers, eight frames ahead, at most 64 queued jobs, eight retained sources per worker); the render cache and the optical-flow cache share one pressure-aware, cost-weighted RAM/disk quota (a VRAM tier is reserved); a bounded priority scheduler with the classes below exists and is tested, **but the decode pool, presenter, scopes, analysis, proxy and export workers still use their own mechanisms**. A Direct3D 11 compositor exists (it holds its own pooled textures; a per-device GPU frame cache and VRAM accounting are not built). `Compositor` is move-only and one instance belongs to one render thread.

Playback scheduler priority:

```text
P0 audio deadline
P1 current display frame
P2 next 3–8 presentation frames
P3 GPU composite needed for those frames
P4 decode/I/O prefetch
P5 thumbnails
P6 waveforms
P7 stabilization/optical-flow analysis
P8 proxy generation
P9 semantic and AI indexing
```

On sustained misses, progressively reduce preview resolution full → 1/2 → 1/4 → 1/8, first avoiding skips in the audio clock. When paused, schedule a full-quality refinement. Interactive scrubbing may request a nearest frame at reduced resolution and omit expensive temporal nodes. Export runs independently and must not starve live audio/playback.

## T12. Audio engine

The audio graph resolves clip gain/pan/time transform, clip plug-ins, track mixer, sends, buses, side-chain inputs, master effects, metering, and device output. It supports sample-accurate automation, fades/crossfades, channel mappings, loudness measurements, dialogue/music classification hooks, and VST3 through a hosted plug-in boundary. Audio analysis and speech cleanup execute outside the real-time callback and deliver render-ready artifacts.

**Status.** Part I, section 10. The audio graph routes clips through track and bus effects, sends and the master; `AudioMixer` renders from the sequence snapshot because a block spans clip edges, transitions and keyframes (effects, automation, crossfade weights and `time_remap` source positions are evaluated per sample). Pitch-preserving WSOLA time stretch follows constant and curved retiming within 0.25–4×; holds, reverse segments and extremes use cubic varispeed and count a fallback. Roles with starting effect chains, loudness targets, automation read/write/touch/latch, voice-over recording, buses, sends and a mixer interface are built. The realtime WASAPI callback is allocation-free and lock-free.

Not built: plug-in hosting and delay compensation (latency compensation must be designed before hosting), waveform generation, input monitoring, control surfaces, reverb/delay/modulation/pitch effects.

## T13. AI and automation

AI is a set of asynchronous providers—not a privileged write path.

| Service family | Capabilities |
|---|---|
| Speech | transcription, diarization, captions, translation, filler/silence detection, text editing |
| Audio | speech enhancement, noise reduction, event indexing, remix-to-length, generation providers |
| Vision | scene detection, object detection/segmentation, mask tracking, auto reframe |
| Semantic | clip/transcript/audio embeddings and search |
| Generative | video/audio/image generation and extension via provider contracts |

Every result is stored with model/provider/version, source media fingerprint, source range, confidence, and user-review status. AI requests produce proposed `CommandEnvelope` objects such as `RippleDelete` ranges; users can preview, accept, modify, or reject them. A `GenerativeProvider` supports local, cloud, and enterprise/private implementations without exposing provider details to the timeline model.

The same command API powers keyboard bindings, macros, Python/JavaScript automation, accessible UI controls, and AI. That makes edits auditable and undoable.

## T14. Plug-ins and interchange

| Extension point | Contract |
|---|---|
| Video effects/transitions | Native SDK and OFX where appropriate; declare render metadata and sandbox process |
| Audio | VST3 host with scanning, crash quarantine, latency reporting, and automation mapping |
| Import/export | Versioned codec/interchange provider interfaces |
| Panels | Web/QML-style panel API with scoped project capabilities |
| AI | Provider SDK with explicit data/consent boundaries |
| Automation | Python/JavaScript command API, never raw database access |

Interchange adapters should normalize to the internal timeline model and retain unrepresentable metadata as warnings, not silently drop it. Priorities: OpenTimelineIO, EDL, Final Cut XML, AAF/OMF where licensing and implementation permit, caption sidecar/embed/burn-in workflows, and common image-sequence/media formats.

## T15. Collaboration

### Local production mode

Many small project packages point to common shared media on managed storage. Use project/bin/sequence locks, user-visible ownership, media verification, and production-level dependency links. This model favors very large jobs and shared high-bandwidth storage.

### Cloud project mode

Clients create optimistic change sets against a revision. The service validates them, stores history, detects overlapping edits, and synchronizes deltas. Sequence and bin locks protect high-conflict ranges; comments and review versions are append-only collaboration records. Offline clients queue change sets and reconcile explicitly—never auto-discard local work.

Conflict policy must be field/entity-specific: label changes may merge; simultaneous incompatible changes to the same clip timing require a clear user resolution UI. Every resolved conflict is audited.

## T16. Security, privacy, and licensing

- Treat local media and metadata as customer data. Make cloud analysis/generative uploads explicit, scoped, and revocable.
- Keep credentials in the operating system credential store; never in project files or command journals.
- Verify plug-in signatures/identity where available; expose an unsafe plug-in warning and isolated host diagnostics.
- Store hashes for integrity and relinking; provide a clear policy for full versus sampled hashing.
- Ship a Software Bill of Materials and codec/license manifest. Review dependencies and binary configuration per target platform.
- Sanitize media metadata and plug-in IPC input; enforce resource/time limits on untrusted decoders and plug-ins.

## T17. Quality, observability, and release gates

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

**Status.** 760 native tests across 12 suites run under CTest, plus 32 application tests that drive the real window offscreen, with the core library compiling clean at `/W4`. Coverage includes time-remap video/audio mapping, optical flow, rolling-shutter/mesh warp, blend modes, graphics/templates, multicam, local speech/transcript editing, realtime audio meters, playback-plan lowering, immutable render-graph compilation, device selection, priority scheduling and UI-domain models in addition to store, media, playback, export, effects and golden-frame checks. Generated fixture media and export round trips remain the independent end-to-end evidence. Tests whose premise does not apply report themselves as skipped rather than passing silently.

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

## T18. Capability baseline

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

## T19. Delivery roadmap and acceptance criteria

### P0 — trusted editorial core

**Status (2026-10-08, from the code).** The engine half is done and verified, and most of the interaction half is built in the Qt shell: J/K/L transport, ruler, playhead, selection, snapping, ripple/roll/slip/slide trim, insert/overwrite/lift/extract, program monitor with safe margins, zoom and fullscreen, export in 21 presets, and (in the engine) crash-recovery replay. Not built: the source monitor and everything that hangs off it (source marks, source playback, source patching, track targeting, four-point and replace edits, match frame), an application prompt for crash recovery, and interfaces for the finished interchange and project-collection engines (see TODO.md, "Interfaces still missing for finished engines"). Of the 55 P0 capabilities, 40 are implemented with named tests, 7 partial and 8 planned. See [ROADMAP.md](ROADMAP.md).

Build project DB/journal/snapshots, rational time, media probe/fingerprint, a CPU software decode path, basic audio output, source/program monitors, sequence and AV tracks, core edits, markers, undo/redo, autosave/recovery, and basic H.264 export. Acceptance: an editor can cut a short mixed-media sequence, save/reopen it exactly, recover after forced interruption, and produce a synchronized export.

### P1 — performance and finishing

Build D3D12/Metal/Vulkan render abstraction, hardware capability discovery, cache/resource manager, proxies, preview policies, render worker, color transform system, core GPU effects/transitions, scopes, mixer, captions, waveform generation, and detailed export presets. Acceptance: a 4K timeline with representative effects remains usable with adaptive preview and matches a final render at export quality.

### P2 — intelligence, extensibility, interchange

Build transcription/captions, semantic media search, paper edit, scene detection, auto reframe, OTIO/XML/EDL adapters, VST3/OFX hosting, and scripting. Acceptance: background analysis never blocks the timeline; AI suggestions are reviewable commands; imported/exported interchange surfaces clear warnings for lossy fields.

### P3 — scale and collaboration

Build production manifests/shared media tooling, cloud change sets, locks, comments, review versions, conflict UI, queue management, and operator diagnostics. Acceptance: two users can make non-conflicting edits, observe remote updates, resolve a conflict explicitly, and recover after one client disconnects.

### P4 — advanced workflow

Build production-grade tracking/object masks/stabilization/optical flow, generative provider routing, advanced HDR/external I/O, multi-GPU policies, and performance auto-tuning. Acceptance: provider failure falls back safely; hardware selection is explainable; golden-frame and deadline metrics remain within published thresholds.

## T20. Decisions

**Made, and reflected in the code:**

1. **Platform delivery: Windows first; GPU architecture: cross-platform and vendor-neutral.** The first backend built is **Direct3D 11 compute** (verified on one AMD adapter); Direct3D 12 remains the target for standard drivers on AMD, NVIDIA and Intel. Metal is the macOS backend for Apple Silicon and supported AMD Macs; Vulkan is the Linux/portable backend. The CPU renderer remains the semantic reference and fallback. Projects and effect definitions contain no backend or vendor identity.
2. **Native stack: a typed C++ core with a declarative UI.** Timing and render authority is native; the browser prototype is a reference only. Qt Quick 6.8.3 hosts the application, and the timeline is a custom `QQuickPaintedItem` over a virtualised layout (a 120-track, 120,000-clip fixture returns only visible geometry); delegates would not have scaled.
3. **FFmpeg is optional and LGPL.** The core builds and every non-decode test runs without it. The SDK is auto-detected at `.tools/ffmpeg` and an LGPL shared build is used, because a GPL build would impose GPL on the application. A reviewed licensing manifest is still required before any distribution.
4. **SQLite is vendored** (3.48 amalgamation, session extension enabled) so the build is reproducible with no package manager.
5. **Undo is changeset-based** rather than inverse-command or preimage-based (section T4).

**Still open:**

1. The exact cross-API surface-sharing contract: D3D12 with Media Foundation/D3D11VA, Metal with VideoToolbox, and Vulkan with VAAPI/Vulkan Video, including when an explicit copy is safer than interop.
2. Licensing and distribution policy for proprietary codecs, OFX/VST3, and any cloud AI provider.
3. Local-first versus hosted collaboration timeline and data residency requirements.
4. P0 file formats/codecs, minimum hardware profile, supported resolution/HDR limit, and the specific acceptance media set.
5. The level of project portability required in the first release and which interchange formats are contractual.
6. Whether render semantics are versioned per sequence (strongly recommended before colour management; see section T9).

Resolving these turns the blueprint into an actionable backlog without compromising the core architecture.

## T21. Preview, replacement, monitoring, and advanced-workflow boundaries

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

`feature-parity.yaml` stays as the broad compatibility roadmap for existing consumers. Detailed manifests in `parity/*.yaml` represent one capability per record and inherit a common contract: ID, name, priority, evidence-based status, implementation notes, measurable acceptance, unit/integration/fixture test placeholders, platform and hardware policy, known gaps, and licensing notes. The generator validates every record and emits JSON/Markdown domain counts without an overall percentage. Status transitions require evidence: `partial` needs a tested runtime component, `implemented` needs the declared acceptance coverage, and `validated` needs the relevant compatibility, failure, and performance evidence. This is intentionally stricter than a UI or schema checklist, and `scripts/cross-check-parity.js` enforces it mechanically: a record may not claim `implemented` without naming unit tests that exist in `tests/native`. The first run of that check moved 43 capabilities to `implemented` with evidence (the manifests had recorded none) and downgraded several that had been recorded optimistically — the effect "graph" is a fixed pipeline, transition alignment is recorded but never used to place anything, linked A/V is stored but nothing links or unlinks, and crash recovery persists changesets that nothing replays.
