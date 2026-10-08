# Cutline

Cutline is a professional, non-destructive, frame-accurate video editor for Windows, written in C++20 with a Qt Quick interface. It edits, grades, mixes, titles, captions and exports entirely on your own machine: **nothing in the editing, saving, recovery, analysis or export paths needs a network connection** (see [OFFLINE_OPERATION.md](OFFLINE_OPERATION.md)).

The native C++ core is the product. The browser prototype in the repository root (`index.html`, `app.js`, `styles.css`, `mcp-server.js`) is a superseded visual reference that shares no code with it and should not be developed further.

| | |
|---|---|
| **Status** | Working editor, not yet validated by people on production footage. The engines are far ahead of the interface in places; section 4 lists exactly where. |
| **Platform** | Windows 10/11, x64. Core and most tests are portable C++; audio output, the GPU compositor, text rasterisation and the build scripts are Windows-specific. |
| **Tests** | 756 native test cases in 12 suites plus 32 application tests that drive the real window offscreen; all pass (a few skip when media fixtures or optional tools are absent, and say so). |
| **Size** | about 66,000 lines of C++ and QML, 28,000 lines of tests |
| **Project format** | a folder, `<name>.cutline/`, holding a SQLite database (schema v13), a journal and snapshots |

---

## Contents

1. [Quick start](#1-quick-start)
2. [What works today](#2-what-works-today)
3. [Using the application](#3-using-the-application)
4. [What does not work yet](#4-what-does-not-work-yet)
5. [Concepts you need before changing anything](#5-concepts-you-need-before-changing-anything)
6. [Repository map](#6-repository-map)
7. [Building](#7-building)
8. [Testing and the checks that keep documents true](#8-testing-and-the-checks-that-keep-documents-true)
9. [Tools: screenshots, benchmarks, the demo](#9-tools-screenshots-benchmarks-the-demo)
10. [The document set](#10-the-document-set)
11. [Working on Cutline](#11-working-on-cutline)
12. [Product principles and scope](#12-product-principles-and-scope)
13. [The browser prototype](#13-the-browser-prototype)

---

## 1. Quick start

You need Windows with Visual Studio 2022 or later (C++ workload), Node.js (for the documentation scripts), and, for the application, Qt 6.8.3 (MSVC 2022 64-bit) under `.tools\qt`. FFmpeg (LGPL shared build) under `.tools\ffmpeg` is optional but needed for real media; without it only synthetic media works. Details in section 7.

```powershell
scripts\build.bat                 # core + native tests            -> build\native
scripts\test.bat                  # build, then run every native suite with the right environment
scripts\build-app.bat             # core + Qt application + app tests -> build\app
scripts\run-app.bat --demo        # open the application on a ready-made project
```

`python run.py --demo` does the same as the last line if you have Python (it builds what changed first); Python is otherwise not used.

The demo project is three synthetic clips (colour bars, a frame counter, a solid) cut on a timeline with linked sound. Before you read any code, spend twenty minutes with it: split a clip with `C`, trim with `B`, add an effect from the Effects panel, put a transition on a cut, switch to the Graphics workspace (`Alt+Shift+8`), export with `Ctrl+M`.

---

## 2. What works today

Everything in this section exists in the code and is covered by tests. "Engine" means the capability is built and tested but has no interface yet (see section 4).

### 2.1 Project and editing

| Area | What works |
|---|---|
| **Project** | create, open, save (snapshot), undo/redo with a History panel that can jump to any step; every edit is a validated command in an append-only journal; stale-revision writes and retried commands are handled; schema migration from older versions, newer versions refused |
| **Timeline** | multi-track video and audio, custom virtualised drawing (hundreds of tracks), ruler with timecode, selection (click, marquee, linked, track-forward), drag to move across tracks (overwrite or insert), trim head/tail, ripple, roll, slip, slide, razor, hand, zoom, snapping to edit points, markers and playhead, track mute/solo/lock, drag media from the Project panel, copy/cut/paste/paste-insert/duplicate, link/unlink, enable/disable, lift/extract/ripple delete, Add Edit (one track or all) |
| **Marks** | sequence in/out, go to in/out, clear, insert/overwrite using the marks (three-point resolution in the engine) |
| **Transport** | play, J/K/L shuttle to 8× with slow motion, frame step, 5-frame step, loop, go to start/end, previous/next edit point; audio is the master clock |
| **Retiming** | constant speed and reverse (Speed/Duration dialog), keyframed speed ramps with eased segments, freezes and reverse segments edited on a graph, pitch-preserving audio |
| **Nested sequences, adjustment layers, solid generators** | engine (compiler, compositor, mixer, store cycle checks); no interface to create them |
| **Multicam** | set-up from chosen clips, sync by audio, timecode, marker or by hand, live cutting with digits or clicks, cut editing, flattening to ordinary clips |
| **Transcript editing** | local whisper.cpp transcription (engine and model not bundled), word-level editing of the timeline, filler and pause removal, speaker names, captions from words |

### 2.2 Picture

| Area | What works |
|---|---|
| **Effects (41 video)** | transform (motion, opacity, crop), grade and colour tools, curves, hue curves, HSL secondary, channel mixer, black & white, tint, LUTs (3D/1D `.cube`), blurs and sharpen, glow, drop shadow, vignette, posterize, lens/wide-angle correction, stabilizer, rolling-shutter repair, mesh warp, wave warp, bulge, chroma and luma key, noise reduction, time remap, frame interpolation (blend and optical flow), blend modes (12) |
| **Keyframes** | hold, linear, bezier, ease over exact rational time; keyframe toggle and jump in the inspector |
| **Masks** | rectangle, ellipse, Bézier path drawn on the monitor, feather, expansion, opacity, invert, add/subtract/intersect, keyed over time, followed by a point/planar tracker |
| **Analysis** | point and planar tracking, similarity stabilisation, rolling-shutter analysis, optical flow — each a cancellable job ending in one undoable edit |
| **Transitions (23)** | cross, additive and film dissolves; dip to black and white; wipes, barn doors, clock; irises; pushes; slides; zoom dissolve; placed at a cut within the clips' handles |
| **Colour management** | per-sequence working and display spaces (Rec.709, sRGB, P3, Rec.2020, PQ, HLG, linear variants, ACEScg/AP0/ACEScc/ACEScct, ARRI LogC3, S-Log3, V-Log, Log3G10), input colour space override per clip, tone and gamut mapping, render versions so old projects keep their look |
| **Scopes** | luma waveform, RGB parade, vectorscope, RGB/luma histogram, measured off the playback thread |
| **Titles and graphics** | designer canvas (text, boxes, ovals, project pictures, rotation, outline, shadow, rounded corners, anchors, entrances), six bundled templates, template import/export, controls edited from the timeline selection |
| **Captions** | caption tracks and cues with styling, SRT/WebVTT import/export, burn-in through the same rasteriser the monitor uses |
| **Monitor** | latest-wins asynchronous presentation, full/half/quarter/eighth/auto playback resolution, fit/fill/100%/custom zoom and pan, safe margins, centre and thirds overlays, full-screen window, timecode badge, mask tools on the picture |
| **GPU** | Direct3D 11 compositor for the common subset (layers, motion, crop, opacity, all colour tools, LUTs, generators, adjustment clips, dissolves) and hardware decode read in place; everything else falls back per frame to the CPU compositor, which is the reference |

### 2.3 Sound

| Area | What works |
|---|---|
| **Mixer** | per-sample mixing from the sequence, track and bus strips, pre/post sends, routing with loop refusal, solo through buses, peak/RMS meters, WASAPI output |
| **Effects (10)** | volume, gain, pan, 3-band EQ, compressor, limiter, gate, auto-duck (side-chain by track), noise reduction, reverb reduction |
| **Essential Sound** | roles (dialogue, music, effects, ambience) with starting effect chains, loudness measurement (EBU R128, true peak), match to a target (R128, A/85, −14 streaming, −16 podcast), automation read/write/touch/latch, voice-over recording to the armed track |

### 2.4 Media and delivery

| Area | What works |
|---|---|
| **Import and ingest** | probe, sampled SHA-256 fingerprint, duplicate detection; File > Ingest copies with a hash while reading and a read-back check, then imports and makes a proxy |
| **Proxies** | ProRes proxy generation as cancellable jobs, fingerprint-checked association, automatic fallback to the original, a Proxies switch; exports always use originals |
| **Export** | 21 presets (H.264, HEVC 8/10-bit, AV1, VP9, MPEG-2 4:2:2 MXF, ProRes ×5, DNxHR ×4, FFV1, WAV, FLAC, AAC, MP3), graphics-card encoders when they work (AMD AMF verified) with reported software fallback, persistent queue with pause/cancel/retry/reorder that survives a restart, render overlapped with encode, finished-file validation |
| **Interchange (engine)** | OpenTimelineIO, CMX 3600 EDL, Final Cut 7 XML in and out with loss reports; project collection with verified copies and a manifest |
| **Recovery (engine)** | journal v2, snapshots, replay, quarantine, inspect-before-recover |

---

## 3. Using the application

### 3.1 Workspaces and panels

Eight built-in workspaces (`Alt+Shift+1` to `8`): **Editing, Assembly, Color, Audio, Effects, Multicam, Text, Graphics**. Panels dock, tab, split and float; arrangements are saved per user and survive a restart (`Window` menu: reset, save as a new workspace).

Seventeen panels: Project, Program Monitor, Timeline, Effect Controls, Effects (effects and transitions), History, Background Jobs, Scopes, Audio Mixer, Essential Sound, Markers *(placeholder)*, Multicam, Export Queue, Transcript, Captions, Titles, Graphic Designer.

`Ctrl+Shift+P` opens the **command palette**: every one of the 109 application commands, searchable by name. `Ctrl+Alt+K` opens the shortcut editor (conflict-checked remapping).

### 3.2 Default keys

| Area | Keys |
|---|---|
| File | `Ctrl+Alt+N` new project, `Ctrl+O` open, `Ctrl+S` save (takes a snapshot), `Ctrl+I` import, `Ctrl+Alt+I` ingest, `Ctrl+M` export, `Ctrl+Q` quit |
| Edit | `Ctrl+Z` undo, `Ctrl+Shift+Z` / `Ctrl+Y` redo, `Ctrl+A` select all, `Ctrl+Shift+A` deselect, `Ctrl+C` / `Ctrl+X` / `Ctrl+V` copy, cut, paste, `Ctrl+Shift+V` paste insert, `Ctrl+Alt+D` duplicate, `Delete` clear, `Shift+Delete` ripple delete, `Ctrl+Comma` preferences |
| Tools | `V` selection, `A` track select forward, `B` ripple edit, `N` rolling edit, `C` razor, `Y` slip, `U` slide, `H` hand, `Z` zoom |
| Transport | `Space` play/pause, `J` `K` `L` shuttle, `Left` `Right` step a frame, `Shift+Left` `Shift+Right` step five, `Home` `End` start/end, `Up` `Down` previous/next edit, `Ctrl+Shift+L` loop |
| Marks and editing | `I` `O` mark in/out, `Shift+I` `Shift+O` go to in/out, `Ctrl+Shift+I` `Ctrl+Shift+O` `Ctrl+Shift+X` clear in / out / both, `Comma` insert, `Period` overwrite, `Semicolon` lift, `Quote` extract, `Ctrl+K` add edit, `Ctrl+Shift+K` add edit to all tracks, `Ctrl+L` link/unlink, `Shift+E` enable/disable, `Ctrl+R` speed/duration, `Ctrl+Alt+R` speed ramp, `M` add marker |
| View | `S` snap, `Equal` `Minus` zoom, `Backslash` zoom to fit, `Ctrl+Backtick` full-screen monitor, `Shift+5` effect controls, `Shift+7` effects |
| Multicam | `1`–`9` cut to angle, `Shift+Space` play, `Alt+Up` `Alt+Down` previous/next cut |

### 3.3 Command-line switches of `Cutline.exe`

`--demo`, `--open <folder>`, `--config <folder>`, `--no-audio`, `--screenshot <png>`, `--delay <ms>`, and repeatable `--do trigger:<command id> | select:<clip name> | seek:<seconds> | effect:<type> | tool:<name> | workspace:<name>` (full list at the top of `app/main.cpp`).

---

## 4. What does not work yet

These are real gaps, found by reading the code on 2026-10-08 (the work queue is [TODO.md](TODO.md); how to build each is [IMPLEMENTATION_GUIDE.md](IMPLEMENTATION_GUIDE.md)).

| Area | Missing |
|---|---|
| **Editing interface** | source monitor, source in/out, four-point and replace edits, match frame, subclips; source patching and track targeting; sync lock; rate-stretch tool; effect copy/paste and presets |
| **Project panel** | bins, relink, search, labels, thumbnails (it is a flat list; the store has bins and relink commands the application never issues) |
| **Sequences** | creating, nesting, opening and switching sequences; breadcrumbs; adjustment layers and generator clips cannot be created from the interface |
| **Timeline drawing** | audio waveforms and video thumbnails on clips, cache-status bar, caption lane; a Markers panel (it is a placeholder) |
| **Engines without a dialog** | OTIO/EDL/XML import and export, project collection, the recovery prompt on opening a damaged project, autosave (the preference exists and does nothing) |
| **Picture** | track mattes, particles, lens flare, lightning, corner pin, mirror, echo, Ultra Key and about 60 other standard effects; colour-wheel and curve widgets (colour tools are typed numbers); most effects on the GPU; OpenColorIO, an ACES output transform, 10-bit end to end, HDR monitoring |
| **Sound** | plug-in hosting (VST3), reverb/delay/chorus/pitch/de-esser/multiband effects, input monitoring, control surfaces |
| **Delivery** | export at a size other than the sequence's, image sequences, GIF, smart render, render-and-replace, watch folders, HDR metadata |
| **Exchange** | AAF/OMF, OFX hosting |
| **Assisted editing** | scene detection, auto reframe, semantic search, translation, speech enhancement |
| **Scale and platform** | camera RAW formats, collaboration and review, VR, accessibility work, other operating systems, CI |
| **Known defects** | every command is stamped `2026-01-01T00:00:00Z`; ten of the 35 preferences are read by nothing; the "Basic Color" effect is listed twice |

Never treat a feature as finished because a button or a manifest entry exists: "implemented" in this repository means a working path with named passing tests, and the table above is the honest remainder.

---

## 5. Concepts you need before changing anything

1. **Everything is a command.** A change to a project is a typed `CommandEnvelope` (70 command types) validated, applied in one SQLite transaction by `ProjectStore` (the only writer), and recorded with the row-level changeset that undoes it. Undo costs bytes proportional to the edit. Several commands run as one undo step with `ExecuteGroup`.
2. **Time is exact.** `RationalTime` (normalised 64-bit rationals with checked arithmetic) is used for everything stored; frame rates such as 30000/1001 are exact; a 254,016,000,000-per-second tick column exists only to index ranges. `double` seconds appear only where QML needs them.
3. **A sequence is compiled, not drawn directly.** The timeline is loaded into an immutable snapshot; a pure `TimelineCompiler` turns a snapshot and a time into a `PlaybackPlan` (what to decode, in what order, with which sampled effect values). The monitor and the exporter render the same plan.
4. **The CPU compositor defines correct output.** The Direct3D 11 compositor must reproduce it within a stated tolerance and refuses (per frame) what it cannot render. Golden frames and GPU parity tests pin both.
5. **Audio is the clock.** Picture follows what is audible; export derives each frame's sample count from absolute time so audio and video never drift.
6. **Rules live in `ui/`, not in QML.** Anything that decides what a gesture means is a pure function returning an `EditPlan` (a label, commands, or a refusal with a reason), tested without a window.
7. **Render versions protect old projects.** If a change would alter the output of an existing project, it needs a new version in `core/model/RenderVersion.h`.
8. **Failures are reported, not hidden.** An unsupported effect, a missing file or a refused edit tells the person why; unknown effect types are kept and serialised.

The full technical description is [ARCHITECTURE.md](ARCHITECTURE.md), Part I.

---

## 6. Repository map

```text
video_editor/
├── core/                  foundation (depends on nothing else here)
│   ├── time/              RationalTime, FrameRate, drop-frame timecode, tick timebase
│   ├── anim/              keyframes and curves (hold / linear / bezier / ease)
│   ├── model/             shared vocabulary, RenderVersion
│   ├── commands/          70 typed commands: payloads, descriptor table, validation, labels, journal JSON
│   ├── db/                SQLite wrappers, changeset recorder (undo)
│   ├── project/           ProjectStore (only writer), Schema v13 + migrations, Consolidate (collect a project)
│   ├── jobs/              PriorityScheduler (bounded, classed, generation-cancelling)
│   ├── resource/          QuotaManager (RAM/disk/VRAM budgets with pressure)
│   └── util/              JSON writer/reader, XML reader, SHA-256, Base64, ParallelRows
├── effects/               EffectRegistry (51 built-ins), GraphicsDocument, MaskDocument
├── media/                 frames and buffers, Source/Writer interfaces, SyntheticSource, ingest, proxies
│   └── ffmpeg/            FFmpeg demux/decode/probe (+ D3D11VA) and encode/mux
├── timeline/              Sequence snapshot, SequenceLoader, TimelineCompiler, Multicam
├── render/                CPU compositor, Direct3D 11 compositor, colour, grading, keys, tracking, warps,
│                          optical flow, noise reduction, graphics, text, captions, transitions, scopes, caches
├── audio/                 mixer, DSP, time stretch, clock, WASAPI sink, capture
├── playback/              PlaybackEngine, DecodePool
├── exporter/              ExportWorker, ExportPresets, ExportQueue, ExportValidation
├── interchange/           neutral timeline model, OTIO, EDL, Final Cut 7 XML
├── captions/              caption model, SRT/WebVTT, sidecars
├── speech/                whisper.cpp adapter, word-timed transcripts
├── ui/                    pure UI logic (no Qt): edit rules, timeline view, monitor, transport, layout,
│                          shortcuts, preferences, inspector, jobs, ramp, multicam, masks, titles, audio workflow
├── app/                   Qt Quick application: Session (+ split files), TimelineItem, MonitorItem, MulticamItem,
│   │                      ScopeItem, DockController; main.cpp; demo_main.cpp (cutline_demo); bench_main.cpp (cutline_bench)
│   └── qml/               Main.qml, Theme.qml, dialogs, panels/ (17)
├── cmake/Fixtures.cmake   generates test media at configure time
├── tests/
│   ├── native/            12 suites, TestHarness.h, GoldenImage.h
│   ├── app/               application tests (Qt Test + the harness)
│   └── golden/            reference pictures (PPM)
├── scripts/               build.bat, build-app.bat, test.bat, run-app.bat, shot.bat, bench.bat, vsenv.bat,
│                          check-docs.js, cross-check-parity.js, generate-parity-report.js, ppm-to-png.js
├── parity/, feature-parity.yaml, premiere-parity-report.{md,json}   capability manifests and generated report
├── guide/                 parts B, C and D of the implementation guide
├── perf/                  raw benchmark output
├── third_party/sqlite3/   vendored SQLite 3.48 amalgamation (session extension enabled)
├── .tools/ (not committed)  ffmpeg, qt, whisper, vcpkg
└── run.py, index.html, app.js, styles.css, mcp-server.js, MCP.md, project-state.json   launcher and the prototype
```

---

## 7. Building

### 7.1 Requirements

| Needed | For | How |
|---|---|---|
| Visual Studio 2022+ (C++), CMake 3.24+, Ninja | everything | `scripts\vsenv.bat` finds them (`vswhere`, or `CUTLINE_VSROOT`) |
| Node.js | documentation and parity scripts, JavaScript prototype tests | nodejs.org |
| FFmpeg shared build (LGPL) | decoding/encoding real media | unpack to `.tools\ffmpeg` (`include`, `lib`, `bin`) or `-DCUTLINE_FFMPEG_ROOT=<path>`; a GPL build would impose GPL on the product. Without it the core, compositor, mixer and every non-decode test still build and run on synthetic media |
| Qt 6.8.3 MSVC 2022 64-bit | the application and app tests | `py -m pip install aqtinstall`, then `py -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O .tools\qt` (or `CUTLINE_QT_ROOT`) |
| whisper.cpp + a ggml model | transcription (optional) | `.tools\whisper` or Preferences > Transcription |

SQLite is **vendored** (3.48 amalgamation with the session extension, which undo needs), so there is no package-manager step.

### 7.2 Commands

```powershell
scripts\build.bat [target]        # configure on first use, build build\native (Debug, no Qt)
scripts\test.bat                  # build, then ctest --output-on-failure with fixture and golden folders set
scripts\build-app.bat [args]      # configure build\app against Qt, build everything incl. Cutline.exe and cutline_app_tests
scripts\run-app.bat --demo        # build if needed, set the Qt runtime path, launch
scripts\bench.bat [mode]          # Release build of cutline_bench: (none) | --gpu | --color | --flow | --range | --playback
build\native\cutline_demo.exe     # command-line end-to-end walkthrough (project, ingest, edit, render, mix, export, decode back, undo)
```

By hand:

```powershell
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCUTLINE_BUILD_NATIVE_APP=OFF
cmake --build build/native
ctest --test-dir build/native --output-on-failure
```

Options: `CUTLINE_BUILD_NATIVE_APP`, `CUTLINE_BUILD_TESTS`, `CUTLINE_ENABLE_FFMPEG`, `CUTLINE_USE_VENDORED_SQLITE`, `CUTLINE_FFMPEG_ROOT`. A change to `CMakeLists.txt` is picked up by the next `scripts\build.bat`. New source files go into the `cutline_core` list, and application files into both the `Cutline` module list and the `cutline_app_tests` list.

Debug timings are meaningless for performance; use Release (`scripts\bench.bat`).

---

## 8. Testing and the checks that keep documents true

| Layer | Where | What |
|---|---|---|
| Native suites (12) | `tests/native/*_tests.cpp` | `core` 48, `store` 129, `timeline` 78, `media` 69, `render` 151, `audio` 81, `playback` 49, `export` 36, `interchange` 31, `ui` 62, `gpu` 15, `speech` 7 |
| Golden frames | `tests/golden/*.ppm` | pixel comparison with per-channel tolerance; a failure writes `<name>.actual.ppm` and `<name>.diff.ppm` beside it; accept a deliberate change with `CUTLINE_UPDATE_GOLDEN=1` and review the picture |
| Application suite | `tests/app/app_tests.cpp` | the real `Session`, `TimelineItem`, `MonitorItem` driven by simulated mouse and keys on an offscreen window |
| Prototype helpers | `tests/*.test.js`, `node tests/run.js` | only the browser prototype's helpers and the parity aggregation |

**Environment variables**

| Variable | Effect |
|---|---|
| `CUTLINE_FIXTURE_DIR` | folder of generated media (`build/<dir>/fixtures`); CTest sets it. **Set it yourself when running a test executable directly**, or media/playback/GPU tests skip |
| `CUTLINE_GOLDEN_DIR` | `tests/golden`; set it too, or some render cases fail |
| `CUTLINE_STRICT=1` | a test skipped for a missing prerequisite (fixture, FFmpeg) becomes a failure; "does not apply here" skips stay skips |
| `CUTLINE_ONLY=text` | run only cases whose name contains `text` |
| `CUTLINE_TRACE=1` | print each case name before it runs, so a crash says where |
| `CUTLINE_UPDATE_GOLDEN=1` | rewrite goldens (review before committing) |

Application tests also need Qt's `bin` and `.tools\ffmpeg\bin` on `PATH`, `QT_QPA_PLATFORM=offscreen` and `QT_QUICK_BACKEND=software` (CTest sets them).

**Checks that must pass before a change is merged**

```powershell
node scripts/check-docs.js              # test/suite/command/effect/app-test counts quoted in the markdown, links, source paths,
                                        # mojibake, stray control characters
node scripts/cross-check-parity.js      # a capability may claim "implemented" only if it names tests that exist
node scripts/cross-check-parity.js --apply          # re-apply the reviewed assessment table to parity/*.yaml
node scripts/generate-parity-report.js  # regenerate premiere-parity-report.{md,json}
node tests/run.js
```

`--execute` on the parity check also runs `build\native` binaries and requires every named test to have passed. The parity manifests once drifted away from the code (they recorded nothing implemented while dozens of capabilities worked); the assessment table at the top of `scripts/cross-check-parity.js` is now the reviewed judgement and the generated report follows from it.

---

## 9. Tools: screenshots, benchmarks, the demo

- **Screenshot** the application without a display: `scripts\shot.bat out.png --demo --do select:Bars --do trigger:workspace.color`. Uses Qt's software renderer and a scratch configuration folder; QML errors go to stderr. Convert `.ppm` frames with `node scripts/ppm-to-png.js <dir> 2`.
- **Benchmarks:** `scripts\bench.bat` (compositing and mixing per frame), `--gpu` (card vs software), `--color`, `--flow`, `--range` (timeline queries up to 50,000 clips), `--playback`. Record results in [PERFORMANCE_LOG.md](PERFORMANCE_LOG.md) with the raw output under `perf/`.
- **`cutline_demo`:** builds a project package, ingests media, assembles a sequence with effects, keyframes and a transition, renders frames, mixes audio through the clock, exports a file and decodes it back to compare with the monitor, and exercises undo and redo.

---

## 10. The document set

| Document | Read it for |
|---|---|
| **This file** | orientation, how to build, run and test |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Part I: how the code works today, module by module (threads, data flow, schema, commands, render, audio, UI). Part II: the long-term target design |
| [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md) | the capability-by-capability truth table with limits and measurements |
| [GAP_ANALYSIS.md](GAP_ANALYSIS.md) | what is missing and in what order to close it |
| [TODO.md](TODO.md) | the ordered work queue (stable ids), the defects found by the code audit |
| [IMPLEMENTATION_GUIDE.md](IMPLEMENTATION_GUIDE.md), [guide/B-editing-surface.md](guide/B-editing-surface.md), [guide/C-effects-audio-colour-delivery.md](guide/C-effects-audio-colour-delivery.md), [guide/D-large-programmes.md](guide/D-large-programmes.md) | step-by-step instructions for every missing feature, with how to test each |
| [ROADMAP.md](ROADMAP.md) | the plan, with the parity cross-check |
| [premiere-parity-report.md](premiere-parity-report.md), [parity/README.md](parity/README.md), [feature-parity.yaml](feature-parity.yaml) | the 220 tracked capabilities and their evidence |
| [MILESTONES.md](MILESTONES.md), [REMEDIATION.md](REMEDIATION.md), [CODE_REVIEW_2026-10-06.md](CODE_REVIEW_2026-10-06.md) | milestone evidence; the 2026-10-06 independent review and the status of each finding |
| [PERFORMANCE_LOG.md](PERFORMANCE_LOG.md) | measured before/after performance changes |
| [OFFLINE_OPERATION.md](OFFLINE_OPERATION.md), [MCP.md](MCP.md), [BUILD_PROMPT.md](BUILD_PROMPT.md) | the offline boundary; the prototype's LLM bridge; the original build brief |

---

## 11. Working on Cutline

Start with [IMPLEMENTATION_GUIDE.md](IMPLEMENTATION_GUIDE.md): section 1 (set-up and daily commands), section 2 (rules and the definition of done), section 3 (recipes for adding a command, a schema change, a planner, a panel, an effect, an audio effect, a preference, a transition). Then take a Phase A package.

The seams in the core, each with a worked example:

- **A decoder** implements `media::Source` and `media::SourceProvider` and registers with `SourceRegistry` (`media/SyntheticSource.cpp` is the smallest; `media/ffmpeg/FFmpegSource.cpp` the full one).
- **An encoder or container** implements `media::Writer` and `media::WriterProvider`.
- **A command** is a payload struct, a row in the descriptor table, a stateless check in `core/commands/Command.cpp` and an `Apply` in `core/project/ProjectStore.cpp`; undo is free; add a test that undoes it.
- **A built-in effect** is a descriptor in `effects/EffectRegistry.cpp` plus a branch in `render/Filters.cpp` (picture) or `audio/AudioMixer.cpp` and `audio/Dsp.cpp` (sound).
- **Anything that changes pixels** must keep the golden frames passing, or accept new ones deliberately and review them.

Definition of done for any change: it works in the application, it has named tests including undo, the whole suite passes strict, `check-docs` passes, and the status documents are updated (the checklist is in the guide, section 2.1).

---

## 12. Product principles and scope

1. **Non-destructive and recoverable.** Every edit is a command recorded in a transaction and a journal; original media is never changed.
2. **Exact time, not floating-point approximations.** Rational time everywhere; sample-accurate audio clock.
3. **Hardware-aware, never vendor-locked.** The core chooses capabilities, not brands; vendor accelerators are optional providers with a CPU fallback.
4. **GPU-native when it is safe and useful.** Decode, colour, effects and composite should stay on GPU surfaces; avoid GPU↔RAM copies. (Today only a subset does; the CPU path is the reference.)
5. **One render graph, multiple quality policies.** Preview and final render are the same edit result with different quality.
6. **One command system.** Mouse, keyboard, scripts, assisted features, undo/redo and collaboration all create validated editor commands.
7. **Failure containment.** Decode, export, analysis and plug-ins should run in separate workers so an auxiliary crash cannot destroy an edit session. (Today everything is in one process; the interfaces are shaped for the split.)

**Scope boundaries.** Cutline aims for functional parity with professional editor workflows. It does not clone proprietary project-file internals, application UI, logos, vendor-specific generative implementations, stock or cloud account systems, or proprietary template formats. Interchange prioritises open and standard formats (OTIO, EDL, Final Cut XML, AAF) where technically and legally appropriate. The bundled looks are plain colour arithmetic, not emulations of any film stock or product.

**Licensing.** FFmpeg must be an LGPL shared build for a distributed product; SQLite is public domain; Qt is used under its open-source terms for development builds (review before distribution); every binary distribution needs a reviewed licence manifest, and restricted codecs, plug-in SDKs and models stay optional.

---

## 13. The browser prototype

`index.html`, `app.js`, `styles.css`, `mcp-server.js`, `project-state.json` and `core/*.js` are a **superseded visual prototype**: timeline positions are CSS pixels, playback is a `requestAnimationFrame` counter, grading is a CSS filter, export is a toast. It shares no code with the native core, is kept only as a UI reference, and `node tests/run.js` exercises only its small helpers and the parity aggregation. [MCP.md](MCP.md) documents its local LLM command bridge. Do not develop it further.
