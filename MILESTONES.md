# Milestones

Evidence for each milestone in [BUILD_PROMPT.md](BUILD_PROMPT.md). Source code and executed tests are the authority; a field, command payload, button or named test is not, on its own, a finished feature. Each milestone separates four things:

- **Model** — the data and rules exist.
- **Runtime** — the native code acts on them, verified by a test that would fail without it.
- **UI** — an application surface drives it.
- **Workflow** — a user task is validated end to end.

Findings and their status are in [REMEDIATION.md](REMEDIATION.md); measurements are in [PERFORMANCE_LOG.md](PERFORMANCE_LOG.md).

| Milestone | Status |
|---|---|
| 1A Correct editing: split, transitions, locks | **Done** (runtime); linked/grouped edits were completed in later hardening; UI integration is unbuilt here |
| 1B Demux/decoder ownership, sample-accurate seeking | **Done** (runtime, against small reference fixtures); production-footage decode not measured |
| 1C Nested and retimed audio, export policy | **Done** (runtime); WSOLA time-stretch and limiter were completed in later audio work |
| 2 Safe, responsive playback | **Ownership, transport safety, bounded decode pool and read-ahead done**; render thread and production-footage latency gate remain |
| 3 Usable native editing workflow (Qt) | **Partial in source**: shell, workspaces, timeline/monitor items, transport, inspector, jobs and LUT browser exist; the Qt 6.8.3 desktop application builds and its offscreen application tests pass (see IMPLEMENTATION_STATUS.md); workflow validation by a person on a display and with production footage remains |
| 4 Color/render semantics, GPU backend | **CPU semantics implemented**: colour management, filters, blend modes, scopes, warps and optical flow; **Direct3D 11 GPU compositor and D3D11VA decode implemented for the common editing subset** (2026-10-07), verified against the CPU reference on one AMD adapter |
| 5 Professional production features | **Engine partially implemented**: audio finishing, keying, tracking/stabilisation, captions, interchange, graphics/templates, multicam, proxies and cache; product UI and production-footage validation remain |
| 6 Differentiating workflows | **Partly started**: offline Whisper transcription, text-based editing, filler and pause removal and transcript-to-caption creation exist; semantic search, scene detection, auto reframe, translation and generative providers do not |
| 2P Local proxy workflow | **Done in the engine**; UI and production 4K/8K validation remain |

---

## 2P — Local proxy workflow

The proxy path is fully local. `ProxyJob` runs generation on a background thread through the bundled media source/writer providers, reports progress and supports cancellation. Schema v9 stores one replaceable proxy association per media item with both proxy and source fingerprints. A replaced original makes the association stale; a missing or stale proxy falls back to the original. Attach, detach and regenerate associations are journalled and undoable.

The monitor may prefer proxies. Export detects that preference and renders through an independent originals-only engine, so preview resolution cannot leak into delivery. `LocalProxyGenerationResizesAndCanBeCancelled`, `ProxiesAreUndoableAndBecomeStaleWhenSourceIdentityChanges`, and `TheMonitorPrefersAProxyAndFallsBackToTheOriginal` cover generation, persistence, invalidation, selection, fallback and the originals-only clone.

This milestone does not claim a performance win yet. It has not been benchmarked on production 4K/8K footage and has no UI for presets, progress, failure or proxy/original toggling.

---

## 1A — Correct editing: split, transitions, locks

### Scope

1. Preserve playback when splitting reversed clips.
2. Preserve animation when splitting keyframed clips.
3. Maintain transition invariants during affected edits (move, trim, speed, split, ripple delete, timing changes).
4. Apply track-lock policy consistently.

Linked A/V: preserved where implemented (the link id is copied by a split). **Grouped edits are not implemented**, and this milestone does not claim them.

### Status by layer

