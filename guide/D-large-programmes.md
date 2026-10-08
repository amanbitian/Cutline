# Phase D: large programmes

Part of the [implementation guide](../IMPLEMENTATION_GUIDE.md). Recipes R1 to R14 are defined there.

**These are not intern tasks on their own.** Each is multi-week work with a design decision that affects other parts of the product (a licence, a process boundary, a file format). The rule for every package here is the same:

1. **Write a design note first** (two to four pages, a markdown file in `docs/design/ (new)`): the problem, the options considered, the decision, the interfaces, the failure behaviour, the test plan, and the licence position. Get it reviewed by the person who owns the project.
2. **Then build it in the milestones listed**, each of which is shippable and tested by itself. No milestone may leave the product worse than before it started.
3. Keep every item behind a **capability check**: if the feature is not installed or not licensed, the application says so and carries on (the product principle "failure containment").

| Package | Title | Size | Depends on |
|---|---|---|---|
| [D1](#wp-d1-vst3-audio-plug-in-host) | VST3 audio plug-in host | XL | design note; C4 helps |
| [D2](#wp-d2-ofx-video-plug-in-host) | OFX video plug-in host | XL | design note |
| [D3](#wp-d3-aaf-and-omf) | AAF and OMF | XL | licence review |
| [D4](#wp-d4-offline-assisted-editing) | Offline assisted editing (scene detection, search, auto reframe, enhancement, translation) | XL | B9, B6 |
| [D5](#wp-d5-colour-management-completion) | Colour management completion (OpenColorIO, ACES output, 10-bit, HDR monitoring) | XL | C9 helps |
| [D6](#wp-d6-camera-formats) | Camera formats | XL | licence review per SDK |
| [D7](#wp-d7-collaboration-and-review) | Collaboration and review | XL | design note |
| [D8](#wp-d8-vr-and-immersive) | VR and immersive | XL | D5, C9 |
| [D9](#wp-d9-accessibility) | Accessibility | L | A3 |
| [D10](#wp-d10-quality-gates-ci-soak-and-performance) | Quality gates: CI, soak tests, performance baselines | L | |
| [D11](#wp-d11-other-platforms-and-hardware) | Other platforms and hardware providers | XL | design note |

---

## WP-D1. VST3 audio plug-in host

**Goal.** Load third-party VST3 effects on tracks and clips, with parameters saved in the project, automation, and without a plug-in crash taking the editor down.

**State.** Nothing is implemented. A VST3 SDK has been fetched and built under `.tools/vst3sdk` and `.tools/vst3sdk-build` (ignored by git, referenced by no `CMakeLists.txt`). **Read the licence file in that folder first** and write what it says in the design note: it decides whether the SDK may be linked into a shipped application.

**Design decisions to make in the note**
- **Out-of-process host.** A plug-in runs in a separate process (`CutlineAudioHost.exe (new)`) so a crash or hang cannot take the editor or corrupt the project. The mixer talks to it through shared memory ring buffers and a small control channel. This is the product principle ("failure containment") and is not optional for a commercial editor.
- **Latency and the real-time rule.** The mixer renders on a producer thread and the device callback is lock-free (see the header of `audio/WasapiSink.cpp`). A plug-in has latency (`getLatencySamples`) and may not return in time. Decide: plug-in effects are rendered **ahead** in the producer thread's block, never in the device callback; a deadline miss bypasses the plug-in for that block and counts an event (like the existing underrun counters). **Plug-in delay compensation** (PDC) must be designed before hosting: each track's latency is the maximum of its plug-ins; all tracks are delayed to match. `docs/design` must show how PDC interacts with the finite-memory rule of `audio/Dsp.h` (a plug-in has unbounded memory; its output can depend on how the render was cut: decide whether to render plug-in tracks only sequentially and cache the result).
- **Offline determinism.** Export must equal what was heard. A plug-in's output may vary run to run (random modulation). Decide whether export "freezes" plug-in tracks first (render to a cache keyed by plug-in id, version, state and input hash — see `render/RenderCache.h` for the addressing idea).

**Milestones**
1. *Scan and list.* An executable that scans the VST3 folders (`C:\Program Files\Common Files\VST3`), loads each module in a **child process with a timeout**, reads its class info and parameters, and writes a JSON cache. Plug-ins that crash are quarantined (listed with the reason, not loaded again until the file changes). Test: a deliberately crashing test plug-in (build a tiny VST3 from the SDK sample that calls `abort()`) is quarantined and the scan continues; a good one is listed with its parameter count.
2. *Process one block.* `AudioHost` loads a plug-in, `setupProcessing`, processes a buffer. Test with the SDK's sample gain plug-in: output = input × gain, bit exact.
3. *Mixer integration.* A new effect type `vst3` (registry descriptor with `preset_name` = plug-in id; state saved as a project asset blob or in a table `plugin_state(effect_id, blob)`; add to `RecordedTables()`). `AudioMixer` calls the host for blocks. Latency reporting and PDC. Test: block independence for a *deterministic* plug-in (the sample gain); latency of a delay plug-in is compensated (an impulse arrives at the same time through the plug-in track and a dry track).
4. *Parameters and automation.* Map plug-in parameters to keyframeable effect parameters (the inspector draws them from the descriptor generated at load time). Test: automation of the gain parameter follows a keyframe ramp.
5. *Editor window.* Embed the plug-in's own UI (`IPlugView` into a native window owned by the host process, shown from the mixer strip). Manual test only.
6. *Crash recovery.* Kill the host process during playback: audio continues with the plug-in bypassed, the strip shows "plug-in stopped", a "Restart" action respawns the host and restores state. Test by killing the process in an app test.

**Check by hand.** Insert a free VST3 EQ on the music track; open its window; move a band; play; export the WAV and compare against what you heard; kill the host in Task Manager during playback — no click, no crash.

**Docs/parity.** `vst3_hosting`, `plugin_scan`, `plugin_crash_recovery`, `plugin_parameter_automation`, `plugin_capability_registry`, `plugin_render_cache`. Add a licence manifest entry (`docs/licences.md (new)`).

---

## WP-D2. OFX video plug-in host

**Goal.** Run OpenFX image effects (third-party filters, keyers, transitions) inside the render path.

**Design decisions for the note**
- **Licence:** the OFX headers are BSD-licensed; the plug-ins' own licences are the user's business. Record it.
- **Process model:** same reasoning as D1. Frames are large; use shared memory with the host (a pool of RGBA float32 buffers) and a command channel. Budget the copy cost: 1080p float RGBA is 33 MB; two copies per effect per frame is the cost to measure and to hide with the render cache.
- **Mapping:** each OFX effect becomes an effect type `ofx:<plugin id>` whose parameter descriptors are *generated at load* from the plug-in (`kOfxParamType...` → `Scalar/Vec2/Vec3/Colour/Choice/Bool`). The registry (`effects/EffectRegistry`) is static; add a dynamic registration API (`effects::RegisterEffect(descriptor)` guarded by a mutex) and make `FindEffect` consult it. Unknown effects must keep working as today ("unknown effects remain serializable": a project saved with an OFX effect opens on a machine without it and shows a placeholder, never loses parameters).
- **Compositor integration:** the CPU compositor calls the host through the same hook as `ApplyFilterEffect`; the GPU path refuses (`Supports` says "OFX effect") until a texture-sharing design exists.

**Milestones**: (1) discover bundles in `C:\Program Files\Common Files\OFX\Plugins`, load in a child process, list effects and parameters (same quarantine rule as D1); (2) render one frame through a simple built-in test plug-in you write (the OFX sample "Invert") and compare to `invert` from C3; (3) parameters round-trip through the project, including keyframes; (4) placeholders for missing plug-ins; (5) caching: key = plug-in id + version + parameter hash + input hash; (6) crash recovery as D1; (7) transitions and generators (later).

**Tests**: the test plug-in is built by the test suite (a tiny OFX bundle in `tests/plugins/ (new)`); native tests drive the host without Qt. Crash test as D1.

**Docs/parity**: `ofx_hosting` and the other `plugins/*` entries; ARCHITECTURE section 14.

---

## WP-D3. AAF and OMF

**Goal.** Exchange timelines with Avid-world tools.

**Why it is a design item.** AAF is a structured-storage file (Microsoft Compound File Binary) holding an object model (SMPTE ST 2001). OMF is the older audio-only format. Options: (a) wrap the open source AAF SDK, (b) wrap `libaaf`, (c) implement a read-only subset ourselves on top of a CFB reader. **Do the licence review first**: the AAF SDK is under the AAF Association public licence; `libaaf` is LGPL; neither may be assumed compatible with the product's distribution model. Record the decision.

**Milestones** (assuming option (c), reading first, since reading real files is what editors need):
1. *CFB reader* (`interchange/Cfb.h (new)`): sector chain, FAT, mini-FAT, directory tree; tests with synthetic files you build in the test (write a minimal CFB writer in the test) and with a public sample.
2. *AAF object model reader*: `CompositionMob` → `TimelineMobSlot` → `Sequence` → `SourceClip`/`Filler`/`Transition`/`OperationGroup` (dissolve, speed); resolve `SourceMob` → `FileDescriptor` → media identity (path or embedded). Produce `interchange::Timeline` (the neutral model) with `Report` entries for everything dropped.
3. *Import dialog hooks* (B12) for `.aaf`.
4. *Writer* for the same subset (clips, gaps, dissolves, markers, audio clip gain, media references as "linked" not embedded).
5. *OMF audio export* (`.omf` for DAW hand-off) last, only if a customer requires it.

**Tests**: fixtures from a real project exported by an Avid-compatible tool (ask for permission to commit a tiny one; otherwise keep them out of the repo and skip the tests with `SKIP_UNLESS`), plus round trip through the neutral model: `Timeline -> AAF -> Timeline` equal within the report's stated losses.

**Docs/parity**: `aaf_import`, `aaf_export`, `omf_audio_export`; `IO-002`.

---

## WP-D4. Offline assisted editing

**Goal.** Analysis that proposes edits, running only on the user's machine (see `OFFLINE_OPERATION.md`). The rule from the architecture: **AI proposes normal commands, the user accepts**; AI never writes to the database directly.

**Common infrastructure (do this first, M)**
1. A provider interface `ai/Provider.h (new)`: `Id()`, `Version()`, `Capabilities()`, `Run(Request, JobContext&) -> Result`. A provider is a loadable module or a child process (like whisper.cpp today: `speech/Whisper.h`). Model files live in the configuration folder; absence is reported, never fatal.
2. A results table `analysis_results(id, kind, media_fingerprint, range_start, range_end, provider, model, version, params_json, status {pending, accepted, rejected}, data_json, created)` (R2, add to `RecordedTables()`), commands `SaveAnalysis`, `SetAnalysisStatus`, `DeleteAnalysis` (R1) — modelled on `SaveTrackingData`.
3. A proposals panel: a list of proposed edits (each is an `EditPlan`), "Preview" (run on a copy? — show the markers/ranges on the timeline), "Accept" (apply as one undo step), "Reject".

**Features, each its own package**
- **Scene edit detection (M).** Pure algorithm, no model: per-frame colour histogram (e.g. 16 bins × 3) at a reduced size, distance between consecutive frames, adaptive threshold (mean + k·sigma over a sliding window), minimum scene length; output cut times. Proposal: markers or `SplitClip` at the cuts of a selected clip. Build `analysis/SceneDetect.h (new)`; tests on synthetic media with known cuts (`media::SyntheticSource` with a pattern change at known frames) — precision/recall = 1 on the synthetic set; a gradual fade is *not* a cut unless the threshold says so.
- **Semantic and transcript search (L).** Transcripts already exist per media (`speech/Transcript`). Step 1: search *all* transcripts (word and phrase, case-insensitive, with a result list that seeks) — no model. Step 2 (needs a model): image/text embeddings with an ONNX CLIP-like model through ONNX Runtime (MIT); store vectors in `embeddings` (blob per sampled frame); nearest-neighbour by brute force (fine to ~1 million vectors) — design note decides the model and its licence.
- **Caption translation (L).** Offline machine translation with CTranslate2 plus an Opus-MT model (licences per model!). Operates on `captions::Track` cues, produces a new caption track in the target language via `AddCaptionTrack` and `AddCaptions` (one undo step), preserving timing; length limits per line re-run through `captions` line-breaking.
- **Speech enhancement (L).** RNNoise (BSD) or a DeepFilter model as a stateful audio effect (R9) *or* an offline render of a cleaned file replacing the clip's audio (via C7.4-style replacement). Prefer the effect form if the model can be made finite-memory; otherwise offline.
- **Auto reframe (L).** Subject tracking: detect salient region (motion energy + a face/person detector via ONNX), smooth the path with the existing keyframe machinery, and write keyframes of the `motion` effect (position/scale) for a target aspect ratio. Reuses `render/Tracking.h` for following. Proposal = keyframes; Accept = `SetKeyframe` commands in one group.
- **Object selection (XL).** Interactive segmentation (SAM-class model) producing a mask polygon; convert to `effects::mask::Document` Bézier and track it with the existing mask tracking. Large model, heavy hardware: design note.
- **Generative extend (XL, last).** Provider-neutral contract; every result is a *derived media asset* with provenance; never overwrite; user must opt in; disabled by default.

**Tests (all)**: provider contract tests with a fake provider (deterministic output) — proposals accept/reject/undo; no-network proof: the analysis code paths must not link a network library (`scripts/check-offline.js (new)` greps the link line and sources for sockets/HTTP and fails the build if found; keep `OFFLINE_OPERATION.md` true mechanically).

**Docs/parity**: the `ai` domain.

---

## WP-D5. Colour management completion

**State.** Working/display transforms, ACES and four camera log families, PQ/HLG, tone mapping, LUTs, scopes exist (`render/ColorManagement.h`). Missing: OpenColorIO, an ACES Reference Rendering Transform and Output Device Transform, an end-to-end 10-bit path, HDR monitoring, tetrahedral LUT interpolation, `.3dl`/`.csp` LUT formats, separate input/display LUT roles.

**Milestones**
1. *Tetrahedral interpolation (S).* Add `Interpolation::Tetrahedral` to `render/CubeLut.*`; make it the default for 3D LUTs in new projects (render version gate: existing projects keep trilinear — `core/model/RenderVersion.h`, add version 4 "tetrahedral LUTs"); GPU shader port; tests: identity LUT exact, a known LUT matches a reference implementation on a lattice and off-lattice (compute expected values by hand in the test for 5 points), CPU vs GPU parity.
2. *More LUT formats (S).* `.3dl` (Autodesk, integer range) and `.csp` (Cinespace) readers beside `CubeLut`; validation messages; tests with small hand-written files.
3. *OpenColorIO (L).* Add OCIO (BSD-3) via vcpkg (`vcpkg.json`); `render/OcioTransform.h (new)` wraps `OCIO::Processor` -> a CPU function over `Layer` and a baked 3D LUT (65^3) for the GPU path (so the card needs no OCIO). Config selection in Sequence > Colour Settings (built-in `ocio://default` ACES studio config plus a user `.ocio` file). Render-versioned. Tests: a Rec.709 → ACEScg → Rec.709 round trip within 1e-3; the built-in ACES view of a grey ramp matches the published values (ask for them: ACES documentation has reference values for 18% grey).
4. *ACES RRT/ODT (M, via OCIO or by transcribing the CTL).* Ship as "ACES view (SDR)" and "ACES view (HDR 1000 nit)" display options.
5. *10-bit end to end (L).* The compositor already supports `Rgba16`/`RgbaF32` output (`render/Compositor.h`), decode reads >8-bit sources as `Rgba16`, the writer narrows to 16 bits for deep presets (EXPORT-002). The gaps: the **GPU path outputs 8-bit or float only** and exports use the CPU; make the swap-chain and monitor path 10-bit capable; verify the whole chain with a 10-bit ramp (1024 distinct levels survive decode → grade → export → decode). Test exists for ProRes (`APictureWithMoreThanEightBitsKeepsThemThroughAProResExport`); add the same through a grade and a LUT.
6. *HDR monitoring (L).* Qt 6.8 on Windows can present with an HDR colour space on a capable display (`QRhiSwapChain` formats `HDRExtendedSrgbLinear` / `HDR10`). Add a monitor mode "HDR (PQ)" that sends PQ-encoded pixels, a nit-level readout, zebra/false-colour overlays, scopes with a nit scale. Needs an HDR display to verify — put "tested on <display model>" in the docs.
7. *Colour pickers/eyedropper and Color Match (M).* Eyedropper on the monitor reads a pixel in working space; "Color Match" estimates a grade to match a reference frame (match mean and covariance of the Lab distribution → write `color_wheels`/`color_adjust` values).

**Docs/parity**: the `color` domain; COLOR-001, COLOR-004.

---

## WP-D6. Camera formats

**Reality check.** Almost every professional camera format is decoded through a vendor SDK with its own licence. FFmpeg decodes ProRes (including 4444), DNxHD/HR, XAVC-I/L (H.264), AVC-Intra, and many broadcast formats, and the product already handles those; RAW formats (RED R3D, ARRIRAW, Blackmagic RAW, Canon Cinema RAW Light, Sony X-OCN, ProRes RAW) are not decoded by FFmpeg.

**Design decisions for the note**
- **A runtime-loaded provider.** Each RAW SDK is a separate optional DLL provider implementing `media::SourceProvider` (the interface exists; see `media/Source.h`). Cutline ships without any vendor SDK; a user (or an installer with the right licence) drops the provider in. The capability registry (`camera_format_capability_registry`) lists installed providers.
- **RAW semantics.** A RAW clip carries decode parameters (debayer quality, white balance, ISO/exposure, colour space/gamma, highlight handling) that are *project data*, not preferences: add `source_settings_json` to `media` (R2) and an inspector section "Source settings". Decoding to `Rgba16/F32` in a documented working space; the camera-log spaces are already defined in `render/ColorManagement.h` for the four families.
- **Proxies** for RAW are mandatory in practice; the ingest workflow (`media/IngestWorkflow.h`) must default them on for these formats.

**Milestones**
1. *Registry and probing.* A table of known formats (container, codec fourcc, file extensions) with status (decoded by FFmpeg / needs provider / unsupported) shown in Properties and the import report ("This RED file needs the R3D provider"). Fixtures: tiny public sample files per FFmpeg-decodable format (XAVC, DNxHR, ProRes 4444, AVC-Intra); tests assert probe and decode.
2. *Provider SDK contract.* `media/ProviderModule.h (new)`: a C ABI (`cutline_provider_open`, `...read_video`, `...read_audio`, `...close`) loaded with `LoadLibrary`; version negotiation; sandbox in a child process like D1 if the SDK is unstable. Write a toy provider as a test double.
3. *One real provider* (choose with the owner: Blackmagic RAW SDK is free to redistribute under its licence; verify). Implement, validate against reference stills exported by the vendor's tool (RMSE threshold documented).
4. *Others* as customers require.

**Docs/parity**: the `camera-formats` domain; a licence entry per SDK.

---

## WP-D7. Collaboration and review

**Goal.** Optional, offline-first collaboration: a shared project folder or a LAN peer, never an internet dependency (`OFFLINE_OPERATION.md`).

**What already helps.** Every edit is a command with an author, a base revision, an idempotency key, and a durable journal record containing its changeset. That is most of a change-set protocol.

**Design decisions for the note**
- **Model.** Two options: (A) *shared package* — several processes open one package on a network share with SQLite WAL (not safe over SMB; do not do this); (B) *separate packages exchanging journals* (recommended). Each user has a local package; "sync" exchanges journal records through a shared folder; the receiver replays the foreign commands against its own store with new base revisions (`ReplayRecordLocked` already replays idempotently); a command that fails validation becomes a **conflict record** for the user to resolve.
- **Locks** to avoid conflicts: sequence and bin locks as data (`locks` table: entity kind/id, owner, expires) with commands `AcquireLock`, `ReleaseLock`, `BreakLock`; the validator refuses edits to a locked entity by anyone else (extend the lock policy in `ProjectStore.cpp`, the same place that handles track locks).
- **Identity**: stable author id per user (a GUID saved in the configuration, plus a display name), never an account system.
- **Undo** stays local: the history panel must not undo someone else's commands (the architecture says remote changes are separate).

**Milestones**
1. *Identity and attribution* (S): author id/name in configuration; `Envelope()` uses it (instead of the literal `"user"`); the History panel shows authors; the journal already stores `author_id`.
2. *Locks* (M): schema, commands, validator, UI badges on bins/sequences ("locked by Ana"), tests: refusal for other authors, break lock, expiry.
3. *Journal export/import* (L): `Session::syncWith(folder)` writes new local records to `<folder>/<author>/<revision>.json` and replays others' unseen records; conflict list; tests with two stores in one process (a harness already exists in `store_tests.cpp`): non-overlapping edits merge identically on both sides; overlapping edits to one clip produce one conflict and neither store is corrupted; replay is idempotent (run sync twice).
4. *Comments and review* (L): tables `comments` (sequence, time, author, text, parent, resolved) and `annotations` (drawing JSON); commands; a Review panel (list, add at playhead, reply, resolve); review package export: a proxy-quality render of the sequence plus `comments.json`, openable by another Cutline in "review mode" (read-only). Approvals as records.
5. *Productions* (XL, later): many small projects sharing one media manifest.

**Tests/guards**: the "editor remains complete with networking disabled" claim becomes a test: all of the above uses only the file system.

**Docs/parity**: `collaboration` and `review` domains.

---

## WP-D8. VR and immersive

Deferred until the conventional pipeline has fixture coverage. Outline for the note: probe spherical/stereo side data (`AV_PKT_DATA_SPHERICAL`, `stereo3d`) into `media_streams` (R2); monitor view transform (equirectangular → rectilinear viewer with yaw/pitch/FOV, a shader in the monitor path, so it depends on C9 step 9); a *Reframe* effect (virtual camera keyframes, non-destructive); export metadata (spatial media boxes in MP4/MOV); spatial audio (first-order ambisonics B-format: channel layouts and a head-tracked binaural monitor mix). Each is a milestone with a synthetic fixture (a generated equirect pattern with known landmarks).

---

## WP-D9. Accessibility

**Goal.** Usable by keyboard only, with a screen reader, at larger sizes and with high contrast.

**Steps**
1. **Keyboard navigation (M).** Define the focus order: menu bar → panels (each panel has one `FocusScope` and a command `panel.focus_<id>`) → within a panel, Tab moves between controls and arrow keys inside lists. Today keys not taken by a text field reach `Session::handleKey`; make panel focus visible (a 2 px accent outline in `PanelGroup.qml`). Test (app): `Alt+Shift+1` ... then `Tab` reaches every primary panel; add `KeyboardAloneReachesEveryPrimaryPanel` (TODO UI-001's acceptance criterion).
2. **Screen reader (L).** Add Qt `Accessible.role`, `Accessible.name`, `Accessible.description` to every interactive QML item (start with `Chip.qml`, `Check.qml`, `NumberBox.qml`, `ParameterRow.qml`, the panels' lists). For painted items (`TimelineItem`, `MonitorItem`) implement `QAccessibleInterface` subclasses that expose clips as children (name, start, duration, track) — a custom accessible for the timeline is the hard part. Verify with Windows Narrator and Accessibility Insights; record results in the docs.
3. **UI scale (S, finishing A3 step 5).** Verify at 150% and 200%: no clipped text; minimum panel sizes still fit (`ui/Layout.cpp` panel minima are in device-independent pixels).
4. **High contrast and colour independence (M).** A high-contrast theme (A3 step 4 gives you the theme mechanism); ensure state is not colour-only: mute/solo/lock/enabled have shapes and text, the safe-area overlays have dash styles, scope traces have legends. Checklist test: `grep` QML for `color:` used as the *only* signal is not automatable — write a manual checklist in `docs/accessibility.md (new)` and review each panel.
5. **Caption accessibility checks (M).** A QC report for caption tracks: maximum characters per line, lines per cue, reading speed (characters per second), minimum duration, overlaps, safe area — pure functions in `captions/` with tests; shown in the Captions panel as warnings.

**Docs/parity**: the `accessibility` domain.

---

## WP-D10. Quality gates: CI, soak, performance

**Goal.** Make the rules in the guide enforced by a machine.

**Steps**
1. **One command for everything (S).** `scripts\ci.bat (new)`: build native and app, run all suites with `CUTLINE_STRICT=1` and the right environment variables, run `node scripts/check-docs.js`, `node scripts/cross-check-parity.js --execute` (fix `runSuites` to look in the build directory you used), `node tests/run.js`; exit non-zero on any failure; write a one-page summary. Run it before every pull request.
2. **A hosted workflow (M)** once a repository host exists: the same script on a Windows runner (cache `.tools`). Without a host, a scheduled task on a build machine.
3. **Performance baselines (M).** `scripts/perf-gate.js (new)`: runs `cutline_bench` modes in Release, parses the output, compares with `perf/baseline.json (new)` (per-case median with an allowed regression, e.g. 15%), prints a table and fails on regression. Update the baseline deliberately in a commit that explains why.
4. **Soak tests (M).** A long-running test binary `cutline_soak (new)`: open the demo project, 10,000 random edits with undo/redo, save/reopen every 500, `ValidateDatabase()` after each save; play for 10 minutes with audio, measure underruns and frame deadline misses; fail on leaks (compare working set at start and end with a tolerance). Run nightly, not per commit.
5. **A/V sync measurement (M).** Export a 1-hour synthetic sequence (frame counter + click track), probe the output: audio sample count equals `frames * rate / fps` exactly (the existing export test does this for 1000 frames); add a long variant behind `CUTLINE_LONG=1`.
6. **Sanitisers (S).** A CMake option `CUTLINE_SANITIZE` building the core and tests with AddressSanitizer (MSVC supports `/fsanitize=address`); run the native suites under it weekly.

**Docs**: `README.md` "Reliability", `ARCHITECTURE.md` section 17, `TODO.md` completion checklist.

---

## WP-D11. Other platforms and hardware providers

A design-note-first programme; each item is independent.

- **Hardware encode/decode providers:** NVENC/QSV verification on machines that have them (the presets already list them; they have never run in this project); Media Foundation as a Windows decode/encode provider; NVDEC; Intel Quick Sync decode. Process per provider: capability probe (the `media::TestEncoder` pattern in `exporter/ExportPresets.cpp`), a preset chain entry, a parity test that decodes/encodes a fixture and checks picture similarity, an entry in the generated compatibility matrix (C10) naming the adapter it ran on.
- **Direct3D 12, Vulkan, Metal executors:** the backend-neutral graph exists (`render/RenderGraph.h`, `RenderGraphBuilder`) but **no executor consumes it** — the D3D11 compositor executes the playback plan directly. Step 1 is to make the D3D11 path execute the compiled graph (reuse of unchanged branches, region-of-interest, intermediate caching), because that proves the graph is sufficient; only then a second API.
- **macOS and Linux:** `audio/` (WASAPI today) needs CoreAudio / ALSA-PipeWire sinks (`audio/AudioSink.h` is the interface), `app/` needs a platform check for the GDI text rasteriser (`render/TextRaster.cpp` uses GDI on Windows; replace behind the same interface with CoreText/FreeType), the build scripts need non-batch equivalents.
- **Professional video I/O (SDI/NDI):** an `OutputDevice` provider interface fed with the program picture and a clock reference; start with NDI (software, no hardware) as the first provider.
- **Multi-GPU scheduling:** after D3D12 or at least the graph executor: the scheduler (`core/jobs/PriorityScheduler.h`) gets a device dimension.

---

Return to the [implementation guide](../IMPLEMENTATION_GUIDE.md).
