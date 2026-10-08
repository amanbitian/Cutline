# Cutline implementation guide for new engineers

**Written for:** an intern or junior engineer who has built C++ before but has not seen this repository. Every work package says which files to open, what to write, how to test it, and how to look at it running. Nothing here assumes you remember another document; where a decision was already made, the reason is given.

This guide was written on 2026-10-08 from the source code (not from the older status documents). It is split into four files so each can be read on its own:

| File | Contents |
|---|---|
| **This file** | How to work in the repository, the recipes every work package reuses, the order of work, and **Phase A** (small defects, four work packages) |
| [guide/B-editing-surface.md](guide/B-editing-surface.md) | **Phase B**: source monitor, patching and targeting, project panel, sequences, timeline drawing, effect copy/paste, interchange dialogs, recovery and autosave |
| [guide/C-effects-audio-colour-delivery.md](guide/C-effects-audio-colour-delivery.md) | **Phase C**: track mattes, new effects, new audio effects, colour widgets, titles, export features, GPU kernels |
| [guide/D-large-programmes.md](guide/D-large-programmes.md) | **Phase D**: plug-in hosts, AAF, offline AI, colour management, camera formats, collaboration, VR |

Conventions used in all four files:

- A path in backticks followed by **(new)** is a file you will create, for example `ui/SourceMonitor.h (new)`. A path without it already exists; open it before you edit.
- **Sizes** are rough, for one engineer who has finished Part 1 of this file: **S** is up to a day, **M** two to four days, **L** one to two weeks, **XL** more than two weeks and needs a design review before it starts.
- "Run the suite" means the commands in section 1.4. "Check docs" means `node scripts/check-docs.js`.
- Test names in this repository are long sentences in `CamelCase` that say what must be true (`ARampIsRefusedWhereItCouldNotPlay...`). Follow that style; a name that states the behaviour is half the documentation.

---

## Contents

