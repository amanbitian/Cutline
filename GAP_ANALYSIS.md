# Cutline repository gap analysis

Audited: 2026-10-07, after the media, render, audio, export, advanced finishing and UI-model slices. This describes runtime behaviour found in the repository, not desired architecture. `IMPLEMENTATION_STATUS.md` holds the per-capability table and the benchmark figures.

## Current architecture

A native C++20 core with a working media pipeline and an optional Qt Quick application shell in source, plus a superseded browser prototype. The shell builds with Qt 6.8.3 and is verified by 60 framework-independent UI tests and 31 offscreen application tests; it has not been exercised by a person on a display.

The pipeline now runs end to end: ingest a file (probe, fingerprint, journal it), edit it through a 70-command typed bus backed by SQLite with row-level changeset undo, compile a playback plan for any instant, decode original or proxy media, composite the picture, mix the sound, and drive the transport from the audio clock. 751 native tests cover it, including golden-frame comparison, real FFmpeg decode, proxy switching, speed ramps/freezes, optical flow, rolling-shutter and mesh warps, blend modes, graphics/templates, multicam, local speech/transcript editing, realtime audio meters, playback-plan lowering, render-graph compilation, device selection, priority scheduling, effects, stabilization, LUTs, UI-domain behavior and originals-only export round trips. `cutline_demo` exercises the whole pipeline.

The browser prototype (`index.html`, `app.js`, `mcp-server.js`) is retained as a visual reference only and shares no code with the core.

## Independent review, 2026-10-06

[CODE_REVIEW_2026-10-06.md](CODE_REVIEW_2026-10-06.md) found that the pipeline is a useful engine foundation rather than a usable editor, and reproduced eleven correctness failures that the tests in this repository had not caught â€” because they used symmetric signals, short timelines, periodic tones and single clips. [REMEDIATION.md](REMEDIATION.md) lists each finding with its status and regression test; [PERFORMANCE_LOG.md](PERFORMANCE_LOG.md) records measured effects. Ten are fixed; the remaining export-size issue now fails clearly before output is created but still lacks resize rendering. Subsequent hardening closed decoder ownership, nested/retimed audio, grouped edits, playback races, ingest/relink consistency and persistence failure handling. The largest open runtime items are the GPU/render path, production-scale latency and memory, colour management, transition handles, time-stretch, and crash recovery replay.

## What was closed in this pass

- **No media layer.** There is now a `media::Source` provider interface with two implementations: FFmpeg (demux, decode, resample, seek, probe, packet-derived timestamp maps) and a deterministic synthetic source. Nothing above the interface names FFmpeg, so it remains an optional build dependency and the tests run with no media files present.
- **No render layer.** A software compositor implements the render semantics: premultiplied source-over, track-order layers, transitions as a single mixed layer, adjustment layers, sequence effects, and a built-in effect set. Effects it does not implement are reported rather than silently skipped.
- **No audio engine.** A mixer (retiming, nesting, per-sample automation, gains, pans, crossfades with handles, an output clamp), a master clock advanced by rendered samples, an offline sink for export and tests, and a WASAPI device sink whose realtime thread neither allocates nor locks, with underruns counted.
- **No ingest.** Probe, native sampled SHA-256 fingerprint, duplicate detection, and recording as journalled undoable commands. Relink by fingerprint works.
- **No golden-frame testing.** PPM goldens with per-channel tolerance and difference maps. This immediately paid for itself: the whole optimisation pass below is known to be behaviour-preserving because not one golden moved.
- **Colour correctness.** YUVâ†’RGB is delegated to swscale with explicit matrix and range rather than hand-written, and a lossless Rec.709 fixture is checked against its RGB twin so a wrong matrix or a missed range expansion fails a test.
- **Per-frame cost.** Layer buffers are reused rather than allocated per clip per frame, layers track the rectangle they wrote, `DrawFrame` bounds its sweep to the projected source, untransformed full-frame clips skip the resampler, opaque pixels skip a divide, and effect ordering is no longer quadratic. A 1-track 1080p composite went from 67.7 ms to 37.3 ms; a single-effect one from 46.9 ms to 18.4 ms.
- **No export.** A `media::Writer` provider interface mirrors `media::Source`, with an FFmpeg implementation, and an export worker renders the same `PlaybackPlan` the monitor does. The round-trip tests decode an exported file back and compare it against the monitor, including which source frame each picture came from, so correctness rests on a comparison rather than on the file having plausible dimensions. Audio is locked to video by deriving each frame's sample count from absolute time; 1000 frames at 30000/1001 produce exactly 1,601,600 samples.
- **Audio mixed per block, not per edit.** `RenderAudio` mixed a whole block from the plan at its start, so a cut falling mid-block played the outgoing clip across the join. That was first fixed by mixing in pieces between clip edges, which still applied one effect value and one crossfade weight to a whole piece, ignored retiming and reversal, and left nested sequences silent. The mixer now renders from the sequence snapshot, per sample, and blocks of any size tile exactly.
- **Plausible defaults that hid a missing value.** Encoder settings defaulted to 25 fps and 48 kHz, so an export request that forgot the rate silently delivered at the wrong one. Unset now means zero and is filled from the sequence.
- **Hand-maintained parity status.** The manifests recorded nothing as implemented while 43 capabilities were built and tested. `scripts/cross-check-parity.js` now requires named, existing tests for any `implemented` claim.
- **An unbounded cache.** The engine's frame cache was counted in frames, which is 8 MB at SD and over a gigabyte at 4K for the same setting. It is now a byte budget.

