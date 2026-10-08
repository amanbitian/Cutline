# Performance log

Append-only record of measured performance changes. Each entry says what was measured, how, on what, and what changed, so a later regression can be compared against a number rather than a recollection. Raw output for every entry is kept in [perf/](perf/).

Reproduce with `scripts\bench.bat [frames]`, which builds and runs `cutline_bench` in **Release**. Debug numbers are meaningless here: MSVC's iterator debugging dominates the inner loops.

## How to read these numbers

- They are short local microbenchmarks on one machine, not product throughput and not a comparison with any other editor.
- The render cases reuse an already-decoded frame, so they measure compile and composite, not file decode, disk, GPU upload or presentation.
- "Before" and "after" are measured on the same machine in the same build configuration. The "before" figures are produced by temporarily reintroducing the old code (see *Method* below), not quoted from elsewhere.
- Cases below 0.1 ms are reported in microseconds.

## 2026-10-06 — spatial effects CPU baseline

Source: `perf/2026-10-06-spatial-effects.txt`. Release, 1920×1080, three frames per case. These cases were added to `cutline_bench` so new effects have explicit budgets rather than disappearing inside a generic composite number.

| One float source track | Time | Throughput |
|---|---:|---:|
| Opacity-only control | 18.531 ms/frame | 54.0 fps |
| Blur, 12 px radius | 38.428 ms/frame | 26.0 fps |
| Sharpen, amount 1 | 30.135 ms/frame | 33.2 fps |
| Vignette | 26.023 ms/frame | 38.4 fps |
| Lens correction | 44.819 ms/frame | 22.3 fps |

Blur uses a separable sliding window, so increasing radius does not multiply work per output pixel. Every spatial effect reuses the compositor's retained scratch layer and performs no per-frame image allocation after warm-up. Lens correction still exceeds a 25 fps frame budget on the CPU reference path; shader execution in the planned GPU compositor remains necessary for interactive production use.

## 2026-10-06 — review remediation: step 1 and the first part of step 2

Source: `perf/2026-10-06-before.txt` and `perf/2026-10-06-after.txt`. Release, 1920×1080, 40 frames per case.

### Per-frame compile cost against project size

`Compile(sequence)` is what the playback engine calls once per frame for each sequence level. It used to wrap a **copy** of the whole sequence in a graph, so every frame duplicated every clip, effect and keyframe in the project and the binary search that finds the active clip was hidden behind an O(project) copy.

| Clips in the sequence | Before | After | Speed-up |
|---:|---:|---:|---:|
| 10 | 1.094 µs | 0.283 µs | 3.9× |
| 1,000 | 153 µs | 0.312 µs | ~490× |
| 10,000 | 2.006 ms | 0.310 µs | ~6,500× |
| 50,000 | 13.039 ms | 0.398 µs | ~33,000× |

The older multi-track cases, which have one clip per track, improved too, because even a small sequence was being copied:

| Compile, one clip per track | Before | After | Speed-up |
|---:|---:|---:|---:|
| 1 video track | 1.260 µs | 0.681 µs | 1.9× |
| 4 video tracks | 3.906 µs | 1.980 µs | 2.0× |
| 16 video tracks | 14.283 µs | 6.990 µs | 2.0× |
| Compile + mix, 1 audio track (4 video tracks in the plan) | 5.184 µs | 3.204 µs | 1.6× |

At 10,000 clips the old cost was 2 ms of a 33 ms frame budget at 30 fps (6%) spent compiling one frame; at 50,000 it was 13 ms (39%). The cost is now independent of project size. `Compile(graph)` was already flat (0.28–0.50 µs before and after), which is the control: nothing else in the compile path changed.

Guarded by `PerFrameCompileCostDoesNotGrowWithTheNumberOfClips`, a ratio test between 10 and 20,000 clips. I confirmed it fails (41/42) when the copy is reintroduced.

### Loading one sequence from a project that has many

`LoadSequence` scoped its *effect* query to the sequence but read **every** parameter and keyframe in the project, and nested sequences repeated that. A project with many animated sequences paid for all of them on every load.

| Other animated sequences | Before | After | Rows read before → after (params / keyframes) |
|---:|---:|---:|---|
| 0 | 0.120 ms | 0.257 ms | 1 / 0 → 1 / 0 |
| 20 | 0.478 ms | 0.234 ms | 201 / 600 → 1 / 0 |
| 80 | 1.623 ms | 0.223 ms | 801 / 2,400 → 1 / 0 |

After the fix the cost is flat at about 0.23 ms and the rows read are exactly those the sequence owns.

**A regression I introduced, stated plainly:** with no other sequences the load is now about 2× *slower* (0.257 ms against 0.120 ms). The scoped set costs a roughly constant 0.1 ms of query planning that the unscoped version did not pay. Break-even is around 8 other sequences. That is the right trade for any project that is not trivial, and it is flat rather than growing, but it is a real cost on the smallest projects. Statements are prepared on every load; caching them is the obvious way to recover it and is listed under *Not yet done*.

It took three attempts to get the flat result, and the intermediate steps are worth keeping:

| Version of the scoping query | 0 others | 20 others | 80 others |
|---|---:|---:|---:|
| `OR` of `IN` subqueries (first attempt) | 0.450 ms | 0.378 ms | 0.420 ms |
| `UNION ALL` of index lookups | 0.225 ms | 0.350 ms | 0.870 ms |
| plus `CROSS JOIN` to fix join order | 0.222 ms | 0.303 ms | 0.600 ms |
| plus `CROSS JOIN` inside each branch (**kept**) | 0.226 ms | 0.222 ms | 0.224 ms |

The planner, left to choose, scanned the large tables and probed the small scoped set, which is linear in the project; each step forced the small set to drive instead. Guarded by `LoadingASequenceDoesNotReadOtherSequencesEffects`, which asserts the exact row counts rather than a time.

