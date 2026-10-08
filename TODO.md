# Cutline implementation backlog

Last updated: 2026-10-07 (`[-]` items have the limitations listed under them; see IMPLEMENTATION_STATUS.md)

This is the operational checklist for building Cutline feature by feature. [IMPLEMENTATION_GUIDE.md](IMPLEMENTATION_GUIDE.md) tells an engineer, step by step and with the test for each step, how to build the items marked absent below. `IMPLEMENTATION_STATUS.md` records verified current behavior, `ROADMAP.md` explains the longer strategy, and the manifests in `parity/` are the capability inventory. Update this file whenever a work item starts or finishes.

## Status rules

- `[x]` means the engine path, persistence, user-facing controls, and relevant tests are complete unless the item explicitly has a narrower scope.
- `[-]` means a useful subset works, but the limitations listed under the item remain.
- `[ ]` means the feature is absent or has not met its acceptance criteria.
- A feature is not complete merely because a button or prototype exists. Preview and export must use the same implementation.
- Every completed engine feature needs deterministic tests. Visual effects need pixel or golden-image coverage. Performance claims need a Release benchmark recorded in `PERFORMANCE_LOG.md`.
- New render behavior must be versioned before it can silently change existing projects.

## Verified foundation

- [x] Exact rational timeline time and broadcast frame rates.
- [x] SQLite project packages, command journal, row-level undo/redo, snapshots, and linked edits.
- [x] FFmpeg probe, ingest, software decode, timestamp maps, relink, encode, mux, and export safety.
- [x] Audio mixer, automation, retiming, crossfades, master clock, and WASAPI output.
- [x] Bounded video decode pool, generation cancellation, read-ahead, and byte-bounded frame cache.
- [x] CPU reference compositor with premultiplied-alpha composition, adjustment layers, nested sequences, and sequence effects.
- [x] Motion, opacity, crop, basic grade, solid generator, blur, sharpen, vignette, and lens/wide-angle correction.
- [x] Cached 3D and 1D `.cube` LUT loading (a fast locale-independent reader, domain bounds, Adobe input ranges) with trilinear interpolation, intensity, asset recovery, error reporting, and the same LUT on the GPU (a 3D texture; GPU-003).
- [x] Ingest: verified copy (hash, write, read back, compare), planning, proxy choice and a proxy status interface (UI-INGEST).
- [x] A shipped pack of nine creative looks written as `.cube` files, thumbnails in the LUT browser, and a user LUT folder (UI-LUT).
- [x] Translational stabilization analysis with editable correction keyframes and automatic crop scaling.
- [x] Import at source resolution; the FFmpeg writer takes explicit output dimensions, but `ExportWorker` refuses any size other than the sequence's (no resize render yet).
- [x] 760 native tests across 12 suites, plus 32 offscreen Qt application tests; implemented parity claims are tied to named tests.

## Current limitations of implemented finishing features

| Feature | What works | What still prevents professional completion |
|---|---|---|
| Color grading | Exposure/contrast/saturation, wheels, curves, hue curves, white balance, vibrance, HSL secondary, working/display transforms, PQ/HLG, scopes; every tool on the card and multi-core in software (GPU-003) | No OpenColorIO, an ACES output transform, shot matching, 10-bit end-to-end path, HDR monitor output, or complete UI |
| LUTs | Cached 3D and 1D `.cube`, domain bounds, intensity, relink recovery, GPU texture path, searchable browser with thumbnails, a shipped look pack and a user folder | No tetrahedral interpolation, `.3dl`/`.csp`, or separate display/input LUT roles; LUTs larger than 129 points a side stay in software |
| Lens correction | Radial and quadratic distortion, center/scale, camera-profile-driven automatic rolling-shutter correction and mesh warp | No chromatic-aberration model, measured camera-profile library, UI, or GPU path |
| Stabilizer | Similarity stabilization (translation, rotation and zoom), RANSAC, crop policies, editable keys | No perspective stabilisation, confidence UI, or GPU path |
| Blur/sharpen/vignette | Keyframeable CPU reference implementations | No GPU shaders, masks, edge modes, or interactive controls |
| Playback | Decode read-ahead plus an asynchronous latest-wins monitor presenter | No GPU upload path, unified priority scheduler, or production-footage frame-deadline validation |
| Export | 21 presets, persistent queue, pause/cancel/retry/reorder, AMD AMF hardware encode with probed NVENC/QSV and software fallback, finished-file validation | No image sequences/stills, smart render, render-and-replace, watch folders, certified broadcast wrappers, or HDR metadata |

## Ordered implementation queue

The identifiers below are stable. Pull requests and milestone notes should reference them.

### Foundation required by UI and GPU

- [x] **FX-001 — Effect descriptor and registry**
  - Describe every built-in effect once: identifier, display name, category, parameter type, unit, default, range, keyframeability, and CPU/GPU availability.
  - Command creation and later constant/keyframe edits validate registered names, dimensions, finite values, ranges, keyframeability, and required assets. Unknown effect types remain serializable for future plug-ins.
  - Video/audio built-in discovery now uses the registry; backend implementation dispatch retains its tested CPU branches.
  - Completed in `effects/EffectRegistry.h` and `effects/EffectRegistry.cpp`; QML control generation is tracked by UI-004.
  - Acceptance met: existing render/audio tests are unchanged; invalid registered parameters are rejected; registry tests enumerate every built-in and verify its defaults (17 when this was written; 51 now, 41 video and 10 audio).

- [x] **RENDER-001 — Render semantic version** — **NEXT**
  - **Status (2026-10-06):** Done: render versions v1–v3, migration, refusal of newer versions.
  - Add `render_version` to sequences and package migration.
  - Make the compiler/compositor receive it and preserve old semantics after future color or resampling changes.
  - Acceptance: opening an older package selects its original behavior; new sequences receive the latest version; migration and round-trip tests pass.

