# Cutline

Cutline is a planned professional, non-destructive non-linear editor (NLE): a frame-accurate, GPU-native editing application with a hardware-agnostic core. The product goal is a modern professional workflow for editorial, color, audio, captions, graphics, AI-assisted editing, collaboration, and deliveryâ€”without reproducing another vendorâ€™s proprietary formats, branding, or cloud ecosystem.

The native C++ core is the product. The browser prototype in the repository root is a superseded UX reference that shares no code with it and should not be developed further.

The ordered, actionable work queue is [TODO.md](TODO.md). The broader plan, with a cross-check of the design, tracked feature list, and optimisation strategy, is [ROADMAP.md](ROADMAP.md). Known defects and their status are in [REMEDIATION.md](REMEDIATION.md); measured performance changes are in [PERFORMANCE_LOG.md](PERFORMANCE_LOG.md). For the complete technical blueprint, read [ARCHITECTURE.md](ARCHITECTURE.md). The runtime audit is [GAP_ANALYSIS.md](GAP_ANALYSIS.md); the human-readable reality check is [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md); and [MCP.md](MCP.md) covers LLM control. The backwards-compatible broad roadmap is [feature-parity.yaml](feature-parity.yaml); atomic domain manifests are in [parity/README.md](parity/README.md), with the generated evidence-based rollup in [premiere-parity-report.md](premiere-parity-report.md).

## Native core

The repository contains a C++20/CMake core and an optional Qt Quick desktop shell, verified by **751 native tests plus 31 desktop-application tests** including golden-frame image comparison, real FFmpeg decode, proxy generation, offline transcript and caption editing, UI-domain behavior and originals-only export round trips.

What runs today: a typed SQLite project package with 70 commands, row-level changeset undo, journal/snapshot recovery, and schema v13 covering effects, first-class animated masks, keyframes, transitions, nested sequences, colour metadata, audio routing, tracking, captions, proxies, multicam groups and project-held graphics and templates; exact rational time with drop-frame timecode and an integer tick timebase; a timeline compiler with speed-remap curves, freezes and reverse segments; FFmpeg demux/decode, local proxy generation and encode/mux; a colour-managed CPU reference compositor with animated geometric masks, filters, 12 blend modes, rolling-shutter/mesh warps, optical-flow interpolation, keying, grading, asynchronously measured scopes, tracking, stabilisation, noise reduction, graphics/templates, render caching and captions; an immutable render-graph compiler with dependency hashes, dead-node elimination and safe unary-node fusion; capability-based D3D12/Metal/Vulkan device selection plus an opaque external-surface contract; a bounded priority scheduler with stale-generation cancellation and an interactive worker reserve; cost-aware shared RAM/disk cache quotas with pressure shedding; an offline adaptive proxy policy; a two-axis virtualized timeline layout; sample-by-sample retimed audio; persisted multicam groups with switching and flattening; and playback/export engines that enforce original media for delivery.

A Direct3D 11 GPU compositor renders the common editing subset of a plan (layers, motion, crop, opacity, grade, the colour tools and LUTs, generators, adjustment clips, dissolves) on the graphics card, exactly as the software compositor does to within a level; hardware-decoded H.264-class pictures stay on the card from decoder to shader. On this machine (AMD Radeon AI PRO R9700, Release build, `scripts\bench.bat --gpu`, perf/2026-10-07-gpu.txt) one 1080p layer with three effects composes in 36.9 ms in software and 2.7 ms on the GPU including the upload and the read-back (0.26 ms on the card); four layers 153.7 ms against 4.5 ms; reading and composing a 1080p H.264 picture takes 5.3 ms with software decode and upload and 2.0 ms with the hardware decoder and no copy. Colour grading is on the card too (3D/1D LUTs, wheels, curves, hue curves, HSL secondary, mixer, tint, black and white, colour adjust: 3.6-5 ms for a 1080p frame in the GPU benchmark, against 7-14 ms in software now and 22-142 ms before the software passes were spread over the cores; perf/2026-10-07-color-completion-after-fastpath.txt). What does not exist: GPU blur and other filters, keys, blend modes and masks (those frames are composed in software), D3D12/Metal/Vulkan executors, a texture shared with the monitor (the finished picture is read back), or validation beyond one AMD adapter. The Qt Quick desktop application is built and tested with Qt 6.8.3: 31 offscreen application tests and 60 framework-independent UI tests cover the docked workspaces, program monitor, custom virtualized timeline, effect inspector, masks, LUT browser, speed ramps, multicam, export presets/queue, local transcript and caption editing, asynchronous video scopes and audio mixer. The Text workspace authors timed caption tracks and cues, styles them, and imports/exports SRT or WebVTT through ordinary undoable commands. It has not been exercised by a person on a display, on a GPU scene graph, or with production footage. The Graphics workspace designs titles and graphics (a canvas with grips, rotation, outlines, shadows, rounded corners, entrances, a thumbnail library, template import/export and six bundled templates), changing one title on the timeline through its controls. A proxy/cache panel and a source monitor remain incomplete; scope GPU aggregation and calibration controls remain. `cutline_demo` and `Cutline.exe` are the verified front ends. See [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md) for the full table and [GAP_ANALYSIS.md](GAP_ANALYSIS.md) for what remains.

