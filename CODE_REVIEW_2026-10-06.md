# Cutline implementation and optimization review

Reviewed on 6 October 2026. This assessment follows the first-party implementation, including its native build, command/store layer, timeline compiler/loader, media providers, compositor, audio sinks/mixer, playback, exporter, tests, and browser/MCP prototype. Architecture documents and parity manifests were cross-checked against runtime behavior. Bundled third-party source was not audited line by line.

## Assessment

Cutline has a useful native editing-engine foundation, but it is not yet a usable native video editing application or a Premiere-equivalent product. Preserve the exact-time model, typed commands, transactional store, changeset undo, provider boundaries, and deterministic media fixtures. The immediate blockers are incorrect editing/audio/export behavior, unsafe ownership across playback threads, a missing native editor UI, and a CPU render path that cannot sustain modest multilayer playback.

Do not use the number of classes, passing tests, or tracked capabilities as a completion percentage. Several capabilities recorded as implemented only work for restricted cases.

## What actually runs

The Qt application in [app/main.cpp](app/main.cpp:12) initializes an in-memory store and opens [Main.qml](app/qml/Main.qml:14), which displays explanatory labels. It exposes no editing models or playback engine to QML. The console demo exercises the native engine. The browser interface, [app.js](app.js:1) and [mcp-server.js](mcp-server.js:19), uses a separate JSON project and separate command implementations. It has no bridge to the native SQLite project, playback engine, or exporter.

The native runtime flow is:

```text
CommandEnvelope -> CommandService -> ProjectStore -> SQLite transaction + changeset journal
                                                       |
                                              SequenceLoader snapshot
                                                       |
                        PlaybackEngine -> TimelineCompiler -> PlaybackPlan
                              |                    |
                   SourceRegistry / FFmpeg   Software compositor / AudioMixer
                              |                    |
                         decoded RGBA       frame returned to caller / audio sink

Export() reuses that engine synchronously -> WriterRegistry -> FFmpeg encode/mux
```

There is no native background-job service, GPU render graph, hardware-surface type, presentation scheduler, or process boundary around decoding/export. `ExportWorker` is a synchronous function, despite its name. The Node job scheduler is not connected to the native runtime.

## Verification performed

- `scripts\test.bat`: eight native suites pass. The case-level result is **255 passed out of 256 registered cases, with one expected skip**: the no-encoder refusal case is inapplicable to this FFmpeg-enabled build.
- `node tests/run.js`: four JavaScript test files pass.
- `node scripts/cross-check-parity.js`: passes; 43 capability claims name existing tests.
- `node scripts/check-docs.js`: passes.
- `scripts\bench.bat 12`: builds/runs the Release render microbenchmark at 1920x1080.
- Additional native diagnostic probes reproduce the failures below, using generated fixtures and synthetic sources.

The native test build is Debug with FFmpeg enabled and the Qt application disabled. Tests do not establish safe concurrent playback or production WASAPI behavior. Performance measurements are short local microbenchmarks, not product throughput promises or a hardware comparison with Premiere.

Probe sources and results are retained under [build/audit](build/audit): [review_probe.cpp](build/audit/review_probe.cpp), [probe-results.txt](build/audit/probe-results.txt), [compile_bench.cpp](build/audit/compile_bench.cpp), and [compile-results.txt](build/audit/compile-results.txt). Reproduce with `build\audit\build_probe.bat` and `build\audit\build_compile_bench.bat` from the workspace root. The scripts use this machine's existing Visual Studio installation and build libraries. Probes are diagnostics, not additions to the production test suite.

## Reproduced correctness problems

P1 means fix before relying on the affected workflow. P2 means fix before presenting the workflow as complete. These priorities describe this review, not the roadmap manifest's numbering.