- [-] **UI-001 — Native application shell**
  - **Status (2026-10-07):** `Main.qml`, dock areas, built-in workspaces, persisted layouts, menus, preferences, shortcut mapping and panels exist; framework-independent layout/preferences/shortcut behavior is tested.
  - Qt 6.8.3 is installed; the desktop target builds, has 32 offscreen application tests and has screenshot smoke coverage for the Audio, Color and Text/Caption workspaces. The remaining work is production-footage and real-display validation.
  - Remaining acceptance: keyboard-only navigation reaches every primary panel and the built application survives workspace save/restore across restart.

- [-] **UI-002 — Program monitor and asynchronous presentation**
  - **Status (2026-10-07):** `FramePresenter` renders on a worker thread with a bounded latest-wins request, suppresses overtaken frames and reports render statistics; `MonitorItem` handles fit and overlays, and quality preferences exist. Native tests cover the presenter and monitor geometry.
  - Fullscreen, safe-margin, centre/thirds overlays, fit/100% zoom and playback-resolution controls exist (`Session` `monitor.*` commands, `MonitorPanel.qml`). Remaining: build and profile Qt texture presentation (the picture is still read back and copied into a `QImage`), a source monitor, and proof that the GUI thread stays responsive on production footage.
  - Acceptance: the UI thread never waits on decode/composition; rapid scrubbing cannot present an obsolete generation.

- [-] **UI-003 — Timeline editing surface**
  - **Status (2026-10-07):** a custom `TimelineItem` and tested timeline/edit models cover the ruler, layout, hit testing, selection, snapping, zoom, linking, insert/overwrite and trim plans; transport supports play, shuttle, step, stop and looping.
  - The QML path is built and tested. Remaining: source patching/track targeting, source and trim monitors, nested-sequence breadcrumbs, waveform/thumbnail atlases and a complete production-footage edit.
  - Acceptance: edit and export a short film without config files or command-line tools.

- [-] **UI-004 — Effect browser and inspector**
  - **Status (2026-10-07):** searchable descriptor-driven effect lists, inspector value editing, jobs, stabilizer/optical-flow analysis hooks, and a searchable `.cube` LUT browser with validation and preview exist; their framework-independent models are tested.
  - The QML path supports add/remove/enable/reorder, parameter edits, keyframe toggles/jumps, reset, LUT browsing and analysis jobs. Remaining: parameter-curve drawing, effect copy/paste, thumbnails and controls tailored to every advanced effect, then production-footage preview/export validation.
  - Acceptance: every currently implemented video effect can be added, edited, animated, removed, undone, reopened, previewed, and exported from the UI.

### Real-time rendering and media performance

- [-] **GPU-001 — Cross-platform GPU compositor**
  - **Status (2026-10-09):** The backend-neutral foundation is implemented (immutable nodes, dependency hashes, fusion, capability-based selection), and the Direct3D 11 compositor renders layers, motion, crop, opacity, grade, generators, adjustment clips, dissolves, sequence effects, the colour stack and nine spatial-effect families. CPU/GPU parity is checked picture by picture in `cutline_gpu_tests`. Hardware-decoded frames remain on the device, and supported monitor frames are presented to Qt as leased native textures. On the AMD Radeon AI PRO R9700, the focused 1080p three-layer call measured 11.1 ms with read-back and 1.3 ms as a direct texture.
  - Remaining for this item: mesh warp, unsharp mask, glow, drop shadow, noise reduction, key cleanup, blend modes, masks and graphics as shaders; the render-graph executor; nested-sequence direct presentation; Direct3D 12/Metal/Vulkan; p95 frame-deadline numbers under playback load; more than one adapter vendor.
  - Define one brand-neutral render-device interface. Implement Direct3D 12 on Windows (AMD, NVIDIA and Intel), Metal on macOS (Apple Silicon and supported AMD Macs), and Vulkan on Linux/portable targets. Retain the CPU compositor as the reference and fallback.
  - Product code selects capabilities and workloads, never a GPU vendor. Backend-specific shaders must implement identical render semantics and report unsupported nodes so the scheduler can fall back explicitly.
  - Implement source-over, motion, crop, grade, LUT, blur, sharpen, vignette, lens correction, transitions, and adjustment layers as shaders.
  - Use the existing goldens plus GPU/CPU difference images and declared tolerances.
  - Acceptance: one 1080p effect track stays below 16.7 ms p95; four common 1080p tracks stay below 33.3 ms p95 on the declared reference machine.

- [-] **GPU-002 — Zero-copy surfaces and GPU frame cache**
  - **Status (2026-10-07):** Direct3D 11 video acceleration is bound: the FFmpeg source decodes onto the compositor's device and returns `media::DeviceFrame`s (NV12/P010 textures plus a reference to the decoder's surface); the compositor reads the planes in place. No pictures are uploaded for a hardware-decoded stream (`AnEngineDecodesH264OnTheGpuAndComposesItWithoutUploadingAPicture`) and the picture differs from software decode by 0.56 levels on average.
  - Remaining: a per-device byte-budgeted GPU frame cache and hardware read-ahead, VRAM quota accounting, hardware encode surfaces (AMF/NVENC/QSV), VideoToolbox and VAAPI/Vulkan Video, HEVC/AV1/ProRes coverage and other vendors' adapters.
  - Carry D3D, Metal or Vulkan hardware surfaces from decode to shaders and keep recently used frames in a per-device, byte-budgeted GPU cache.
  - Acceptance: common hardware-decoded H.264/HEVC frames reach the monitor without a CPU RGBA round trip; eviction stays within budget.

- [-] **MEDIA-001 — Proxy workflow**
  - **Status (2026-10-06):** Engine complete: cancellable background generation through bundled media providers, resolution/codec presets, persisted attach/detach/regenerate association, ready/missing/stale status, automatic monitor switching and original fallback.
  - Export automatically creates an originals-only engine when the monitor prefers proxies.
  - Proxy make/remake/remove, per-item progress and the Proxies switch exist in the Project panel and ingest dialog (UI-INGEST). Remaining product work: a proxy/cache panel, error UI, production 4K/8K benchmarks, and presets other than ProRes 422.
  - Acceptance: 4K source can play through an HD proxy while export matches the original-source reference.