## Current browser prototype

Open `index.html` in a modern browser. No package install or build step is required.

| Area | Available now |
|---|---|
| Media | Browser, file-picker import, local HTML-video preview for the active browser session, selection state |
| Timeline | Multi-track AV timeline, add video track, snap toggle, drag between tracks, split, ripple delete, and clip insertion |
| Review | Program monitor, timecode, safe areas, marker command, Spacebar playback for browser-decodable local media |
| Inspector | Transform, opacity, compositing, and live non-destructive exposure/contrast/saturation/temperature preview controls |
| System UX | Performance-mode control, GPU-path status, and persistent export-queue records |
| Command layer | `Ctrl/Cmd + K` command palette stub, `Ctrl/Cmd + Z` / redo, revision-safe command dispatch, checkpointed journal, and local MCP controls |

## Repository map

```text
video_editor/
â”œâ”€â”€ core/
â”‚   â”œâ”€â”€ time/            Exact rational time, drop-frame timecode, tick timebase
â”‚   â”œâ”€â”€ anim/            Keyframes and curves (hold/linear/bezier/ease)
â”‚   â”œâ”€â”€ model/           Domain vocabulary shared across layers
â”‚   â”œâ”€â”€ commands/        70 typed commands: payloads, labels, journal JSON, validation
â”‚   â”œâ”€â”€ db/              SQLite wrappers and changeset-based undo
â”‚   â”œâ”€â”€ project/         Schema v13 and the project store (the only writer)
â”‚   â””â”€â”€ util/            Write-only JSON, SHA-256
â”œâ”€â”€ media/
â”‚   â”œâ”€â”€ VideoFrame       Rgba8 / Rgba16 / RgbaF32 picture buffers
â”‚   â”œâ”€â”€ AudioBuffer      Planar float audio, gain and equal-power pan
â”‚   â”œâ”€â”€ Source           Provider interface + registry (no FFmpeg above this line)
â”‚   â”œâ”€â”€ SyntheticSource  Deterministic generated media for tests and goldens
â”‚   â”œâ”€â”€ Ingest           Probe, fingerprint, journal, relink
â”‚   â””â”€â”€ ffmpeg/          FFmpeg demux, decode, resample, seek, probe
â”œâ”€â”€ render/              Software compositor: the reference render semantics
â”œâ”€â”€ audio/               Mixer, master clock, offline sink, WASAPI device sink
â”œâ”€â”€ playback/            Playback engine: decode, composite, mix, transport
â”œâ”€â”€ timeline/            Immutable sequence snapshot, loader, plan compiler
â”œâ”€â”€ app/
â”‚   â”œâ”€â”€ demo_main.cpp    cutline_demo: end-to-end walkthrough
â”‚   â”œâ”€â”€ bench_main.cpp   cutline_bench: per-frame cost of render and mix
â”‚   â””â”€â”€ Session, MonitorItem, TimelineItem, DockController, main.cpp, qml/   Qt Quick desktop application (Cutline.exe)
â”œâ”€â”€ third_party/sqlite3/ Vendored SQLite 3.48 amalgamation
â”œâ”€â”€ .tools/ffmpeg/       Optional FFmpeg SDK (not committed; see below)
â”œâ”€â”€ tests/
â”‚   â”œâ”€â”€ native/          751 tests and the test harness
â”‚   â””â”€â”€ golden/          Golden frames (PPM)
â”œâ”€â”€ cmake/Fixtures.cmake Generates media fixtures at configure time
â”œâ”€â”€ scripts/             build.bat, test.bat, bench.bat, ppm-to-png.js
â”œâ”€â”€ parity/              23 domain manifests behind premiere-parity-report.md
â”œâ”€â”€ ARCHITECTURE.md      Native application blueprint and staged plan
â”œâ”€â”€ IMPLEMENTATION_STATUS.md  What runs today, with benchmark figures
â”œâ”€â”€ GAP_ANALYSIS.md      Remaining architectural gaps and implementation order
â”œâ”€â”€ ROADMAP.md           Parity plan, design cross-check, optimisation programme
â”œâ”€â”€ MILESTONES.md        Evidence for each build milestone, and what is still missing
â”œâ”€â”€ REMEDIATION.md       Status of every finding from the 2026-10-06 code review
â”œâ”€â”€ PERFORMANCE_LOG.md   Measured before/after performance changes (raw logs in perf/)
â””â”€â”€ (browser prototype)  index.html, styles.css, app.js, mcp-server.js, MCP.md,
                         project-state.json â€” superseded visual reference
```