| Priority | Finding and evidence | Recommended correction |
|---|---|---|
| P1 | **Ordinary long timelines overflow during tick conversion.** [RationalTime.cpp:62](core/time/RationalTime.cpp:62) multiplies the numerator by the large tick rate before dividing by the denominator. `FromFrames(108001, {30000,1001}).ToTicks()` at approximately one hour throws **“RationalTime multiplication overflow”**, although the resulting tick count fits easily in 64 bits. The compiler and edit paths use this conversion. | Cancel common factors before multiplication, or use a portable wider intermediate with checked final conversion and exact rounding. Audit `Rescale`/`ToFrames` for the same unnecessary intermediate overflow. Add frame-aligned long-form timeline tests across all supported rates, including adjacent frames near hour boundaries. |
| P1 | **Stereo export overwrites previously buffered channels.** [FFmpegWriter.cpp:186](media/ffmpeg/FFmpegWriter.cpp:186) grows a packed planar buffer, then copies channels in increasing order. Writing channel 0's new tail overwrites channel 1's old data before it is moved. Two 600-sample blocks with deliberately different left/right signals produce a first right-channel sample of **0.200012 instead of 0.6**. | Use per-channel queues, an audio FIFO, or a separate correctly sized destination. Avoid in-place growth with overlapping channel regions. Test distinct signals in stereo and surround, including partial encoder frames. |
| P1 | **Alternating real-file audio/video reads corrupt the decoded audio timeline.** [FFmpegSource.cpp:424](media/ffmpeg/FFmpegSource.cpp:424) and [FFmpegSource.cpp:554](media/ffmpeg/FFmpegSource.cpp:554) read one shared demux context and discard packets belonging to the other stream. Each stream can also seek that context without coherently invalidating the other decoder. Alternating reads from `av-sync.mkv` differ from an audio-only reference in **34 of 50 blocks**, with worst mean absolute sample error **0.151746**. | Give demux state one owner and route packets into bounded per-stream queues; alternatively use independent reader contexts initially. Separate readers are also needed for independent instances of the same media at different source positions. |
| P1 | **Audio seeking is not sample accurate.** [FFmpegSource.cpp:225](media/ffmpeg/FFmpegSource.cpp:225) seeks backward but never discards samples before the requested point. `pending_audio_start_` is not populated from decoded timestamps. Seeking to sample 12,345 produces mean absolute error **0.0881173** against a decode-from-start reference. | Track decoded PTS, stream start offsets, and resampler delay. Decode through the seek point, discard exact leading samples, preserve the remaining samples, and drain the resampler at EOF. Test non-packet-aligned seeks against known PCM data. |
| P1 | **Reverse playback changes after a razor cut.** [ProjectStore.cpp:672](core/project/ProjectStore.cpp:672) uses the forward source mapping for every split. A reversed 0-10 second source split at timeline 4 seconds changes the source selected at timeline 1 second from **9 seconds to 3 seconds**. | Partition source ranges according to playback direction. Assert that rendering before and after splitting is identical over both halves, including different playback rates and the first/last source frames. |
| P1 | **A razor cut restarts the right half's animation.** [ProjectStore.cpp:754](core/project/ProjectStore.cpp:754) copies keyframe times unchanged, while the compiler samples relative to each clip's new start. A 0-to-1 opacity ramp evaluates to **0.5 before the split and 0.1 afterward** at timeline 5 seconds. | Preserve an animation origin or rebase the copied curve by the split offset. Preserve interpolation across the boundary, including Bezier handles; copying keys and adding a boundary key naïvely can change the curve. |
| P1 | **Selecting a different export audio rate changes the signal.** [ExportWorker.cpp:89](exporter/ExportWorker.cpp:89) computes the sample count using the requested rate, but `RenderAudio` uses the engine's fixed rate. [FFmpegWriter.cpp:176](media/ffmpeg/FFmpegWriter.cpp:176) checks channels but does not validate the input sample rate. Exporting a 1 kHz tone from the default engine at 44.1 kHz measures about **926 Hz** over the probe window. | Render/export with an explicit output audio format, or correctly resample from the engine rate. Validate rate, layout, and sample count at every boundary. Apply the same negotiation to playback devices whose mix rate differs from 48 kHz. |
| P1 | **Nested-sequence audio is silent.** [AudioMixer.cpp:106](audio/AudioMixer.cpp:106) drops requests below depth zero, while [PlaybackEngine.cpp:182](playback/PlaybackEngine.cpp:182) refuses sequence-kind audio requests. The nested-audio probe returns peak **0** for an audible source. | Recursively render the nested audio bus, apply its sequence effects there, then apply the wrapper clip/track processing. Map boundaries through the nesting offset, speed, and direction. |
| P2 | **Audio does not implement clip retiming/reversal.** [PlaybackEngine.cpp:186](playback/PlaybackEngine.cpp:186) reads a forward block at the mapped start time, ignoring the requested rate/direction within the block. A 2x clip still yields roughly a 1 kHz tone rather than simple 2x sample playback; no pitch-preserving time-stretch implementation exists. | Implement a continuous sample-time mapping and reverse reads; define whether rate changes preserve pitch, then implement that policy. Verify time-varying envelopes and impulses, not only periodic tones or mapped block start times. |
| P2 | **Export applies mute/solo policy differently to picture and sound.** [ExportWorker.cpp:76](exporter/ExportWorker.cpp:76) supplies export options to `RenderFrame`, but calls `RenderAudio` without them. With `honour_mute_and_solo=false`, a muted audio track exports silence, contrary to the API's stated policy. | Pass one explicit render context to both paths. Make export policy visible and deliberate; preserve monitor/delivery equivalence for the policy the user chose. |
| P2 | **Export resizing is accepted by settings but cannot render.** [ExportWorker.cpp:24](exporter/ExportWorker.cpp:24) accepts a different output size, while the shared engine still produces its configured sequence size. Exporting a 64x36 sequence at 32x18 throws **“Frame size does not match the export settings.”** | Build a dedicated export render context with requested dimensions, quality, rate, pixel aspect, color transform, and audio format. Do not mutate the active monitor's context to achieve this. |