- [-] **CACHE-001 — Render cache and preview rendering**
  - **Status (2026-10-06):** Content-addressed cache, disk tier, render in-to-out done. Remaining: UI indicators.
  - Hash source versions, timeline ranges, effect parameters, render version, and output format.
  - Add memory and disk budgets, precise invalidation, render-in-to-out, and cache indicators.
  - Acceptance: an unchanged range becomes a cache read; an edit invalidates only affected nodes/ranges.

- [-] **MEDIA-002 — Hardware decoding**
  - **Status (2026-10-07):** Direct3D 11 video acceleration is done for the monitor (see GPU-002). Remaining: Media Foundation, VideoToolbox and VAAPI/Vulkan Video, hardware decode on export, HEVC/AV1 coverage and a per-clip decoder diagnostic in the interface.
  - Add Media Foundation/D3D11VA on Windows, VideoToolbox on macOS and VAAPI/Vulkan Video on Linux for H.264, HEVC and AV1 where supported. Optional vendor providers may compete through the same capability registry.
  - Preserve software fallback and report active/fallback decoder in diagnostics.
  - Acceptance: output timestamps and pixels stay within declared parity tolerance; failure falls back without losing timeline position.

- [-] **EXPORT-001 — Hardware encoding and export queue**
  - **Status (2026-10-07):** Implemented: 21 presets (web, broadcast, ProRes/DNxHR, FFV1, audio only) resolved to the encoders that work on the machine, graphics-card encoders (AMD AMF verified: H.264, HEVC 8/10-bit, AV1; NVENC and QSV are listed and skipped when they will not start) with software fallback reported, a persistent queue with pause/cancel/retry/reorder that survives a restart, a check of every finished file, and the dialog and queue panel in the application. Encoding 25 identical 1080p frames on this machine (one run, synthetic picture, writer only): h264_amf 98 ms, hevc_amf 87 ms, av1_amf 85 ms, libopenh264 122 ms, ffv1 10-bit 192 ms, prores_ks 357 ms, libsvtav1 583 ms, libvpx-vp9 2267 ms.
  - Remaining: NVENC/QSV on hardware that has it, Media Foundation/VideoToolbox/VAAPI providers, image sequences and stills, smart render, render-and-replace, watch folders, export at a size other than the sequence's, certified broadcast wrappers (XDCAM, AS-11, IMF, DCP), HDR metadata.
  - Add capability-based Media Foundation, VideoToolbox, VAAPI/Vulkan Video and optional NVENC/QSV/AMF provider selection, presets, background queue, pause/cancel/retry, and persisted jobs.
  - Acceptance: failed or cancelled jobs never replace delivery files; software fallback is explicit and tested.

- [-] **PERF-GRAPH-001 — Immutable render graph and reusable nodes**
  - **Status (2026-10-07):** Graph compilation validates references/cycles, removes unreachable work, fuses compatible unary chains and assigns dependency hashes so unchanged branches retain cache addresses across revisions.
  - `PlaybackPlan` source layers, sampled effects, transitions, sequence effects, media generations, colour policy and quality policy now lower into those nodes.
  - Remaining: execute the graph on CPU/GPU backends, retain intermediate surfaces by hash and add constant folding plus tile/ROI propagation.

- [-] **PERF-SCHED-001 — Unified priority scheduler**
  - **Status (2026-10-07):** A bounded native scheduler orders realtime audio, visible frames, presentation, interactive work, read-ahead, scopes, export, cache render, proxies and analysis; it deduplicates identities, cancels stale generations, replaces lower-priority queued work under pressure, and reserves worker capacity for interactive work.
  - Remaining: migrate decode, presenter, scopes, flow analysis, proxy generation, cache render and export from their existing private workers to this scheduler and add deadline telemetry.

### P1 performance and scale work

- [x] **Single-layer software compositor memory-pass optimization**
  - Reused layers clear dirty rows in parallel. Full-frame sources and generators discard storage that they immediately overwrite, and the opaque canvas no longer clears before its full fill.
  - A single ordinary layer is flattened directly over the background, avoiding a second full-size float canvas. On the declared 32-thread machine the 1080p no-effect floor fell from 10.6 to 7.2 ms; grade from 11.4 to 9.0 ms; a 33-point LUT from 14.3 to 9.6 ms.
  - Multi-layer composites, transitions, blend modes, captions, sequence effects and working-to-display colour conversion retain the general canvas path. See `PERFORMANCE_LOG.md`.

- [x] **Timeline range-query optimization (engine only)**
  - `BoundariesIn` binary-searches the existing sorted clips and visits only clips intersecting the window. Exact rational comparisons preserve sub-tick edges; transition enumeration still scans its list.
  - `scripts\bench.bat --range` measures small windows near the end of timelines with up to 50,000 clips. Exhaustive-scan equivalence is checked across gaps, transitions, multiple sequences and exact boundaries. See `PERFORMANCE_LOG.md` for measured results.

- [x] **Optical-flow search optimization (CPU engine only)**
  - Build two luma planes once, calculate valid search intersections once per candidate and stop candidates that cannot beat the runner-up. Float-input picture scratch storage is reduced by 75%.
  - An independent exhaustive-search oracle checks identical motion, confidence and occlusion across flat, noisy, translated, HDR and 8-bit pictures, including partial edge blocks. Analysis remains an offline operation at 1080p; see `PERFORMANCE_LOG.md`.

- [ ] **PERF-ROI-001 — Tile and ROI rendering**
  - Layers already retain dirty rectangles and compositing skips clean pixels.
  - Remaining: propagate effect halos and regions through a per-tile graph, retain unchanged tiles across frames, and invalidate tiles from parameter dependencies.
  - Acceptance: a local parameter edit rerenders only intersecting tiles plus the effect halo.

