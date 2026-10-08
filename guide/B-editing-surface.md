# Phase B: the editing surface

Part of the [implementation guide](../IMPLEMENTATION_GUIDE.md). Read sections 1 to 3 of that file first; the recipes (R1 to R14) are used throughout and are not repeated here.

**What this phase is.** The engines under these features are built and tested. What is missing is the way a person reaches them: a source monitor, patching, a real project panel, nested sequences, drawing on the timeline, copy and paste of effects, import/export dialogs and a recovery prompt. Most work is in `ui/` (rules), `app/` (the `Session` object and Qt items) and `app/qml/` (screens), with small additions to the project store.

**Paths marked (new)** are files you create. Add them to `CMakeLists.txt` (R12).

| Package | Title | Size |
|---|---|---|
| [B1](#wp-b1-source-monitor) | Source monitor | L |
| [B2](#wp-b2-source-marks-three--and-four-point-edits-replace-edit) | Source marks, three- and four-point edits, replace edit | M |
| [B3](#wp-b3-match-frame-and-reverse-match-frame) | Match frame and reverse match frame | S |
| [B4](#wp-b4-subclips-and-master-clip-properties) | Subclips and master-clip properties | M |
| [B5](#wp-b5-source-patching-and-track-targeting) | Source patching and track targeting | M |
| [B6](#wp-b6-project-panel-bins-relink-search-labels-thumbnails) | Project panel: bins, relink, search, labels, thumbnails | L |
| [B7](#wp-b7-sequences-create-nest-switch-breadcrumbs) | Sequences: create, nest, switch, breadcrumbs | L |
| [B8](#wp-b8-adjustment-layers-colour-mattes-and-bars) | Adjustment layers, colour mattes, bars | S |
| [B9](#wp-b9-timeline-drawing-waveforms-thumbnails-cache-bar-caption-lane) | Timeline drawing: waveforms, thumbnails, cache bar, caption lane | L |
| [B10](#wp-b10-copy-and-paste-of-effects-paste-attributes-effect-presets) | Copy/paste effects, paste attributes, effect presets | M |
| [B11](#wp-b11-rate-stretch-sync-lock-and-small-editing-additions) | Rate stretch, sync lock, small editing additions | M |
| [B12](#wp-b12-interchange-and-project-manager-dialogs) | Interchange and project-manager dialogs | M |
| [B13](#wp-b13-recovery-on-open-and-the-unclean-shutdown-notice) | Recovery on open and the unclean-shutdown notice | M |

---

## WP-B1. Source monitor

**Problem.** Double-clicking a media item inserts the whole file at the playhead (`Session::insertMedia`). There is no place to look at footage, play it, or choose a part of it. `PlaybackEngine` plays *sequences*, so the simplest honest design is to play the media as a one-clip sequence that exists only in memory.

**Files to read first (in this order)**

1. `timeline/Sequence.h` — the snapshot structs you will build by hand.
2. `playback/PlaybackEngine.h` — the engine's public methods, especially `RenderFrame`, `Play(sink)`, `Seek`, `position()`, `UpdateSequence`, `SetOutputSize`, `EngineConfig`.
3. `app/Session.cpp`: `StartEngine`, `RequestFrame`, `OnTick`, `OnFrame`, `ToImage`.
4. `ui/Monitor.h` — `FramePresenter` (latest-wins worker thread) and `PresentationMode`.
5. `app/SessionMulticam.cpp` — precedent for a *second* presenter and monitor beside the main one (`mc_presenter_`).
6. `app/MonitorItem.cpp` — how a frame is drawn fitted or zoomed (`ui::FrameRect`).
7. `ui/Transport.h` — play, shuttle, step, loop, with a caller-supplied clock tick.

**Design**

```text
 Project panel (select / double-click)
        |
        v
  Session::openInSource(mediaId)
        |  builds
        v
  timeline::SourceGraph  (one Sequence: 1 video track + 1 audio track, 1 clip each, the whole media)
        |
        v
  source_engine_ (its own PlaybackEngine)  ->  source_presenter_ (FramePresenter)  ->  QImage  ->  SourceMonitorItem
        ^
  source_transport_ (ui::Transport): play / shuttle / step / seek, ticked by the same 16 ms timer as the program monitor
```

Why a second engine rather than reusing the first: the program engine's snapshot is the project's sequence; swapping it for a source would stop playback of the edit. A second engine is independent, costs one decode pool and one frame cache (give it a small cache), and shares nothing mutable.

**Steps**

1. **Pure builder.** Create `timeline/SourceSequence.h (new)` and `timeline/SourceSequence.cpp (new)`:
   ```cpp
   struct SourceInfo {
     std::string media_id, name;
     time::RationalTime duration;
     bool has_video{false}, has_audio{false};
     time::FrameRate frame_rate{25, 1};       // of the media's primary video stream
     std::int64_t width{0}, height{0};
     std::int64_t sample_rate{48000};
   };
   [[nodiscard]] SequenceGraph MakeSourceGraph(const SourceInfo& info);
   ```
   It returns a graph whose root `Sequence` has id `"source:" + media_id`, the media's own frame rate and size (so stepping lands on the source's own frames), and a video track `source-v` and audio track `source-a` (only those the media has), each holding one `Clip` with `source_in = 0`, `source_out = duration`, `timeline_start = 0`, `playback_rate = {1,1}`, a shared `linked_group`. Fill `start_ticks`/`end_ticks` by the same rule the loader uses (see `timeline/SequenceLoader.cpp`, search `start_ticks`). Throw `std::invalid_argument` for a zero-length media or one with neither video nor audio.
2. **Test the builder** in `tests/native/timeline_tests.cpp`:
   - `ASourceGraphPlaysTheWholeMediaFromItsStart`: compile at `t` with `TimelineCompiler` and assert `plan.video[0].source_time == t` and `plan.audio[0]` likewise; at `duration` the plan is empty.
   - `ASourceGraphOfAPictureOnlyOrSoundOnlyMediaHasOnlyThatTrack`, and the refusals.
3. **Read the media's facts.** In `app/SessionSource.cpp (new)` (R4, R12) write `std::optional<timeline::SourceInfo> Session::ReadSourceInfo(const std::string& media_id)`: one SQL statement against `media` and `media_streams` (look at `InsertPlanAt` in `Session.cpp` for the existing pattern; add `frame_rate_num/den`, `width`, `height`, `sample_rate` columns — read the `media_streams` definition in `core/project/Schema.cpp` for the exact names).
4. **Share the engine configuration.** `Session::StartEngine` builds an `EngineConfig` inline. Extract the locator and proxy lookup into private methods (`MakeLocator()`, `MakeEngineConfig(bool monitor_quality)`) so the source engine uses the same lookup. Source engine settings: `use_gpu = false` at first, `frame_cache_bytes = 64 MB`, `read_ahead_frames` as the preference, `decode_workers = 1`, no render cache.
5. **Session state and API.** In `app/Session.h` add:
   - properties: `sourceOpen` (bool), `sourceName`, `sourcePosition` (seconds), `sourceDuration`, `sourcePlaying`, `sourceTimecode`;
   - invokables: `openInSource(mediaId)`, `closeSource()`, `sourceSeek(seconds)`, `sourceStep(frames)`, `sourcePlay(bool)`;
   - a signal `sourceChanged()` and `sourceFrameReady()`; a getter `sourceFrame()` returning the `QImage`.
   Members: `std::unique_ptr<playback::PlaybackEngine> source_engine_; std::unique_ptr<ui::FramePresenter> source_presenter_; ui::Transport source_transport_; QImage source_frame_;`.
6. **Implement** `openInSource`: read the info; build the graph; if `source_engine_` is null create it, else `UpdateSequence(graph)`; set the transport's frame rate and duration; `source_transport_.Seek(0)`; request a frame. The presenter callback mirrors `StartEngine`: render with the source engine and queue a `QImage` back to the GUI thread. Request a frame with `PresentationMode::LatestOnly` for seeks and `Playback` while playing.
7. **Ticking.** In `Session::OnTick` after the program transport logic: if `source_transport_` is playing, `Advance(dt)` it, and request a frame when the position moved. Without audio first (wall clock only). Stop at the end of the media (or loop if the loop preference is on).
8. **Source audio** (do this after the picture works, as its own commit): create a second `audio::AudioSink` (see how the program one is created in `Session::StartPlayback`, search `WasapiSink`/`OpenDefaultSink`), call `source_engine_->Play(*source_sink_)`, and use `source_engine_->position()` as the clock like `OnTick` does for the program engine. Make sure only one of the two monitors plays at a time: starting one pauses the other.
9. **The Qt item.** Create `app/SourceMonitorItem.h (new)` and `app/SourceMonitorItem.cpp (new)`: a `QQuickPaintedItem` (copy the skeleton of `MonitorItem`, drop the mask code) that paints `session_->sourceFrame()` fitted with `ui::FrameRect`, the timecode badge, and the mark overlay from B2 later. Register with `QML_NAMED_ELEMENT(SourceMonitorItem)` and add to the module `SOURCES`.
10. **The panel.** Register `source_monitor` (R5): title "Source Monitor", add `app/qml/panels/SourceMonitorPanel.qml (new)` with the item, a transport row (go to start, step back, play, step forward, go to end — copy `MonitorPanel.qml`), a scrub slider bound to `sourcePosition`/`sourceDuration`, and the timecode. Put it in the built-in "Editing" and "Assembly" workspaces as a tab beside the program monitor (see the layout builders near the bottom of `ui/Layout.cpp`). Panel command `panel.source_monitor`, default `Shift+2`.
11. **Open from the Project panel.** In `ProjectPanel.qml` change the row's `onDoubleClicked` to `session.openInSource(row.modelData.id)` and add a small "Insert" and "Overwrite" button per row (the old double-click behaviour) so nothing is lost. A single click only selects.
12. **Focus routing for the keyboard.** The keymap has one `Space`, one `J/K/L`. Add `QString focused_panel_` to `Session` with an invokable `setFocusedPanel(id)`; every panel QML calls it from `onActiveFocusChanged` or a `TapHandler`. In `Session::trigger`, for the transport commands (`transport.*`) choose the target by `focused_panel_ == "source_monitor"`. Keep the program monitor the default. Document the rule in the shortcut's description text.
13. **Close and clean up.** `closeProject()` must reset `source_presenter_` and `source_engine_` *before* the store goes away (copy the order used for `mc_presenter_`). Destroying the engine joins its threads; do not do it from inside a presenter callback.

**Tests**

- `timeline_tests.cpp`: step 2.
- `tests/app/app_tests.cpp`:
  - `TheSourceMonitorShowsTheChosenMediaAndStepsByItsFrames`: `session.openInSource("Bars")`; `QTest::qWaitFor` until `sourceFrame()` is non-null; `sourceStep(1)`; assert `sourcePosition()` advanced by one source frame (`1/25 s` for the demo media) and the **program** playhead did not move.
  - `ThePictureOfTheSourceDiffersFromTheProgramWhenTheyShowDifferentThings`: compare a pixel.
  - `KeysGoToTheSourceMonitorOnlyWhileItHasFocus`: `setFocusedPanel("source_monitor")`, trigger `transport.play_pause`, assert `sourcePlaying()` and not `playing()`; then `setFocusedPanel("program_monitor")` and the reverse.
  - `ClosingTheProjectWhileTheSourceMonitorIsOpenDoesNotCrash` (open, close, reopen).

**Check by hand.** `scripts\run-app.bat --demo`; click "Counter" in the Project panel, double-click it: the Source Monitor tab shows its frame counter; press `Right` to step, `Space` to play (the counter runs), `L` to shuttle; the program monitor is unchanged. Click the program monitor and press `Space`: now the timeline plays. Take `scripts\shot.bat source.png --demo --do select:Counter` after adding `--do trigger:panel.source_monitor`.

**Docs.** `IMPLEMENTATION_STATUS.md`: UI-002 row ("no source monitor") and the "Desktop application" table: add a UI-SOURCE row. `TODO.md`: tick the monitor part of `UI-SOURCE-001`. `parity`: `source_monitor` and `source_playback` become `partial` until B2 is done (R13).

**Pitfalls.** (a) Two engines opening the same file is fine; two threads in one `Source` is not (read the class comment in `media/Source.h`). (b) A media with sound but no picture: the item must show a "sound only" card, not a black rectangle; draw the waveform in B9. (c) The program and source engines both want the GPU if you enable it later: the D3D11 compositor is per engine; test with both open on a machine that has one adapter before enabling `use_gpu`.

---

## WP-B2. Source marks, three- and four-point edits, replace edit

**Problem.** `ui::ResolveThreePoint` and `ui::PlanInsertEdit` already take a source in/out, and are tested (`ThreePointEditsResolveTheMissingPointAndRefuseWhatCannotBe`). The application never provides source marks (`Session::InsertPlanFor` passes the whole media), and the header comment promises a "fit to fill" option that the struct does not have.

**Files to read first**: `ui/EditPlanner.h` (`ThreePointInput`, `ResolveThreePoint`, `SourceRange`, `PlanInsertEdit`) and its tests; `Session::InsertPlanFor`; `Session::trigger` branches `timeline.insert|overwrite|mark_in|mark_out|lift|extract`.

**Steps**

1. **Source marks state.** In `Session`: `std::optional<RationalTime> source_in_, source_out_;` (reset by `openInSource`), properties `sourceMarkIn`, `sourceMarkOut` (seconds, -1 for none), signal `sourceMarksChanged`.
2. **Commands.** Add application commands (R6): `source.mark_in`, `source.mark_out`, `source.clear_in`, `source.clear_out`, `source.clear_marks`, `source.go_in`, `source.go_out`, `source.mark_clip` (mark the whole media; default `X`; `Ctrl+Shift+X` is already Clear In and Out). Route the existing `timeline.mark_in` (`I`), `timeline.mark_out` (`O`), `timeline.clear_in`, `timeline.clear_out`, `timeline.clear_in_out`, `timeline.go_in`, `timeline.go_out` to the source equivalents when `focused_panel_ == "source_monitor"` (WP-B1 step 12). Marks are in source time and are snapped to the source's frames.
3. **Draw the marks.** In `SourceMonitorPanel.qml`, a slim bar under the scrub slider shows the range between in and out; the item draws a small `{` and `}` at the corners when the playhead is on a mark.
4. **Use the marks.** Change `Session::InsertPlanFor` to take the source from the *source monitor* when one is open:
   ```cpp
   ui::ThreePointInput in;
   in.source_in = source_in_;           // optional
   in.source_out = source_out_;
   in.sequence_in = mark_in_;           // the program marks, as today
   in.sequence_out = mark_out_;
   in.playhead = transport_.position();
   in.source_duration = duration;       // the media's
   const auto resolved = ui::ResolveThreePoint(in);
   ```
   then `PlanInsertEdit(ctx, SourceRange{media, name, resolved.source_in, resolved.source_out, has_video, has_audio}, resolved.at, ...)`. When no source monitor is open, keep today's behaviour (the selected media, whole length).
5. **Fix the four-point fit policy.** `ResolveThreePoint` currently trims the source out when all four points are set. Add to `ThreePointInput`:
   ```cpp
   enum class Fit { TrimSourceOut, ChangeSpeed, IgnoreSequenceOut };
   Fit fit{Fit::TrimSourceOut};
   ```
   and to `ThreePointResult` a `RationalTime playback_rate{1,1}`. For `ChangeSpeed`, `playback_rate = (source_out - source_in) / (sequence_out - sequence_in)`; refuse if the rate is outside `0.01 .. 100`. Add `RationalTime playback_rate{1,1}` to `SourceRange` and make `PlanInsertEdit` put it in each `ClipSpec`/`InsertClip` (`maintain_pitch` true for audio). Correct the comment in `EditPlanner.h` that mentions `fit_to_fill`.
6. **Ask the user.** When all four points exist, `Session` emits `fitChoiceRequested()`; `app/qml/FitDialog.qml (new)` offers "Change clip speed (fit to fill)", "Trim source out", "Ignore sequence out", "Cancel". The chosen fit is passed to `insertFromSource(fit, insert)`.
7. **Replace edit.** Add `ui::PlanReplace(ctx, target_clip_id, SourceRange, ReplaceAlign)` in `ui/EditPlanner.cpp`. Rule (document it in the header): the target clip (and its linked partner) keep their timeline start and duration, track, and effects; their source becomes `SourceRange` shifted so that *`ReplaceAlign::PlayheadToPlayhead`* the source position shown in the source monitor lines up with the program playhead (Premiere's default), or *`SourceInToClipStart`*. Build it from `SpecOf` (copy the clip), then commands: `DeleteClip` for the targets, `InsertClip` for the new source with the old effects reapplied (reuse the effect-copy code from B10 — do B10's helper first or implement a private one here and refactor later). Refuse if the source range is shorter than the clip.
8. **Commands for the keyboard.** `timeline.replace` (`F11` in other editors; here `Ctrl+Shift+Period`), calling `PlanReplace` for the selected clip (or the clip under the playhead on a targeted track once B5 exists).
9. **Drag to timeline** (optional, last): `SourceMonitorPanel.qml` starts a `Drag` carrying `{media, in, out}`; `TimelineItem::dropMedia` gains `dropSource(x, y, mediaId, inSeconds, outSeconds, insert)`.

**Tests**

- `ui_tests.cpp`: `FourPointsFitByTrimmingChangingSpeedOrIgnoringTheSequenceOut` (each fit gives the expected `source_out`/`playback_rate`; refusing a 1000x rate); `AnInsertEditPlacesOnlyTheMarkedPartOfASource` (execute the plan against a real store, then check the clip's `source_in/out`); `AReplaceEditKeepsPositionLengthAndEffectsAndChangesTheSource` including undo.
- `core_tests.cpp`: none (no new commands).
- `app_tests.cpp`: `MarkingTheSourceThenInsertingPlacesOnlyThatRange`: `openInSource`, `sourceSeek(2)`, `trigger("source.mark_in")`, `sourceSeek(5)`, `trigger("source.mark_out")`, `insertFromSource(...)`, assert the new clip's duration is 3 s.

**Check by hand.** Open "Counter" in the source monitor, mark 2 s to 5 s, move the program playhead to 10 s, press `,` (insert): a 3 s clip appears with picture and linked sound; `Ctrl+Z` removes it in one step. Set program in and out 4 s apart as well, press `,`: the dialog appears.

**Docs.** IMPLEMENTATION_STATUS "Editorial commands" row; parity `source_mark_in`, `source_mark_out`, `source_clear_marks`, `four_point_edit`, `replace_edit` — add tests (they must be native tests) and entries (R13).

---

## WP-B3. Match frame and reverse match frame

**Problem.** Given the frame under the playhead, find its media and the position in that media; and the opposite.

**Design.** Both are pure functions. Do not recompute source time by hand: ask the compiler, which already handles speed, reverse and time-remap.

**Steps**

1. `ui/MatchFrame.h (new)` / `.cpp (new)`:
   ```cpp
   struct SourceHit { std::string media_id; std::string clip_id; time::RationalTime source_time; };
   // The picture visible at `at`: the topmost video request that is a media clip (depth 0).
   [[nodiscard]] std::optional<SourceHit> MatchFrame(const timeline::SequenceGraph&, const time::RationalTime& at);
   // A time on the timeline where `source_time` of `media_id` is shown, nearest to `near`.
   [[nodiscard]] std::optional<time::RationalTime> ReverseMatchFrame(const timeline::Sequence&, const std::string& media_id,
                                                                      const time::RationalTime& source_time, const time::RationalTime& near);
   ```
   `MatchFrame`: `TimelineCompiler().Compile(graph, at)`, take the **last** entry of `plan.video` at depth 0 whose `source_kind == Media` (the plan is in ascending track order, last is top). For `ReverseMatchFrame`, scan clips of that media whose `[source_in, source_out)` contains `source_time`; for a plain clip `timeline = clip.timeline_start + (source_time - source_in) / playback_rate` (mirror for `reversed`); if the clip has a `time_remap` effect the mapping is not linear: skip such clips and say so in a returned reason, or search by compiling at candidate times (bisect on the clip's extent). Pick the hit nearest `near`.
2. Commands: `timeline.match_frame` (`F`) opens the hit in the source monitor at `source_time` (B1) and sets nothing else; `source.reverse_match` (`Shift+R`) seeks the program playhead.
3. Status messages for failure: "Nothing at the playhead", "That media is not in the sequence".

**Tests** (`ui_tests.cpp`): a clip at 10 s with `source_in = 4`: `MatchFrame(graph, 12)` gives `source_time == 6`; with `playback_rate = 2`: `source_time == 8`; reversed; a gap gives nothing; the topmost of two layers wins; `ReverseMatchFrame` is the inverse of `MatchFrame` for a sample of times (property test: for every frame time `t` in the clip, `ReverseMatchFrame(MatchFrame(t)) == t`). App test: trigger `timeline.match_frame` and assert `sourceName()`/`sourcePosition()`.

**Check by hand.** Demo project: park the playhead at 8 s (inside "Counter"), press `F`: the Source Monitor opens the Counter media at 4 s (it was placed with source-in 2 s at 6 s). Scrub the source, press `Shift+R`: the program playhead jumps to the matching timeline time.

**Docs/parity.** `match_frame`, `reverse_match_frame`.

---

## WP-B4. Subclips and master-clip properties

**Problem.** A subclip is a named range of a media item that appears in the project panel and behaves like media. There is no way to store one.

**Design.** A `subclips` table in the project plus three commands. A subclip is *not* a new media file and not a new clip: it is a bookmark that the source monitor and the project panel understand.

**Steps**

1. **Schema (R2).** New version, new table:
   ```sql
   CREATE TABLE IF NOT EXISTS subclips(
     id TEXT PRIMARY KEY,
     media_id TEXT NOT NULL REFERENCES media(id) ON DELETE CASCADE,
     bin_id TEXT REFERENCES bins(id) ON DELETE SET NULL,
     name TEXT NOT NULL CHECK(name <> ''),
     in_num INTEGER NOT NULL, in_den INTEGER NOT NULL CHECK(in_den > 0), in_ticks INTEGER NOT NULL,
     out_num INTEGER NOT NULL, out_den INTEGER NOT NULL CHECK(out_den > 0), out_ticks INTEGER NOT NULL,
     CHECK(out_ticks > in_ticks)
   );
   CREATE INDEX IF NOT EXISTS subclips_media ON subclips(media_id);
   ```
   Store times the way `markers` does (exact pair plus ticks; use the same helper that writes them). Add `subclips` to `RecordedTables()`.
2. **Commands (R1):** `CreateSubclip {id, media_id, bin_id?, name, in, out}`, `RenameSubclip {id, name}`, `DeleteSubclip {id}`, `SetSubclipRange {id, in, out}`. Rules: range inside the media's duration; name 1–200 characters. Wire names `subclip.create` etc.
3. **Read model.** `Session::RefreshMedia` appends subclips to `media_` as entries with `"kind": "subclip"`, `"mediaId"`, `"in"`, `"out"`, so the Project panel shows them under their media (indented) or in their bin.
4. **UI.** Source monitor button/command `source.make_subclip` (`Ctrl+U`): requires both marks; asks a name (a small `Dialog`), runs `CreateSubclip`. Project panel: double-click a subclip → `openInSource(media)` with the marks set to its range; insert/overwrite use that range. Context menu: Rename, Edit range (sets the source marks to the range; "Update subclip" writes them back), Delete.
5. **Master clip properties.** `app/qml/MediaInfoDialog.qml (new)`, opened from the Project panel's context menu "Properties…": a read-only table from `media_streams` (codec, size, rate, cadence, bit depth, chroma, field order, colour primaries/transfer/matrix/range, audio rate/channels) and a *Colour interpretation* section with menus for primaries, transfer, matrix and range. Changing them runs the existing `SetMediaStreams` command with the edited stream rows (read the current rows, change four fields, send all). Frame-rate and pixel-aspect overrides are out of scope: say so on the dialog.

**Tests**: `store_tests.cpp` — create, rename, change range, delete, undo/redo, deleting the media deletes the subclips, the refusals, migration test (R2 step 4). `ui_tests.cpp` — a planner `PlanSubclipInsert` that turns a subclip into a `SourceRange`. `app_tests.cpp` — make a subclip from marks; it appears in `session.media`; inserting it places the right range.

**Check by hand.** Mark 2–5 s on "Counter", `Ctrl+U`, name "Middle"; it is listed under Counter; drag/insert it: 3 s. Save, close, reopen: still there. Properties dialog: change the transfer of "Gradient" to PQ, the preview changes (colour managed sequences only).

**Docs/parity.** `subclip_creation`, `master_clip_properties`; schema version text everywhere (R2 step 5).

---

## WP-B5. Source patching and track targeting

**Problem.** `Session::InsertPlanFor` always uses the lowest unlocked video track and the lowest unlocked audio track. Editors choose which source track goes to which sequence track (patching), and which sequence tracks an operation affects (targeting). Locking a track is the only control today.

**Design.** Two flags per sequence track, persisted: `targeted` (the track takes part in lift, extract, add edit, paste, sequence-in/out operations) and `patch` (for the source's video and audio: which sequence track the source's first video / first audio stream goes to). Plus `sync_lock` (B11). They live on `tracks` and change through one new command.

**Steps**

1. **Schema (R2)**: `ALTER TABLE tracks ADD COLUMN targeted INTEGER NOT NULL DEFAULT 1 CHECK(targeted IN (0,1));`, `ADD COLUMN source_patch INTEGER NOT NULL DEFAULT 0 CHECK(source_patch IN (0,1));`, `ADD COLUMN sync_lock INTEGER NOT NULL DEFAULT 0 CHECK(sync_lock IN (0,1));`. For new sequences set `source_patch = 1` on `V1` and `A1` and `targeted = 1` everywhere (mirror the defaults in the project-creation code `Session::newProject`).
2. **Command (R1)**: `SetTrackTargeting {id, targeted, source_patch, sync_lock}`. Validation: exactly one track of each kind may have `source_patch` per sequence — the command that sets it on one clears it on the others of that kind (do this in `Apply`, in one transaction). Add it to the lock policy? Targeting on a locked track is allowed.
3. **Snapshot**: add `bool targeted{true}, source_patch{false}, sync_lock{false};` to `timeline::Track` (`timeline/Sequence.h`) and fill them in `timeline/SequenceLoader.cpp` (copy how `muted` is loaded).
4. **Use them in the planners.**
   - `InsertPlanFor` (Session): video track = the track with `source_patch` (video), audio likewise; if none, fall back to the old rule. If the patched track is locked, refuse with a message that names it.
   - `PlanLift`, `PlanExtract`, `PlanSplit` ("Add Edit" with no track list), paste: when `track_ids` is empty use the *targeted* tracks (Premiere uses targeted tracks for these; the selection, if any, still wins for split/delete).
   - Put the helper in `EditContext`: `std::vector<const Track*> TargetedTracks(const EditContext&)`.
5. **Track header buttons.** In `app/TimelineItem.cpp` the track header currently draws lock, mute, solo; add two small buttons: **patch** (a `V1`/`A1` box that is filled for the patched track — click makes this track the patch) and **target** (a toggle). See how `HitKind` and the header hit-testing work in `ui/TimelineView.h` (add `HitKind::HeaderTarget`, `HeaderPatch` and the geometry in the layout) and how `Session::setTrackFlag` is called for lock/mute/solo.
6. **Keyboard:** `timeline.target_video_N` / `timeline.target_audio_N` (`Alt+1`..`Alt+9`? check the registry for free chords), and `timeline.toggle_patch`. Optional.

**Tests**: `store_tests.cpp` (command, exclusivity, undo/redo, migration); `ui_tests.cpp` (`LiftAndExtractActOnlyOnTargetedTracks`, `InsertGoesToThePatchedTracks`, refusal when the patched track is locked); `app_tests.cpp` (click the target button on A1, lift: sound untouched).

**Check by hand.** Demo: untarget A1 (click its target button), mark in/out over the first clip, press `;` (lift): the picture is removed, the sound stays. Patch V2 and insert from the source monitor: the picture goes to V2.

**Docs/parity**: `source_patching`, `track_targeting` (both P0).

---

## WP-B6. Project panel: bins, relink, search, labels, thumbnails

**Problem.** `ProjectPanel.qml` lists media flat. The store has bins (`CreateBin`, `RenameBin`, `MoveBin`, `DeleteBin`) and `RelinkMedia`; the app never issues them; and no command moves media into a bin or labels it.

**Files to read first**: `app/qml/panels/ProjectPanel.qml`, `Session::RefreshMedia`, `core/project/ProjectStore.cpp` (the bin handlers), `media/Ingest.h` (`IngestFile`, fingerprint function), `app/LutPreviewProvider.h` (a `QQuickImageProvider` example).

**Sub-packages** (do them in this order; each is a commit and a pull request).

### B6.1 Commands the store lacks
1. R1 + R2: `MoveMediaToBin {media_id, bin_id?}`, `RenameMedia {id, name}`, `SetMediaLabel {id, color}`; schema adds `media.label_color TEXT NOT NULL DEFAULT ''` (CHECK in a fixed palette of 8 names plus ''). Validation: bin exists and is not the media itself, name non-empty.
2. Tests: store_tests for each, undo, and `MoveBin` cycle refusal still holds.

### B6.2 Bin tree
1. `Session`: `Q_PROPERTY(QVariantList bins ...)` (flat list with `id`, `parent`, `name`, `depth`, `count`), and the media list entries gain `binId`. Build it in `RefreshMedia` from `SELECT id, parent_id, name FROM bins ORDER BY parent_id, sort_order`.
2. Invokables: `binCreate(parentId, name)`, `binRename`, `binDelete`, `binMove`, `mediaMoveToBin(mediaId, binId)`, `mediaRename`, `mediaSetLabel`. Each via `Run(...)`.
3. QML: a left-hand tree (`TreeView` or a `ListView` with indent) and the media list shows the selected bin (or "All"). Drag media onto a bin (Qt `DropArea`). Context menus: New Bin, Rename, Delete (the store un-files media rather than deleting it — say so in the confirm text).
4. Tests: `app_tests.cpp` — create bin, move media into it, the list filtered by bin; undo restores both. Native: none new.

### B6.3 Search, sort, columns
1. Pure `ui/MediaBrowser.h (new)`: `FilterMedia(entries, query)` — case-insensitive words, each word must match name, path, codec, label name; `SortMedia(entries, column, ascending)`. Define a small struct `MediaRow` (not QVariant) for testability.
2. QML: a search field above the list, a header row with sortable columns (Name, Duration, Video, Audio, Frame rate, Size, Label), list/icon toggle.
3. Tests: `ui_tests.cpp` — word matching, order stability, empty query.

### B6.4 Relink
1. `Session::relinkMedia(mediaId, newPath)`: probe + fingerprint the new file (`media::IngestFile`'s helpers; look for a function that returns the fingerprint without importing) and compare with the stored fingerprint: if equal, run `RelinkMedia` with `missing=false`; if different, ask the user ("different content; relink anyway?") and pass the new fingerprint (the payload invalidates stale proxies).
2. A job (R7) `relinkFolder(folderUrl)`: scan a folder recursively for files whose name matches offline items, fingerprint candidates, relink all that match. Progress and a result summary.
3. QML: offline items show "OFFLINE" in red (already) plus a **Locate…** button and a **Find in folder…** header button.
4. Tests: `playback_tests.cpp` already covers engine relink; add `app_tests.cpp`: move a file, the item is offline, `relinkMedia` fixes it and the clip plays (frame non-black). Use a copy of a fixture in a temp folder.

### B6.5 Thumbnails and the icon view
1. `media/Thumbnail.h (new)` / `.cpp (new)`: `std::optional<VideoFrame> PosterFrame(const std::string& path, int max_width)` — open with `SourceRegistry`, `ReadVideo` at 10% of the duration (or the first frame for a still), scale down (bilinear into a new `VideoFrame`; write a small `Downscale` helper or reuse one if you find it with `grep -n "Downscale\|Resize" media render`).
2. `app/ThumbnailProvider.h (new)`: a `QQuickImageProvider` (copy `LutPreviewProvider.h`) answering `image://thumb/<mediaId>`; look the path up through `Session`; cache in memory (LRU, 200 entries) and on disk under the cache root (`<cache>/thumbs/<fingerprint>-<width>.png`) so the second start is instant. Generation happens on the provider's thread (Qt calls `requestImage` off the GUI thread for `QQuickImageProvider::Image` by default — check with `QQmlImageProviderBase::ForceAsynchronousImageLoading`).
3. Register the provider in `app/main.cpp` like the LUT preview one.
4. QML: `Image { source: "image://thumb/" + modelData.id; asynchronous: true }` in the list row and the icon grid.
5. Tests: native — `PosterFrameOfAFixtureIsThePictureAndScalesDown` (media_tests; skip without fixtures); app — the provider returns a non-null image of the requested width for a demo media.

**Check by hand (B6 as a whole).** Import three files; make bins "Interviews" and "B-roll"; drag; search "int"; rename; label one red; `Ctrl+Z` twice; rename a file on disk, reopen the project, press Locate…; toggle icon view and see thumbnails.

**Docs/parity**: media_labels, media_metadata, media_search, relink_media, path aliases are separate (C8). Update `UI-PROJECT-001` in `TODO.md`.

---

## WP-B7. Sequences: create, nest, switch, breadcrumbs

**Problem.** The project has one sequence (`seq-1`). The compiler, mixer and store nest sequences, but nothing in the app creates a second one, nests a selection, or opens one.

**Files to read first**: `Session::newProject` (how a sequence and its tracks are created), `Session::Reload` (`graph_ = LoadSequenceGraph(store, sequence_id_)`), `timeline/SequenceLoader.cpp`, `ui/EditPlanner.cpp` (`PlanPlace`, `SpecOf`, `Builder`), the nesting tests in `timeline_tests.cpp` (`NestedSequencesResolveThroughToTheirSources`).

**Steps**

1. **Read model.** `Session`: `Q_PROPERTY(QVariantList sequences ...)` ( `id`, `name`, `duration`, `rate`, `width`, `height` from `SELECT … FROM sequences`), `Q_PROPERTY(QString sequenceName)`, `Q_PROPERTY(QString sequenceId)`, and a stack `QStringList sequence_trail_` (breadcrumbs).
2. **Switching.** `Session::openSequence(id)`: stop playback; set `sequence_id_ = id`; clear selection; `Reload()`; push on the trail. The transport, marks and caption selection reset. `Session::closeNested()` pops the trail. Everything else already follows `graph_.root()`.
3. **New sequence.** Planner `ui/Sequences.h (new)`: `PlanNewSequence(project_settings, name, width, height, rate, ...)` returning an `EditPlan` with `CreateSequence` followed by `AddVideoTrack` ×3 and `AddAudioTrack` ×3 (names V1..V3, A1..A3, as `Session::newProject` does). Dialog `app/qml/NewSequenceDialog.qml (new)`: name, preset (HD 1080p 25/29.97/30/50/60, UHD, 720p, vertical 1080x1920, square), editable width/height/rate/sample rate. Command `file.new_sequence` (`Ctrl+N`).
4. **Nest selection.** `PlanNestSelection(ctx, selection, name)` — the interesting one. As one `EditPlan`:
   1. `CreateSequence` (same format as the current one) and the tracks it needs (as many video/audio tracks as the selected clips span, ordered like the originals);
   2. for each selected clip, the commands that put an equivalent clip in the new sequence at `clip.timeline_start − earliest_start`. Do this with `SpecOf` + `PlanPlace` run against an **in-memory `timeline::Sequence` for the new sequence** (an empty `Sequence` object with the new tracks; the planner only needs `FindTrack` and empty clip lists) and append its commands; this carries effects, links and speed;
   3. `DeleteClip` for the originals (plain delete, not ripple);
   4. `InsertClip` with `source_kind = Sequence`, `nested_sequence_id = new id`, `source_in = 0`, `source_out = new sequence duration`, at the earliest start, on the lowest selected video track and (a second clip, linked) the lowest selected audio track.
   Because the plan is one list, it is one undo step. Refuse: nothing selected; locked tracks; a selection that would nest the current sequence into itself (the store refuses cycles too; surface its message).
5. **Open nested.** Double-click on a nested clip (`TimelineItem::mouseDoubleClickEvent`, near the ramp double-click) calls `session.openSequence(clip.sourceId)`; the trail shows `Main > Nest 1`.
6. **Breadcrumbs and tabs.** `app/qml/panels/TimelinePanel.qml` gets a header row: tabs for open sequences (a `TabBar` over `session.sequenceTabs`) and breadcrumbs. Closing a tab only forgets it. `Session::sequenceTabs` is a list of ids kept in memory (and saved in `<project>/session.json`? keep it in memory for now).
7. **Delete sequence** (command exists, `DeleteSequence`): context menu on the tab/project panel; refuse while another sequence nests it or while it is open.
8. **Project panel** lists sequences in a "Sequences" section; double-click opens; drag one onto the timeline inserts a nested clip (reuse `dropMedia` with a `sequence:` prefix).

**Tests**

- `ui_tests.cpp`: `NestingTheSelectionMovesTheClipsIntoANewSequenceAndLeavesOneNestedClipThatPlaysTheSame` — build a sequence with two clips and an effect, run the plan against a real store, then compile the **outer** sequence at several times and compare the `source_time` and effect values to those compiled before nesting (the pictures must be identical); undo restores the original clips in one step; `NestingRefusesLockedTracksAndAnEmptySelection`.
- `store_tests.cpp`: exists; add the case "a nested clip of a sequence that nests the parent is refused" if missing.
- `app_tests.cpp`: nest, open, close, and the timeline shows the right clips; `openSequence` on a missing id is a no-op with a status message.
- Render-level: a golden-free check using `Compositor` on the two graphs (before/after nesting) — compare mean pixel difference < 1e-4 using the helpers in `render_tests.cpp`.

**Check by hand.** Select the first two clips in the demo; Edit > Nest (give `timeline.nest` a free chord; `Ctrl+Alt+N` is already New Project, and the registry test tells you if you pick a taken one); the timeline shows one purple nested clip; play: same picture; double-click: the nested content; breadcrumb back.

**Pitfalls.** Nested sequences use the root's render version; a mismatch is already handled by the compiler. Effects *on the nested clip* apply to the composite of the nested sequence. The audio of a nested clip is mixed through the nested sequence's own buses, then through the outer track.

**Docs/parity**: nesting, `UI-SEQUENCE-001`.

---

## WP-B8. Adjustment layers, colour mattes and bars

**Problem.** Adjustment clips and the `solid` generator render on the CPU and the GPU; nothing in the app creates one, and `solid` is hidden in the effect browser.

**Steps**

1. **Planner** in `ui/EditPlanner.cpp`: `PlanAddAdjustment(ctx, track_id, at, duration)` → `InsertClip` with `source_kind = Adjustment` (look at how `Session` or the tests create one: `grep -rn "SourceKind::Adjustment" tests ui core`); `PlanAddGenerator(ctx, track_id, at, duration, kind, colour)` → `InsertClip` with `source_kind = Adjustment`? **Check first** how the existing `solid` effect is intended to be used (it is an effect on a source-less clip, like graphics: `Video("solid", ... "Generate")`). A colour matte is a source-less clip carrying a `solid` effect; confirm by reading `Compositor.cpp` for `"solid"` and a test that uses it.
2. Menu: **Project panel "New item" button → Adjustment Layer / Colour Matte / Bars and Tone / Black Video / Universal Counting Leader**. Only the first two need new code; "Bars and Tone" can be the synthetic source (`media::SyntheticPattern::Bars` with a sine tone) imported as media using the `synthetic:` URI the demo uses — read `Session::newDemoProject` (`make("Bars", ...)`) for the exact URI form.
3. Insert at the playhead on the first free video track with the default length (use the `timeline.default_still_seconds` preference once WP-A3 step 9 is done; 5 s until then).
4. **Timeline drawing**: adjustment clips draw grey (`TimelineItem.cpp` already does); give mattes their colour.
5. Make `solid` visible in the Effects panel? No — a matte is a clip, not an effect. Leave it hidden.

**Tests**: `ui_tests.cpp` planners (refusals; undo); `app_tests.cpp`: add an adjustment layer above a clip, add a "black and white" effect to the adjustment layer through the inspector, assert the program frame's centre pixel is grey (`session.currentFrame()` after waiting).

**Check by hand.** Add an adjustment layer over the whole demo and add Black & White to it: everything below goes grey; disable the clip (`Shift+E`): colour returns.

---

## WP-B9. Timeline drawing: waveforms, thumbnails, cache bar, caption lane

**Problem.** `TimelineItem::paint` draws coloured rectangles with names. A professional timeline shows what is in the clips.

**Files to read first**: `app/TimelineItem.cpp` `paint` (the clip drawing loop, lines near `badge`), `ui/TimelineView.h` (the layout rows and boxes), `media/Source.h` (`ReadAudio`, `ReadVideo`), `ui/Jobs.h`, `playback/PlaybackEngine.h` (`CachedFrames`).

### B9.1 Audio peaks
1. `media/PeakFile.h (new)` / `.cpp (new)`:
   ```cpp
   struct Peaks { std::int64_t buckets_per_second{100}; std::vector<std::int8_t> min, max; time::RationalTime duration; };
   // Builds peaks for a whole file (mono mix at 8 kHz, one bucket per 10 ms), calling progress and stopping when cancel() is true.
   [[nodiscard]] std::optional<Peaks> BuildPeaks(Source&, const std::function<bool()>& cancel, const std::function<void(double)>& progress);
   [[nodiscard]] bool WritePeaks(const std::filesystem::path&, const Peaks&);
   [[nodiscard]] std::optional<Peaks> ReadPeaks(const std::filesystem::path&);
   ```
   File: `<cache>/peaks/<fingerprint>.pk` with a header (magic, version, buckets_per_second, count) and the two int8 arrays. Read the file back in the test and compare.
2. A job (R7) per media, started when media is imported or first drawn, throttled to one at a time (use the existing job runner; check the kind string `"peaks"` so the Jobs panel names it).
3. `Session::peaks(mediaId)` returns a shared `std::shared_ptr<const Peaks>` (cache in memory). Emit `peaksReady(mediaId)`; `TimelineItem` repaints.
4. Paint: for an audio clip box, for each pixel column compute the source time span the column covers (take speed, `reversed` and `source_in` into account — look at how the ramp band computes times), take the min/max over the buckets in that span, draw a vertical line. Fill below with a darker colour. Skip when the clip is narrower than 4 px.
5. Tests: native `PeaksOfASineHaveItsAmplitudeAndRoundTripThroughTheirFile` (use the synthetic source at `synthetic:` with a known tone; peaks ≈ amplitude); `CancellingStopsPeakBuilding`. App: after import the media has peaks within a few seconds (`qWaitFor`) and the timeline paints without crashing.

### B9.2 Video thumbnails
1. `media/Thumbnail.h (new, created in B6.5)` provides `PosterFrame`; add `FramesAt(path, times, width)` to read several frames with one open `Source`.
2. `app/ThumbnailStrip.h (new)`: for a clip box of width W and height H (the picture part), the needed times are `source_in + k * (H * aspect / pixels_per_second)`; ask `Session::thumbnailAt(mediaId, sourceSeconds)` which returns a cached `QImage` or an empty one while a job fills it (quantise times to a 1 s grid so the cache hits).
3. Paint: draw the tiles left to right inside the clip box; fall back to the clip colour while loading. Preference `timeline.show_thumbnails` (new, R10, default on).
4. Tests: app — `ClipsShowThumbnailsOnceTheyAreMade` (images appear in the cache after a wait); performance — a timeline with 1,000 clips must not stall painting: measure `paint` time with `bench` or a stopwatch test and record in `PERFORMANCE_LOG.md`.

### B9.3 Cache bar
1. `PlaybackEngine::CachedFrames(in, count)` already returns which frames of the range are in the render cache. Add `Session::cachedRanges(visibleStart, visibleEnd)` → list of `[start, end]` seconds, computed at most twice a second (a `QTimer`) for the visible range, from the **program** engine only.
2. Paint a thin green line under the ruler for cached ranges (yellow for ranges that have a stored but stale hash if you add that later).
3. Command `timeline.render_in_to_out` (`Enter`): runs `PlaybackEngine::PreRender(in, out, cancel, progress)` as a job (R7). The job fills the cache, the bar turns green.
4. Tests: app — `RenderingInToOutFillsTheCacheBar`: set marks, trigger, wait, assert `cachedRanges` covers the marks.

### B9.4 Caption lane
1. Rows in `ui/TimelineView`: add an optional caption row (`TrackRow` with a new kind) below the video tracks when the sequence has a caption track; the layout test `TimelineLayoutStacksTracksLikeAnEditor...` shows how rows are tested.
2. Paint cues as boxes with their text; click selects the cue (`captionSelect`); drag the edges to retime (use the existing `captionUpdate`).
3. Tests: ui — layout includes the row; app — dragging an edge changes the cue's end by the right amount and undo restores it.

**Docs/parity**: waveform_generation (audio), UI-TIMELINE-002; CACHE-001's "no status UI" limit goes.

---

## WP-B10. Copy and paste of effects, paste attributes, effect presets

**Problem.** Copying a clip copies its effects (`edit.copy`, `edit.paste`), but there is no way to copy *only* effects, apply them to other clips, remove them in bulk, or save a stack as a preset.

**Files to read first**: `ui/EditPlanner.cpp` (`SpecOf`, the part of `PlanPlace` that re-creates effects after `InsertClip`), `ui/Inspector.cpp` (`PlanAddEffect`, the parameter edit planners), `core/commands/Command.h` (`AddEffectPayload`, `EffectParameter`, `SetKeyframePayload`).

**Steps**

1. **Extract the helper.** The code in `PlanPlace` that turns `ClipSpec::effects` into `AddEffect` (+ keyframes + masks) commands should be a free function `AppendEffectCommands(Builder&, owner_kind, owner_id, effects, time_offset)`. Refactor first, run the suite (no behaviour change), commit.
2. **Planners** in `ui/Inspector.cpp` (or `ui/EffectClipboard.h (new)`):
   - `PlanPasteAttributes(ctx, std::vector<timeline::Effect> source, std::set<std::string> target_clips, PasteOptions)`; `PasteOptions { bool motion, bool opacity, bool colour, bool effects; bool replace_existing; bool scale_to_clip_length; }`. Keyframes are shifted from the source clip's start to each target's start, and truncated or scaled to the target length (`scale_to_clip_length`).
   - `PlanRemoveAttributes(ctx, clips, options)`.
3. **Session**: `effectClipboard_` (vector of `timeline::Effect`), invokables `copyEffects()`, `pasteEffects()` (`edit.paste_attributes`, `Ctrl+Alt+V`), `pasteEffectsDialog()` (checkbox dialog `PasteAttributesDialog.qml (new)`), `removeEffects(options)`.
4. **Presets.** `ui/EffectPresets.h (new)`: `Save(name, effects)` writes `<config>/effect_presets/<name>.json`:
   ```json
   {"version": 1, "name": "Warm look", "effects": [{"type": "color_adjust", "preset": "", "parameters": [{"name": "temperature", "components": [12.0]}]}]}
   ```
   Keyframed parameters are stored as `{"keys": [{"t": 0.0, "components": [..], "interpolation": "linear"}]}` relative to the clip start. `Load` validates against the registry (unknown type → skipped with a warning). `Session::effectPresets()` lists them; the Effects panel gets a "Presets" tab; double-click applies to the selected clip as one undo step (`PlanApplyPreset` = `AppendEffectCommands`).
5. **Tests**: `ui_tests.cpp` — paste attributes to two clips of different lengths with keyframes; remove; preset round trip; unknown type skipped; one undo step each; the target's own other effects untouched unless `replace_existing`. `app_tests.cpp` — copy from clip A, select B, paste: B's inspector lists the same effects with the same values.

**Check by hand.** Select "Bars", add Color Adjust (temperature 20) and a vignette; `Ctrl+C`... no: use `Edit > Copy Effects`; select "Counter"; `Ctrl+Alt+V`: it shows the same two effects. Save as preset "Warm"; apply to "Sunset" from the Effects panel.

**Docs/parity**: `effect_presets`; UI-EFFECTS-002 (copy/paste part). Curve drawing and colour widgets are in C5.

---

## WP-B11. Rate stretch, sync lock, small editing additions

1. **Rate stretch tool** (`R`). Planner `PlanRateStretch(ctx, clip_id, edge, to_time)` in `ui/EditPlanner.cpp`: dragging the tail (or head) to a new time changes the clip's *playback rate* so the same source range fills the new length (`rate = (source_out − source_in) / new_duration`), keeping the opposite edge fixed. Emit `SetClipSpeed` (look at `Session::setClipSpeed` for how it fills the payload, including the audio partner and `maintain_pitch`) then `MoveClip` if the head moved. Refuse: clip with a speed ramp (the store refuses; say "remove the ramp first"), rate outside `0.01..100`, overlap with a neighbour unless the Insert/ripple mode is on (reuse the ripple code in `PlanTrim`). Add `Tool::RateStretch` handling in `TimelineItem::BeginDrag` (the `tool == "ripple"` branch is the model); tool id `tool.rate_stretch`, key `R`. Tests: `ui_tests.cpp` (length, rate, linked partner, refusals, undo); app (drag changes the clip length, speed badge shows). Parity: `rate_stretch`.
2. **Sync lock.** Using B5's column: when a ripple operation (`PlanTrim` Ripple mode, `PlanDelete(ripple)`, `PlanPlace(Insert)`) shifts clips on some tracks, it must also shift clips on **sync-locked** tracks that are not otherwise being edited (in Premiere sync lock keeps tracks aligned). Today `EditContext::ripple_all_tracks` does this for all tracks; make it per-track: a track ripples if it is edited, targeted-and-ripple, or `sync_lock`. Tests prove a sync-locked track shifts with a ripple delete elsewhere and an unlocked one does not.
3. **Clip name / label colour.** `RenameClip` is missing as a command? Check `InsertClipPayload.name`; there is no `SetClipName`. Add `SetClipLabel {id, name, color}` (R1, schema `clips.label_color`), double-click a clip name to rename, right-click for colour. Tests as R1.
4. **Gap handling.** `edit.ripple_delete` on an empty area (a gap) removes the gap: `PlanRemoveGap(ctx, track, at)`; add tests and a timeline click behaviour (select gap, `Shift+Delete`).
5. **Go to next/previous marker, gap, clip start/end** — small `ui/` functions with tests plus commands.
6. **Freeze frame.** `timeline.freeze_frame` (`Shift+R`? choose a free chord): `SetSpeedRamp` with a hold segment at the playhead — see `ui/Ramp.h` `PlanApplyRamp` and the existing freeze helper `rampFreeze`; command exposes it without opening the editor.

**Tests/check**: per item above; run the full suite for regressions in `LinkedClipsAreTrimmedTogether...` and ripple tests.

---

## WP-B12. Interchange and project-manager dialogs

**Problem.** `interchange/` (OTIO, EDL, Final Cut 7 XML) and `core/project/Consolidate.h` are complete engines with round-trip tests. No menu reaches them.

**Files to read first**: `interchange/Interchange.h` (`FromProject`, `ApplyToProject`, `Report`, `Issue`), `interchange/Otio.h`, `Edl.h` (`EdlOptions`), `FcpXml.h`, `core/project/Consolidate.h`, `app/SessionExport.cpp` (an example of a Session part with dialogs and jobs), `app/qml/ExportDialog.qml`.

**Steps**

1. **Export timeline.** `app/SessionInterchange.cpp (new)` (R4, R12): `exportTimeline(formatId, pathUrl)` with `formatId` in `otio|edl|fcp7xml`. Build `interchange::Report report; auto timeline = interchange::FromProject(*store_, sequence_id_, report);` then `WriteOtio/WriteEdl/WriteFcpXml`, write the text with the file helper `WriteText` (check it writes atomically; if not, write to a temporary name and rename). EDL needs `EdlOptions{title}`. Return the report as text and issue counts to QML.
2. **Report dialog.** `app/qml/InterchangeReportDialog.qml (new)`: a list of issues with severity icons (Note/Warning/Error), the `where` and the message, and "Copy to clipboard". Always shown after export if `report.Lossy()`, otherwise a status line.
3. **Import timeline.** `importTimeline(pathUrl)`: choose the reader by extension (`.otio`, `.edl`, `.xml`); an EDL needs a frame rate and drop-frame flag — ask in the dialog (default the current sequence's). Run the read and `ApplyToProject` **in a job** (R7) with `ApplyOptions{id_prefix = "imp1-"}` (a counter so two imports do not collide).
4. **Fix the timestamp default.** `ApplyOptions::timestamp_utc` defaults to the fixed 2026 stamp (see WP-A1): use `cutline::clock::NowUtc()` at the call site, and change the default in the header to empty with a "stamp at the call" comment.
5. **Make an import one undo step.** `ApplyToProject` runs each command separately (`store.Execute`), so an import is hundreds of undo steps. Add `ApplyToProject(..., options.group_label)`: when set, collect the envelopes and call `ExecuteGroup` in chunks? A single group of thousands of commands is fine for SQLite but the in-memory history entry will be large; implement with one group and measure. Test: import a 200-clip OTIO, press undo once, the project is as before.
6. **Menu entries**: File > Export > Timeline as OTIO / EDL / Final Cut 7 XML; File > Import > Timeline.
7. **Project manager.** `Session::collectProject(destinationUrl, optionsMap)` → job (R7) that fills `ConsolidateOptions{destination, sequence_ids, include_unused_media, copy_package}` and the `progress`/`cancel` hooks, calls `project::Consolidate(*store_, options)` and returns `ConsolidationReport::ToText()`. Dialog `ProjectManagerDialog.qml (new)`: destination folder, "Selected sequences / all", "Include unused media" and "Copy the project too" checkboxes, a size estimate from `PlanConsolidation` (sum of `duration`-based file sizes — use file sizes of `MediaUse::source`), progress, result list. A second command "Verify collected folder" runs `VerifyManifest(manifest, root)`.
8. **Tests**: native tests exist for the engines. New: `app_tests.cpp` — export OTIO of the demo then import it with a prefix into a fresh project and compare clip counts, durations, track counts (`session` getters or the store); the EDL path asks for a rate; the project manager copies two fixtures and the manifest verifies (skip without fixtures).

**Check by hand.** Export the demo as EDL: open the `.edl` in a text editor — events with reel names, record timecodes. Import it into a new project: same cuts, media offline (EDL has no files) — relink with B6.4.

**Docs/parity**: IO-001/IO-002/PROJECT-001 become `[x]` only when this ships; `project_archive`, `UI-IO-001`.

---

## WP-B13. Recovery on open and the unclean-shutdown notice

**Problem.** `ProjectStore::InspectPackage` and `RecoverPackage` exist and are tested; `Session::openProject` calls neither. A damaged package is just an error message. Autosave (WP-A3 step 1) exists but nobody is told that a crash happened.

**Files to read first**: `core/project/ProjectStore.h` (`RecoveryReport`, `RecoveryOptions`), the recovery tests in `store_tests.cpp` (`RecoveryFromAnyRevisionBoundaryLandsOnThatRevisionExactly`, `ATornOrMissingRecordStopsRecoveryAtTheLastGoodRevisionAndSaysSo`, `WithNothingToRebuildFromRecoveryFailsClearlyAndTouchesNothing`), `Session::openProject`.

**Steps**

1. **Open path.** In `Session::openProject(folder)`:
   1. `auto report = ProjectStore::InspectPackage(path);`
   2. If `report.database_healthy` and `report.reachable_revision <= report.database_revision` → `OpenPackage` as today.
   3. Otherwise emit `recoveryRequested(reportText, canRecover)` where the text is `report.notes` joined, and keep the path in `pending_recovery_path_`. Do not open.
2. **Dialog.** `app/qml/RecoveryDialog.qml (new)`: shows the notes in plain words ("The project database could not be opened. The newest snapshot is revision 250; 12 later edits can be replayed."), buttons **Recover** (calls `Session::recoverProject()`), **Open anyway** (only when the database is healthy but behind the journal: opens without roll-forward), **Cancel**.
3. **Recover.** `Session::recoverProject()`: `RecoveryReport out; auto store = ProjectStore::RecoverPackage(path, RecoveryOptions{roll_forward = true}, &out);` then `Open(std::move(store))` and show `out.notes` in a summary message (quarantine folder path included). Run it in a job for big projects.
4. **Unclean shutdown marker.** On `Open`, create `<package>/session.lock` containing the time (use `clock::NowUtc`); delete it in `closeProject` and in `~Session`. If the file exists when opening, show a notice "Cutline did not close properly last time. Your edits up to the last autosave/commit are safe. Open the History panel to review." (informational only — WAL makes the database consistent) and, if a snapshot newer than the database exists (it never should), offer recovery. Tests below.
5. **Tests**
   - `store_tests.cpp` has the engine; add `AHealthyPackageNeedsNoRecoveryAndAnOldDatabaseWithALongerJournalDoes` if not covered by `InspectPackage` tests.
   - `app_tests.cpp`: `ADamagedPackageOffersRecoveryAndRecoversToTheLastGoodRevision`: create a project, make 5 edits, close; corrupt `project.db` (overwrite its first 100 bytes with zeros); `openProject` returns false and emits `recoveryRequested`; `recoverProject()` opens it at the last good revision with the 5 edits; the quarantine folder exists. `AnUncleanShutdownIsNotedAndACleanOneIsNot` (leave `session.lock` behind on purpose).
6. **Check by hand.** Make edits, kill the process in Task Manager, start again and open the project: the notice appears and every edit is there. Corrupt the database with a hex editor on a copy: recovery works.

**Docs/parity**: RECOVERY-001 back to `[x]`; `crash_recovery` entry gets `implemented`; delete `UI-RECOVERY-001` from `TODO.md`.

---

Continue with [C-effects-audio-colour-delivery.md](C-effects-audio-colour-delivery.md).
