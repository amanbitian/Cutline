# Cutline roadmap: full NLE parity

Written for: engineers and technical leads planning this codebase's next two to three years.

Last reviewed 2026-10-06. Export is built and verified; the next dependency is the application shell. An independent code review the same day found correctness defects that should be closed before more is built on top of them â€” see [REMEDIATION.md](REMEDIATION.md) for the ordered list; it supersedes the order below wherever they conflict.

This is the plan for reaching professional-NLE feature parity, together with a cross-check of the design, the tracked feature list, and the optimisation strategy. It is grounded in what the repository actually does today, which [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md) records and [GAP_ANALYSIS.md](GAP_ANALYSIS.md) audits.

## Where the project actually stands

Cross-checking the 220 tracked capabilities in `parity/` against the code and the test suite, with `scripts/cross-check-parity.js`:

| | Count | Share |
|---|---:|---:|
| Implemented, with named passing tests | 64 | 29% |
| Partial | 16 | 7% |
| Prototype (browser-only) | 7 | 3% |
| Planned | 114 | 52% |
| Deferred | 19 | 9% |

Of the 55 P0 capabilities, **35 are implemented, 8 partial, 10 planned and 2 are browser prototypes** (the count moved with the desktop-application work of 2026-10-07).

That ratio is the headline finding, and it is not what the manifests said before this cross-check: they recorded 0 implemented because their statuses were hand-maintained and had drifted from the code. `scripts/cross-check-parity.js` now refuses to let a capability claim implementation without naming a test that exists, and the report generator refuses to build without that evidence. The number can no longer drift upward by wishful editing.

**The shape of what remains is the important part.** The engine is largely built: decode, timestamp maps, the timeline compiler, the compositor, the mixer, the audio clock and device, undo, and export all work and are tested. What is missing at P0 is almost entirely the *editing surface* â€” Source Monitor, in/out marks, three-point edit, source patching, track targeting, J/K/L transport, the ruler, the playhead, clip selection, snapping. Nineteen of the twenty-six unfinished P0 items are interaction, not engine.

That is a comfortable position to be in. The hard, slow, correctness-critical half is done and verified; the remaining P0 work is mostly building a surface on top of capabilities that already exist and are already covered by tests.

---

# Part 1 â€” Design cross-check

## What the current design will carry to full parity

These decisions should not be revisited. Each has been load-bearing already.

**Exact rational time with an integer tick timebase.** Every duration, position and rate is a reduced fraction; the 254016000000/s tick value exists only so SQL can index ranges. Proven lossless for every broadcast rate and audio sample rate. This is what makes mixed-rate timelines, retiming and drift-free export work, and it will not need to change for any feature below.

**Typed command bus with row-level changeset undo.** Adding a command costs a payload struct, a validator and an apply function; undo comes free because it is derived from the rows the command touched. The cost of history is proportional to the edit, not the project. Thirty-eight commands exist; three hundred would not change the architecture.

**Provider interfaces for media in and out.** Nothing above `media::Source` or `media::Writer` names FFmpeg. Hardware decoders, camera-vendor SDKs and alternative muxers slot in as peers.

**A pure compiler over an immutable snapshot.** `TimelineCompiler` performs no I/O and holds no state, so it is trivially parallelisable, cacheable and testable. It is already called from both the monitor and the export worker with no special-casing.

**Golden-frame testing.** The entire optimisation pass that cut 1080p composite time by 45% is known to be behaviour-preserving because no golden moved. This is the mechanism that will let a GPU backend be verified against the software one.

## What will break, and what to do about it

These are not defects; they are the places where the present design meets its limits and will need deliberate change.

### 1. Serial composition and presentation â€” the binding constraint

The audio path is properly threaded with a lock-free handoff. Video decode now has a bounded worker pool, read-ahead and stale-work cancellation, but composition and presentation still run synchronously on the caller. At 37 ms per 1080p frame the software compositor cannot sustain real-time playback even with decode removed from the hot path.