- [-] **PERF-MEM-001 — Shared RAM, VRAM and disk budgeting**
  - **Status (2026-10-07):** A shared pressure-aware quota manager coordinates render-frame and optical-flow entries across RAM and disk, reserves a VRAM tier for GPU backends, and evicts cheap render frames before expensive flow analysis. Elevated and critical pressure reduce budgets to 75% and 50%.
  - Remaining: compositor scratch-layer pooling, decoded-frame integration, GPU surface accounting and OS memory-pressure adapters.

- [ ] **PERF-SNAPSHOT-001 — Incremental timeline snapshots**
  - Apply command changes to affected owners, ranges and dependency generations instead of broadly rebuilding a sequence graph.
  - Acceptance: editing one clip remains constant-latency as unrelated tracks and sequences grow.

- [ ] **PERF-DB-001 — Prepared-statement pooling and bulk writes**
  - Cache hot statements per connection and batch ingest/metadata records within bounded transactions.
  - Acceptance: statement preparation and transaction count stay bounded during imports containing tens of thousands of clips.

- [-] **PERF-PROXY-001 — Automatic proxy policy**
  - **Status (2026-10-07):** A deterministic offline policy selects 1080p, 720p or 540p bundled-codec presets from resolution, frame rate, bit depth, codec complexity, measured deadline misses, decode time and memory pressure.
  - Remaining: feed live playback telemetry into the scheduler and automatically launch or replace background proxy jobs from the UI.

- [-] **PERF-EXPORT-001 — Bounded export pipeline**
  - **Status (2026-10-08):** `FramePipeline` in `exporter/ExportWorker.cpp` renders picture and sound for the next frame on a worker while the calling thread encodes and writes the last one; one queued packet bounds memory, an error in the renderer is rethrown to the caller, and destroying the pipeline stops the worker. Export always renders on an originals-only `ExportClone` at sequence size, even when the monitor engine does not use proxies. Remaining: separate decode, render, audio and encode stages, hardware-provider selection, the GPU (export clones disable it), and a measurement (none is recorded in `PERFORMANCE_LOG.md`).
  - Pipeline decode, render, audio, encode and mux through bounded queues with cancellation and hardware-provider selection.
  - Acceptance: stages overlap without exceeding the configured RAM/VRAM budget or changing A/V timing.

- [-] **PERF-TIMELINE-001 — Production-scale timeline drawing**
  - **Status (2026-10-07):** Layout virtualizes off-screen tracks and clips and binary-searches sorted clip lists at the visible time boundary; a 120-track/120,000-clip fixture returns only visible geometry.
  - Remaining: QML draw batching, glyph caching and quota-managed waveform/thumbnail atlases on a built Qt target.

- [-] **PERF-SCOPES-001 — Asynchronous scopes**
  - **Status (2026-10-07):** A latest-frame worker downsamples to a fixed input bound, computes requested scopes off the playback thread and drops superseded queued frames. The Color workspace displays waveform, RGB parade, vectorscope and histogram results.
  - Remaining: GPU histogram/waveform kernels and scheduling through the unified priority scheduler.

- [-] **PERF-CACHE-001 — Unified cache policy**
  - **Status (2026-10-07):** Render and optical-flow memory/disk entries participate in one cost-aware, pressure-sensitive quota manager while retaining checksummed persistence and local hard caps.
  - Remaining: decoded frames, waveform/thumbnail atlases, GPU surfaces, a visible cache-status model and user controls.

### Professional color pipeline

- [-] **COLOR-001 — Color-management core**
  - **Status (2026-10-06):** Spaces, transfers, transforms and render-version gating done. Remaining: OpenColorIO, ACES.
  - Define scene/display-referred working spaces, input transforms, display transforms, linear-light composition policy, and render-version behavior.
  - Evaluate OCIO integration rather than inventing transform math.
  - Acceptance: tagged Rec.709, P3, Rec.2020, PQ, and HLG fixtures transform to declared reference values in preview and export.

- [-] **COLOR-002 — Full grading controls**
  - **Status (2026-10-06):** Wheels, curves, hue curves, white balance, vibrance, shadows/highlights, HSL secondary done, on the CPU and (GPU-003) on the card; the inspector shows them as typed numeric rows. Remaining: wheel and curve-graph widgets.
  - Lift/gamma/gain and shadows/midtones/highlights wheels, RGB and luma curves, hue-vs-hue/sat/luma curves, white balance/tint, vibrance, highlights/shadows, and HSL secondary.
  - Acceptance: every parameter is serializable, keyframeable where appropriate, undoable, and identical in preview/export.

- [-] **COLOR-003 — Video scopes**
  - **Status (2026-10-07):** Scope measurements, latest-frame asynchronous scheduling and the Qt display are done. Remaining: GPU aggregation, matrix/scale selectors and HDR calibration controls.
  - Waveform RGB/luma, vectorscope, parade, histogram, configurable color space and scale.
  - Compute asynchronously from the displayed frame; scopes may drop intermediate frames rather than block playback.
  - Acceptance: synthetic ramps, bars, and out-of-range fixtures land at known scope coordinates.

- [-] **COLOR-004 — HDR and wide gamut**
  - **Status (2026-10-06):** PQ/HLG, tone mapping, gamut mapping done. Remaining: 10-bit path, HDR monitoring.
  - PQ/HLG monitoring, metadata, HDR-to-SDR tone mapping, gamut mapping, and 10-bit output path.
  - Acceptance: 10-bit precision is retained through decode, effects, monitoring where supported, and export metadata.

### Masks, keying, tracking, and advanced stabilization