### Time arithmetic

| | Before | After |
|---|---|---|
| `FromFrames(…).ToTicks()` at 29.97, about one hour in | **threw** `RationalTime multiplication overflow` | 29.4 ns/call |

This is a correctness fix rather than a speed-up: the tick conversion multiplied by 254,016,000,000 before dividing, so an unreduced time near an hour overflowed 64 bits although the result fits easily. It now multiplies and divides in 128 bits and narrows once. The new path is cheap enough that it does not show up in the compile figures above.

### Unchanged, as intended

| Case | Before | After |
|---|---:|---:|
| Composite, 1 track, 3 effects, 8-bit source | 36.570 ms | 37.024 ms |
| Composite, 4 tracks, 3 effects, 8-bit source | 136.041 ms | 135.862 ms |
| Composite, 1 track, 3 effects, float source | 31.809 ms | 31.644 ms |
| 17-effect stack, float source | 129.341 ms | 129.762 ms |

None of the changes touched the compositor, and the figures agree to within noise.

### Not measured

The export fixes (per-channel audio queues, resampling to the requested rate, publish-through-a-temporary) are correctness changes and were **not** benchmarked. The per-channel queues use `erase` at the front, which is O(queued samples) per encoder frame; the queue holds at most one block plus one frame, so this is expected to be negligible, but that is an expectation, not a measurement.

### Method

"Before" was produced by `make-before.js` (kept with the review scratch work, not in the repository), which backs the two source files up, reintroduces the copy and the unscoped queries, builds and runs the benchmark, and restores the backups byte for byte.

**Incident worth recording.** The first "after" run following a restore reported the *old* compile cost. The restore preserved the backup's older timestamp, so the Release build saw the source as older than the object built from the temporarily patched code and skipped recompiling it; the benchmark was measuring stale code. It was caught because the figures contradicted an earlier "after" run and a passing scaling test in the Debug tree, not by any check in the process. The files were touched, rebuilt, and the run repeated; the saved logs are from that corrected run. **When swapping code in and out between benchmark builds, confirm the build log shows the file being recompiled.**

## 2026-10-06 — milestone 1A: edit-command latency and the cost of the new invariant checks

Milestone 1A added work to editing commands (transition validity and detachment, track-lock checks, keyframe splitting). This entry measures what editing now costs and what the added checks cost. It measures the **command service and store only** — not UI feedback latency, not rendering.

**Platform.** Release build (MSVC, VS 18), AMD Ryzen 9 9950X3D (16 cores / 32 threads), 93.6 GB RAM, Windows 11 build 26200, Radeon RX 9070 XT (not used: these cases never touch the GPU). The on-disk package lives in the user temp directory; **the storage class of that drive was not established** (Get-PhysicalDisk lists an HDD first), so the on-disk figures say "WAL commit on this machine's temp drive", nothing more.

**Fixture.** 1000 one-second clips on 4 video tracks (250 each) in a 25 fps 1080p sequence over one 2-hour media item; 25 cross-dissolves on track 0; 125 clips on track 1 carry a three-key eased exposure animation plus a 2-component position. Built and driven only through typed commands. Warm database, 150 rounds per command (fewer where the fixture limits it, shown as *n*).

**Commands measured.** `MoveClip` to free space, `TrimClip` as a slip, `SplitClip` on an animated clip (divides the curves), `SetClipSpeed` on a clip with no transition, `AddTransition`, `SetTransitionTiming`, and `Undo` (changeset replay).

### Results — in-memory database (ms)

| Command | n | Median | p95 | p99 | Max |
|---|---:|---:|---:|---:|---:|
| MoveClip | 150 | 0.067 | 0.072 | 0.112 | 0.124 |
| TrimClip (slip) | 150 | 0.063 | 0.069 | 0.076 | 0.083 |
| SplitClip, animated | 125 | 0.228 | 0.249 | 0.280 | 0.323 |
| SetClipSpeed | 125 | 0.061 | 0.068 | 0.084 | 0.098 |
| AddTransition | 115 | 0.102 | 0.110 | 0.136 | 0.150 |
| SetTransitionTiming | 24 | 0.087 | 0.105 | 0.118 | 0.118 |
| Undo | 150 | 0.338 | 0.381 | 0.409 | 0.515 |

### Results — on-disk package, WAL (ms)

| Command | n | Median | p95 | p99 | Max |
|---|---:|---:|---:|---:|---:|
| MoveClip | 150 | 0.613 | 0.749 | 2.927 | 5.841 |
| TrimClip (slip) | 150 | 0.599 | 0.712 | 3.018 | 3.474 |
| SplitClip, animated | 125 | 0.899 | 1.154 | 4.247 | 4.416 |
| SetClipSpeed | 125 | 0.604 | 0.715 | 4.086 | 5.817 |
| AddTransition | 115 | 0.661 | 0.759 | 1.070 | 3.499 |
| SetTransitionTiming | 24 | 0.659 | 0.856 | 0.921 | 0.921 |
| Undo | 150 | 0.381 | 0.463 | 2.754 | 3.317 |

Every command is far inside the 50 ms visible-feedback target *for the store*; the tail on disk (a few ms at p99) is the commit, and is the number to watch once a UI sits on top.

### Cost of the new checks

A second run with `RequireValidTransition` and `DetachBrokenTransitions` returning immediately (`perf/2026-10-06-milestone-1a-checks-disabled.txt`) approximates the old behaviour **for move, trim, speed and the two transition commands only**; it does not reproduce the old split, ripple or lock behaviour, and it is not the pre-1A code. Medians, in-memory, checks off → on:

| Command | Off | On | Δ |
|---|---:|---:|---:|
| MoveClip | 0.059 | 0.068 | +0.009 |
| TrimClip (slip) | 0.056 | 0.064 | +0.008 |
| SetClipSpeed | 0.054 | 0.068 | +0.014 |
| AddTransition | 0.052 | 0.111 | +0.059 |
| SetTransitionTiming | 0.040 | 0.088 | +0.048 |

("On" is the first 1A run, `perf/2026-10-06-milestone-1a.txt`; the clean re-run above agrees within run-to-run spread.) On disk the difference is about +0.01 to +0.1 ms on a 0.6 ms command. The checks cost tens of microseconds per edit and buy a structural guarantee, so they stay.

### Caveats

- **Run-to-run spread is real.** Undo's in-memory median was 0.519 ms in the first run and 0.338 ms in the re-run on identical code, and tail figures moved by up to 2×. Treat differences under ~0.05 ms as noise; only the transition commands' doubling is outside it, and it is explained by the added validation query.
- **The first on-disk run printed its figures and then failed to delete its temporary package** (the benchmark still held the database open). That was a benchmark bug, now fixed (`EditProject::Close`); the re-run (`perf/2026-10-06-milestone-1a-rerun.txt`) completed cleanly. Figures are from the runs named above; none were discarded for being unfavourable.
- The Release tree was confirmed to recompile `ProjectStore.cpp` in both directions (checks off, then restored), per the incident recorded above.
- Not measured: UI-visible latency, decode/seek, playback, the cost of `ValidateDatabase()` on open, and projects larger than 1000 clips.

## 2026-10-06 — milestone 1B: demux and decode latency on real files

Milestone 1B changed how files are read (a demuxer per stream, sample-accurate audio placement with a discarded lead-in, a resampler that is aligned and flushed). This entry records what reading costs now. **There is no "before" column**: on alternating reads the old code returned the wrong samples, so a faster-but-wrong figure would not be a baseline for anything. The comparison that matters is correctness, and it is in the test suite.

**Platform.** Release build (MSVC, VS 18), AMD Ryzen 9 9950X3D, 93.6 GB RAM, Windows 11 build 26200, software decode only (FFmpeg LGPL build in `.tools/ffmpeg`; the Radeon is not used). Files are the generated fixtures in `build/native/fixtures` on drive F: and, for the long file, a generated file in the user temp directory. **All reads are from a warm OS file cache**; cold-disk figures were not measured, and the storage class of the drives was not established.

**What these fixtures are.** Small: 64x48 lossless FFV1, 160x90 and 320x180 H.264 (GOP 25 and 30), and mono or stereo audio of 4 s. They exercise demux, seek, lead-in decode and resample, and say **nothing** about the cost of decoding production-resolution video or long-GOP footage. Median, p95, p99 and max are over *n* reads of 1024 output frames unless stated; command: `scripts\bench.bat 40`, raw output in `perf/2026-10-06-milestone-1b.txt`.

### Audio, blocks read in order (ms)

| Case | n | Median | p95 | p99 | Max |
|---|---:|---:|---:|---:|---:|
| PCM, 48 kHz stereo (Matroska) | 185 | 0.002 | 0.007 | 0.018 | 8.976 |
| AAC, 48 kHz mono (MP4) | 185 | 0.004 | 0.006 | 0.011 | 6.486 |
| PCM 44.1 → 48 kHz mono (WAV) | 185 | 0.000 | 0.009 | 0.015 | 0.163 |

The maxima are the first read of each file, which opens the decoder and builds the converter.

### Audio, a block at a random place — every read seeks (ms)

| Case | n | Median | p95 | p99 | Max |
|---|---:|---:|---:|---:|---:|
| PCM in Matroska, stereo | 300 | 0.022 | 0.062 | 0.088 | 0.226 |
| AAC in MP4, mono | 300 | 0.041 | 0.164 | 0.198 | 0.271 |
| PCM 44.1 → 48 kHz, Matroska | 300 | 0.045 | 0.099 | 0.112 | 0.180 |
| PCM 44.1 → 48 kHz, WAV | 300 | 0.026 | 0.073 | 0.121 | 0.177 |
| PCM mono, Matroska, **one hour** (347 MB) | 300 | 0.251 | 0.976 | 1.242 | 1.826 |

Each seek decodes an 8192-sample lead-in (about 170 ms of audio) before the samples it keeps; that is the price of being sample-accurate and is included above.

**One-off index.** Matroska's millisecond timestamps need an exact packet index before the first seek (see REMEDIATION.md). The first read of the one-hour file, which includes building it, took **99.4 ms**; for the 4-second fixtures it took 0.2–0.7 ms. The cost is proportional to the file's packet count and, on a cold cache, to its size on disk, which was not measured. A UI that opens a long Matroska file and plays from the middle will feel it once.

### Video (ms)

| Case | n | Median | p95 | p99 | Max |
|---|---:|---:|---:|---:|---:|
| H.264 320x180, GOP 30, in order | 90 | 0.035 | 0.104 | 13.466 | 14.407 |
| H.264 320x180, GOP 30, random (each seeks) | 200 | 1.138 | 1.872 | 2.090 | 2.102 |
| FFV1 64x48 intra-only, in order | 90 | 0.007 | 0.015 | 0.022 | 2.080 |
| FFV1 64x48 intra-only, random | 200 | 0.200 | 0.332 | 0.367 | 0.398 |

The H.264 in-order tail (13–14 ms) is the first frame, which opens the decoder; it is a p99 here only because *n* is 90. A random H.264 read decodes up to a GOP from the previous keyframe, so 1.1 ms at 320x180 will not transfer to a 4K long-GOP file.

### Audio and video from one file, alternating

H.264 + AAC (160x90, 25 fps), 40 ms of audio and one video frame per step, *n* = 150: median 0.018 ms, p95 0.033, p99 0.538, max 2.479. This is the pattern that used to corrupt audio.