*Change:* extend the completed per-worker decode pool into a priority scheduler with per-thread `Compositor` instances (already move-only and non-shareable). Run composite ahead of present, add asynchronous delivery to the UI, and coordinate playback with proxy/background jobs.

*Blocks:* real-time playback of anything, proxy generation, background conform, responsive export.

### 2. The compositor is a fixed pipeline, not a graph

`Compose` draws, applies effects in order, and composites. There is no node model and no per-node caching, so changing one parameter in a stack of twenty effects re-runs all twenty, and a clip whose inputs did not change is re-rendered from scratch each frame.

*Change:* an effect DAG where each node caches its output keyed on a hash of its inputs and parameters. The benchmark shows effect cost is linear and dominant â€” 17 effects is 134 ms against 18 ms for one â€” so caching unchanged nodes is the single largest algorithmic win available on the CPU path.

*Blocks:* usable grading, masks, nested effect stacks, anything an artist iterates on.

### 3. Snapshots are whole-sequence

`LoadSequenceGraph` rebuilds the entire snapshot after every edit. For a feature-length timeline with tens of thousands of clips that is O(project) per keystroke.

*Change:* apply the command's changeset to the in-memory snapshot incrementally, falling back to a full reload only when the change is structural. The snapshot already records `source_revision` for exactly this.

*Blocks:* editing long-form projects at all.

### 4. Compositing happens in the source's transfer function

Decode normalises to full-range RGB with the correct matrix, and per-stream primaries, transfer and matrix survive end to end â€” but nothing transforms to a working space. Compositing in a gamma-encoded space is what most NLEs do by default in Rec.709 and is defensible, but it is not colour management, and HDR requires the real thing.

*Change:* a working-space transform at decode and a display transform at present, with the sequence's `working_color_space` driving both.

*Critically:* this changes the appearance of every existing project. The sequence must record which render version it was cut under, and old projects must keep rendering the old way. Retrofitting that after projects exist is far harder than designing it in now, so **the render-version field should be added before colour management is, not with it.**

### 5. The audio graph is flat

Clip â†’ track â†’ master, with gain and pan. Premiere has submixes, sends, track and master effects, and plug-in delay compensation â€” which requires a graph that can measure each path's latency and align them.

*Change:* a proper audio node graph with latency reporting and compensation, built before plug-in hosting rather than after. Retrofitting delay compensation into a flat mixer means rewriting it.

### 6. Effects are data, with no plug-in ABI

The effect model is complete as data â€” stacks, parameters, keyframes, all persisted and sampled. But the built-in effects are a `if (type == "...")` chain in the compositor, and there is no stable ABI, no process isolation, and no way for a third party to add one.

*Change:* an internal effect interface first (registry, declared parameter schema, render callback), then the built-ins reimplemented against it, then VST3/OFX hosts as out-of-process clients of the same interface. Doing it in that order means the internal effects prove the interface before any third-party code depends on it.

### 7. Optimistic single-writer concurrency

The store's revision check assumes one writer. That is correct for a local editor and will not extend to collaboration.

*Change:* for shared projects, either check-out locking at the sequence level (simpler, matches how edit teams actually work) or change-set merging. The command journal with per-command changesets is a good substrate for either. This is a P3 concern and should not influence earlier design.

### 8. No render-version field

Stated separately because it is small, urgent and easy to miss. Any change to render semantics â€” colour management, a different resampling filter, a corrected blend mode â€” silently alters existing projects. A `render_version` on the sequence, with the compositor honouring old versions, costs little now and is close to impossible to add retroactively.

---

# Part 2 â€” Feature cross-check

## Gaps in the tracked list

The 220 capabilities are a good inventory but incomplete. Cross-checking against a professional NLE's actual surface, these are **not tracked anywhere** and should be added to `parity/`:

**Editorial and timeline**
- Multicam UI (built: angle monitor panel, live switching against a running clock, cut refinement, set-up and lay-out; still open: monitoring sound while it plays); groups, cuts and flattening are persisted with commands, and an angle-monitor mosaic exists
- Merge clips (dual-system sound)
- Freeze frame / frame hold as a one-click command and a ramp editor UI (a hold segment in `SetSpeedRamp` already does it)
- Track mattes
- Undo history panel (the stack is in memory and labelled; nothing shows it)
- Render in-to-out / preview rendering, and the green-yellow-red render bar
- Frame snapshot / export single frame