1. [Getting set up and working day to day](#1-getting-set-up-and-working-day-to-day)
2. [Rules that apply to every change](#2-rules-that-apply-to-every-change)
3. [Recipes](#3-recipes)
4. [The order of work and what depends on what](#4-the-order-of-work-and-what-depends-on-what)
5. [Phase A: small defects](#5-phase-a-small-defects)

---

## 1. Getting set up and working day to day

### 1.1 What you need

| Item | Where it comes from | How to check |
|---|---|---|
| Windows 10/11 with Visual Studio 2022 or later (C++ workload) | Installed by you | `scripts\vsenv.bat` finds it through `vswhere` and prints the compiler version |
| CMake and Ninja | Bundled with Visual Studio, or on `PATH` | `scripts\build.bat` finds them itself |
| FFmpeg shared build (LGPL) | Unpack so that `.tools/ffmpeg/include`, `lib`, `bin` exist | CMake prints whether it found FFmpeg; without it (or without the generated fixtures) about 33 media tests skip |
| Qt 6.8.3 MSVC 2022 64-bit | `py -m pip install aqtinstall`, then `py -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O .tools\qt` | `.tools\qt\6.8.3\msvc2022_64\bin\Qt6Core.dll` exists |
| whisper.cpp and a model (only for transcript work) | `.tools/whisper` holds the executable folder and `ggml-base.en.bin` | One speech test skips without it |
| Node.js (for the documentation and parity scripts) | nodejs.org | `node --version` |

Python is **not** required. Every script in `scripts/` is a batch file or Node.

### 1.2 First build

```text
scripts\build.bat                 core + native tests    -> build\native
scripts\build-app.bat             core + Qt application  -> build\app   (needs Qt)
```

The first build takes several minutes. Do both once, then use incremental builds.

### 1.3 First run

```text
scripts\run-app.bat --demo        opens a ready-made project (Bars, Counter, Sunset on a timeline)
```

Spend twenty minutes using it before you read any code: import a file, split it with `C`, trim with `B`, add an effect from the Effects panel, add a transition, open the Graphics workspace (`Alt+Shift+8`), export (`Ctrl+M`). You cannot judge your own change without knowing what normal looks like.

Useful switches (full list at the top of `app/main.cpp`):

```text
--demo                       build the demo project
--open <folder>              open a .cutline package
--do trigger:<command id>    run a command after opening (repeatable)
--do select:<clip name>      select a clip
--do seek:<seconds>          move the playhead
--do tool:<name>             pick a timeline tool
--do workspace:<name>        switch workspace
--screenshot out.png         save the window and exit
```

### 1.4 The commands you will run all day

| Purpose | Command | Notes |
|---|---|---|
| Build core and native tests | `scripts\build.bat` | Debug build. Add a target name to build one thing |
| Run the native suites | `scripts\test.bat` | Builds, then runs CTest; sets the fixture and golden folders for you |
| Build the application and the app tests | `scripts\build-app.bat` | Needed for anything under `app/` |
| Run the application tests | `build\app\cutline_app_tests.exe` | Put Qt's `bin` and `.tools\ffmpeg\bin` on `PATH`, set `QT_QPA_PLATFORM=offscreen` and `QT_QUICK_BACKEND=software` |
| Run **one** native suite directly | `build\app\cutline_ui_tests.exe` | **Set both** `CUTLINE_FIXTURE_DIR` (to `build\app\fixtures`) and `CUTLINE_GOLDEN_DIR` (to `tests\golden`) or media tests skip and golden tests fail |
| Run only some tests of a suite | set `CUTLINE_ONLY=Marker` (any text): only cases whose name contains it run; `CUTLINE_TRACE=1` prints each name before it runs, so a crash says where | Works for every native suite and for the app suite |
| Screenshot of the application | `scripts\shot.bat out.png --demo --do select:Bars` | Software renderer; QML errors go to stderr |
| Release benchmarks | `scripts\bench.bat`, `--gpu`, `--color`, `--flow`, `--range`, `--playback` | Debug timings are meaningless |
| Check that documentation matches the code | `node scripts/check-docs.js` | Counts, links, paths, mojibake |
| Check that parity claims have real tests | `node scripts/cross-check-parity.js` | Add `--apply` after editing the assessment table |
| Regenerate the parity report | `node scripts/generate-parity-report.js` | Run after `--apply` |
| JavaScript tests | `node tests/run.js` | Only the prototype helpers and the parity aggregation |

`CUTLINE_STRICT=1` turns a test that skipped because a prerequisite was missing (a fixture, FFmpeg) into a failure. Use it before you say a media test passed.

### 1.5 Habits that save you hours

1. **Write the failing test first** whenever the behaviour can be tested without a window. Every rule in `ui/` was written that way, which is why the Qt layer is thin.
2. **Never write a number in a document by hand** if `check-docs` can check it. If you add a new kind of number (a count of panels, say), teach `scripts/check-docs.js` to verify it.
3. **Read before you write.** For every work package, open the "files to read first" and run the existing tests that touch them. If a test you did not touch starts failing, you have found a coupling; understand it before you change anything else.
4. **Small commits.** One recipe step, one commit. The message says what and why. The repository's history so far is two commits; you are starting the habit for everyone.
5. **Windows paths and backslashes.** Some editors and shells corrupt backslashes. Check `git diff` on any file that contains a path with them.

---

## 2. Rules that apply to every change

These come from the project's own principles and from defects found by earlier reviews. A pull request that breaks one is sent back.

1. **Every mutation of a project is a command.** Nothing outside `core/project/ProjectStore.cpp` writes to the database. The UI never runs SQL that changes rows. If you need a change that no command can express, you add a command (recipe R1).
2. **Undo is free and must work.** Undo replays row-level changesets; you write no inverse. You **must** still add a test that does the edit, undoes it, redoes it and compares the project (`ValidateDatabase()` plus the rows you touched).
3. **One gesture is one undo step.** Compose several commands with `ProjectStore::ExecuteGroup` (the `ui::EditPlan` type does this). Never make the user press undo three times for one thing they did once.
4. **Exact time.** Use `time::RationalTime`, never `double` seconds, for anything stored or compared. Convert to `double` only at the QML boundary.
5. **Preview and export share code.** Anything that changes pixels or sound must be implemented once, in `render/` or `audio/`, and reached by both the monitor and `exporter/`. A feature that only the monitor shows is a defect.
6. **Anything that changes rendered output for existing projects needs a render version.** See `core/model/RenderVersion.h`. If a project saved yesterday would look different today, you broke the rule.
7. **No allocation, locks or I/O on the realtime audio callback** (`audio/WasapiSink.cpp`). If you touch audio, read that file's header first.
8. **The GUI thread never waits for decode, composition or disk.** Use `ui::JobRunner` (background jobs the user can see and cancel) or an existing worker.
9. **Failure is visible.** An unsupported effect, a missing file or a refused edit tells the user why (a status message, a job message, a report entry). It never silently does nothing.
10. **Tests that depend on something missing must skip, not pass.** Use `SKIP_UNLESS(condition, "reason")` (a prerequisite that should exist; it fails under `CUTLINE_STRICT=1`) or `SKIP_INAPPLICABLE(condition, "reason")` (the premise does not hold on this machine) from `tests/native/TestHarness.h`, so the output says what did not run.
11. **Add tests to the right place**: pure rules to `tests/native/*_tests.cpp`; anything that needs the window to `tests/app/app_tests.cpp`. New `.cpp` files go into the lists in `CMakeLists.txt` (section 3, R12).
12. **Docs move with code.** Section 2.1 lists exactly what to update.

### 2.1 Definition of done (copy this into every pull request)

- [ ] The feature works in the application (not only in a test): you looked at it, or took a screenshot with `scripts\shot.bat`.
- [ ] New or changed behaviour has named native tests; anything involving the window also has an app test.
- [ ] Undo/redo of the feature is tested.
- [ ] The whole native suite and the app suite pass with `CUTLINE_STRICT=1`.
- [ ] `node scripts/check-docs.js` passes.
- [ ] If a parity capability changed status: the entry in the `ASSESSMENT` table of `scripts/cross-check-parity.js` is updated with real test names, then `--apply` and `generate-parity-report.js` were run, and `node scripts/cross-check-parity.js` passes.
- [ ] `IMPLEMENTATION_STATUS.md` (the row for the feature), `TODO.md` (tick or reword the item), and, if the architecture changed, `ARCHITECTURE.md` are updated. If you added a user-visible command, panel or preference, `README.md`'s feature table is updated too.
- [ ] If you touched a per-frame, per-sample or interactive path: a Release benchmark was run and the numbers recorded in `PERFORMANCE_LOG.md` with the raw output in `perf/`.
- [ ] No new compiler warnings (the core compiles clean at `/W4`).

---

## 3. Recipes

Each recipe is a checklist you will use several times. Work packages refer to them as R1, R2, and so on.

### R1. Add a command (a new kind of project edit)

Use this when no existing command can express the change. Study the last command added, `RenameMulticamAngle`, as the smallest complete example; `grep -rn RenameMulticamAngle --include=*.cpp --include=*.h .` lists every place it appears.

1. **`core/commands/Command.h`**
   1. Add the enumerator to `enum class CommandType` (keep the groups; add yours with its neighbours).
   2. Add a payload struct `YourThingPayload final { ... }` next to the related payloads. Use `std::string` ids, `time::RationalTime` for times, `std::optional` for "may be absent".
   3. Add the payload to the `CommandPayload` `std::variant` list.
2. **`core/commands/Command.cpp`**
   1. Add a row to the `Descriptors()` table: `{CommandType::YourThing, "area.verb_noun", "Label Shown In Undo Menu", IndexOf<YourThingPayload>()}`. **Increase the `std::array<Descriptor, N>` size by one.** The wire name must be unique and never changes once released.
   2. Add `std::string ToJson(const YourThingPayload&)` (the journal record; use `json::Object().Add(...)`).
   3. Add `void Check(const YourThingPayload&)` for rules that need no database: non-empty ids (`RequireIdentifier`), ranges (`Require(cond, "message a person can read")`).
3. **`core/project/ProjectStore.cpp`**
   1. Add `void Apply(sqlite3*, const CommandEnvelope&, const commands::YourThingPayload&)`. Look rows up with `RequireExists`/`Statement`; throw `std::runtime_error` (via the helpers) with a message that says what is wrong. Use prepared statements with bound parameters; never build SQL from user text.
   2. If the command edits clip contents, honour the **track-lock policy**: call `RequireTrackUnlocked(database, track_id)` (or `RequireTransitionUnlocked`) exactly as the `InsertClip` and `TrimClip` handlers do.
   3. If the command can move or shorten a clip next to a transition, call `DetachBrokenTransitions(database, clip_ids)` and rely on `RequireValidTransition`, as the clip commands do; `ValidateDatabase()` applies the same rule.
4. **Schema** (only if you need a new column or table): recipe R2.
5. **Tests**, in `tests/native/store_tests.cpp`, using its `Harness` (an in-memory store):
   - the happy path and the resulting rows;
   - every refusal in step 2.3 and 3.1 (assert `CHECK_THROWS`);
   - **undo then redo** restores the exact rows (copy an existing undo test, for example `UndoReversesARippleDeleteIncludingTheShift`, and adapt it);
   - the command inside `ExecuteGroup` with another command, and a failure in the second command taking back the first.
   Also add the case to `tests/native/core_tests.cpp` if you added validation rules (`Validate(...)` throws).
6. **Documentation**: the command count appears in docs; `check-docs` will fail until you update every "N typed commands" sentence (`README.md`, `ARCHITECTURE.md`, `GAP_ANALYSIS.md`, `IMPLEMENTATION_STATUS.md`).
7. **How to check**: `scripts\test.bat`; then `node scripts/check-docs.js`.

### R2. Change the database schema

Every change is a **migration**; the project format never breaks.

1. `core/project/Schema.h`: increase `kSchemaVersion`.
2. `core/project/Schema.cpp`:
   1. For a **new table or column in a new project**, edit the `CREATE TABLE` text in `kCoreTables`/`kEffectTables` so a freshly created project has it.
   2. In `MigrateSchema`, add a new block **at the top of the chain**, `if (from_version <= <old version>) { ... }`, using `CREATE TABLE IF NOT EXISTS` or `ALTER TABLE ... ADD COLUMN ... DEFAULT ...` guarded by a `pragma_table_info` check (copy the v12→v13 block).
   3. If you added a table that holds rows owned by something else, add delete triggers in `kCascadeTriggers` and extend `ValidateDatabase()` (search for "orphan").
3. **Add the table to `RecordedTables()` in `core/db/ChangeSet.cpp`.** Undo only reverses the tables that list names; a table missing from it will silently not undo. (`command_journal` is deliberately absent.)
4. Tests in `tests/native/store_tests.cpp`: copy `AProjectFromBeforeMulticamGetsEmptyMulticamTablesAndMulticamWorkAfterwards`: build a database at the previous version (the tests keep helpers for this; search for "FromBefore"), open it with the new code, assert the new table or column exists with the default, and that a command using it works.
5. Update the schema version in `README.md`, `ARCHITECTURE.md`, `IMPLEMENTATION_STATUS.md` ("Project schema (v13)").

### R3. Add an editing rule ("planner") the UI can call

Rules that decide what a gesture means live in `ui/` as pure functions returning an `ui::EditPlan` (a label, a list of commands, or a refusal). They need no window, which is why they are easy to test.

1. Open `ui/EditPlanner.h` and `ui/Transitions.h` as models. A planner takes an `EditContext` (the sequence snapshot, an id generator, a media-duration lookup, the ripple/linked preferences) and returns `EditPlan::Refuse("reason")` or a plan whose `commands` the session runs as one undo step.
2. Put the function in an existing `ui/*.cpp` if it belongs there, or in `ui/YourThing.h/.cpp (new)` and add both to the `cutline_core` source list in `CMakeLists.txt` (R12).
3. Write the tests first in `tests/native/ui_tests.cpp`: build a small `timeline::Sequence` by hand (search for helper names such as `MakeSequence` or `Fixture` near the existing planner tests), call the planner, assert the commands, then **execute the plan against a real `ProjectStore`** (the existing tests show how) and compare the resulting sequence.
4. Refusals: each refusal string is user-facing. Test the common ones (locked track, nothing selected, no room).

### R4. Expose a planner or a read model to QML through `Session`

`Session` (`app/Session.h`, split across `app/Session*.cpp`) is the only object QML talks to. QML sees it as `session` (and `appSession` where a component needs an id).

1. In `app/Session.h`: add `Q_PROPERTY(...)` with a `NOTIFY` signal for read-only state QML binds to, and `Q_INVOKABLE` methods for actions. Return `QVariantList`/`QVariantMap` for lists and records (see `media()` and `captionTracks()` for the shape).
2. Implement in the `Session*.cpp` file closest in subject, or in a new `app/SessionYourThing.cpp (new)`. A new file must be added to **both** source lists in `CMakeLists.txt` (the `Cutline` QML module and `cutline_app_tests`).
3. Run a plan with `Apply(plan)`; run a single command with `Run(type, payload)`. Both reload the sequence snapshot and emit `sequenceChanged`. Use `NewId("prefix")` for ids and `EditContextFor()` for the context. Do **not** build timestamps by hand (see WP-A1).
4. After any change that alters what QML shows, make sure the matching signal is emitted (`mediaChanged`, `inspectorChanged`, ...). A property that never notifies is a silent bug.
5. Tests: `tests/app/app_tests.cpp`. Use its `Fixture` (it builds the demo project, a window and a `TimelineItem`). Call your invokable, assert on the property, undo with `session.trigger("edit.undo")`, assert it came back.

### R5. Add a panel

1. **Register the panel**: `ui/Layout.cpp`, `BuiltInPanels()`: `r.Add({"your_panel", "Display Name", min_width, min_height});`.
2. **Write the QML**: `app/qml/panels/YourPanel.qml (new)`. Copy the structure of `ExportsPanel.qml` (a root `Item` with `property string panelId`, bindings to `session`). Reuse `Chip.qml`, `Check.qml`, `NumberBox.qml`, `Theme` colours; never hard-code colours.
3. **Map it**: `app/qml/PanelGroup.qml`, the `switch` that picks the `source` of the `Loader`.
4. **Add it to the CMake list**: `QML_FILES` in `CMakeLists.txt`.
5. **Let people open it**: `ui/Shortcuts.cpp` `BuiltInCommands()`: `add("panel.your_panel", "Your Panel", "Window", "Show or hide ...");`; and `app/qml/Main.qml` — the Window menu's `model:` list of panel ids.
6. **Put it in a workspace** (optional): `ui/Layout.cpp` workspaces section. Changing a built-in workspace changes what people see after "Reset Workspace"; update the test `EveryBuiltInWorkspaceIsAValidLayoutAndArrangesWithinTheWindow` expectations if it counts panels.
7. **Tests**: `ui_tests.cpp` (the panel is registered and in the layout), `app_tests.cpp` (opening it through `session.trigger("panel.your_panel")` makes it present in `session.workspaces().layout()`; a button does what it says).
8. **Count**: `README.md` and `ARCHITECTURE.md` list the panels; update them.

### R6. Add a command to the keyboard and menus (an "application command")

(This is different from R1; application commands are things you can bind a key to. They run code in `Session::trigger`.)

1. `ui/Shortcuts.cpp`, `BuiltInCommands()`: `add("area.verb", "Label", "Category", "One sentence description.", {"Ctrl+Alt+Q"});`. A default chord may not be used by another command; `TheBuiltInCommandsAreUniqueHaveNoSharedDefaultAndCanBeSearched` checks this.
2. `app/Session.cpp`, `Session::trigger`: find the long `if (id == ...)` chain and add your branch. Keep the branch short: call a method that does the work.
3. `app/qml/Main.qml`: add `CommandItem { cmd: "area.verb"; label: qsTr("Label") }` to the right menu.
4. Test in `app_tests.cpp`: `session.trigger("area.verb")` has the expected effect; `session.shortcutText("area.verb")` shows the chord.

### R7. Add a background job (analysis, rendering, scanning)

1. Use the job runner: `runner_->Run("kind", "Title", [..](ui::JobContext& context) { ... });` (see `Session::importMedia`). Inside, call `context.Progress(done, total, "message")` often, and stop when `context.Cancelled()` is true.
2. The job runs on a worker thread. It may read the project through `store_->mutex()`; it must **not** touch QObjects or QML directly. To tell the UI, `QMetaObject::invokeMethod(this, [...]{ ... }, Qt::QueuedConnection)`.
3. When the job produces an edit, it ends by running a plan on the GUI thread so that the whole result is one undo step (the stabiliser's job in `Session::analyseClip` is the model).
4. Tests: `ui_tests.cpp` has `JobsReportProgressCanBeCancelledAndFailuresAreRecordedNotThrown`; add one for your job's pure part. In `app_tests.cpp`, run the job and wait with `QTest::qWaitFor([&]{ return condition; }, milliseconds)`, as the existing tests do (search `qWaitFor` in the file).

### R8. Add a built-in video effect

A video effect is: a **descriptor**, a **function that changes a picture**, and (optionally) a **GPU path**.

1. **Descriptor**: `effects/EffectRegistry.cpp`, inside `BuiltInEffects()`, add `Video("your_id", "Display Name", "Category", {Scalar("amount", "Amount", default, Unit::Normalized, min, max), ...})`. Use `Vec2/Vec3/Vec4` for multi-component values. Make **the default a no-op** (dropping the effect on a clip must not change the picture).
2. **Picture function**: `render/Filters.cpp`. Write `void ApplyYourEffect(Layer& layer, const SampledEffect& effect)` (or the `Layer& scratch` overload for neighbourhood effects). Read parameters with `ScalarParameter(effect, "amount", default)`. Layers are **premultiplied float RGBA**; un-premultiply before colour math and re-premultiply after (look at `ApplyTint`). Use `ParallelRows(0, height-1, [&](int y){ ... })` so it uses all cores; a row body may write only its own row.
3. **Register it**: add the id to `FilterEffectTypes()` and a branch in `ApplyFilterEffect`.
4. **Neutral detection**: `render/D3D11Compositor.cpp`, `IsNoOpEffect`: add the line that says when the parameters do nothing. This keeps a neutral instance from forcing the whole frame onto the CPU.
5. **Tests**:
   - `tests/native/render_tests.cpp`: copy `PosterizeSnapsEachChannelToTheNearestLevel`; use `ApplyEffect("your_id", {{"amount", Value::Scalar(...)}}, SolidFrame(...))` and assert on exact pixels that you can compute by hand.
   - Add the id to the list in `EveryNewEffectHasADescriptorAndItsDefaultsLeaveThePictureAloneWhereTheyShould` if its default is neutral.
   - `tests/native/core_tests.cpp`, `EffectRegistryDescribesUniqueBuiltInsWithValidDefaults`: increase the expected count (`CHECK_EQ(descriptors.size(), ...)`).
   - A golden frame only for effects with a distinctive look (`CUTLINE_UPDATE_GOLDEN=1` writes it; review the picture).
6. **UI**: nothing — the Effects panel and the inspector are built from the descriptor. Check by `scripts\shot.bat fx.png --demo --do select:Bars --do effect:your_id`.
7. **Docs**: effect count (51 now) appears in several documents; `check-docs` will tell you which.
8. **GPU later**: see WP-C9. Until then the frame falls back to the CPU compositor and the status bar counts it.

### R9. Add an audio effect

1. **DSP function** in `audio/Dsp.cpp` and declaration in `audio/Dsp.h`: `struct YourSettings` and `void YourEffect(media::AudioBuffer&, const YourSettings&)`. **Rule from the header of `Dsp.h`:** the output at a sample may depend only on input within a finite window before (`memory`) and after (`lookahead`) it. This is what makes any block size give the same samples. A filter with unbounded memory must be run from silence over a long lead-in (see how `ApplyEq` is handled).
2. Add `Margins YourMargins(settings, sample_rate)` returning that window in samples.
3. **Mixer**: `audio/AudioMixer.cpp`: add the type to `IsStatefulEffect`, and a branch in `PlanStateful` (copy the `gate` branch): read parameters with `get("name", default)`, set `plan.margins`, and `plan.run`.
4. **Descriptor**: `effects/EffectRegistry.cpp`: `Audio("your_id", "Name", "Category", {...})`.
5. **Tests** in `tests/native/audio_tests.cpp`:
   - a numeric property of the output (an echo at 250 ms is a copy of the input at -6 dB 250 ms later);
   - **block independence**: render a span in one block and in many blocks (the existing `BlocksOfAnySizeTileToTheSameAudioAsOneLargeBlock` shows how) and compare;
   - the default is neutral;
   - bypass leaves the input bit-identical.
6. Update the effect-count assertion in `core_tests.cpp` as in R8.

### R10. Add a preference

1. `ui/Preferences.cpp`: declare it with `toggle(...)`, `choice(...)` or the numeric helper beside the others (key like `area.name`, default, limits, group, label, one-sentence help).
2. **Read it where it matters**: `session.prefs().GetBool("area.name")` in `app/`, or pass the value down into an engine config. A preference nothing reads is a bug (see WP-A3).
3. React to changes: `Session`'s constructor has `prefs_.Observe(...)`; add a case if the change must take effect immediately.
4. Tests: `PreferencesAreDeclaredTypedValidatedObservedAndOnlyDifferencesAreSaved` covers the mechanism; add one app test that changes the value and observes the behaviour.

### R11. Add a transition

1. `render/Transitions.cpp`: add a row to the table (`{"id", "Name", "Family", "Description", Shape::YourShape}`), add the `Shape` enumerator, and its arithmetic in `MixTransition` (a function of the two layers and progress 0..1).
2. Tests in `render_tests.cpp`: the existing test `EveryTransitionStartsAsTheOutgoingPictureAndEndsAsTheIncomingOneAndNeverLeavesTheRange` iterates over **all** kinds, so it checks yours automatically; add one that checks the midpoint property of your kind.
3. UI: the Effects panel's Transitions tab lists the table; nothing more.

### R12. Add a source file to the build

- **Core library**: `CMakeLists.txt`, the `add_library(cutline_core STATIC ...)` list. Add both `.cpp` files. Headers need no entry.
- **Application**: the `qt_add_qml_module(Cutline ... SOURCES ... QML_FILES ...)` list **and** the `qt_add_executable(cutline_app_tests ...)` list.
- **A new native test file**: add `cutline_add_test(cutline_yourname_tests tests/native/yourname_tests.cpp)` and a row in `IMPLEMENTATION_STATUS.md`'s suite table (`check-docs` requires a row for every suite).
- After editing `CMakeLists.txt`, run `scripts\build.bat` (it reconfigures).

### R13. Change documentation and parity

1. Update the prose where the change is described (see section 2.1).
2. Parity: open `scripts/cross-check-parity.js`, find the capability id (they are listed in `premiere-parity-report.md` and `parity/*.yaml`), edit or add its entry in `ASSESSMENT`: `id: ['implemented' | 'partial', ['TestName', ...], ['file/it/lives/in.cpp']]`. The named tests must exist in `tests/native`. Then:
   ```text
   node scripts/cross-check-parity.js --apply
   node scripts/generate-parity-report.js
   node scripts/cross-check-parity.js
   node scripts/check-docs.js
   ```
3. A test that exists only in `tests/app` cannot back a parity claim (the script reads `tests/native`). Put the rule in a native test.

### R14. Take a screenshot to show your work

`scripts\shot.bat build\my-change.png --demo --do select:Bars --do trigger:panel.your_panel`. Attach the image to the pull request. If the picture is blank or wrong, run with `QT_FORCE_STDERR_LOGGING=1` (the script already sets it) and read the QML warnings.

---

## 4. The order of work and what depends on what

```text
Phase A (anyone, in any order, all S/M)
  A1 timestamps   A2 effect alias   A3 dead preferences   A4 Markers panel
        |                                  |
        v                                  v
Phase B (editing surface)                (A3 autosave feeds B14)
  B1 source monitor  ->  B2 source marks + 3/4-point + replace  ->  B3 match frame
        |                         |
        |                         +-> B4 subclips + master clip properties
        v
  B5 patching and targeting (needs B2)
  B6 project panel: bins, relink, search, labels, thumbnails   (independent)
  B7 sequences: new, nest, tabs, breadcrumbs                    (independent)
  B8 adjustment layers and generators                           (independent, S)
  B9 timeline: waveforms, thumbnails, cache bar, caption lane  (needs R7 jobs)
  B10 effect copy/paste, paste attributes, presets             (independent)
  B11 rate stretch, sync lock, clip-level odds and ends
  B12 interchange and project-manager dialogs                   (independent)
  B13 recovery on open and autosave                             (needs A3)
Phase C (look and delivery)
  C1 track mattes (needs B7 nothing; needs R8)    C2 dip to colour (S)
  C3 new video effects (R8, batchable)            C4 new audio effects (R9, batchable)
  C5 colour wheel and curve widgets               C6 titles: roll/crawl, on-canvas text
  C7 export: size, image sequences, GIF, smart render, render and replace, watch folders
  C8 path aliases and relink polish               C9 GPU kernels for filters, keys, blends, masks
Phase D (large; each needs a design note first)
  D1 VST3   D2 OFX   D3 AAF/OMF   D4 offline AI   D5 colour management   D6 camera formats
  D7 collaboration and review      D8 VR
```

A sensible first month: **A1, A2, A3, A4** (learn the codebase on small things), then **B1 and B2** together (the biggest missing feature), then **B5**, **B13**.

### Effort table

| Package | Title | Size | Needs |
|---|---|---|---|
| A1 | Real timestamps in the journal | S | |
| A2 | The duplicate "Basic Color" effect | S | |
| A3 | Wire or remove ten dead preferences | M | |
| A4 | Markers panel and marker editing | M | |
| B1 | Source monitor | L | A1 |
| B2 | Source marks, three- and four-point, replace edit | M | B1 |
| B3 | Match frame and reverse match frame | S | B1 |
| B4 | Subclips and master-clip properties | M | B2 |
| B5 | Source patching and track targeting | M | B2 |
| B6 | Project panel: bins, relink, search, labels, thumbnails | L | |
| B7 | Sequences: create, nest, switch, breadcrumbs | L | |
| B8 | Adjustment layers, colour mattes and bars | S | |
| B9 | Timeline waveforms, thumbnails, cache bar, caption lane | L | R7 |
| B10 | Copy/paste effects, paste attributes, effect presets | M | |
| B11 | Rate stretch tool, sync lock, small editing odds and ends | M | B5 |
| B12 | Interchange and project-manager dialogs | M | |
| B13 | Recovery on open and autosave | M | A3 |
| C1 | Track mattes | M | |
| C2 | Dip to colour and transition handle display | S | |
| C3 | New video effects (a batch) | L | |
| C4 | New audio effects (a batch) | L | |
| C5 | Colour wheel and curve widgets | M | |
| C6 | Titles: roll, crawl, on-canvas text | L | |
| C7 | Export features | L | |
| C8 | Media path aliases | M | B6 |
| C9 | GPU kernels | XL | |
| D1-D8 | Large programmes | XL | design review |

---

## 5. Phase A: small defects

These four packages are small on purpose. They teach the command bus, `Session`, preferences, panels and the test suites while fixing real defects. Do them in order.

### WP-A1. Real timestamps in the journal  (size S)

**Problem.** Every command the application sends carries the fixed text `2026-01-01T00:00:00Z`. The store writes it into `command_journal.created_at_utc`, the per-revision journal files and the project's modified time. The audit trail and "last modified" are therefore meaningless.

**Where it happens** (find them all with `grep -rn "2026-01-01T00:00:00Z" app core`):

- `app/Session.cpp`: `newProject` (create command), `Envelope`, `undoTo` (undo and redo), the import job.
- `app/SessionAudio.cpp`, `app/SessionIngest.cpp` (twice).
- `core/project/Consolidate.cpp` (twice).
- `interchange/Interchange.h`: `ApplyOptions::timestamp_utc` defaults to the same fixed text; make the default empty and have `ApplyToProject` use `NowUtc()` when it is empty.
- Leave `app/demo_main.cpp`, `app/bench_main.cpp` and the tests alone: they want fixed values so output is reproducible.

**Steps**

1. Create `core/util/Clock.h (new)`:
   ```cpp
   #pragma once
   #include <string>
   namespace cutline::clock {
   // The current time in UTC as "YYYY-MM-DDTHH:MM:SSZ" (ISO 8601, seconds).
   [[nodiscard]] std::string NowUtc();
   }
   ```
   and `core/util/Clock.cpp (new)` implementing it with `std::chrono::system_clock`, `std::time_t` and `gmtime_s` (Windows) / `gmtime_r`. Add both to the `cutline_core` list (R12).
2. Replace the constants listed above with `cutline::clock::NowUtc()`. In `Session::Envelope` it is one line. `undoTo` calls `Undo("user", NowUtc())`.
3. Keep `Validate` requiring a non-empty stamp (`core/commands/Command.cpp`, "Command timestamp must not be empty").
4. Tests:
   - `tests/native/core_tests.cpp`: `NowUtcIsIso8601AndDoesNotGoBackwards`: the text is 20 characters, matches `dddd-dd-ddTdd:dd:ddZ` (write the check by hand; no regex library needed), ends in `Z`, and two calls in a row satisfy `a <= b` as strings.
   - `tests/app/app_tests.cpp`: after `session.trigger("timeline.add_marker")`, read `session.projectFolder()`, call `session.closeProject()`, reopen with `cutline::project::ProjectStore::OpenPackage(folder)` and run `SELECT MAX(created_at_utc) FROM command_journal` through `store->connection()` under `store->mutex()` (use `cutline::db::Statement`, as `Session.cpp` does). Assert it does not start with `2026-01-01`. (The demo project is built through `Envelope` too, so it is fixed by the same change.)
5. Search the tests for `2026-01-01` — if any application test asserts the fixed stamp, change it to assert the format instead.

**Check by hand.** Open the demo project, add a marker with `M`, close, then open `<project>.cutline/journal/` and read the newest `.json` file: `"timestampUtc"` must be today.

**Docs.** Delete the `JOURNAL-TIME-001` item from `TODO.md` ("Defects found by the code audit") and the matching sentence in `GAP_ANALYSIS.md`'s technical-debt list.

**Pitfalls.** Do not make `ProjectStore` stamp commands itself: replay and tests need to control the stamp.

---

### WP-A2. The duplicate "Basic Color" effect  (size S)

**Problem.** `grade` and `lumetri` are both registered as "Basic Color" in `effects/EffectRegistry.cpp`, so the Effects panel lists it twice. `lumetri` is a legacy id from the browser prototype (and borrows another product's name); old projects and `app/demo_main.cpp` still contain it, so it cannot be deleted.

**Steps**

1. Open `effects/EffectRegistry.h`. Add a field to `EffectDescriptor`: `bool listed{true};` (a legacy id that is still understood but not offered).
2. In `EffectRegistry.cpp`, mark `lumetri` as not listed: after building the descriptor, set `listed = false`. (Follow how `GpuVideo` wraps `Video` for a small helper, for example `LegacyVideo`.)
3. `ui/Inspector.cpp`, `EffectCatalogue`: skip descriptors with `!listed`.
4. Keep the descriptor count unchanged (the core test expects 51) and every existing use of the id (`Compositor.cpp`, `D3D11Compositor.cpp`) as it is.
5. Tests:
   - `ui_tests.cpp`: `TheEffectBrowserOffersEachEffectOnceAndHidesLegacyNames`: build the catalogue with an empty query; assert names are unique and `lumetri` is absent while `grade` is present.
   - `core_tests.cpp`: `FindEffect("lumetri")` is still non-null and has the same parameters as `grade`.
6. Check in the application: Effects panel, type "Basic": one entry.

**Docs.** Remove `EFFECT-ALIAS-001` from `TODO.md`.

---

### WP-A3. Wire or remove the ten dead preferences  (size M)

**Problem.** `ui/Preferences.cpp` declares 35 preferences; ten are shown in the Preferences dialog, saved and restored, and read by nothing. The worst consequence: there is no autosave.

| Key | Meaning | What to build |
|---|---|---|
| `general.autosave_minutes` | Save a snapshot every N minutes (0 = off) | Step 1 |
| `general.restore_last_project` | Reopen the last project at start | Step 2 |
| `general.confirm_quit` | Ask before quitting with background jobs | Step 3 |
| `appearance.theme` | `dark` or `light` | Step 4 |
| `appearance.ui_scale` | Interface size | Step 5 |
| `media.cache_folder` | Where caches live | Step 6 |
| `playback.pre_roll_seconds`, `playback.post_roll_seconds` | Play-around | Step 7 |
| `timeline.default_transition_seconds` | Length of a transition added with a key | Step 8 |
| `timeline.default_still_seconds` | Length of a still image clip | Step 9 |

Do one step per commit. After each, delete nothing from the schema; the preference now has a reader.

**Step 1: autosave**

1. In `Session` add `QTimer autosave_timer_;` (header) and in `Open(...)` start it with `prefs_.GetInt("general.autosave_minutes") * 60 * 1000`; stop it in `closeProject`. When the preference changes, restart it (the `prefs_.Observe` lambda in the constructor).
2. On timeout, if a project is open and `store_->CurrentRevision()` is greater than the revision at the last snapshot, call `store_->CreateSnapshot()` (this is what File > Save does; read the `file.save` branch of `Session::trigger`). Keep a `last_snapshot_revision_` member. Show `"Autosaved"` in the status line.
3. Never block the GUI thread for more than a few milliseconds: `CreateSnapshot` copies the database through its own connection, so it does not hold the store's lock for long, but measure with a project of a few thousand clips (`cutline_bench` builds large ones; `app/bench_main.cpp` shows how).
4. Tests: `app_tests.cpp`: set `general.autosave_minutes` to a tiny value through a new test-only hook (`Session::setAutosaveIntervalForTest(int milliseconds)`), make an edit, wait, and assert `store.SnapshotCount()` increased. (Use a hook rather than waiting minutes.)
5. Check by hand: set it to 1, edit, wait, look in `<project>.cutline/snapshots/`.

**Step 2: restore the last project**

1. After a project opens or is created, write its folder to a small file `<config dir>/last_project.txt` (`Session::SetConfigDirectory` already knows the folder and `Session.cpp` has a `WriteText` helper).
2. In `app/main.cpp`, after the session is configured and if no `--open`/`--demo` was given and the preference is on and the file exists and the folder exists, call `session.openProject(...)`. If it fails, show the status message and start empty.
3. Test: `app_tests.cpp`: create a project in a temp config dir, destroy the session, create a new session with the same config dir, call the new `Session::restoreLastProject()` (extract the logic from `main.cpp` into this method so it is testable) and assert `projectOpen`.

**Step 3: confirm quit**

1. Quitting goes through `Session::trigger("file.quit")`, which emits `quitRequested`; `Main.qml` answers it with `Qt.quit()` (a `Connections` block near line 328). The window has no `onClosing` handler, so closing the window with its button also bypasses any check: add `onClosing: (close) => { ... }` to the `ApplicationWindow` as well. If `general.confirm_quit` and `session.jobs` contains a running job or the export queue has a running job (`exportJobs` where `state == "running"`), show a `Dialog` ("An export is running. Quit anyway?"). `close.accepted = false` until the dialog's accept button is pressed.
2. Test (app): `session.hasRunningWork()` (new invokable that returns true when any job is running); assert true during an export job, false after.

**Step 4: theme**

1. `app/qml/Theme.qml` is a singleton of constant colours. Make the colours *properties* computed from a `property bool light: false`. Define the light palette (about 16 colours: background, panel, panel header, border, text, muted, faint, accent, danger, warning, ...). Do not leave any hard-coded `"#…"` colour in a panel QML that should follow the theme: `grep -rn '"#' app/qml` and fix each, or document why it must stay (video monitor background, clip colours).
2. `Session` exposes `Q_PROPERTY(QString theme ...)` from the preference; `Main.qml` sets `Theme.light = session.theme === "light"`.
3. `TimelineItem::paint` and `MonitorItem::paint` use C++ colours (`QColor("#141515")`): give them a small palette struct chosen by the theme and call `update()` when it changes.
4. Test (app): changing the preference changes `Theme.light` — read it through a `Session::themeIsLight()` invokable; plus take two screenshots with `scripts\shot.bat` (dark and light) and look at them.

**Step 5: UI scale**

1. Qt reads `QT_SCALE_FACTOR` before the application object exists. In `main.cpp`, load `preferences.json` (the file already exists in the config folder; read it with `ui::Preferences::Load`) **before** constructing `QGuiApplication`, and `qputenv("QT_SCALE_FACTOR", ...)` from `appearance.ui_scale` (a percentage; convert to 1.25 etc.). State in the preference's help text that it takes effect after a restart.
2. Test: factor the percentage→factor conversion into a pure function in `ui/Preferences.cpp` (`ScaleFactorFor(int percent)`) and test it in `ui_tests.cpp` (clamps to 75–200, 100 gives "1").

**Step 6: cache folder**

1. Today the flow cache is created at `<package>/cache/flow` (`Session::Open`) and the render cache is memory only (`RenderCacheConfig{256 MB, {}, 0}` in `StartEngine`). Add a helper `Session::CacheRoot()`: the preference if it is non-empty and exists, otherwise `<package>/cache`.
2. Pass `CacheRoot() / "flow"` to `FlowCacheConfig` and `CacheRoot() / "render"` plus a disk quota (for example 2 GiB) to `RenderCacheConfig`. Create the folders.
3. Test (app): set the preference to a temp folder, open a project, render a frame twice, assert files appear in the folder.
4. Careful: a cache is disposable; never put project data in it.

**Step 7: pre-roll and post-roll**

1. New application command `transport.play_around` ("Play Around", default `Shift+K`): seek to `playhead - pre_roll`, play, and stop at `playhead + post_roll`, then return the playhead to where it was.
2. Implement in `Session` with a `std::optional<RationalTime> play_around_stop_` and `play_around_return_` checked in `OnTick` (the loop logic there is the model).
3. Test (app): trigger it, tick the timer (`QTest::qWait`), assert the playhead returns and `playing` is false.

**Step 8: default transition length**

1. `Session::addTransition(kind, seconds)` takes a length from the dialog. Add the command `timeline.add_default_transition` ("Apply Default Transition", `Ctrl+D`) that calls the same planner with the preference as the length and "Cross Dissolve".
2. Test: `ui_tests`-level planner tests exist; add an app test that the command places a 1 second transition on the cut nearest the playhead.

**Step 9: default still length**

1. Find how still images are imported: a PNG probes with a one-frame duration. In `Session::InsertPlanAt`, when the media has a video stream and no audio stream and its duration is at most one frame, use `default_still_seconds` as the placement length (and allow trimming longer: `TrimClip` clamps to media duration, so the clip must be created with an extended source range; check how `InsertClip` validates `source_out` against the media duration, and if it refuses, add a "hold last frame" rule only for stills: see how graphics clips are source-less to decide whether to import stills as generators instead).
2. This step is the hardest of the ten. If it takes more than a day, write down what you found in `TODO.md`, remove the preference from the schema with a note in the changelog, and move on.

**Check the whole package.** `grep -rn` every key; each must have at least one reader outside `ui/Preferences.cpp`. Write a native test that enforces it: `EveryDeclaredPreferenceIsReadSomewhere` in `ui_tests.cpp` can open the files under `app/`, `ui/`, `core/`, `playback/`, `render/` as text and search for each declared key; fail with the list of unread keys. (This turns the defect into a permanent check.)

**Docs.** Delete `PREF-WIRING-001` from `TODO.md`; delete the clause about ten preferences from the UI-001 row of `IMPLEMENTATION_STATUS.md`.

---

### WP-A4. Markers panel and marker editing  (size M)

**Problem.** `markers` is a registered panel (part of the Effects workspace) that shows "Not built yet". Markers can be added (`M`) but not named, coloured, listed, moved or deleted: `UpdateMarker` and `RemoveMarker` exist and are never issued. `Session::Reload` loads only marker *times* (`markers_`).

**Design.** A marker is a row of the `markers` table (id, owner kind/id, start, end, label, kind, colour, metadata). The panel lists the sequence's markers ordered by time with: colour dot, timecode, label, duration; double-click seeks; a toolbar adds, deletes, and edits.

**Steps**

1. **Read model.** In `app/Session.h` add `Q_PROPERTY(QVariantList markerList READ markerList NOTIFY markersChanged)`. Implement `markerList()` in a new `app/SessionMarkers.cpp (new)` (R4, R12): query `SELECT id, start_num, start_den, end_num, end_den, label, kind, color FROM markers WHERE owner_kind='sequence' AND owner_id=? ORDER BY start_ticks` under `store_->mutex()`; return maps `{id, start (seconds), end, label, kind, color, timecode}`. Rebuild in `Reload` and emit `markersChanged`. (`markersChanged` is a new signal.) While there, change the existing `markers_` times vector to be derived from the same query so the ruler and snapping keep working.
2. **Editing rules** in `ui/Markers.h/.cpp (new)` (R3): `PlanAddMarker(ctx, at, label)`, `PlanUpdateMarker(ctx, marker, newStart, newEnd, label, kind, color)`, `PlanRemoveMarkers(ctx, ids)`. They emit `AddMarker`, `UpdateMarker`, `RemoveMarker` commands. Refusals: end before start; label longer than 200 characters (check the actual limit in `Check(const AddMarkerPayload&)`).
3. **Invokables**: `markerAdd(label)`, `markerUpdate(id, startSeconds, endSeconds, label, kind, color)`, `markerRemove(id)`, `markerSeek(id)`. Convert seconds to a frame-aligned `RationalTime` with the same helper other invokables use (search `Session.cpp` for how `seek` snaps to frames).
4. **Panel**: `app/qml/panels/MarkersPanel.qml (new)` following R5; map `case "markers"` in `PanelGroup.qml`; add to `QML_FILES`. A `ListView` over `session.markerList`; a `TextField` for the label (commit with Enter), a `ComboBox` for kind (Comment, Chapter, Segmentation, Web link), eight colour swatches, Add / Delete buttons.
5. **Timeline.** `TimelineItem::paint` draws markers from `session_->markerTimes()`; change it to draw each marker in its colour and, when the zoom allows, its label. Find the marker drawing by searching the file for `markers`.
6. **Keys**: `M` already adds; add `timeline.next_marker` / `timeline.previous_marker` (`Shift+M`, `Ctrl+Shift+M`) in `ui/Shortcuts.cpp` (R6) that seek to the next/previous marker.
7. **Remove the placeholder**: the `markers` case in `app/qml/panels/PlaceholderPanel.qml` can stay (it is the default for unknown ids) but the "Markers panel remains a placeholder" sentence in the docs must go.
8. **Tests**
   - `ui_tests.cpp`: the three planners (happy path; refusals) run against a real store; a marker survives undo/redo; markers move with `PlanUpdateMarker`.
   - `app_tests.cpp`: `AMarkerIsAddedNamedColouredMovedAndDeletedFromThePanel`: call the invokables; assert `markerList`; seek by marker; delete; undo restores it.
   - Native store test that `RemoveMarker` of a non-existent id is refused (add if missing).
9. **Check by hand**: Effects workspace, press `M` three times at different times, name them, give one a colour, double-click to jump, delete one, press `Ctrl+Z`, save, reopen, they are still there. Screenshot with `scripts\shot.bat markers.png --demo --do workspace:Effects --do trigger:timeline.add_marker`.

**Docs.** Delete `UI-MARKERS-001` from `TODO.md`; remove "The markers panel remains a placeholder" from the UI-001 row of `IMPLEMENTATION_STATUS.md`; update the parity manifest if you want `media_labels` etc. to move (they will not; they are about media).

---

Continue with [guide/B-editing-surface.md](guide/B-editing-surface.md).