## Architectural and implementation risks found by inspection

These findings follow the code directly; they were not verified by a hardware stress test or fault-injection run.

### Playback ownership and transport need redesign

[PlaybackEngine.cpp:216](playback/PlaybackEngine.cpp:216) calls `RenderAudio` on the sink's producer thread, while a monitor caller can render video and an editor can call `UpdateSequence`. Those paths share `sources_`, offline state, statistics, graph state, and the FFmpeg reader. The transport mutex protects cursor access, not those shared objects. Concurrent operations can cause C++ data races and simultaneous access to FFmpeg packet/frame/format objects. Adding one large engine mutex would serialize work and increase stalls.

Use explicit ownership instead: a session actor publishes immutable sequence snapshots; decoder workers own their contexts; audio receives prepared blocks; video receives immutable frame handles; each render context owns its compositor workspace. Send commands between owners and expose statistics through atomics or snapshots. Give export its own frozen project revision and readers.

[PlaybackEngine.cpp:251](playback/PlaybackEngine.cpp:251) changes the cursor and resets the clock without flushing the WASAPI ring or device queue. Old sound can remain buffered after the visible playhead jumps. An in-flight render callback can also overwrite the new cursor when it stores its old `advanced` position. Use a transport generation number: reject old blocks/results, flush queues safely, re-prime, and restart clock mapping after a seek.

The WASAPI producer [WasapiSink.cpp:237](audio/WasapiSink.cpp:237) sets `exhausted_` when the callback returns false but continues producing. The device loop continues too, and `PlaybackEngine::playing_` is not updated on exhaustion. The offline test breaks its own `RenderBlocks` loop and does not prove the device transport stops. Device start/loop errors also fail to publish a stopped/error state; exceptions escaping the producer thread can terminate the application. Implement an explicit transport/device state machine, queue draining at EOF, startup priming, and exception-to-error reporting.

### Float pixels do not establish correct color management

[VideoFrame.cpp:19](media/VideoFrame.cpp:19) converts integer samples to float by division. It does not linearize the transfer function. The compositor blends and grades those values, clamps grades to 0-1 at [Compositor.cpp:316](render/Compositor.cpp:316), and does not consume the sequence's working/display color spaces. It also defaults to 8-bit output. If configured for float output, [FFmpegWriter.cpp:146](media/ffmpeg/FFmpegWriter.cpp:146) converts that to 8-bit before encoding.