**Graphics**
- Done: a title/graphic designer, thumbnails, template file import/export, rotation, outlines, shadows and rounded corners; graphics and a versioned template library are stored in the project with commands. Open: bundled fonts, a keyframe editor, SVG import

**Effects**
- GPU mesh warp and rolling-shutter validation on real camera footage; CPU analysis, camera profiles, scan-line correction and the `mesh_warp` effect already exist
- Morph cut

**Audio**
- Auto-ducking, Essential Sound equivalent, submixes and sends, plug-in delay compensation

**Project management**
- Project manager: consolidate, transcode, trim-and-copy, collect files
- Watch folders and ingest presets
- XMP / sidecar metadata read and write
- Productions equivalent (multi-project working sets)

**Application**
- **Workspaces, panel docking, keyboard shortcut mapping.** There is no `ui` or `workspace` domain at all, which is a significant omission: it is a large body of work, it gates every editorial P0, and nothing in the plan currently accounts for it.
- Preferences and settings
- Panel/scripting extensibility

**Delivery**
- Export queue with watch folders and presets (built 2026-10-07 without watch folders: 21 presets, persistent queue, hardware encoders where they work, checks of the finished file)
- Direct publishing destinations

Adding these would take the inventory from 220 to roughly 265. **The missing `ui` domain is the most consequential**: the 19 unfinished P0 capabilities are mostly interaction, and none of the panel, docking or shortcut infrastructure they need is tracked or estimated.

## Corrections the cross-check made

Capabilities the manifests recorded as planned or prototype that the code had already implemented, now corrected with test evidence: sequence nesting, adjustment layers, track lock/mute/solo, the transform, opacity and crop effects, motion keyframes, effect bypass, the transition model, cross dissolve, audio crossfade, the audio mixer, audio keyframes, and basic colour correction â€” plus the whole decode, probe, fingerprint, export and playback set.

Capabilities recorded optimistically that the cross-check downgraded: the effect *graph* (a fixed pipeline, not a graph), transition alignment (recorded but never used to place anything), audio effects (volume, gain and pan only), linked A/V groups (stored and carried, but nothing links or unlinks), audio callback budget (underruns counted, no budget measured), and crash recovery (changesets persisted, nothing replays them).

---

# Part 3 â€” The plan

Phases are ordered by dependency, not by appeal. Each states what must be true to call it done.

## Phase A â€” The editing surface (the current P0 gap)

Everything here depends on an application shell that does not exist. **Build the shell first**; the editorial capabilities are comparatively cheap once there is somewhere to put them.

1. **Application shell.** Panel system with docking and workspaces, keyboard shortcut mapping, preferences. Decide and record whether the timeline is a custom `QQuickItem` with a batched renderer â€” it must be; delegates will not sustain 60 fps with a hundred tracks.
2. **Monitors.** Program monitor consuming `PlaybackPlan` (the engine already produces it), then the source monitor, then safe margins, overlays, zoom and fullscreen.
3. **Transport.** J/K/L shuttle including reverse at speed, frame step, sequence start/end, loop, and audio scrubbing.
4. **Timeline surface.** Ruler, playhead, clip selection, snapping, drag, track headers.
5. **Three-point editing.** Source marks, source patching, track targeting, insert and overwrite as distinct gestures, four-point with a fit policy.
6. **Trimming.** Ripple trim, roll, slip, slide, with a trim monitor.
7. **Linked A/V.** Link, unlink, and move or trim a group as a unit.

*Done when:* an editor can cut a short film end to end without touching a config file, and the result exports correctly.

## Phase B â€” Performance to real time

Covered in detail in Part 4. In summary: job system, GPU compositor, effect graph with caching, proxies, render cache, hardware decode.

*Done when:* 1080p multi-track playback holds the frame budget on a mid-range machine, and 4K holds it with proxies.

## Phase C â€” Finishing