| Item | Model | Runtime | UI | Validated workflow |
|---|---|---|---|---|
| Split of reversed/retimed clips | yes | **yes** — exact source-time equality across 7 rate/direction cases × 4 cuts | shell source exists but is unbuilt in this environment | no |
| Split of keyframed clips | yes | **yes** — all 6 interpolation modes, multi-component, retimed, repeated, undone | none | no |
| Transition invariants | yes — one SQL rule | **yes** — enforced on create, retime, and after move/trim/speed/split/ripple; checked on open by `ValidateDatabase()` | none | no |
| Track-lock policy | yes — documented block in `ProjectStore.cpp` | **yes** — 23-operation matrix, each refused and then shown to succeed when unlocked | none (no lock control exists in a native UI) | no |
| Linked/grouped edits | link id stored | **no** (implemented later, in the 1C hardening: see below) | none | no |

"Validated workflow" is *no* throughout because no editor UI can be built here; what is validated is the command service, which the UI will call.

### Baseline, reproduction, regressions

Before changing code, each defect was reproduced by a test written first and run against the old behaviour:

- Timeline suite: 6 failures, including a reversed split reading source 126/25 where 25 was expected, a ramp restarting from 0.9 to 0.1 at the cut, and ripple delete leaving a centre-aligned fade behind.
- Store suite: 12 failures, including the transition cases and the lock matrix, which stopped at `SetClipEnabled`.

Regression coverage added (all fail on the old behaviour unless noted):

- `cutline_core_tests` (+4): `SplitKeyframes*` — curve reproduced on both sides, boundary keys and rebasing, empty/unsorted input, eased segments becoming explicit Bezier handles.
- `cutline_timeline_tests` (+7 here): rate/direction matrix, reversed halves, every interpolation, multi-component retimed, double split, ripple-delete transitions, undo exactness.
- `cutline_store_tests` (+17): transition adjacency/coverage/alignment/timing/overlap, split re-attachment (both directions), move and cross-track detach with undo, trims (joined edge, far edge, slip), speed, `ValidateDatabase` detection, ripple shift; the lock matrix and its exemptions; locked-destination moves; and `ARandomSessionOfEditsNeverLeavesTheProjectInvalidAndUndoesCompletely` — a fixed-seed 600-step random session in which every accepted command leaves the database valid, every refused one changes nothing, and undoing everything restores the starting state exactly.
- **Mutation check:** disabling `DetachBrokenTransitions` fails five tests, including the random session, so the tests are sensitive to the behaviour they claim to guard.

### Executed checks (Windows 11, MSVC / VS 18, Debug test tree, Release benchmark tree)

- Full native suite: **302 tests across 8 suites; 301 pass, 1 skipped** (`ExportIsRefusedWhenNothingCanWrite`, whose premise — a build that cannot encode — does not hold on the FFmpeg build; it is skipped, not passed).
- Golden-frame comparisons unchanged by the refactor of the Bezier solver.
- `node scripts/cross-check-parity.js` — 42 capabilities implemented with existing named tests; `extract` downgraded to partial. `node scripts/check-docs.js` — test counts, links and paths.
- Release benchmark (`scripts\bench.bat`): see the 1A entry in the performance log. Store-only edit latency: in-memory medians 0.06–0.34 ms (p99 ≤ 0.41 ms); on-disk medians 0.38–0.90 ms (p99 ≤ 4.25 ms). The added invariant checks cost 0.01–0.06 ms per edit.

### Policy decisions this milestone fixed (behaviour changes)

- A transition now must join *adjacent* clips, *cover the cut*, and *match its alignment*. Previously only "same track" was required, so a persisted project could hold transitions the new rule rejects; `ValidateDatabase()` reports them. One test fixture's incoherent fade-in was corrected.
- An edit that moves the cut a transition joins removes that transition within the same undoable command, rather than refusing the edit or leaving it dangling.
- A locked track protects clips, transitions, the effects and keyframes on either, and its own removal. It deliberately does not block changing the lock itself, markers, or sequence-owned effects.

### Limitations and skipped paths (current status after later work)