## Product principles

1. **Non-destructive and recoverable.** Every edit is a command recorded in a transaction and command journal; original media is never changed.
2. **Exact time, not floating-point approximations.** Timeline operations use rational time and audio uses a sample-accurate master clock.
3. **Hardware-aware, never vendor-locked.** The core chooses capabilities, not brands. Vendor accelerators are optional providers, with a CPU fallback.
4. **GPU-native when it is safe and useful.** Decode, color, effects, composite, and encode should stay on GPU surfaces whenever possible; unnecessary GPU â†” RAM copies are avoided.
5. **One render graph, multiple quality policies.** Preview and final render are the same edit result with different performance/quality choices.
6. **One command system.** Mouse, keyboard, macros, scripts, AI, undo/redo, and collaboration all create validated editor commands.
7. **Failure containment.** Decode, export, AI, indexing, and plug-ins run in separate workers or hosts so an auxiliary crash does not destroy an edit session.

## Reliability features in this build

The native store is the authority. Every mutation is a validated typed command applied inside one SQLite transaction, recorded in an append-only journal together with the row-level changeset needed to reverse it. Stale-revision writes are rejected, retried commands are idempotent, undo and redo replay changesets rather than snapshots, full database snapshots are taken on a configurable interval, and `ValidateDatabase()` re-checks integrity, foreign keys, polymorphic owners and clip overlap on demand.

Crash recovery uses journal v2, periodic snapshots, replay to the last valid revision and quarantine of damaged state. The in-memory undo cursor is intentionally not reconstructed after recovery.

The older browser bridge (`mcp-server.js`, `project-state.json`) is part of the superseded prototype and is not the persistence layer described above.

## Native build and tests

The core needs only CMake 3.24+, a C++20 compiler, and Ninja. **SQLite is vendored** as the 3.48 amalgamation in `third_party/sqlite3`, so there is no package manager step and the build is reproducible. Its session extension is enabled there because undo is built on row-level changesets.

On Windows with Visual Studio installed, the supplied scripts find the toolchain, CMake and Ninja themselves:

```powershell
scripts\build.bat          # configure and build into build\native
scripts\test.bat           # build, then run the full CTest suite
scripts\bench.bat          # build Release and measure the per-frame render cost
build\native\cutline_demo.exe
```

`cutline_demo` is the command-line end-to-end walkthrough. The Qt desktop front end is `build\app\Cutline.exe`; run `python run.py --demo` to incrementally build changed sources, configure the Qt runtime path and open a ready-made project. Run `python run.py` for the normal empty start, pass `--open "<project.cutline>"`, or use `--no-build` to launch the existing binary immediately. The Windows-only `scripts\run-app.bat` is a smaller launcher for an existing build. The walkthrough creates a project package, ingests media, builds a sequence with effects, keyframes and a transition, renders frames to `<package>/frames` as PPM, mixes audio through the clock, exports a file and decodes it back to compare against the monitor, and exercises undo and redo. Convert the frames with `node scripts/ppm-to-png.js <dir> 2` to look at them.