1. **Colour management**, after the render-version field exists. Working space, display transform, LUT management, scopes, then HDR PQ and HLG, wide gamut, colour match.
2. **Effects.** The CPU reference now includes production filters, 12 blend modes, keying, grading, tracking/stabilisation, noise reduction, keyframed time remapping, rolling-shutter/general mesh warp and optical-flow interpolation. Optical-flow analysis is cached with a background job. Next: the GPU registry/backend, masks and perspective stabilisation.
3. **Audio.** Node graph with latency compensation, submixes and sends, built-in effects, waveform generation, loudness metering, auto-ducking, audio sync alignment.
4. **Graphics.** Graphics and versioned templates are now stored clip sources with animation and anchors; the authoring panel, image elements bound to project media, thumbnails and template files are done; next a keyframe editor and SVG import.
5. **Captions.** Caption track type, styling, burn-in, sidecar import and export.

## Phase D â€” Ecosystem and scale

1. **Interchange.** OTIO first â€” the time model is already structurally aligned, so it is the cheapest and it validates the data model against an external standard. Then EDL, XML, AAF.
2. **Plug-ins.** VST3 then OFX, out of process, against the effect interface from Phase C.
3. **Project management.** Consolidate, transcode, collect, watch folders, ingest presets, XMP.
4. (Multicam UI done 2026-10-07 on the persisted engine.) Then merge clips.
5. **Delivery.** (Export queue, presets and validation done 2026-10-07.) Smart render, render and replace, image sequences, certified wrappers.

## Phase E â€” Intelligence and collaboration

Transcription, text-based editing, semantic search, scene edit detection, speech enhancement, auto reframe, object selection; then shared projects, locking, review and comments.

Deliberately last. These are differentiators only once the editor is trustworthy, and every one of them is easier against a stable data model.

---

# Part 4 â€” Optimisation programme

Ordered by measured or expected impact per unit of work. Current figures come from `scripts/bench.bat` at 1920Ã—1080.

| # | Optimisation | Expected effect | Effort | Notes |
|---|---|---|---|---|
| 1 | **GPU compositor** | 37 ms to 1-3 ms | Large | **Done for the common subset (2026-10-07):** 36.9 ms to 2.7 ms for one 1080p layer with three effects, 153.7 to 4.5 ms for four, verified against the software compositor picture by picture. Colour tools and LUTs are on the card as well (2026-10-07: a 1080p frame with a LUT is 3.7 ms on the card against 61 ms in single-threaded software before and 9.6 ms in parallel software now). Remaining shaders: filters, keys, blend modes, masks. |
| 2 | **Effect node graph with per-node caching** | Near-total removal of re-render on parameter change | Compiler foundation done / execution remains | Sampled playback plans lower into immutable nodes with media/quality dependencies; validation, hashes, dead-node removal and safe unary fusion are implemented. Execute nodes and retain intermediate surfaces by hash. |
| 3 | **Job system: parallel render and presentation** | Keeps UI responsive and scales export; bounded parallel decode/read-ahead is already implemented | Scheduler foundation done / migration remains | The bounded priority scheduler handles ordering, deduplication, stale generations, queue pressure and interactive capacity. Migrate existing private workers and add deadlines. |
| 4 | **Proxy media** | 4K â†’ HD during editing; workload-dependent | Engine and selection policy done / scheduling, UI and validation remain | Generation, association, stale/missing status, monitor switching and originals-only export are implemented. A deterministic offline policy chooses 1080p/720p/540p from complexity, misses and memory pressure. Wire telemetry to background scheduling, add UI and benchmark production 4K/8K codecs. |
| 5 | **Hardware decode** (NVDEC/QSV/AMF/D3D11VA) | Large on H.264/HEVC; frees CPU | Medium | **D3D11VA done for H.264-class streams (2026-10-07):** 2.0 ms against 5.3 ms for read + compose of a 1080p picture. Hardware read-ahead, HEVC/AV1 and other vendors remain. |
| 6 | **Render cache UI and node-level reuse** | Existing range cache makes unchanged rendered ranges a memory/disk read; node reuse would avoid recomputing unaffected effects | Shared quotas done / graph and UI remain | Render and flow entries now share cost-aware, pressure-sensitive RAM/disk quotas, with VRAM represented for future backends. Enrol decoded frames and UI atlases, expose status, then extend caching inside the future effect graph. |
| 7 | **Incremental snapshot updates** | O(edit) instead of O(project) per keystroke | Small | Apply the changeset to the snapshot. |
| 8 | **SIMD the software compositor** | 2â€“4Ã— on the CPU path | Medium | Still matters: export and the reference renderer stay on CPU. |
| 9 | **Tile-based rendering with dirty regions** | Large when little changes between frames | Medium | Layers already track dirty rectangles; this extends it across frames. |
| 10 | **Zero-copy decode to GPU** | Removes a full-frame copy per frame per layer | Medium | **Done for D3D11VA pictures**: they are composed in place, with no upload. |
| 11 | **Frame cache in GPU memory** | Removes upload cost on scrub | Small | After 1. |
| 12 | **Prepared-statement caching in the store** | Noticeable on projects with tens of thousands of clips | Small | Statements are currently prepared per call. |
| 13 | **SIMD audio mixing** | Audio is 0.005 ms/block today | Small | **Do not do this.** Listed to be explicit that it is not worth it. |