### Caveats

- These are microbenchmarks of the reader, not of playback: no compositing, no audio device, no concurrency.
- Everything is a warm cache, small file or short GOP. Do not read them as scrub-to-correct-frame latency, which needs a defined local-media workload (milestone 2).
- The benchmark section is new and has no earlier run to compare with. The edit-latency section of the same run is within the spread recorded in the previous entry.

## 2026-10-06 — milestone 1C and hardening: audio mixing, linked edits, durability

Same machine and method as the two entries above (Release, Ryzen 9 9950X3D, warm cache, `scripts\bench.bat 40`; raw output in `perf/2026-10-06-milestone-1c.txt`). The storage class of the drive holding the temporary packages was not established.

### Audio mixer (1024-frame blocks, 48 kHz stereo, µs or ms per block)

The mixer was rewritten (it now renders per sample from the sequence snapshot), so there is **no before**: the old mixer ignored retiming and reversal, skipped nested audio, and applied one effect value per block, so a figure for it would be for different work. The block budget at 48 kHz is 21.3 ms.

| Case | Per block |
|---|---:|
| 4 tracks of plain clips | 16.6 µs |
| 4 tracks, every clip retimed 3/2 (cubic interpolation) | 19.8 µs |
| 4 tracks, every clip reversed and retimed | 20.0 µs |
| 4 tracks, a gain **keyframed and evaluated at every sample** | 547 µs (2.6% of the budget) |
| One track of 10,000 clips (block near the end) | 4.6 µs |

These measure the mixer alone: the resolver hands back silence, so there is no decode and no resampling in them. The keyframed case is the expensive one (about 130 ns per sample per animated parameter: a rational-time construction and a curve lookup); a block with several animated parameters on several clips would scale with it. The 10,000-clip figure is the point of the windowed search: it is the same as for a short track, where the previous design scanned every clip and transition in the project for every block (not measured).

### Edit commands (ms, store only; same fixture as the previous entry)

| | MoveClip median | SplitClip (animated) median | Undo median |
|---|---:|---:|---:|
| In-memory | 0.073 | 0.237 | 0.342 |
| On-disk package, `synchronous=NORMAL` | 0.769 | 1.072 | 0.387 |
| On-disk package, `synchronous=FULL` | 1.148 | 1.395 | 0.919 |

In-memory is unchanged within noise from the previous entry. **On disk the median moved up by roughly 0.15–0.25 ms** (MoveClip 0.61 ms in the milestone-1A run, 0.77 to 0.83 ms in two runs now): the journal copy is now written to a temporary and renamed, and checked, instead of written in place. That is the price of a copy that cannot be left half-written; it is within the spread seen between runs of unchanged code, so treat it as a likely cost, not a measured one. **`synchronous=FULL` (every commit forced to disk) adds about 0.3–0.4 ms at the median on this machine**; on a slower disk it would add much more, which is why it is a choice and `Normal` is the default.

Linked edits, in-memory, 1000 clips, each edit also editing its partner (n = 60 each):

| Command | Median | p95 | p99 | Max |
|---|---:|---:|---:|---:|
| MoveClip, linked pair | 0.118 | 0.126 | 0.128 | 0.169 |
| TrimClip tail, linked pair | 0.105 | 0.110 | 0.119 | 0.149 |
| SplitClip, linked pair | 0.157 | 0.168 | 0.181 | 0.204 |
| SetClipSpeed (reverse), linked pair | 0.104 | 0.109 | 0.114 | 0.116 |

A linked move costs about 1.6× the single-clip move (0.118 vs 0.073 ms) for twice the work.

### Decode

Unchanged within noise from the 1B entry, including after the video reader began decoding one frame ahead (H.264 in order 0.026 ms median; random seek 1.2 ms). The look-ahead costs one extra decode on the first frame after a seek, which these fixtures are too small to show.

### Caveats

Everything above is store-only or mixer-only. No UI, no decode of production footage, no end-to-end playback latency has been measured, and none of it is a comparison with another editor.

## 2026-10-06 — milestone 2: bounded decode pool and read-ahead

Source: [perf/2026-10-06-milestone-2-read-ahead.txt](perf/2026-10-06-milestone-2-read-ahead.txt). Release build, reproduced with `scripts\bench.bat --playback` on the same machine as the entries above.

The playback engine now uses one independent `Source` set per decode worker, a bounded pending queue, seek/edit generations that discard stale work, and the existing byte-bounded LRU as the publication point. The benchmark uses one worker and a one-frame window so each timing is for the requested frame rather than parallel look-ahead. It makes 80 unique, nonlocal seeks through `bars-2997.mp4` (H.264, 320×180, 29.97 fps, GOP 30, four seconds), waits for the worker to publish the raw frame, and then renders those cached positions.

| Stage | n | Median | p95 | p99 | Max |
|---|---:|---:|---:|---:|---:|
| Seek request → raw frame in shared cache | 80 | 1.139 ms | 1.944 ms | 2.122 ms | 4.793 ms |
| Cached raw frame → composed RGBA frame | 80 | 0.357 ms | 0.426 ms | 0.695 ms | 0.783 ms |

All 80 rendered frames were consumed from read-ahead. The production default is two workers, eight frames ahead, at most 64 queued decodes, and at most eight open sources per worker. Cache identity changed from clip id plus requested time to media id plus requested time, so duplicate clips now share their raw frame; a regression test proves one miss and one hit for two stacked clips.

These numbers do **not** validate the proposed 150 ms product target. The media is tiny, short, local and warm-cache; there is no Qt event, GPU upload, display scheduling, proxy choice, simultaneous audio, cold disk, 1080p/4K decode, or concurrent export. They establish a reproducible baseline for the new queue-to-cache path. A declared production workload and visible-frame instrumentation remain required.