- [-] **MASK-001 — Geometric masks**
  - **Status (2026-10-07):** The CPU engine has versioned rectangle, ellipse and cubic Bezier masks with feather, expansion, opacity, inversion, add/subtract/intersect, keyframes, sequence loading and compositor application. The monitor supports rectangle/ellipse/pen drawing, point editing, keying and stored tracker binding. Mask edits are undoable and survive split and head trim. Remaining: AI object selection, GPU parity and production-footage validation.
  - Rectangle, ellipse, and Bezier masks with feather, expansion, opacity, inversion, multiple-mask combine modes, and keyframes.
  - Store masks as first-class effect-owned data rather than packing them into numeric parameters.
  - Acceptance: masks survive split/trim/undo/reopen and match preview/export goldens.

- [-] **KEY-001 — Chroma and luma keying**
  - **Status (2026-10-06):** Chroma and luma key done with matte cleanup. Remaining: key-specific UI (the generic inspector shows the matte controls as numbers; a mask can now be added to a key effect like any effect).
  - Color selection, tolerance, softness, spill suppression, matte cleanup, garbage/core masks, and matte view.
  - Acceptance: reference green-screen and luminance fixtures produce measured alpha mattes with no transparent-color halos.

- [-] **TRACK-001 — Point and planar motion tracking**
  - **Status (2026-10-06):** Point and planar tracking, stored data, stale detection done; a track drives a motion effect and a mask (`FollowTrack`, UI-MASK). Remaining: applying a track to graphics and effect parameters other than masks and motion.
  - Offline analysis jobs, confidence values, reviewable tracking data, cancellation, provenance, and reanalysis after source changes.
  - Apply results to motion, masks, graphics, and effects without destructive source changes.
  - Acceptance: synthetic known-motion fixtures recover translation/rotation/scale within declared error bounds.

- [-] **STAB-002 — Similarity and perspective stabilization**
  - **Status (2026-10-07):** Similarity stabilisation with translation, rotation/scale, RANSAC outlier rejection and crop policies is done. Rolling-shutter analysis is implemented separately under STAB-003.
  - Remaining: perspective stabilization and production-footage validation.
  - Acceptance: known shake fixtures meet pixel/angle tolerances and render deterministically after reopen.

- [-] **STAB-003 — Rolling-shutter repair and mesh warp**
  - **Status (2026-10-06):** CPU scan-line correction and reusable bilinear control mesh are implemented, including direction, horizontal/vertical skew, rotation and scan curvature. Analysis (`render/RollingShutter.h`) measures the picture's velocity between neighbouring frames and turns it into a keyframed `rolling_shutter` effect through a camera profile's readout fraction (profiles are validated data; the built-ins are nominal classes, not measurements); frames that cannot be measured hold the nearest correction. A `mesh_warp` effect exposes a 4x4 control grid.
  - Remaining: compositor/inspector UI and GPU warp; testing on footage from a real rolling-shutter camera (only synthetic skew fixtures so far); measured per-camera profiles.
  - Acceptance: synthetic skew fixtures are corrected within declared line displacement tolerance.

### Temporal effects and retiming

- [-] **TIME-001 — Speed curves and ramp editor**
  - **Status (2026-10-06):** Persisted `time_remap` effect curves drive video and sample-by-sample audio source mapping, including eased ramps, hold freezes and reverse segments. `SetSpeedRamp`/`ClearSpeedRamp` are the edit commands: segments lay end to end, the clip's length and source window become what they sweep, a linked audio partner follows, split divides the curve so each half plays what it played, trim and steady-speed edits on a ramped clip are refused, and undo/reopen restore it. Pitch-preserving audio follows the curve (WSOLA along it) wherever the source advances at 0.25-4x; holds, reverses and extreme speeds play varispeed and are counted as fallbacks.
  - The ramp editor exists (UI-RAMP). Remaining: pitch-kept audio through reverse segments; speech-material listening tests; smooth (not stepped) eases.
  - Acceptance: output-to-source mapping remains exact at segment boundaries and survives split/trim/undo/reopen.

- [-] **TIME-002 — Optical flow and frame interpolation**
  - **Status (2026-10-06):** Deterministic CPU block flow, confidence, forward/backward occlusion checks, safe blend fallback and compositor frame-blend/optical-flow modes are implemented. `render/FlowCache.h`: a content-addressed analysis cache (memory + checksummed disk, single-flight) that the compositor uses via `CompositorConfig::flow_cache`, so a pair of source frames is analysed once for any number of in-between pictures, preview and export; `FlowAnalysis` is a background job that fills it with progress, cancel and per-pair skip reporting. Release timing at 1080p: 4.9 s to analyse a pair against 8.9 ms to key and look up a cached one (perf/2026-10-06-optical-flow.txt).
  - The Qt session and inspector contain a background-job action with progress/cancel, and the Qt application builds, with an app test that runs it to an undoable edit.
  - Remaining: production tuning (a 1080p pair is 4.9 s on the reference search, so uncached slow motion is far from real time), GPU compute, and validation of the built UI path once Qt is available.
  - Acceptance: known-motion fixtures interpolate expected positions; preview and export use the same cached analysis.

- [-] **FX-002 — Spatial and temporal noise reduction**
  - **Status (2026-10-06):** CPU temporal and spatial noise reduction done. Remaining: GPU path.
  - Luma/chroma controls, motion-aware temporal accumulation, edge preservation, GPU path, and memory budget.
  - Acceptance: noise fixtures show a measured reduction without exceeding declared detail-loss and ghosting thresholds.

- [-] **FX-003 — Additional production filters**
  - **Status (2026-10-06):** Ten filters plus 12 premultiplied-alpha blend modes are done on the CPU. Remaining: GPU parity.
  - Gaussian/directional blur, unsharp mask, glow, drop shadow, distort/warp, channel mixer, black-and-white, tint, posterize, and blend modes.
  - Add only effects with descriptors, CPU reference behavior, GPU parity, and tests.

### Graphics, captions, and multicam