## Already done, and what it bought

The optimisation pass already applied, all verified behaviour-preserving by the golden frames:

- Layer buffers reused rather than allocated per clip per frame (33 MB each at 1080p)
- Per-layer dirty rectangles, so effects and compositing skip untouched pixels
- `DrawFrame` bounded by the projected corners of the transformed source
- An untransformed clip matching the output resolution is a straight copy, not a bilinear resample
- Opaque pixels skip the un-premultiply divide
- Effect ordering no longer quadratic in stack depth
- The frame cache budgeted in bytes rather than frames
- Bounded per-worker decode pool, generation cancellation, and eight-frame read-ahead
- Cost-aware shared RAM/disk quotas across render and optical-flow caches, including pressure shedding
- Timeline layout virtualized in both axes with binary-search clip entry
- Downsampled latest-frame scope measurement off the playback thread
- Offline proxy preset selection from media complexity and measured playback health
- Playback-plan lowering and immutable render-graph compilation with dependency hashes, dead-node elimination and unary-node fusion
- Capability-based D3D12/Metal/Vulkan device selection with explicit CPU fallback
- Bounded cross-workload priority scheduler with interactive capacity and generation cancellation

Net: 1 track 1080p went 67.7 â†’ 37.3 ms; a single-effect frame 46.9 â†’ 18.4 ms.

## Optimisation principles for this codebase

**Measure before and after, in Release.** `scripts/bench.bat` exists for this. Debug numbers are meaningless here â€” MSVC's iterator debugging dominates the inner loops.

**Protect every optimisation with a golden frame.** The whole pass above is trustworthy only because no golden moved. An optimisation without a pixel-level test is a refactor with unknown consequences.

**Do not optimise the audio path.** It is four orders of magnitude inside its budget. The audio work that matters is architectural â€” the node graph and latency compensation â€” not arithmetic.

**Prefer removing work to doing work faster.** The largest win so far was not making the sampler faster; it was noticing that the common case needs no sampler. The effect graph is the same shape of win at a larger scale.

---

# Sequencing rules

A few constraints that matter more than the phase order:

1. **The render-version field goes in before any change to render semantics.** It is cheap now and impossible later.
2. **The effect interface goes in before third-party plug-ins**, and the built-ins should be its first clients.
3. **Audio latency compensation goes in with the audio graph**, not after plug-ins need it.
4. **The job system comes before the GPU backend.** A fast renderer behind a serial decoder is still a serial pipeline.
5. **Add the missing `ui` domain to `parity/` before estimating Phase A.** Nineteen P0 capabilities depend on infrastructure that is not currently tracked or costed.
6. **Keep `scripts/cross-check-parity.js` in CI.** The report drifted from the code once; the gate is what stops it happening again.