## 2026-10-06 — processors in the mixer

Source: [perf/2026-10-06-audio-dsp.txt](perf/2026-10-06-audio-dsp.txt). Release build, `scripts\bench.bat`, 1024-frame blocks at 48 kHz stereo, four tracks each carrying an equaliser, a compressor and a limiter, silence as input (the cost of the processing, not of decoding).

| Case | Cost per block | Budget (21.3 ms) |
|---|---:|---:|
| Processors, no cell cache | 16.2 ms | 76% |
| Processors, cell cache | 1.07 ms | 5% |

Without the cache each block re-runs the 4096-sample cells it touches, which is correct and nearly as slow as real time; the playback engine shares the cache, so steady playback pays the second figure. Nothing else in this run was benchmarked in Release: the filters, keying, grading, tracking, noise reduction, colour management and render cache have only Debug-build test timings, and the render cache's value (a hit instead of a composite) has not been measured against production footage.

## Not yet done

The 2026-10-06 proxy implementation removes full-resolution decode from the monitor path when a valid proxy is selected, but no production-media timing is recorded yet. The next proxy benchmark must compare original and proxy playback on declared 4K/8K codecs, include generation throughput and storage cost, and verify that export still decodes originals.

Ordered by expected value. These are remaining review items; partial progress is noted below.

| Item | Why it matters |
|---|---|
| Index transitions for range queries | Clip boundaries now use the existing sorted, non-overlapping snapshot to seek directly into the window (see the 2026-10-07 range-query entry). Transitions still require a linear scan; no new snapshot index is retained |
| Cache prepared statements in the loader | Recovers the ~0.1 ms fixed cost above on small projects |
| Add a rendered-frame cache; canonicalise decoded keys to media version, stream, actual decoded PTS and format | Raw frames now use media id plus requested source time and are shared across clips, but VFR requests resolving to one PTS can still duplicate and rendered results are not cached |
| Session-wide memory budget (decoders, nested composites, compositor scratch) | The 256 MiB LRU bounds only one of several retained resources; 4K scratch buffers can retain ~630 MiB |
| GPU compositor, hardware decode, zero-copy surfaces | Done for the common subset on one adapter: see the 2026-10-07 GPU entry below |
| Complete-pipeline metrics: queue delay, present time, memory, p95/p99, underruns | Engine counters now expose read-ahead and render totals and the benchmark measures seek-to-cache, but there is no presentation/UI timing, memory telemetry or concurrent-export workload |

## 2026-10-06 — optical-flow analysis and what caching it saves

Source: [perf/2026-10-06-optical-flow.txt](perf/2026-10-06-optical-flow.txt). Release build, `scripts\bench.bat --flow`, two 1080p textured float pictures (the second the first moved six pixels), default search (8-pixel blocks, radius 12).

| Step | Cost |
|---|---:|
| Estimate flow for the pair (forward and backward pass with occlusion check) | 4.87 s |
| Synthesise one in-between picture from a known flow | 78 ms |
| Content key for the pair (hash of both pictures) | 8.9 ms |
| Pair already analysed: key plus cache lookup | 8.9 ms |

A clip played at a quarter of its speed asks for four in-between pictures from each pair of source frames. Without the analysis cache that is about 19.5 s of estimation per source pair; with it, one estimate and three lookups. The cache key hashes the full pictures, which is what makes it content-addressed and what bounds a hit at about 9 ms at 1080p.

The estimate itself is not real time: 4.9 s per pair means uncached motion-compensated slow motion on 1080p footage is an offline operation (run the background analysis first, or accept frame blending). This is a CPU reference with an exhaustive block search, one machine and synthetic texture; real footage with larger motion, noise and flat areas was not measured, and nothing here is a statement about a GPU implementation, which does not exist.

## 2026-10-07 — P0 scheduling and render-graph foundations

Implemented structural prerequisites for later GPU and pipeline benchmarks:

- playback-plan lowering to immutable render nodes with media/quality dependencies, cycle/reference validation, dependency hashes, dead-node elimination and safe unary fusion;
- capability-based D3D12, Metal, Vulkan and CPU device selection for AMD, NVIDIA, Intel, Apple and software descriptors;
- opaque external-surface and completion-fence contracts for future zero-copy bindings;
- a bounded priority scheduler with identity deduplication, stale-generation cancellation, lower-priority queue replacement and reserved interactive worker capacity.

These changes do not alter the compositor hot path yet, so no GPU frame-time improvement is claimed. The existing 37.3 ms 1080p CPU baseline remains the relevant number until a platform graph executor passes CPU/GPU golden comparisons. Scheduler ordering and cancellation are deterministic unit tests; production deadline telemetry is still pending.

## 2026-10-07 - Direct3D 11 compositor and hardware decode

Source: [perf/2026-10-07-gpu.txt](perf/2026-10-07-gpu.txt). Release build, `scripts\bench.bat --gpu`, adapter AMD Radeon AI PRO R9700 (32 GB), 1920x1080, 8-bit sources handed over the way a decoder does (so the upload is counted), 60 frames per case, one machine.

| Case | Software | GPU (total) | Of which upload / on the card / read back |
|---|---:|---:|---|
| 1 track, 3 effects | 36.9 ms | 2.7 ms (13.6x) | 0.4 / 0.26 / 2.0 ms |
| 2 tracks, 3 effects | 77.3 ms | 3.3 ms (23.1x) | 1.0 / 0.33 / 2.1 ms |
| 4 tracks, 3 effects | 153.7 ms | 4.5 ms (34.4x) | 1.8 / 0.40 / 2.3 ms |
| 1080p H.264, read one picture and compose one layer | 5.3 ms (software decode 3.3 + upload + GPU compose) | 2.0 ms (hardware decode, no copy) | 2.7x |