- **Head-trim animation anchoring (open, needs a product decision).** Trimming a clip's head keeps its source frame at the same time but moves the keyframe origin, so a ramp reads 0.3 where it read 0.5. Confirmed by a throwaway probe; no regression test, because the intended behaviour is undecided.
- **Grouped/linked edits are missing.** A split of a linked pair leaves all four clips sharing one link id; locks do not propagate across links.
- **No handle validation** for transitions; the compiler clamps each side at its clip's edge. `transition_alignment` stays partial and `transition_handle_diagnostics` planned.
- **`extract`/range operations** across tracks do not exist.
- **No razor/split capability** in the parity manifests; the evidence is filed under the capabilities it protects.
- The edit-latency benchmark is store-only, on one machine, and says nothing about UI feedback, decode or playback. Its on-disk storage class was not established. Run-to-run spread is up to ~2× at the tail.
- No UI, playback, or export path was exercised by new tests in this milestone beyond the existing suite.

### Next dependency

*(Completed in 1B, below.)* **1B — demux/decoder ownership and sample-accurate seeking.** It is independent of 1A's store work and is the prerequisite for 1C (retimed/nested audio needs sample-accurate reads), for playback ownership (2), and for any honest scrub or playback measurement. One decision is wanted from the product owner before it can be closed: the head-trim animation anchoring above. Milestone 3 cannot start until **Qt 6.5+ (Quick, Quick Controls) is installed**; that is the single missing prerequisite for any native UI work, and a headless build cannot validate the editor application.

---

## 1B — Demux and decode: ownership, accuracy, timestamps

### Scope

Fix demux/decoder ownership and discarded audio/video packets. Implement sample-accurate seeking, stream-start normalisation, resampler-delay handling, independent consuming reader state, and safe timestamp indexing. Validate real-file alternating A/V reads against independent references.

### Status by layer

| Item | Model | Runtime | UI | Validated workflow |
|---|---|---|---|---|
| One demuxer and decoder per stream (no discarded packets) | n/a | **yes** — alternating, random-order and two-source tests against references | none | no |
| Sample-accurate audio seeking | n/a | **yes** — any start sample, any order, to 1e-6 (same rate) and 1e-4 (resampled), PCM and AAC | none | no |
| Stream-start normalisation | n/a | **yes** — late-starting audio, container clock starting at 1.4 s | none | no |
| Resampler delay and tail | n/a | **yes** — aligned lead-in, flushed at end of stream | none | no |
| Independent reader state | n/a | **yes** for audio vs video on one `Source`; **not** concurrent (a `Source` is still single-threaded) | none | no |
| Timestamp indexing | n/a | **yes** on a separate demuxer, normalised, with reasons for failure — the failure paths are **untested** | none | no |
| Decode of production footage (long-GOP, 4K, HEVC, hardware) | n/a | **not measured** | none | no |

### How it was validated

The references are independent of the code under test and were generated by the `ffmpeg` binary, not by Cutline's encoder:

- **A video whose every frame states its own index** (red = N mod 256, green = N / 256, lossless), so a decoded frame says which frame it is.
- **An audio chirp with a closed-form value**, `0.5 sin(2π(200t + 1500t²))` left and `0.4 sin(2π(300t + 900t²))` right, stored as 32-bit float. A chirp never repeats, so a read that is a single sample early or late, or that came from the wrong place, cannot match the formula. At 12 kHz a half-sample error is about 0.4 in amplitude, against a tolerance of 1e-4.
- Containers and shapes: Matroska (millisecond time base) at 48 kHz and 44.1 kHz; WAV; AAC in MP4 with encoder priming; H.264 + AAC interleaved; audio starting 0.5 s after the picture; both streams starting at 1.4 s; a 0.1 s hole in the audio's timestamps.

14 reference tests were written first and run against the old code: 12 failed (output in `perf/2026-10-06-milestone-1b-baseline-failures.txt`), including audio wrong by almost the full signal amplitude after an alternating video read, the first frame reporting a presentation time other than 0, and AAC placed 2 samples out. The two that passed (frame identity, sequential audio) are the cases that already worked. Three more were added once the design was settled (hole, tail, coarse-time-base resampling).