### FFmpeg (optional)

Without FFmpeg the core, the compositor, the mixer and every test still build and run against the synthetic media source; only reading real files is unavailable, and `cutline_demo` reports which it has. To enable it, put an FFmpeg SDK with `include/`, `lib/` and `bin/` at `.tools/ffmpeg`, or point `-DCUTLINE_FFMPEG_ROOT=<path>` at one. An LGPL shared build is the right choice for a commercial product; a GPL build would impose GPL on the application. CMake reports which it found, copies the DLLs beside the binaries, and uses the bundled `ffmpeg` executable to generate the media fixtures the decode tests read.

Fixtures are generated at configure time into `build/<dir>/fixtures` rather than committed: a few seconds of video is large next to the source, and a file encoded by one FFmpeg build is not byte-identical to one encoded by another. When they cannot be generated, the tests that need them report themselves as **skipped** rather than passing silently.

### Configuring by hand

```powershell
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCUTLINE_BUILD_NATIVE_APP=OFF
cmake --build build/native
ctest --test-dir build/native --output-on-failure
```

Options: `-DCUTLINE_USE_VENDORED_SQLITE=OFF` links a system SQLite; `-DCUTLINE_ENABLE_FFMPEG=OFF` builds without file decoding. Qt 6.5+ (6.8.3 is what is verified) is optional and builds the desktop application and `cutline_app_tests`: `scripts\build-app.bat` configures `build\app` against Qt at `.tools\qt\6.8.3\msvc2022_64` (or `CUTLINE_QT_ROOT`); `scripts\shot.bat out.png --demo --do select:Bars` runs it off-screen and saves a picture.

### Parity cross-check

`scripts/cross-check-parity.js` checks the capability manifests in `parity/` against the test suite: a capability may
not claim to be implemented unless it names tests that exist, and the report generator refuses to build without that
evidence. Run it in CI â€” the manifests drifted away from the code once already.

```powershell
node scripts/cross-check-parity.js            # verify; non-zero on a mismatch
node scripts/generate-parity-report.js        # regenerate premiere-parity-report.md
node scripts/check-docs.js                    # test counts, links and file paths quoted in the markdown
```

`scripts/check-docs.js` does for the prose what the parity check does for the manifests: it fails if a document quotes a test count, suite count, link or source path that is no longer true of the code, and if a Windows path has been mangled into control characters by an escape. Documentation that states a number rots silently the moment the number changes, so both checks belong in CI.

### Tests

751 cases across 12 binaries; see [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md) for what each covers. The core library compiles clean at `/W4`.

Golden-frame tests compare rendered output pixel by pixel against committed PPM files in `tests/golden`. On a mismatch they write `<name>.actual.ppm` and `<name>.diff.ppm` next to the golden so the change can be looked at. To accept a deliberate change:

```powershell
$env:CUTLINE_UPDATE_GOLDEN="1"; build\native\cutline_render_tests.exe; $env:CUTLINE_UPDATE_GOLDEN=""
```

Review the result before committing it â€” that is the entire value of the mechanism.

## Prototype development checks

The browser prototype has its own small JavaScript suite, which exercises only the JS-side helpers (`core/rational-time.js`, `core/job-scheduler.js`, `core/media-identity.js`) and the parity-manifest aggregation:

```powershell
node tests/run.js
```

It certifies nothing about the native product. Decode, render, mixing, playback, export, undo and the project store are covered by the native suite described under *Native build and tests*, which is the one that matters.

## Feature-completeness contract

The requirements are tracked as 220 atomic items across 23 domain manifests in `parity/`; `feature-parity.yaml` remains the compatibility-facing broad roadmap. Generate the unweighted rollup with `node scripts/generate-parity-report.js`. A feature is only `implemented` when it has a real runtime, stated acceptance test, and documented failure behavior; `validated` also requires its relevant compatibility/failure/performance evidence. A UI affordance, schema field, architecture description, or MCP command that merely stores a parameter is correctly classified as `prototype` or `planned`; it is never treated as media-engine parity. This protects the project from false completion claims and makes release scope auditable. The rule is enforced by `scripts/cross-check-parity.js`, which refuses an `implemented` claim that does not name tests that exist; see [ROADMAP.md](ROADMAP.md) for where the 218 items currently stand.