What the numbers say: the work on the card is a fraction of a millisecond; what is left is the upload of each source picture (about 0.4 ms for an 8 MB picture) and, above all, reading the finished picture back to memory (2.0-2.3 ms), which exists only because the monitor receives a picture in memory. A texture shared with the monitor would remove it. These are timings of one render call, not a measured frame deadline under playback load, and not a p95/p99; they were taken on a single machine and one vendor.

## 2026-10-07 - Encoders on this machine

A writer-only probe (no compositor): 25 identical 1920x1080 frames of a synthetic picture, 8,000 kbit/s requested, one run each, so a rough ordering and not a benchmark of quality or of real footage.

| Encoder | 25 frames | Notes |
|---|---:|---|
| h264_amf (AMD graphics card) | 98 ms | works; 405 kB |
| hevc_amf, 8-bit / 10-bit | 87 / 130 ms | works |
| av1_amf | 85 ms | works |
| libopenh264 (software) | 122 ms | works; 241 kB |
| ffv1 10-bit 4:2:2 (lossless) | 192 ms | 8.3 MB |
| prores_ks 4:2:2 10-bit | 357 ms | 24 MB |
| libsvtav1 (software) | 583 ms | works |
| libvpx-vp9 (software) | 2267 ms | works, slow |
| h264_nvenc, h264_qsv | n/a | do not start on this machine (no NVIDIA or Intel graphics) |

Not measured: encoding real footage, the effect on a full export of overlapping decode, compose and encode (the export worker is still serial: render a frame, then encode it), or any quality comparison. The card's encoders are not faster than the software H.264 encoder here by a margin worth claiming from one run of a synthetic picture; their value is that they are not using the CPU that the compositor is.


## 2026-10-07 - Optical-flow and timeline range-query optimization

Release measurements on the same local machine and inputs, before and after the changes. Reproduce with `scripts\bench.bat --flow` and `scripts\bench.bat --range`. Timings are averages, not deadline percentiles or end-to-end editor latency.

Sources: [flow before](perf/2026-10-07-optimization-flow-before.txt), [flow after](perf/2026-10-07-optimization-flow-after.txt), [range before](perf/2026-10-07-optimization-range-before.txt), [range after](perf/2026-10-07-optimization-range-after.txt).

### Optical flow

The existing workload uses two synthetic 1920x1080 float RGBA pictures, translated six pixels, 8-pixel blocks, radius 12, and three estimates per measurement. The final after measurement ran after the build and tests completed.

| Operation | Before | After |
|---|---:|---:|
| Estimate forward motion plus backward occlusion check | 5075.4 ms | 1005.2 ms |
| Synthesize one in-between picture | 75.6 ms | 74.7 ms |
| Cache key plus lookup for an already analyzed pair | 8.86 ms | 8.88 ms |

Estimation is **5.05x faster**. Brightness is computed once into two contiguous luma planes, valid pixel intersections are calculated per search candidate, and candidates stop once their partial error cannot beat the runner-up. Preserving the runner-up preserves confidence, ties and occlusion decisions as well as the winning displacement. The search still evaluates the same displacement grid.

For float inputs, allocated picture scratch storage falls from two RGBA clones (63.3 MiB at 1080p) to two luma planes (15.8 MiB), a calculated 75% reduction. This excludes the original inputs, flow fields, allocator overhead and other caches. Non-float inputs still require a temporary RGBA conversion, released after each plane is built. Cancellation is checked during plane construction and between block rows.

**Limits:** the estimate still takes about one second per pair and remains an offline operation. Interpolation and cache hits have not materially changed. Real footage, production workloads, GPU flow and peak process memory were not benchmarked.

### Timeline ranges

The new `--range` benchmark makes 1,000 queries per case over a one-second window near the end of one track of non-overlapping, one-second clips, each with an effect. Each window contains two clips and two returned boundaries. Snapshot construction is outside the timing. Values below are microseconds per query; the generic formatter in the baseline log labels costs above 0.1 ms as ms/frame, but these are query costs.

| Clips | Boundaries before | Boundaries after | CompileRange before | CompileRange after |
|---:|---:|---:|---:|---:|
| 10 | 0.731 | 0.449 | 1.717 | 1.399 |
| 1,000 | 60.985 | 0.888 | 61.260 | 1.777 |
| 10,000 | 621 | 1.047 | 623 | 1.868 |
| 50,000 | 3188 | 1.297 | 3198 | 2.119 |

Clip boundary discovery now uses the snapshot's existing sorted, non-overlapping clips: binary-search for the first clip ending after the window starts, then visit clips until the window ends. No additional retained index or invalidation mechanism is needed. Exact rational comparisons preserve edges below tick resolution. Clip traversal per track is O(log N + K), where K is the number of intersecting clips; sorting/deduplicating returned boundaries is unchanged.

**Limits:** every sequence and track is still visited, and transitions still require a full scan because their intervals are not guaranteed ordered and non-overlapping. The benchmark has one track and no transitions or nested sequences; its approximately 1509x speedup at 50,000 clips is specific to that small-window workload. Playback currently prepares individual future frame plans, so this is an improvement to the public range API rather than a measured monitor frame-rate gain.

### Verification and recovery

[Native test log](perf/2026-10-07-optimization-native-checks.txt): all 12 Debug CTest suites passed (712 passed, one skipped). The inapplicable export-refusal case was skipped because this build can encode. Golden images passed without updates. The two new exhaustive-oracle cases also passed in Release: [Release check log](perf/2026-10-07-optimization-release-checks.txt). The test harness prints registry totals when filtered; only the two named cases were executed in that Release check.