**Mutation check.** Seven mutations of the new reader, one at a time, each build-and-test: no resampler flush, no lead-in, no discard of lead-in samples, container clock instead of earliest timestamp, no packet index, no alignment unit, no gap handling. **Every one is caught by at least one test** (the least-covered by one; the packet index by eight). An earlier version of the suite let two of them through, which is why the tail test now checks the last 48 samples sample by sample and why a Matroska 44.1 kHz fixture exists.

### Executed checks

Windows 11, MSVC / VS 18, Debug test tree; Release benchmark tree.

- Full native suite: **319 tests across 8 suites; 318 pass, 1 skipped** (`ExportIsRefusedWhenNothingCanWrite`, inapplicable on an FFmpeg build). Media suite 43 → 60. Goldens and all earlier suites unchanged.
- `node scripts/cross-check-parity.js` (42/42 claims name existing tests; five decode capabilities gained tests, none changed status), `node scripts/check-docs.js`, `node scripts/generate-parity-report.js`.
- Release benchmark `scripts\bench.bat 40`: seek and read latency on the fixtures (see the 1B entry in the performance log). Audio seeks including the discarded lead-in: median 0.02–0.05 ms, p99 ≤ 0.2 ms on 4 s fixtures; 0.25 ms median, 1.2 ms p99 on a one-hour PCM file, plus a one-off 99 ms packet index on its first seek. Random H.264 seek at 320x180: median 1.1 ms.

### Limitations and skipped paths

- **Fixture scale.** Everything is small and warm-cache. Nothing here measures production-resolution video, long-GOP seeking, HEVC or hardware decode, or a cold disk. The product targets in BUILD_PROMPT.md (scrub-to-correct-frame p95) are untouched until milestone 2.
- **Coarse time bases cost an index and limit gap precision.** The first seek into a Matroska file scans its packets once (99 ms for an hour). A timestamp *discontinuity* in such a stream can only be placed to the container's resolution (here 16 samples out of 4800).
- **Probe duration** still reports the container's figure, which can overstate content (5.4 s for 4 s in the 1.4 s-offset fixture). `ReadVideo` is correct past the last frame regardless; the stored duration is not.
- **Untested paths:** the timestamp map's "no timestamps" and "duplicate timestamps" reports; a stream without a sample rate; seek failure fallbacks; files with an unspecified channel layout.
- **Not concurrent.** A `Source` is single-threaded. Audio and video are independent of each other, not safe to read from two threads at once.
- **Cadence** on millisecond time bases is approximate (unchanged).
- **No UI, playback or export path changed**; the existing playback and export suites pass against the new reader.

### Next dependency

**1C — nested audio with correct effect scope and time mapping, continuous retiming and reverse sample mapping, an explicit pitch policy, per-sample automation and fades** (REMEDIATION findings 8 and 9). It needs exactly what 1B delivered: reads at any sample, at any rate, in any order, with exact placement. Reverse playback in particular needs a block read as one forward read and then reversed, which this reader can serve (reads are independent and exact) but which has not been tried. Decision wanted from the product owner for 1C: the **pitch policy** for retimed audio (resample and shift pitch, as a razor-speed change does; or time-stretch and hold pitch), which the brief asks to be explicit.

---

## 1C — Audio: nested, retimed, automated; and the hardening that followed