Consequences include transfer-encoded blending, incorrect mixed-space rendering, loss of HDR headroom, and an 8-bit bottleneck even when a high-bit-depth delivery format is requested. Implement defined input transforms, working-space processing, display/output transforms, metadata propagation, and precision-aware encoding. Preserve values above 1 where the working representation permits them. Distinguish YUV matrix/range conversion from gamut and transfer-function conversion. Add high-bit-depth ramps, transparent-edge blends, log/HDR fixtures, and independently calculated color references. Verify pixel aspect and interlacing too: storing their metadata does not make the compositor handle them.

### Transitions and effect stacks have restricted semantics

[TimelineCompiler.cpp:183](timeline/TimelineCompiler.cpp:183) clamps transition sides to their clip timeline edges. This holds edge times instead of continuing through available source handles, and can request an exclusive source-out time. Transition kinds/effects are carried by the plan, but the compositor uses the same linear mix for every kind. `TransformFor` and `CropFor` select only the first matching effect; additional transforms and their ordering are ignored. An unknown effect is accepted by the store, and its skip statistics are discarded by the normal engine call.

Define supported effect/transition descriptors, parameter schemas, order, source-handle policy, and diagnostic behavior. Render a real ordered stack; fuse operations only when that preserves semantics. Report unsupported operations before delivery. Validate transition adjacency and handles on creation and after edits.

The transition model is also not maintained consistently by edits: `MoveClip` [ProjectStore.cpp:635](core/project/ProjectStore.cpp:635) does not move or detach its transition; `SetTransitionTiming` [ProjectStore.cpp:880](core/project/ProjectStore.cpp:880) lacks creation's overlap check. Track locking is checked for some mutations but not consistently for effects, enable, and transition changes. Use shared validation helpers and atomic edit operations that maintain affected clips, transitions, animation, markers, and linked groups together.

Audio effects and crossfade weights are sampled once per piece/block. Splitting blocks at edit boundaries does not produce per-sample automation ramps. Add ramp generation and smoothing to avoid stepped gain/fades. The current “limiter” is hard sample clipping, not a look-ahead dynamics processor; implement a deliberate monitoring/delivery policy and a real processor if that feature is promised.

### Media ingest and relink are incomplete transactions/workflows

[Ingest.cpp:115](media/Ingest.cpp:115) imports media and records streams in a second transaction. An intervening edit or failure can leave media imported without stream metadata. A retry can hit fingerprint deduplication and return without repairing the metadata. One undo only removes the second command. Use one composite ingest command/transaction for the item's complete initial state, after expensive probe/hash work completes outside the store lock.

[PlaybackEngine.cpp:194](playback/PlaybackEngine.cpp:194) drops frame caches on edits but preserves opened readers and the offline blacklist. Updating a media path cannot replace an already opened source, and an offline item is never retried. Introduce media identity/version invalidation and explicitly close/reopen affected readers on relink. Validate a relink candidate's streams/duration, with an override workflow when it intentionally differs.

The timestamp index scan [FFmpegSource.cpp:369](media/ffmpeg/FFmpegSource.cpp:369) saves an `avio_tell` byte offset, then passes it to a timestamp seek at line 402 without byte-seek flags. It also moves the shared demuxer while invalidating only video state. Build indexing in a separate reader/worker; seek back using a proper timestamp or avoid disturbing playback at all. Packet indexes should not be presented as universally exact decoded-frame indexes for every codec/container.

### Persistence is a useful foundation, but recovery is unfinished

SQLite content and its changeset journal commit together. However, [ProjectStore.cpp:1297](core/project/ProjectStore.cpp:1297) commits before writing external JSON and periodic snapshots. A subsequent filesystem exception reports failure after the edit has actually succeeded. Distinguish command commit success from auxiliary-write failure and repair those artifacts independently.