## Intended workflows

### Editorial

Import camera, screen-capture, image, audio, and image-sequence media; organize bins; create proxies; mark source in/out points; perform three-point edits; use insert/overwrite/lift/extract; trim with ripple/roll/slip/slide; nest and multicam-edit sequences; retime clips; use adjustment layers, transitions, effects, graphics, and captions.

### Finishing

Manage source and output color spaces; work in a wide-gamut scene-linear pipeline; grade, mask, track, stabilize, composite, mix audio, author captions, inspect scopes, and export a final master with the appropriate codec, captions, metadata, and quality checks.

### Assisted editing

Index speech, faces/objects, scenes, audio events, and semantic embeddings in the background. The user can search media, transcribe/caption/translate, build paper edits, find silences, reframe around a subject, and call a provider-neutral generative workflow. AI proposes normal editor commands; it never mutates project data directly.

### Collaboration

Cutline is an offline application. Editing, proxies, analysis, saving, recovery and export use local code and bundled dependencies; see [OFFLINE_OPERATION.md](OFFLINE_OPERATION.md). Future collaboration is optional and must work through a shared folder or user-configured LAN peer without turning an internet service into an editor dependency.

## Scope boundaries

Cutline can aim for functional parity with professional editor workflows, but will not clone proprietary project-file internals, application UI, logos, vendor-specific generative implementations, stock systems, cloud account systems, or proprietary template formats. Interchange should prioritize open/standard formats such as OTIO, AAF, EDL, and Final Cut XML where technically and legally appropriate.

## Phased path from prototype to product

| Phase | Outcome | Minimum proof |
|---|---|---|
| P0 â€” Editorial core | Reliable local timeline editor | Create/open project, import, source/sequence playback, core edits, save/reopen, undo/redo, crash recovery â€” *import, edits, save/reopen, undo/redo, decode, composite and mix are done; export and crash replay are not* |
| P1 â€” Professional finishing | Usable color, audio, proxy, and export workflow | GPU render graph, cache, proxy toggle, scopes, mixer, captions, render worker |
| P2 â€” Intelligence and interchange | Faster editorial and ecosystem compatibility | Transcription, search, paper edit, captions/translation, OTIO/XML/EDL, plug-in hosts |
| P3 â€” Team scale | Secure shared work | Productions, cloud change sets, locking, review/comments, queue farm capabilities |
| P4 â€” Advanced R&D | Differentiated high-end workflow | Object masks, tracking, advanced optical flow, generative providers, external monitoring, multi-GPU tuning |

P0 should be complete, stable, and tested before starting high-cost AI or collaboration work. A professional editor earns trust by never losing edits and by playing media reliably before it earns trust through features.

## How to extend the core

The seams are the provider interfaces, and each has a worked example.

- **A new decoder** implements `media::Source` and `media::SourceProvider` and registers with `SourceRegistry`. `media/SyntheticSource.cpp` is the smallest complete one; `media/ffmpeg/FFmpegSource.cpp` is the full one. One video read and one audio read may run together; concurrent readers of the same stream need independent instances, which the decode pool gives each worker.
- **A new encoder or container** implements `media::Writer` and `media::WriterProvider` and registers with `WriterRegistry`.
- **A new command** is a payload struct, an entry in the descriptor table in `core/commands/Command.cpp`, a stateless check there, and an `Apply` overload in `core/project/ProjectStore.cpp`. Undo comes free: it is derived from the rows the command touched. Add a test that undoes it â€” especially if it deletes anything that cascades.
- **A new built-in effect** is currently a branch in `render/Compositor.cpp` (picture) or `audio/AudioMixer.cpp` (sound), with a golden frame or a numeric assertion. [ROADMAP.md](ROADMAP.md) explains why this should become a registered effect interface before any third-party plug-in exists.
- **Any change that alters pixels** must keep the golden frames passing, or accept new ones deliberately with `CUTLINE_UPDATE_GOLDEN=1` and review the result.

The service interfaces, data model and acceptance criteria are in [ARCHITECTURE.md](ARCHITECTURE.md); the order to build things in is in [ROADMAP.md](ROADMAP.md).