- [-] **GFX-001 — Title and graphics clips**
  - **Status (2026-10-06):** Graphics are first-class project records (schema v11; Create/Update/DeleteGraphic, `AddGraphicClip`). A graphic clip is a source-less clip whose effect names `project:<id>`; the compositor draws it as a generator and the render cache keys it by content. Documents animate per element (position, size, opacity, font size, colour; hold/linear/eased) and carry responsive anchors that keep margins and type size on another aspect ratio; font lists fall back and warn when no family is installed. Versions: a document using animation or anchors is schema 2 and older builds refuse it.
  - **Status (2026-10-07):** schema 3 adds rotation, outlines, drop shadows and rounded corners (animatable, written only when used), image elements show project media or a file, and the Graphics workspace is the authoring interface: designer canvas with grips and snapping, properties list with its own undo, entrances, thumbnails, Add to timeline (ui_tests `TheDesigner...`, `ClickingTheCanvas...`, `Dragging...`, `GraphicsAreCreatedPlaced...`; render_tests `AGraphicElementTurns...`, `AnOutlineIsDrawn...`, `AShadowFollows...`; app test `ATitleIsMadeFromATemplate...`).
  - Remaining: a keyframe editor (entrances only), on-canvas text editing, multi-select and grouping, gradients, SVG import, nesting a graphic inside another.
  - Acceptance: graphics are first-class clips with undo, templates, nesting, and deterministic rendering.