Snapshot creation runs `sqlite3_backup_step(..., -1)` while holding the same store mutex. Snapshot files and the durable journal have no retention policy; the undo limit bounds entry count, not total bytes. Add a separate backup connection/worker, incremental backup work, bounded retention, and explicit byte budgets. Use read transactions/connections for snapshot loading to avoid retaining the writer lock across a large load. Keep disk writes out of GUI interactions.

`OpenPackage` validates the database but does not restore journal history or perform application-level snapshot/journal replay. SQLite WAL already handles normal process interruption; missing application replay does not mean every process crash loses the project. The current `synchronous=NORMAL` policy can sacrifice recent commit durability on power loss/hard reset, as described by [SQLite's WAL documentation](https://www.sqlite.org/wal.html). Define durability requirements, package-copy behavior, checkpointing, and tested recovery procedures. Add disk-full, interrupted snapshot, interrupted commit, and migration tests. Avoid deriving filesystem names directly from unconstrained command identifiers.

### Export must protect existing deliveries

[FFmpegWriter.cpp:118](media/ffmpeg/FFmpegWriter.cpp:118) opens the destination directly for writing. Cancellation deletes that path, so an existing delivery can be truncated and then removed. `Finish` sets `finished_` before draining encoders/writing the trailer; a failure afterward prevents destructor cleanup and can leave an invalid file.

Write to a unique sibling temporary with an explicit container, finish/close successfully, then atomically publish according to an explicit overwrite policy. Delete only this job's temporary on cancellation/failure. Set the finished state only after successful completion. Keep jobs, progress, diagnostics, and cancellation state in a native job service.

## Performance findings and recommended changes

### Measured render limit

The existing benchmark reuses a decoded synthetic source frame, so it primarily measures compilation/compositing. It does not include real file decode, disk access, GPU upload, window presentation, or UI responsiveness.

| Release case, 1920x1080 | Measured mean | Equivalent throughput |
|---|---:|---:|
| 1 track, 3 effects, 8-bit source | 37.369 ms/frame | 26.8 fps |
| 2 tracks, 3 effects, 8-bit source | 70.745 ms/frame | 14.1 fps |
| 4 tracks, 3 effects, 8-bit source | 137.352 ms/frame | 7.3 fps |
| 4 tracks, 3 effects, float source | 119.649 ms/frame | 8.4 fps |
| 1 track, 17 effects, float source | 132.270 ms/frame | 7.6 fps |

At 30 fps the complete frame budget is 33.3 ms; at 60 fps it is 16.7 ms. The multilayer software path already exceeds these without the rest of the application. Converting the source to float in advance helps but cannot by itself solve the measured render bottleneck.

### A hidden whole-sequence copy occurs per video frame

[PlaybackEngine.cpp:107](playback/PlaybackEngine.cpp:107) invokes the compiler's single-sequence overload. [TimelineCompiler.cpp:321](timeline/TimelineCompiler.cpp:321) copies the complete sequence into a new graph, including all clips/effects/keyframes. Binary search on the active clip therefore does not make this hot path logarithmic in project size.

A separate Release probe with one track, one active clip, and one effect per clip measured:

| Clips in sequence | Current `Compile(sequence)` | Existing `Compile(graph)` |
|---:|---:|---:|
| 10 | 0.001610 ms | 0.000338 ms |
| 1,000 | 0.187972 ms | 0.000314 ms |
| 10,000 | 2.059970 ms | 0.000343 ms |

Remove the copy through a non-owning sequence/graph view or a graph-aware per-level compilation entry point. Preserve nesting/effect scope when changing this path. The probe isolates this overhead and does not predict whole-project playback speed.

### Highest-value optimization work

1. **Index the immutable snapshot once per revision.** `BoundariesIn` scans every clip and transition for every audio block. Nested boundary times are also used without mapping them into root time. Build sorted/root-mapped boundary indexes and clip/transition/sequence lookup indexes. Pre-sort effect stacks on snapshot creation instead of sorting each sample. Query only the relevant range.
2. **Scope database loads.** [SequenceLoader.cpp:82](timeline/SequenceLoader.cpp:82) and line 97 load all project keyframes/parameters for each sequence, despite scoping the effect query. Nesting repeats this project-wide work. Restrict all queries to the reachable owner/effect set or load shared indexes once per graph revision. Rebuild only changed sequences where safe.
3. **Separate raw decode and rendered-result caches.** Current keys use clip ID plus requested time, duplicating identical media frames across clips and different times inside the same held/VFR frame. Key raw frames by media identity/version, stream, decoded PTS, and decode format. Key render results by content dependencies, effect values, output policy, and time. Keep decoded media through unrelated marker/effect edits; invalidate changed dependencies rather than clearing everything.
4. **Bound all retained resources.** The 256 MiB LRU bounds only one cache. Open decoders, their internal threads/buffers, nested composites, and compositor scratch buffers are outside it. A 4K RGBA float buffer is about 126.6 MiB; after both ordinary and transition workspaces have been used, five such scratch buffers can retain about 632.8 MiB before sources/cache/output. Add session-wide CPU/GPU budgets, decoder eviction, a surface/buffer pool, and memory-pressure behavior. Avoid letting every stream choose unlimited automatic decoder threading.
5. **Build a GPU backend behind the existing semantic interfaces.** Start with one supported Windows path, retain CPU rendering as a reference/fallback, and compare results. Add frame handles that can own CPU pixels or hardware surfaces, with plane format, color metadata, synchronization, and lifetime information. Hardware decode alone is insufficient if every frame is downloaded, converted to RGBA, then uploaded. [FFmpeg's hardware-decode example](https://ffmpeg.org/doxygen/trunk/hw_decode_8c-example.html) is a starting point for provider support; an editor should additionally design surface interoperation and end-to-end ownership.
6. **Keep rendering asynchronous and budgeted.** Use bounded decode/prefetch/presentation queues; prioritize audio and the current frame; cancel obsolete scrub work using generations. Render preview at a declared scale and use proxies for expensive media. Supply completed GPU textures through the native display path, following [Qt Quick scene-graph integration](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html), instead of expensive per-frame image conversion in QML.
7. **Reduce CPU memory traffic where measurements justify it.** Reuse output/converted-frame buffers, cache suitable conversions, tile independent image work, and use SIMD for conversion/blend kernels. Fuse compatible effects only after defining color/effect semantics. An aligned row stride is not an explicitly aligned allocation; use aligned storage if kernels depend on aligned loads. Preserve golden/reference results.
8. **Measure the complete pipeline.** Add decode/seek time, queue delay, render time, present time, audio underruns, CPU/GPU memory, and cache-hit metrics. Record median and p95/p99 with warm/cold conditions. Cover 1080p/4K, 30/60 fps, long GOP, VFR, high bit depth, long timelines, transitions, nested sequences, and concurrent export. The current one-clip-per-track benchmark cannot reveal all scaling problems.

## Missing product capabilities

Adobe's current [Premiere feature overview](https://www.adobe.com/products/premiere/features.html) includes multicam, text-based editing, professional color/audio workflows, graphics, and integrated collaboration. The following assessment is based on the repository's native runtime, not a claim that its manifests exhaustively describe Adobe's product.

| Area | Current native reality | Work required |
|---|---|---|
| Editor UI | Placeholder Qt window | Media/bin models, source/program monitors, virtualized timeline, inspector, keyboard mapping, docks/workspaces, accessible controls, real command binding |
| Editorial | Basic single-clip commands | Linked A/V operations, multiselect/grouped transactions, insert/overwrite with track targeting, range lift/extract, ripple/roll/slip/slide policies, snapping, trim feedback, J/L cuts, multicam |
| Playback/media | Software readers and synchronous frame requests | Robust stream selection, proxies, thumbnails/waveforms, relink workflow, scrub/prefetch, hardware decode, presentation deadlines, tested format matrix |
| Color/effects | Small built-in stack and basic grade | Managed color pipeline, curves/LUTs/scopes, masks/tracking/keying, more transitions, ordered processing, high-bit-depth/HDR display and delivery |
| Audio | Gain/pan/summing and basic fades | Sample-accurate automation/retime, routing/buses, EQ/dynamics, loudness/true peak, meters, surround handling, recording/device controls, optional plugin host |
| Graphics/captions | No native title/caption renderer or command model | Text shaping/fonts, graphics objects and animation, caption text/timing/style tracks, import/export, accessibility QC |
| Advanced workflows | No native implementation | Transcription/text editing, intelligent analysis, interchange, review/collaboration, plugins, broader camera formats, VR/pro I/O where required |
| Delivery/reliability | Synchronous export and SQLite foundation | Native queue/presets, format negotiation, safe publishing, worker isolation, recovery UX, packaging/install/update pipeline |

Implement complete user workflows in vertical slices: import -> edit -> monitor -> save/reopen -> export. A stored field or typed payload alone does not establish an implemented feature.

## Feature reporting and test improvements

The parity cross-check verifies that a named test exists. It does not prove that the test passed on the claimed platform or fully establishes the capability. For example, `extract` is backed by a single-clip ripple-delete test, device output/buffering by offline/fallback tests, and sequence nesting by picture/plan tests despite silent nested audio. The audio/video timeline test checks reported timestamps that `ReadAudio` assigns from the request; it does not compare actual sample alignment.

Track model support, runtime support, UI support, and integration acceptance separately. Record executed test outcomes, provider/build configuration, platform, fixture provenance, skipped cases, and limitations. Make the eleven reproduced failures actual regression cases during their fixes. Add signal comparisons with different channels, nonperiodic waveforms/impulses, sample-offset seeking, full before/after edit equivalence, real-file alternating AV reads, long-form timelines, and device/transport stress coverage. Use sanitizers/race detection on suitable builds and fault injection for persistence/export.

The build scripts hard-code a Visual Studio location, the native test script deliberately disables the desktop host, and FFmpeg/Qt can be absent while CMake still succeeds. Keep optional-dependency core builds, but make product builds require and test their advertised capabilities. Add portable toolchain discovery, explicit dependency/version configuration, Debug/Release presets, CI for core and full application, and packaged install smoke tests. This workspace has no Git repository metadata, so the review is not pinned to a commit and cannot provide a Git diff baseline.

## Browser prototype findings

The browser's `requestAnimationFrame` transport advances one frame per display callback, independent of the declared 24 fps. Its snap button toggles appearance but is not used by drag positioning. It rebuilds the complete timeline DOM and listeners on selection/refresh, sends commands for each grade input event, and polls project state. `dispatchCommand` locally applies edits after any server error, including authoritative validation/revision rejection. Imported names/state are interpolated into `innerHTML` without escaping. Export only records a queue entry.

Because the repository describes this interface as superseded, avoid investing in a second editing engine. Keep it as a clearly labeled reference or archive it. If it is retained as a usable interface, use authoritative error handling, safe text rendering, timestamp-driven transport, event delegation/virtualization, coalesced edits, and a genuine bridge to the native command service.

## Recommended implementation order

1. **Correctness milestone:** repair long-timeline arithmetic, channel buffering, demux ownership/sample seeking, split equivalence, nested/retimed audio, format negotiation, and consistent export policy. Add regression cases for every reproduced failure.
2. **Safe playback milestone:** implement session/worker ownership, snapshot publication, transport generations, queue flush/re-prime, device errors/EOF, independent export contexts, and relink invalidation.
3. **Usable editor milestone:** bind Qt models/actions to the existing command service; deliver one complete edit/save/reopen/export workflow with deliberate error reporting.
4. **Performance milestone:** remove snapshot copies/scans and global cache clears, add resource budgets/proxies/prefetch, then GPU render and hardware-surface interoperation. Benchmark the complete workflow on declared hardware.
5. **Professional workflow milestone:** establish color/precision semantics, advanced editorial, audio, graphics/captions, format support, delivery queue, and tested recovery. Add AI/collaboration/plugins as separate complete workflows when the base is dependable.

The architecture is worth continuing, but optimization should follow explicit correctness and workload contracts. A GPU implementation can accelerate incorrect output just as effectively as correct output.