This section covers the next milestone in BUILD_PROMPT.md and, at the request "optimise these issues and fix all the bugs", the open findings that did not depend on a user interface: the 1A and 1B leftovers, playback ownership and transport safety (milestone 2's correctness half), atomic ingest and relink, persistence, and reporting. What is **not** done is stated at the end.

### Scope and status by layer

| Item | Model | Runtime | UI | Validated workflow |
|---|---|---|---|---|
| Nested audio, with its own effect scope and time mapping | yes | **yes** — synthetic ramps and a real file through the engine | none | no |
| Continuous retiming and reversal of audio | yes | **yes** — exact to rounding against closed-form values | none | no |
| Pitch policy | **varispeed**, stated | yes; time-stretch **not implemented** | none | no |
| Per-sample automation, fades, crossfades with handles | yes | **yes** | none | no |
| Export audio follows the same policy | n/a | **yes** — sample-counted blocks, same mixer | none | export suite passes |
| Head-trim animation anchoring | yes | **yes** | none | no |
| Linked (grouped) edits | yes (`LinkClips`, `UnlinkClips`, `propagate_links`) | **yes** | none | no |
| Probe duration, cadence on millisecond time bases, VFR frame duration | n/a | **yes** | none | no |
| Playback ownership and threading, seek/flush, end of stream, failure reporting, device rate | n/a | **yes** — device path exercised on this machine's WASAPI device | none | no |
| Atomic ingest, relink | yes | **yes** | none | no |
| Persistence after commit; retention; durability choice | yes | **yes** (failures simulated, not a real full disk) | none | no |
| Parity reporting that executes; strict skips; toolchain discovery | n/a | **yes** | n/a | n/a |

### How it was validated

- **Audio against values that say which sample they are.** A ramp whose value at source sample *s* is *s* × 10⁻⁶ (linear, so a cubic read of it is exact) lets a test state the source position every output sample must have read: copies are compared exactly, retimed and reversed reads to rounding error, and transition handles by which side of the cut a value came from. On a real file the closed-form chirp from 1B is used, through the decoder, the mixer and the engine.
- **Block-size independence.** A deliberately awkward project (retimed and reversed clips, a crossfade with handles, a retimed nested sequence, animated gain, track and sequence effects) is rendered whole and in blocks of 317, 1000, 1, 4999, 48000… samples; the results are **bit-identical**. The same is checked on a real file through the engine.
- **Mutation checks.** Twelve mutations of the mixer, one at a time (rate or direction ignored, nested silent, automation once per block, no handles, equal-power as linear, clamp, interpolation linear, sequence or track effects dropped…): ten are caught by one to four tests each. Two survive and are explained: scanning every clip instead of the window changes cost, not output (so it is shown by the benchmark and a read-count test); and a guard in the transition weight loop (the outgoing clip silent after its transition) is unreachable for any transition the store accepts. Three mutations of linked editing (move ignores partners, split leaves one group, trim ignores the tail) and the head-trim shift are each caught.
- **Threads.** There is no thread sanitizer on this toolchain, so concurrency is argued by construction and exercised, not proved: one test runs audio, picture and edits/seeks on three threads and must complete with a valid cursor; the seek race is made deterministic by seeking from inside a render; the device handshake runs on the real device (flush, end of stream, callback failure).
- **Persistence failures are simulated** by blocking the journal and snapshot directories; a real disk-full and a power cut were not exercised.

### Executed checks (Windows 11, MSVC / VS 18, Debug test tree; Release benchmark tree)

- Full native suite: **376 tests across 8 suites; 375 pass, 1 skipped** (`ExportIsRefusedWhenNothingCanWrite`, inapplicable on an FFmpeg build). The suites went from 319 to 376: store 76 → 93, timeline 49 → 53, media 60 → 64, audio 21 → 40, playback 22 → 35. Run repeatedly (the playback suite six times in a row) without a flake.
- `CUTLINE_STRICT=1`: the same suites pass with every prerequisite present, and the media suite fails 33 tests when the fixtures are hidden, which is the point.
- `node scripts/cross-check-parity.js --execute`: **42/42** capabilities claiming implementation have every named test *passing in the run* (365 executed at the time), not merely existing.
- `scripts\build.bat`, `test.bat`, `bench.bat` locate Visual Studio through `vswhere`.
- Release benchmark: see the 1C entry in the performance log.

### Limitations and skipped paths

- **Audio finishing is implemented in the engine:** WSOLA pitch preservation, buses/sends, metering, EQ, dynamics, limiter, gate, ducking, denoise and de-reverb. The mixer, automation modes and voice-over recording have since been built (UI-AUDIO, AUDIO-004); production speech profiling, input monitoring, control surfaces and plug-in latency compensation remain.
- **Picture transitions** still clamp at the clip edges and the engine does not validate handles (the placement planner keeps a transition within the media's length); the library has since grown to 23 kinds with their own arithmetic.
- **Asynchronous latest-wins monitor presentation now exists**, alongside the bounded decode pool and read-ahead cache. A unified priority scheduler, GPU presentation and production-resolution latency gates remain.
- **Crash recovery replay exists** through journal v2, snapshots, replay and quarantine; the interactive undo stack still does not survive reopening.
- **CPU colour management exists** with working/display transforms, PQ/HLG, tone/gamut mapping, LUTs and scope measurements. OpenColorIO, the ACES output transform,, 10-bit output, HDR monitoring and full UI remain.
- **UI is built but partial.** The Qt/QML application builds with Qt 6.8.3 and is tested offscreen (workspaces, timeline/monitor, transport, inspector, jobs, LUT browsing, speed-ramp editor); professional panels and workflow validation by a person remain.
- **No CI.** The repository is under git (two commits at the time of the 2026-10-08 audit); nothing runs `scripts/check-docs.js` or the parity cross-check automatically.
- **Untested or simulated:** real disk-full and power-cut behaviour; a file with reordered frames and no presentation timestamps; audio devices other than this machine's default.
- **Measured on small fixtures, warm caches, one machine** (see the performance log); not comparative.

### Next dependency

The next engine dependency on the GPU side of milestone 4 is the rest of the shader set (filters, keys, blend modes, masks; LUTs and the colour tools are done), a shared texture with the monitor, production-footage latency/memory gates and picture-transition handle validation. **Milestone 3 (the Qt editor)** now builds (Qt 6.8.3) and has offscreen tests; what blocks calling it done is workflow validation on production footage and the missing professional panels.

---

## 2 — Bounded decode pool and read-ahead (first responsiveness slice)

The playback engine now owns a bounded worker pool. Each worker owns an LRU-bounded set of independent `Source` instances, because FFmpeg seek/decode state cannot be shared by two video readers. A scrub `Seek` cancels obsolete queued work by generation and primes the requested frame; a completed render queues the following eight frames by default. Workers publish only raw decoded frames into the byte-bounded LRU, under a short cache lock. Composition never waits for a worker decoder, and falls back to the existing synchronous read when a requested frame is not ready.

The cache key is now media id plus source time, so two clips using the same media frame share one decode. Relinks and edits advance the generation and clear the cache. `EngineStatistics` reports queueing, completions, read-ahead consumption, queue saturation, and total/max render time.

Validation added three playback cases: `PrimeReadAheadWarmsTheRequestedScrubFrame`, `RenderingQueuesTheFollowingFramesForReadAhead`, and `DuplicateClipsShareOneDecodedMediaFrame`. The full native suite is now **379 tests across 8 suites; 378 pass and 1 is an expected inapplicable skip**. The Release benchmark has a focused `scripts\bench.bat --playback` mode. On this machine, 80 random seeks through the bundled H.264 320×180, GOP-30 fixture reached the raw-frame cache at **1.139 ms median / 1.944 ms p95**, then composed from cache at **0.357 ms median / 0.426 ms p95**.

This is evidence that the queue and cache path work; it is not the product's 150 ms scrub claim. The fixture is four seconds long, 320×180, warm-cache, software-decoded, and has no UI upload or presentation. The next playback evidence must use declared 1080p and 4K codecs/GOPs, cold and warm storage, concurrent audio, proxy policy, and visible presentation. A render thread or asynchronous frame-delivery API is still required so the UI can remain responsive while a synchronous fallback is slow.