The flow oracle covers 45 combinations of dimensions, texture patterns and search settings, including tiny/partial edge blocks, flat ties, translation, independent noise, negative/HDR float values and 8-bit conversion. It checks exact displacement, confidence and occlusion equality against the pre-optimization search. The boundary oracle checks 349 windows across gaps, muted/disabled clips, retimed clips, multiple sequences, unordered transitions and sub-tick edges against a full scan. Existing CompileRange tests also pass.

Documentation and parity-evidence checks passed. The directory has no Git metadata, so pre-change production sources are preserved in [OpticalFlow.cpp.before](perf/optimization-baseline/OpticalFlow.cpp.before) and [TimelineCompiler.cpp.before](perf/optimization-baseline/TimelineCompiler.cpp.before).

## 2026-10-07 - Completing the software grading floor

Claude's colour work parallelised each pixel pass but left the common one-layer frame moving through redundant full-size buffers. A reused 1080p float layer is about 31.6 MiB: the compositor cleared the layer before overwriting every pixel, cleared the canvas before filling it, then composited the layer into that canvas and flattened it into another frame.

The layer reset now clears dirty rows in parallel. Full-frame pictures and solid generators discard storage immediately before overwriting it, while transformed and partial layers retain the clear required for transparent holes. The opaque canvas discards its old contents before its full fill. When a plan contains one ordinary layer and no transition, blend mode, caption, sequence effect or working-to-display conversion, the processed layer is flattened directly over the configured background without creating the second float canvas.

Release benchmark on the same 32-thread CPU and AMD Radeon AI PRO R9700, 40 frames per case, 1920x1080 float source to 8-bit output. Decode is excluded. [Baseline](perf/2026-10-07-color-completion-before.txt) and [final result](perf/2026-10-07-color-completion-after-fastpath.txt):

| Case | Before this pass | After | Change |
|---|---:|---:|---:|
| No effect (compositor floor) | 10.6 ms | 7.2 ms | 32% faster |
| Basic grade | 11.4 ms | 9.0 ms | 21% faster |
| 33-point 3D LUT | 14.3 ms | 9.6 ms | 33% faster |
| Colour wheels | 16.5 ms | 11.1 ms | 33% faster |
| Hue curves | 19.3 ms | 14.4 ms | 25% faster |
| HSL secondary | 18.1 ms | 12.8 ms | 29% faster |

The GPU path is unchanged at roughly 3.6-4.2 ms wall time in this run, including upload and read-back. These are averages on one synthetic frame and one machine, not p95 playback deadlines. Multi-layer work still needs the general canvas and does not receive the direct-flatten saving. The result remains memory-bandwidth heavy and production footage has not been profiled.

The render suite includes a regression that reuses one compositor for a full frame, a missing frame and then a transformed inset, proving discarded storage cannot leak stale pixels into a later partial layer. All render cases and unchanged golden images pass.

## 2026-10-08 - Playback scheduling and GPU eligibility

Release audit on the same AMD Radeon AI PRO R9700, using 60 sequential frames from a 3840x2160 H.264 source. [Raw results and limitations](perf/2026-10-08-playback-gpu-audit.txt).

| GPU playback case | Median before | Median after | p95 before | p95 after | Software read-ahead jobs before / after |
|---|---:|---:|---:|---:|---:|
| Hardware decode and GPU compose, read-ahead enabled | 16.17 ms | 10.48 ms | 19.97 ms | 10.91 ms | 469 / 0 |

Software read-ahead no longer decodes media that the hardware decoder already sends directly to the GPU. This removes duplicate CPU and storage work; the result is now close to the same run with read-ahead disabled (10.41 ms median). Publishing a new read-ahead position also moved off the caller: the synthetic lock-contention probe fell from 109.13 ms to 0.0118 ms.

The GPU eligibility check now ignores unsupported effects only when their sampled parameters are neutral, while active instances still cause an explicit software fallback. The colour stack limit increased from eight to sixteen operations, and the compositor can bind eight ordered LUTs instead of one. Hardware parity tests cover a nine-grade stack and two LUTs. Effect registry metadata now agrees with the compositor for the audited built-ins.

These results are from one machine and source and include the current monitor read-back. Active blur, sharpen, vignette, lens distortion, warps, keys, masks, graphics, optical-flow interpolation, and other unsupported spatial effects still use the software compositor. The core, playback, and hardware GPU test suites pass.

### Follow-up full-codebase audit

The audit found and fixed three additional hot-path problems. A failed hardware decoder now releases its device source so CPU read-ahead resumes. Scope processing downsamples 8-bit, 16-bit and float inputs directly into its small working image, reducing the measured 4K waveform worker median from 20.06 ms to 3.24 ms (84%); Session also avoids two redundant full-frame clones during QImage/scope delivery. Finally, mask blending, blur, sharpen and lens correction now use the existing bounded row pool. At 4K, blur fell from 129.3 to 53.9 ms, sharpen from 70.7 to 49.5 ms, and lens correction from 125.2 to 49.6 ms.

The D3D11 compositor also allocates targets on demand. A normal 4K 8-bit/no-transition compositor now starts with a calculated 189.8 MiB of fixed targets instead of 569.5 MiB, saving 379.7 MiB. Float output/staging and transition layers are created only when used.

The editor is not fully optimized. Render-cache stores and hits still copy an entire 4K frame and measured about 3.0-3.5 ms each. Every displayed GPU frame still synchronously reads back to CPU memory before Qt copies it again. An active unsupported effect sends the whole frame to software; measured 4K medians include 111.7 ms rolling shutter, 156.5 ms mesh warp, 402.0 ms Gaussian blur, 445.6 ms glow, 425.4 ms drop shadow and 3253.7 ms noise reduction. Every edit reloads the sequence and broadly clears decode/device state, while export explicitly disables GPU composition and runs render/encode work serially. The remaining order of work is shared-texture monitor presentation, GPU spatial effects plus hybrid per-layer execution, selective edit invalidation, zero-copy render-cache ownership, and a pipelined export path.