- [-] **GFX-002 — Motion graphics templates**
  - **Status (2026-10-06):** Versioned `.cuttemplate` packages with exposed text/asset/numeric/colour controls (labels, ranges), strict validation and instantiation. The project holds a template library: `InstallGraphicTemplate` by id and version (a version is immutable; a changed package must be a new version; a version in use cannot be removed), and instances (`CreateGraphic` kind template plus control values, validated against the package) keep drawing the version they were made with.
  - **Status (2026-10-07):** template files read and write (JSON) through the Titles panel, a graphic becomes a template with a control per text, picture and colour, six templates come with the application, thumbnails and the browser exist, and a title's controls are edited from the timeline selection.
  - Remaining: bundling font files (today a missing family falls back along the element's font list and is reported); a second project's round trip is untested by a person.
  - Acceptance: a template authored in one project can be installed and rendered identically in another.

- [-] **MULTI-001 — Multicam editing**
  - **Status (2026-10-06):** Engine supports timecode, marker and local audio-correlation sync, angle-monitor requests, frame-exact live switch recording and flattening. Groups, angles, offsets and cuts are persisted (schema v10) with commands to create, store a sync result, record/remove a cut, flatten and delete; flattening makes ordinary linked picture+sound clips (sound follows the picture's angle, or one fixed angle) and keeps the group. `render/AngleMonitor.h` tiles every angle with the live one marked and resolves a click to an angle.
  - **Status (2026-10-07):** the interface exists: set-up from chosen clips (names, reference, sync by sound, timecode, marker or by hand), the angle monitor panel, digit/click cuts against a running clock, moving/nudging/changing/removing cuts, per-angle sync nudges and rename (`RenameMulticamAngle`), and laying the programme out as clips with the sound following the picture or from one angle (ui_tests `AMulticamGroupIsSetUpFromClips...`, `LiveCutsAreSnapped...`, `AMulticamProgrammeIsLaid...`, `TheAngleMonitorReads...`; app tests `TheMulticamMonitorShowsEveryAngle...`, `AMulticamGroupIsCreatedFromChosenClips...`).
  - Remaining: sound while the group plays, storing audio envelopes so a sync can be re-run without re-analysing, sync validation on real multi-camera footage, merged-clip sources.
  - Acceptance: switches are frame-exact and retain access to every original angle and audio source.

- [-] **CAPTION-001 — Caption tracks**
  - **Status (2026-10-07):** Model, SubRip/WebVTT, sidecars and compositor burn-in are done. The Text workspace now has undoable track/cue authoring, timing, speaker metadata, track typography/layout controls, SRT/WebVTT import/export, and transcript-to-caption creation. Remaining: a caption lane in the timeline, direct monitor manipulation, per-cue style controls, safe-area warnings, embedded caption delivery and production subtitle-file validation.
  - First-class caption model, styling, safe-area preview, SRT/WebVTT import/export, burn-in, and sidecar delivery.
  - Acceptance: timed text round-trips without timing drift and burn-in matches preview.

- [-] **AI-001 — Offline transcription and text-based editing**
  - **Status (2026-10-07):** A local Whisper process adapter, word timestamps, persisted transcript correction/search, speaker labels, caption creation, text-based ripple/lift edits and a Transcript workspace are implemented and tested. No network service is required. Remaining: ship/licence model packs, diarization quality validation, semantic search, scene detection, object selection and local generative providers.
  - Bundled local transcription provider, word timestamps, speaker labels, transcript correction, captions, and text-based cuts. Models and runtimes install with the application or an offline model pack.
  - Acceptance: no upload or account is required; edits remain ordinary undoable timeline commands.

### Professional audio

- [x] **AUDIO-001 — Pitch-preserving time stretch**
  - **Status (2026-10-06):** Done in the engine: WSOLA stretch with explicit varispeed fallback.
  - High-quality offline path and bounded preview path with explicit fallback.
  - Acceptance: duration is exact, pitch-error and artifact fixtures meet declared thresholds, and varispeed remains selectable.

- [-] **AUDIO-002 — Mixer, buses, sends, and metering UI**
  - **Status (2026-10-07):** Engine and primary mixer interface are done: buses, editable pre/post sends, routing, `MixTrack`, peak/RMS/LUFS measurement, channel strips, live post-fader meters from the played block, fader/pan, mute/solo, rename, output routing and bus creation/removal. Automation modes and voice-over recording are done (AUDIO-004). Remaining: input monitoring, control surfaces and plug-in latency compensation.
  - Track/submix/master buses, pre/post sends, mute/solo/record state, peak/RMS/LUFS meters, limiter, and automation modes.
  - Acceptance: routing is sample-exact, latency-compensated, persistent, and safe on the real-time callback.

- [-] **AUDIO-003 — Essential audio tools**
  - **Status (2026-10-06):** Engine done (EQ, dynamics, denoise, de-reverb, side-chain ducking, loudness); roles and the Essential Sound panel followed under AUDIO-004. Remaining: speech-material validation of the chains.
  - EQ, compressor, limiter, gate, denoise, de-reverb, loudness match, auto-ducking, and dialogue/music/SFX roles.
  - Acceptance: effects never allocate or lock on the real-time callback and preview/export results agree.

### Interchange, plug-ins, project scale, and collaboration

- [-] **IO-001 — OpenTimelineIO interchange**
  - **Status (2026-10-06):** Engine done: OpenTimelineIO read/write with round trip and an unsupported-feature report. **Remaining (found by the 2026-10-08 code audit):** no menu, dialog or command in the application reads or writes it, so by this file's own rule (`[x]` includes user-facing controls) it is `[-]`.
  - Import/export clips, tracks, transitions, nested sequences, markers, rational times, and unsupported-feature reports.
  - Acceptance: supported timelines round-trip without frame drift; unsupported data is reported rather than discarded silently.

- [-] **IO-002 — EDL, XML, and AAF**
  - **Status (2026-10-06):** EDL and Final Cut 7 XML done. Remaining: AAF.
  - Implement in that order after OTIO validates the adapter boundary.
  - Acceptance: format-specific conformance fixtures and round trips pass.

- [ ] **PLUGIN-001 — VST3 host**
  - Out-of-process scanning, validation/quarantine, parameter persistence, automation, latency compensation, and crash isolation.
  - Acceptance: a crashing plug-in cannot corrupt the project or terminate the editor; delayed plug-ins remain sample aligned.

- [ ] **PLUGIN-002 — OFX video-effect host**
  - Out-of-process hosting, GPU/CPU negotiation, parameter UI bridge, caching, version pinning, and missing-plug-in placeholders.
  - Acceptance: plug-in failure bypasses only the affected node and the project retains all parameter data.

- [-] **PROJECT-001 — Project manager and media consolidation**
  - **Status (2026-10-06):** Collect, verify, manifest, relink done. Remaining: trim with handles and transcode (need an encoder callback).
  - Collect, copy, transcode, trim with handles, verify hashes, and produce a portable package report.
  - Acceptance: copied projects reopen offline and every referenced frame/sample remains available.

- [-] **RECOVERY-001 — Crash-recovery replay**
  - **Status (2026-10-06):** Engine done: journal v2, snapshot plus replay, quarantine. **Remaining (2026-10-08 audit):** `Session::openProject` calls `ProjectStore::OpenPackage` only; `InspectPackage`/`RecoverPackage` are never called, so the "expose recovery choice" part of the item has no interface and a damaged package is an error message.
  - Read the existing persisted changesets, identify the latest consistent snapshot, replay idempotently, and expose recovery choice.
  - Acceptance: forced termination at each commit boundary recovers to a consistent revision without duplicating commands.

- [ ] **COLLAB-001 — Shared projects and review**
  - Optional local collaboration over a shared project folder or user-configured LAN peer, with stable identity, optimistic conflict handling, range/asset locks, comments, versions, and audit history.
  - Acceptance: the editor remains complete with networking disabled; two local clients can edit nonconflicting areas and receive a reviewable conflict for overlapping changes.

## Interfaces still missing for finished engines (2026-10-08 code audit)

Found by reading the code rather than this file: each engine below is built and tested, and the application never calls it. Until the interface exists the feature cannot be used, whatever the engine rows say.

- [ ] **UI-SOURCE-001 — Source monitor and source marks.** `ui::PlanInsertEdit` takes a source range and `ui::ResolveThreePoint` resolves three-point edits, but `Session::InsertPlanFor` always places the whole media item (sequence marks decide only where). Needs a source monitor, source in/out, clear marks, four-point and replace edits, match frame / reverse match frame, subclips and master-clip properties. *Guide: B1 to B4 in [guide/B-editing-surface.md](guide/B-editing-surface.md).*
- [ ] **UI-TARGET-001 — Source patching and track targeting.** `InsertPlanFor` picks the lowest unlocked video and audio track; there is no patch or target control and no sync lock. *Guide: B5 in [guide/B-editing-surface.md](guide/B-editing-surface.md).*
- [ ] **UI-PROJECT-001 — Bins, relink, search and metadata in the Project panel.** The store has `CreateBin`/`RenameBin`/`MoveBin`/`DeleteBin` and `RelinkMedia`; the panel is a flat list with proxy controls only (`CreateBin` is issued only by `cutline_demo`; `RelinkMedia` is never issued by the application). No search, labels, thumbnails or icon view. *Guide: B6 in [guide/B-editing-surface.md](guide/B-editing-surface.md).*
- [ ] **UI-SEQUENCE-001 — Nested and multiple sequences.** The compiler, mixer and store nest sequences; the application never issues `CreateSequence` after project creation, has no "nest selection", no sequence tabs and no breadcrumbs. *Guide: B7 in [guide/B-editing-surface.md](guide/B-editing-surface.md).*
- [ ] **UI-MARKERS-001 — Marker list and editing.** The Markers panel (`markers`, part of the Effects workspace) is `PlaceholderPanel.qml` ("Not built yet"); `UpdateMarker` and `RemoveMarker` are never issued, markers have no colour or name entry in the interface. *Guide: A4 in [IMPLEMENTATION_GUIDE.md](IMPLEMENTATION_GUIDE.md).*
- [ ] **UI-GENERATE-001 — Adjustment layers and generator clips.** The compiler and both compositors support adjustment clips and the `solid` generator, and the timeline draws them, but nothing in the application creates one (`solid` is hidden from the effect browser and no command inserts an adjustment clip). *Guide: B8 in [guide/B-editing-surface.md](guide/B-editing-surface.md).*
- [ ] **UI-IO-001 — Interchange and project manager dialogs.** OTIO, EDL and Final Cut XML import/export and `core/project/Consolidate.h` (collect, verify, relink) have no menu entry. *Guide: B12 in [guide/B-editing-surface.md](guide/B-editing-surface.md).*
- [ ] **UI-RECOVERY-001 — Recovery choice on open.** See RECOVERY-001. *Guide: B13 in [guide/B-editing-surface.md](guide/B-editing-surface.md).*
- [ ] **UI-TIMELINE-002 — Waveforms and thumbnails on clips, a cache-status indicator, a caption lane.** `TimelineItem::paint` draws coloured rectangles with a name, speed badges and the ramp band only; nothing reads the audio peaks or video frames for display (PERF-TIMELINE-001 and CAPTION-001 already list this). *Guide: B9 in [guide/B-editing-surface.md](guide/B-editing-surface.md).*
- [ ] **UI-EFFECTS-002 — Copy/paste effects between clips, effect presets, parameter-curve drawing, colour-wheel and curve widgets.** *Guide: B10 in [guide/B-editing-surface.md](guide/B-editing-surface.md) and C5 in [guide/C-effects-audio-colour-delivery.md](guide/C-effects-audio-colour-delivery.md).*

## Defects found by the code audit

- [ ] **JOURNAL-TIME-001 — Hard-coded timestamps.** Every command the application issues carries `timestamp_utc = "2026-01-01T00:00:00Z"` (`app/Session.cpp`, `SessionAudio.cpp`, `SessionIngest.cpp`, `core/project/Consolidate.cpp`), and the store writes it to `command_journal.created_at_utc`, the journal files and `project_meta`. The project's modified time and its audit trail are therefore wrong, and `command_journal_created` indexes a constant. Use the wall clock at the call site (the store should stamp when the field is empty). *Guide: A1 in [IMPLEMENTATION_GUIDE.md](IMPLEMENTATION_GUIDE.md).*
- [ ] **PREF-WIRING-001 — Preferences that do nothing.** Ten of the 35 preferences declared in `ui/Preferences.cpp` are shown in the Preferences dialog, saved and restored, and read by no code in `app/`, `ui/`, `core/`, `playback/` or `render/`: `general.autosave_minutes` (the application has no autosave timer; a snapshot is taken only by File > Save), `general.restore_last_project`, `general.confirm_quit`, `appearance.theme` (`Theme.qml` is fixed dark), `appearance.ui_scale`, `media.cache_folder`, `playback.pre_roll_seconds`, `playback.post_roll_seconds`, `timeline.default_transition_seconds` and `timeline.default_still_seconds`. Wire each or remove it from the schema; a setting that does nothing is worse than none. *Guide: A3 in [IMPLEMENTATION_GUIDE.md](IMPLEMENTATION_GUIDE.md).*
- [ ] **EFFECT-ALIAS-001 — Duplicate catalogue entry.** `grade` and `lumetri` are both registered with the display name "Basic Color" (`effects/EffectRegistry.cpp`), so the effect browser lists it twice; `lumetri` is a legacy id kept for the browser prototype and should be an alias, not a second effect. *Guide: A2 in [IMPLEMENTATION_GUIDE.md](IMPLEMENTATION_GUIDE.md).*

## Not started, and why

- UI-001..004, UI-RAMP, UI-MULTI and UI-EXPORT: built with Qt 6.8.3 and tested offscreen (see IMPLEMENTATION_STATUS.md, desktop application section).
- GPU-001/GPU-002 now have a Direct3D 11 compositor and decode binding with tests and measurements on one AMD adapter; the rest of their scope (more shaders, other APIs and vendors, frame cache) and MEDIA-002 and EXPORT-001 need platform GPU/codec implementations and hardware validation.
- PLUGIN-001/002: no host code exists. A VST3 SDK has been fetched and built under `.tools/vst3sdk` (ignored by git, referenced by nothing in `CMakeLists.txt`); there is no OFX SDK.
- COLLAB-001: optional engine work; design for shared-folder/LAN operation without an internet service.
- AI-001 is partial: offline transcription and text-based editing are built; local generative/object/semantic providers remain. MASK-001 has a tested CPU engine, monitor editor and tracker binding; its object-selection and GPU paths remain. MULTI-001, TIME-001, TIME-002, STAB-003, GFX-001 and GFX-002 have tested engine implementations with persistence; what remains on each is listed on the item.
- FX-002 and FX-003 GPU paths: need a GPU/driver; the CPU references and tests are done.

## Resolution and codec follow-up

- [ ] Publish a generated import/export compatibility matrix from the actual FFmpeg build rather than a hard-coded marketing list.
- [ ] Add declared fixtures for SD, 720p, 1080p, UHD, DCI 4K, 6K, and 8K sequence dimensions.
- [ ] Add portrait, square, anamorphic pixel aspect, interlaced metadata, variable-frame-rate, 10-bit, HDR, and alpha-channel fixtures.
- [ ] Add export presets only after each codec/container/rate combination has a decode round-trip test.
- [ ] Record maximum validated resolution per codec and hardware path; do not claim an untested maximum based only on integer dimensions.

## Completion checklist for every work item

- [ ] Data model and migration are explicit.
- [ ] Command validation, undo/redo, split/trim behavior, and persistence are covered where relevant.
- [ ] Preview and export share one semantic implementation.
- [ ] Failure, cancellation, missing-asset, and fallback behavior are visible and tested.
- [ ] CPU/GPU memory and queue limits are bounded.
- [ ] Named tests are added to the relevant `parity/` capability.
- [ ] `scripts\test.bat`, `node scripts/check-docs.js`, and `node scripts/cross-check-parity.js --execute` pass.
- [ ] Release performance is recorded when the work touches a per-frame, per-sample, or interactive path.
- [ ] `IMPLEMENTATION_STATUS.md`, `PERFORMANCE_LOG.md`, and this file reflect the final behavior.