Two real defects were found by the work itself rather than by review. The demo exposed an undo failure where a changeset's deletes collided with the database's own foreign-key and trigger cascades â€” fixed with `SQLITE_CHANGESETAPPLY_FKNOACTION` plus a trigger-suppression flag, and pinned by three regression tests. The playback engine reported "no more audio" one block early, cutting the final block of a sequence.

## Remaining architectural gaps

**The GPU path covers a subset.** A Direct3D 11 compositor renders layers, motion, crop, opacity, grade, the colour tools and LUTs, generators, adjustment clips, dissolves and sequence effects on the GPU, tested against the software compositor; everything else in a plan (filters, keys, blend modes, masks, graphics, captions, colour conversion) is composed in software, frame by frame, with the reason counted. On this machine (AMD Radeon AI PRO R9700, Release build, `scripts\bench.bat --gpu`, perf/2026-10-07-gpu.txt) one 1080p layer with three effects composes in 36.9 ms in software and 2.7 ms on the GPU including the upload and the read-back (0.26 ms on the card); four layers 153.7 ms against 4.5 ms; reading and composing a 1080p H.264 picture takes 5.3 ms with software decode and upload and 2.0 ms with the hardware decoder and no copy. The compiled render graph is not what it executes, the picture is read back for the monitor, and Direct3D 12/Metal/Vulkan do not exist. CPU scopes and their display panel run asynchronously; GPU aggregation remains open.

**Hardware decode is partial.** Direct3D 11 video acceleration decodes 8/10-bit 4:2:0 streams the adapter supports into shader-readable textures that the compositor reads in place. There is no hardware read-ahead or frame cache. AMD AMF export is verified and NVENC/QSV are capability-probed with software fallback, but other platform providers and hardware combinations remain unvalidated; only H.264 decode on one AMD adapter has been tried.

**Application shell is built but partial.** Docking and workspaces, the asynchronous program monitor, the custom timeline with its editing tools, the effect inspector, history, jobs, LUT browser, command palette, shortcuts, preferences, speed-ramp editor, multicam interface, export queue, local transcript editor, caption authoring, asynchronous scopes and audio mixer exist and are tested offscreen. The caption panel creates and edits tracks and timed cues, styles tracks, imports/exports SRT/WebVTT and records mutations in undo history. Graphics and template authoring exist (the Graphics workspace); proxy/cache controls and a source monitor remain absent or incomplete; delivery still lacks watch folders, image sequences, smart render and certified broadcast wrappers.

**Composition and presentation are still synchronous.** The audio path has a lock-free handoff, and video decode has a bounded worker pool with independent `Source` instances, read-ahead and generation cancellation. A bounded cross-class priority scheduler is now tested, but decode, presenter, analysis, proxy and export still use their private mechanisms. The compositor runs on the caller; there is no GPU queue, background indexer or worker process.