After these follow-up changes, all 151 render tests pass; playback passes 46/49 and GPU passes 12/15 on the AMD adapter, with all six skips caused by generated media fixtures missing from this checkout. The offscreen app suite passes 31/32: its pre-existing bundled-look discovery case still finds fewer than eight entries, while the frame-delivery, playback and async-scope cases pass. Full raw measurements and limitations remain in [perf/2026-10-08-playback-gpu-audit.txt](perf/2026-10-08-playback-gpu-audit.txt).

### Re-run of the full suites (2026-10-08, later the same day)

The existing `build/app` binaries were run again with `CUTLINE_FIXTURE_DIR=build/app/fixtures` and `CUTLINE_GOLDEN_DIR=tests/golden` set, which running the executables directly (rather than through CTest) requires: without the fixture variable the media, playback and GPU suites skip the tests that read generated files, and without the golden variable some render cases fail. With both set: core 48/48, store 129/129, timeline 78/78, media 69/69, render 151/151, audio 81/81, playback 49/49, export 35 passed and 1 skipped (`ExportIsRefusedWhenNothingCanWrite`: this build can encode), interchange 31/31, ui 62/62, gpu 15/15, speech 7/7 (the Whisper integration test ran). The application suite was run in the first pass, without the fixture variable: 31 passed and 1 skipped (`FilesAreCopiedExactlyImportedAndGivenAProxy...` reads a generated media file), and the bundled-look discovery case that the entry above reports as failing passed. The binaries are those in the build directory, not rebuilt from the working tree; their timestamps predate the last commit, though every suite's test count equals the count in the source.

## 2026-10-09 - Spatial effects, edit invalidation, ingest and export

The remaining spatial CPU effects now divide disjoint rows over the bounded render pool. Release 4K medians on the same machine improved as follows: rolling shutter 111.7 to 46.8 ms, mesh warp 156.5 to 47.4 ms, Gaussian blur 402.0 to 50.6 ms, directional blur 1375.9 to 124.3 ms, unsharp mask 317.1 to 58.4 ms, glow 445.6 to 66.9 ms, drop shadow 425.4 to 67.2 ms, noise reduction 3253.7 to 322.2 ms, wave warp 116.9 to 49.0 ms, bulge 59.5 to 40.4 ms, chroma key 187.7 to 45.4 ms and luma key 139.9 to 45.6 ms. [Raw results](perf/2026-10-09-effects-parallel-all.txt). Heavy spatial effects remain above a 40 ms 25 fps budget and still need GPU kernels.

Visual-only edits now retain decoded media/device decoders while relink, proxy and media-stream changes still invalidate them. Simultaneous ingest jobs are serialized at the project boundary to avoid revision races and competing full-file I/O. Export uses an isolated full-resolution/original-media engine and a one-packet producer/encoder pipeline; requested delivery dimensions may differ from the sequence and use FFmpeg's cached bicubic scaler. LUT discovery now falls back from failed canonicalization without collapsing distinct `.cube` files.

The complete fixture-backed CTest run passed all 13 targets. With the final GPU additions, the current registry contains 791 individual passing cases plus one inapplicable export-refusal skip: 760 native cases across 12 suites and 32 offscreen Qt application cases. The export suite was rebuilt and rerun after delivery resizing was added; the application suite was rebuilt and rerun after ingest serialization. See [the full audit and remaining limits](OPTIMIZATION_AUDIT_2026-10-09.md).

## 2026-10-09 - Direct Qt presentation, GPU spatial effects and shared cache frames

The monitor can now present a D3D11 compositor texture directly through the Qt Quick scene graph. Qt and playback share the renderer's D3D11 device, and a small leased texture pool prevents the compositor from overwriting a texture still referenced by a scene-graph node. Ordinary supported frames skip the staging copy, synchronous GPU read-back, CPU `VideoFrame`, and QImage upload. The CPU path remains available for software Qt rendering, unsupported effects, nested-sequence plans and the periodic sample needed by visible scopes.

In the GPU suite's 1920x1080 synthetic three-layer case with motion and grade, the same AMD Radeon AI PRO R9700 rendered a frame in 11.1 ms through the read-back path and 1.3 ms as a direct texture. That is about 8.5x faster for this render call; direct presentation reported zero read-back time. The CPU reference took 82.4 ms. These Debug-build figures are a focused compositor benchmark on one adapter, not an end-to-end playback p95.

D3D11 compute passes now cover box blur, Gaussian blur, directional blur, sharpen, vignette, lens/wide-angle correction, rolling shutter, wave warp and bulge. Posterize joins the fused GPU colour stack. Mixed chains preserve effect order by flushing colour-operation groups around spatial passes. CPU/GPU comparisons pass for every added effect and for a motion -> grade -> blur -> grade chain. Mesh warp, unsharp mask, glow, drop shadow, noise reduction and chroma/luma-key cleanup remain on the parallel CPU path.

`VideoFrame` now shares immutable pixel backing. Cache stores, cache hits and same-format conversions are O(1) snapshots until a caller requests writable pixels, at which point only that frame detaches. This removes the earlier measured 3.0-3.5 ms 4K ownership copy from normal memory-cache store/hit operations while preserving independent metadata and pixel mutation.

Final sequential verification rebuilt the changed targets and passed all 13 CTest targets in 391.45 seconds: 791 individual cases passed, with the same one inapplicable export-refusal case skipped. The GPU suite is 18/18. See [the optimization audit](OPTIMIZATION_AUDIT_2026-10-09.md) for the complete function-by-function table and remaining platform/effect boundaries.