**Editorial UI is incomplete.** The command/store layer and edit planner cover insert, overwrite, delete, lift, extract, move, split, ripple/roll/slip/slide trim, linking and source placement. Multicam groups, angles, sync results and cuts are persisted through six commands, with angle-monitor composition, live cut recording and flattening. The ramp editor and the multicam interface now exist; the missing work is source patching/targeting, trim monitor polish and a validated cut of real footage.

**Advanced render foundations are CPU-only.** Speed-remap curves and audio freezes/reverse segments, variable-rate WSOLA with explicit fallbacks, 12 blend modes, automatic rolling-shutter analysis/mesh warp, optical-flow interpolation with a persistent analysis cache, and versioned graphics/templates (with rotation, outlines, shadows and rounded corners) have deterministic CPU behavior. The GPU compositor and GPU equivalents for filters, noise reduction, warps, optical flow and graphics remain open.

**Colour management is implemented but incomplete.** Per-stream colour metadata survives from probe to plan; decode normalises range; render-version-gated input, working-space and display transforms, LUTs, PQ/HLG, tone mapping, gamut mapping and an asynchronous CPU scope display exist. OpenColorIO, an ACES output transform, a 10-bit end-to-end path, HDR monitoring and GPU scope aggregation remain open.

**Interchange is partial.** OpenTimelineIO, CMX 3600 EDL and Final Cut 7 XML read/write paths have round-trip tests and report unsupported data. AAF is not started.

**Proxy and render-cache engine paths now exist.** Local background proxy generation, persisted fingerprint-safe associations, monitor switching/original fallback, originals-only export, and a content-addressed memory/disk render cache are implemented. Proxy controls, progress display and production 4K/8K evidence remain UI and validation gaps.

## Technical debt and risks

- The browser prototype is still in the tree and reads as live code. It should move to a clearly marked reference directory or be deleted.
- Effects reference their owner polymorphically (`owner_kind` + `owner_id`), which SQLite cannot enforce with a foreign key. Cascade is supplied by triggers, checked by `ValidateDatabase()`, and suppressed during changeset replay. It is the schema's one soft spot and it has already produced one bug.
- Rational times are stored twice: the exact pair, which is authoritative, and a derived `_ticks` integer for indexed range queries. One function keeps them in step, but it is duplicated state.
- The interactive undo cursor remains in memory, while journal v2 and snapshots recover durable project state. Undo history itself is intentionally not reconstructed after crash recovery.
- `media::Source` allows one video and one audio reader together, but seek state cannot be shared by concurrent readers of the same stream. The decode pool therefore owns one source set per worker, increasing decoder memory with worker count.
- The compositor holds its scratch buffers, so one instance belongs to one thread. That is documented and enforced by making it move-only, but it is a constraint a multi-threaded renderer must respect.
- Fixtures are generated at configure time by the bundled ffmpeg binary. If it is absent, 12 media tests skip rather than fail â€” visible in the output, but still a silent reduction in coverage on a machine without it.
- `scripts/cross-check-parity.js` verifies that every implemented parity claim names real tests and can execute them, but most planned/partial scope remains a maintained product inventory rather than executable acceptance coverage.
- No soak or leak testing, no A/V sync measurement over a long run, no codec compatibility matrix, no performance regression gate in CI.
- Not a git repository. For a project of this size that remains the cheapest unfixed problem.

## Implementation order

1. (Done) Qt 6.8.3 is installed under `.tools\qt`; the desktop shell builds and has offscreen application tests. Profile it against production footage.
2. Add a render/presentation thread and scheduler priorities around the completed bounded decode pool.
3. (Done for the common subset) Windows GPU compositor verified against the software one, with zero-copy hardware decode. Colour tools and LUTs are on the GPU too (GPU-003). Next: filters/keys/blend modes/masks on the GPU, a texture shared with the monitor, and hardware encode.
4. Complete the missing product interfaces: proxies/cache (graphics and templates, speed ramp, multicam, transcript, caption authoring, scopes, mixer and export interfaces exist).
5. Validate the engine and UI on production 4K/8K, HDR, long-form and multicamera footage with p95/p99 latency, memory and A/V-sync gates.
6. Add AAF, plug-in hosting and the remaining professional workflow integrations after the local offline editing path is solid.


