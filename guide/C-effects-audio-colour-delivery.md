# Phase C: effects, audio, colour, titles and delivery

Part of the [implementation guide](../IMPLEMENTATION_GUIDE.md). Recipes R1 to R14 are defined there. Phase C does not depend on Phase B except where a package says so, so a second engineer can work on it in parallel.

| Package | Title | Size |
|---|---|---|
| [C1](#wp-c1-track-mattes) | Track mattes | M |
| [C2](#wp-c2-dip-to-colour-and-transition-handle-display) | Dip to colour and transition handle display | S |
| [C3](#wp-c3-new-video-effects-a-batch) | New video effects (a batch) | L |
| [C4](#wp-c4-new-audio-effects-a-batch) | New audio effects (a batch) | L |
| [C5](#wp-c5-colour-wheel-and-curve-widgets) | Colour wheel and curve widgets | M |
| [C6](#wp-c6-titles-roll-crawl-and-on-canvas-text) | Titles: roll, crawl, on-canvas text | L |
| [C7](#wp-c7-export-features) | Export features | L |
| [C8](#wp-c8-media-path-aliases) | Media path aliases | M |
| [C9](#wp-c9-gpu-kernels) | GPU kernels | XL |
| [C10](#wp-c10-decode-validation-and-the-codec-matrix) | Decode validation and the codec matrix | M |

---

## WP-C1. Track mattes

**Problem.** A clip can be shown only where another layer is opaque or bright: the standard way to put video inside text or a shape. The compositor has blend modes, masks and keys, but no matte that reads *another track*.

**Files to read first**: `render/Compositor.cpp` — the function `Compose` (the loop `for (const auto& [key, track_requests] : by_track)`, `RenderRequest`, `ApplyPostEffects`), `render/Layer.h` (premultiplied float pixels, `CompositeOnto`, dirty rectangle), `timeline/TimelineCompiler.h` (`SampledEffect::preset_name`), the audio side-chain use of `preset_name` in `audio/AudioMixer.cpp` (`wants_sidechain = !effect.preset_name.empty()`).

**Design**

- The matte is an **effect on the clip being matted**, named `track_matte`. Its `preset_name` holds the id of the matte track (the same idea as the audio side-chain: effect parameters are numbers, so a track id rides in the preset field). Parameters: `mode` (0 alpha, 1 luma) and `invert` (0 or 1).
- Rule 1: a clip with an active `track_matte` is multiplied, pixel by pixel, by the matte value at that pixel: `matte = alpha` of the matte track's composited layer (alpha mode) or its luma times alpha (luma mode); inverted: `1 - matte`.
- Rule 2: a track that is used as a matte by any enabled clip at this instant is **not composited itself** (it is only a stencil). Document this in the effect's description.
- Rule 3: if the matte track has nothing at this instant (empty, muted, or not in the plan), the matte is **0** (the clip disappears) and the compositor records an entry in `Statistics::effect_errors` ("track matte: track V3 has nothing at this time"). This is stated, tested, and visible.
- Preview and export share the code (it is in `Compositor`); the render cache key already hashes the whole plan, so changing the matte track changes the key — **prove it with a test**, do not assume.

**Steps**

1. **Descriptor** (R8 step 1): `Video("track_matte", "Track Matte", "Keying", {Scalar("mode", "Mode", 0.0, Unit::None, 0.0, 1.0), Scalar("invert", "Invert", 0.0, Unit::None, 0.0, 1.0)})`. Default = no-op is impossible (a matte with no track hides the clip), so the neutral test list excludes it, and `IsNoOpEffect` treats a `track_matte` with an empty `preset_name` as a no-op. Update the effect count in `core_tests.cpp`.
2. **Command to choose the track.** `AddEffect` can set `preset_name`, but nothing changes it afterwards. Add `SetEffectPreset {effect_id, preset_name}` (R1, wire name `effect.set_preset`). Validation: the track exists in the same sequence, is a video track, is not the clip's own track; refuses a loop of mattes (A matted by B matted by A). The audio side-chain selector will use it too.
3. **Compositor.** In `Compose`:
   1. Before the track loop, build `std::set<std::string> matte_tracks` from all depth-0 requests' `track_matte` effects (only enabled effects with a non-empty preset).
   2. Skip tracks in `matte_tracks` in the main loop (but not their requests when they are needed as mattes).
   3. For a request with a `track_matte`: render its clip layer as today into `workspace.layer`; render the **matte track's request(s)** for this instant into a second workspace layer (`workspace.matte`, add it next to `workspace.layer` in the workspace struct, sized with `Reset(width, height)`); if the matte track has a transition, use `MixTransition` the same way the main path does (extract the "render a track's layer" code into a lambda `RenderTrackLayer(track_id, Layer& out)` and use it in both places — do this refactor first, run the golden tests, commit).
   4. Multiply: `ParallelRows` over rows; for each pixel `m = clamp(matte_value)`, `layer.at(x, y) *= m` (all four premultiplied channels). Mark the whole layer dirty.
   5. Order: the matte is applied **after** the clip's other effects and before blend/composite.
4. **Nested matte**: if the matte track's clip is itself a nested sequence, `RenderRequest` already resolves it; no extra work.
5. **UI.** In `app/qml/panels/InspectorPanel.qml` the effect shows two sliders; add a special case when `effect.type == "track_matte"`: a `ComboBox` of the other video tracks (`session.trackIds("video")` exists) calling a new invokable `setEffectPreset(effectId, trackId)` that runs `SetEffectPreset`. Show the track's name, not its id.
6. **GPU**: the D3D11 compositor must *refuse* plans containing `track_matte` (the `Supports` check refuses unknown effect types with a reason; add a gpu test that it says so). A real shader is C9.

**Tests**

- `store_tests.cpp`: `SetEffectPreset` happy path, refusals (missing track, audio track, own track, loop), undo/redo.
- `render_tests.cpp`:
  - `ATrackMatteShowsAClipOnlyWhereTheMatteLayerIsOpaque`: two tracks, the bottom solid red full frame, a top matte track with a rectangle graphic or a solid with a mask; assert pixels inside are red, outside are the background (transparent).
  - `LumaMatteFollowsBrightness` (grey ramp matte → alpha follows the ramp at three columns).
  - `InvertFlipsTheMatte`; `TheMatteTrackIsNotDrawnItself` (its colour appears nowhere); `AMatteTrackWithNothingHidesTheClipAndSaysSo` (pixels transparent, `effect_errors` has the message).
  - `ChangingTheMatteChangesTheRenderCacheKey`: compose with a `RenderCache` twice; the second must miss.
- `gpu_tests.cpp`: `AplanWithATrackMatteIsRefusedByTheCardWithAReason` (name it in the existing style).
- `app_tests.cpp`: pick the matte track in the inspector, the program monitor changes.

**Check by hand.** Two video tracks: V1 = a colour matte (B8) with a rectangle mask; V2 = Bars. Add Track Matte to the Bars clip, choose V1: bars appear only in the rectangle. Toggle invert. Export 2 s and open the file: identical.

**Docs/parity.** `IMPLEMENTATION_STATUS` GFX/transitions "no track mattes" limits; ARCHITECTURE section 8 "Not built: ... track mattes".

---

## WP-C2. Dip to colour and transition handle display

**Problem.** The library has Dip to Black and Dip to White only. And when a transition is refused or shortened because the media has no spare frames, the user is told only in a status note.

**Steps**

1. **Dip to colour.**
   1. `render/Transitions.h`: add a parameter struct `TransitionParams { std::array<float,3> colour{0,0,0}; }` and change `MixTransition(kind, from, to, progress, into)` to take `const TransitionParams&` (default argument keeps the old call sites compiling). Add the kind `dip_to_color` ("Dip to Color", family "Dip"): outgoing fades to the colour over the first half, the colour fades to the incoming over the second half (copy `DipBlack`'s arithmetic, substitute the colour; remember layers are **premultiplied**).
   2. The compiler already carries `TransitionMix::effects` (a `std::vector<SampledEffect>`). Define that a `dip_to_color` transition owns an effect `transition_colour` (Vec3 `color`). Register it in `EffectRegistry.cpp` as a hidden effect (`listed = false`, WP-A2) and let `Compositor::Compose` read it from `transition->effects` and pass it in `TransitionParams`.
   3. `PlanAddTransition` / `PlanChangeTransition` (`ui/Transitions.cpp`): when the kind is `dip_to_color`, add an `AddEffect` for the transition (`owner_kind = Transition`) with the chosen colour; changing the colour is `SetParameterConstant`.
   4. `app/qml/TransitionDialog.qml`: when the kind is `dip_to_color`, show a colour button (a `ColorDialog` is in `QtQuick.Dialogs`; pass r,g,b 0..1).
   5. Tests: `render_tests.cpp`: the existing "every kind starts as the outgoing and ends as the incoming" loop covers it; add `DipToColourReachesTheColourAtTheMiddle`. `ui_tests.cpp`: plan has the effect; undo removes it. GPU: the card handles only dissolves; `Supports` must refuse this kind (test).
2. **Handle display.**
   1. `ui::HandlesOf(ctx, clip)` already returns head and tail handles. Add `Session::transitionHandles(clipFromId, clipToId)` that returns seconds of spare picture available on each side.
   2. In `TransitionDialog.qml` show "Outgoing clip has 0.4 s of spare picture, incoming has 2.0 s" and colour the length slider's maximum accordingly; set the slider maximum to what is available for the chosen alignment. Warn in red when the maximum is below the requested length.
   3. Draw the available handle as a lighter extension of the transition in `TimelineItem` while the dialog is open.
   4. Tests: `ui_tests.cpp` `TheHandlesOfAClipAreWhatTheMediaHasBeyondItsRange` (probably exists; add the dialog-level helper test), app: dialog values.

**Check by hand.** Add Dip to Color to the first cut of the demo, choose magenta, scrub through it: magenta at the middle. Open the dialog on a cut with no spare frames: the slider cannot exceed the available length.

**Docs/parity**: `dip_to_color`, `transition_handle_diagnostics`, TRANS-001 limit "no per-kind parameters beyond length and alignment".

---

## WP-C3. New video effects (a batch)

Each effect follows recipe **R8**. Do them in the groups below, one effect per commit, in this order (earlier ones are easier and teach the pattern). The table gives the exact behaviour to implement and the test that proves it. All parameters default to a no-op unless noted.

| # | Effect (id) | Category | Parameters | Algorithm | Test (render_tests) |
|---|---|---|---|---|---|
| 1 | Invert (`invert`) | Color | `amount` 0..1 | `c' = lerp(c, 1 - c, amount)` on un-premultiplied colour | solid 0.2/0.4/0.9 → 0.8/0.6/0.1; amount 0.5 → grey 0.5 |
| 2 | Threshold (`threshold`) | Color | `level` 0..1, `softness` 0..0.5 | luma `y`; `t = smoothstep(level-softness, level+softness, y)`; output grey `t` | 2-column ramp: left black, right white; level moves the edge |
| 3 | Solarize (`solarize`) | Stylize | `level` 0..1 | per channel: `c > level ? 1 - c : c` | values above/below the level flip |
| 4 | Mosaic (`mosaic`) | Stylize | `blocks_x`, `blocks_y` 1..2000 | block average (`ParallelRows` over blocks) | a 4×4 checker at 2×2 blocks → each block the mean of its pixels |
| 5 | Mirror (`mirror`) | Distort | `center` Vec2, `angle` degrees | reflect across a line through `center` at `angle`: for pixels on the far side sample the mirrored position (inverse mapping; use the sampler that `ApplyBulge` uses) | left half red, right half blue; mirror at the middle → both halves red |
| 6 | Offset (`offset`) | Distort | `shift` Vec2 pixels, `wrap` 0/1 | translate with wrap-around (modulo) | a single bright pixel moves by `shift` and wraps |
| 7 | Find Edges (`find_edges`) | Stylize | `strength` 0..4, `invert` 0/1 | Sobel on luma; output grey edge magnitude × strength | vertical step edge → bright column at the step, zero elsewhere |
| 8 | Emboss (`emboss`) | Stylize | `direction` degrees, `relief` 0..4 | directional derivative + 0.5 | flat area → 0.5; step edge → ±offset |
| 9 | Radial Blur (`radial_blur`) | Blur | `center` Vec2, `amount` 0..100, `mode` spin/zoom | accumulate 16–64 samples along a rotation (spin) or radial line (zoom); weights equal | centre pixel unchanged; pixels at the rim of a bright dot smear tangentially (spin) |
| 10 | Corner Pin (`corner_pin`) | Distort | four `Vec2` corners (normalised) | solve the homography from the unit square to the 4 points; inverse map each output pixel; bilinear sample; alpha 0 outside | unit-square corners = identity (default); pinning to a half-size square at the centre gives half-size picture |
| 11 | Replicate (`replicate`) | Stylize | `count` 1..8 | tile the picture `count × count` (nearest-pixel decimation by `count`) | a pixel pattern repeats at the right period |
| 12 | Gradient generators (`ramp`, `four_color_gradient`) | Generate | endpoints and colours | pure function of `(x,y)`; used on a source-less clip like `solid` | corner pixel values equal the corner colours |
| 13 | Checkerboard / Grid (`checkerboard`, `grid`) | Generate | size, colours, line width | pure function of `(x,y)` | alternating cell values |
| 14 | Strobe (`strobe`) | Time | `period` seconds, `duty` 0..1 | needs **clip time**: look at how `time_remap` or noise reduction obtain it (the sampled effect carries values at clip-local time; add an implicit parameter `clip_time` that the compiler sets for every sampled effect — see `TimelineCompiler` effect sampling) | alpha 1 in the on-phase and 0 in the off-phase at three sampled times |
| 15 | Echo (`echo`) | Time | `frames` 1..16, `decay` 0..1 | blend with previous frames: use the **neighbour layer** mechanism noise reduction uses (`colour.neighbour_layers`, the code near `neighbour_request` in `Compositor.cpp`); generalise it from "noise" to "any effect that declares it needs N previous frames" first | a bright dot that moved leaves decaying copies |
| 16 | Lens Flare (`lens_flare`) | Generate | `position` Vec2, `brightness`, `type` | additive radial gaussian halo + 3 ghosts along the line through the frame centre; pure function | brightest at `position`; ghost positions satisfy the geometry |
| 17 | Lightning / Turbulent displace (`turbulent_displace`) | Distort | `amount`, `size`, `evolution` | 2-D value noise (hash of integer lattice, **seeded by parameters only — no global random**) displacing the sampling position | deterministic: same parameters → bit-identical output twice |
| 18 | Ultra Key (`ultra_key`) | Keying | `key_color`, `tolerance`, `pedestal`, `choke`, `soften`, `spill` | key on chroma distance in YCbCr (cb, cr only; luma-independent), spill by moving the colour toward grey along the key hue, choke/soften by a shrink + blur of alpha: build on `ApplyChromaKey` and its `ApplyMatte` helper in `Filters.cpp` | a green screen of any brightness is removed; a skin tone is kept; spill pixel loses its green cast |
| 19 | Difference / Colour key (`color_key`, `difference_key`) | Keying | colour/second layer, tolerance | chroma distance in RGB; the difference key needs a second layer: same mechanism as the matte in C1 | exact colour removed |
| 20 | Auto Color / Auto Levels (`auto_levels`) | Color | `black_clip`, `white_clip` | **analysis**, not a per-pixel function: it must be computed once per clip (a job that stores the result as effect parameters like the stabiliser's analysis) | the stored parameters stretch a known histogram to 0..1 |

**For every effect, in order**

1. Descriptor (R8 1) with units and ranges; default no-op where possible.
2. Function in `render/Filters.cpp`, registered in `FilterEffectTypes()` and `ApplyFilterEffect`; keep functions pure and deterministic.
3. Neutral-default entry in `IsNoOpEffect` (R8 4).
4. Tests from the table plus: default leaves the picture alone (add the id to the list in `EveryNewEffectHasADescriptor...`), **parallel equals serial** (render with `ParallelRows` forced to one thread — the existing filter tests do this by comparing to a straightforward loop; for new effects compare against a reference function written in the test), alpha is preserved (a half-transparent source stays half-transparent unless the effect is meant to change alpha).
5. Effect count in `core_tests.cpp`; docs counts (`check-docs` finds them).
6. A benchmark case in `app/bench_main.cpp` (the `cases.push_back({"tint", ...})` list near line 1000), run `scripts\bench.bat --color` or the matching mode, record 1080p time in `PERFORMANCE_LOG.md`.

**Check by hand.** For each effect: screenshot `scripts\shot.bat fx-<id>.png --demo --do select:Bars --do effect:<id>` at default (unchanged) and with the parameters changed in the inspector (take a second screenshot after setting through `session.setParameter` in a small app test, or by hand); put the pair in the pull request.

**Pitfalls.** Un-premultiply before nonlinear colour math and re-premultiply afterwards (look at `ApplyTint` and `Straight(...)`). Neighbourhood effects (blur, find edges, emboss, radial blur) need the scratch layer overload so they do not read pixels they have already written. Never use `rand()`; noise must be reproducible so preview equals export.

---

## WP-C4. New audio effects (a batch)

Each follows recipe **R9**. The governing rule from `audio/Dsp.h`: an effect's output at a sample may depend only on input within a finite window around it (`memory` before, `lookahead` after). That makes every block size and every seek give identical samples. Start with the easiest.

| # | Effect (id) | Parameters | Algorithm | Memory / lookahead | Numeric test |
|---|---|---|---|---|---|
| 1 | Delay / Echo (`delay`) | `time_ms` 1..2000, `feedback` 0..0.95, `mix` 0..1 | `y[n] = x[n] + sum_{k=1..K} feedback^k * x[n - k*T] * mix`, K chosen so `feedback^K < 0.001` (finite!) | memory = K*T, lookahead 0 | impulse in → impulses at multiples of T with the right gains |
| 2 | Parametric EQ (extend `eq`) | up to 8 bands: `type` (peak/low shelf/high shelf/low cut/high cut), `freq`, `gain`, `q` | cascade of biquads from `audio/Dsp.h` (`Peaking`, `LowShelf`, ...) | as `ApplyEq` | `ResponseDb` at band centre equals the gain |
| 3 | Graphic EQ (`graphic_eq`) | 10 octave bands `gain_31` .. `gain_16k` | ten peaking filters in series | as EQ | a sine at a band centre changes by that band's gain |
| 4 | Chorus / Flanger (`modulated_delay`) | `rate_hz`, `depth_ms`, `delay_ms`, `feedback`, `mix`, `mode` | LFO-modulated fractional delay line (linear/cubic interpolation) | memory = max delay | with depth 0 it equals a fixed delay; the modulation period matches `rate_hz` |
| 5 | De-esser (`deesser`) | `frequency` 4–10 kHz, `threshold`, `ratio` | band-pass the signal at `frequency`, compute its level, use it as the **detector** for a compressor acting on the high band (split into low + high with complementary filters, compress high, sum) | compressor margins | a sine at 7 kHz is attenuated; a sine at 500 Hz is not |
| 6 | De-hum (`dehum`) | `base` 50/60, `harmonics` 1..8, `q` | notch filters at multiples of `base` (biquad notches; add `Notch` to `Dsp`) | IIR: use the EQ lead-in rule | a 60 Hz sine is reduced > 30 dB, 1 kHz untouched |
| 7 | Reverb (`reverb`) | `room`, `decay_s`, `damping`, `mix`, `pre_delay_ms` | a Schroeder/Freeverb network (4 comb + 2 allpass), **truncated** to `decay_s * 1.5` of tail so memory is finite; run each cell from silence over a lead-in of that length | memory = tail length | impulse response energy decays at the requested RT60 within 10% |
| 8 | Pitch shifter (`pitch_shift`) | `semitones` -12..12, `formant_keep` | reuse the WSOLA machinery in `audio/TimeStretch.h` (stretch then resample) as a stateful effect | WSOLA window | a 440 Hz sine becomes 880 Hz at +12 (measure by zero crossings) |
| 9 | Multiband compressor (`multiband_compressor`) | 3 bands, crossovers, per-band threshold/ratio | Linkwitz-Riley crossovers + three `Compress` calls + sum | max of the bands | a loud low band is compressed while a quiet high band is not |
| 10 | Stereo imager / mid-side (`stereo_width`) | `width` 0..2, `balance` | `m=(l+r)/2, s=(l-r)/2; s*=width` | none | width 0 → mono; width 1 → identity |
| 11 | Channel volume / mute (`channel_volume`) | per-channel gain | multiply | none | gains applied per channel |

For every effect also write:

- **Block independence** (the most important test): render 2 seconds in one call and in blocks of 480, 1024, 4096 and 7777 frames, and compare every sample; copy `BlocksOfAnySizeTileToTheSameAudioAsOneLargeBlock`.
- **Seek independence**: render [1.0 s, 1.5 s] directly and as the tail of [0, 1.5 s]; equal.
- **Bypass**: a disabled effect leaves the input bit-identical.
- **Automation**: parameters are read once per 4096-sample cell (`kCell` in `AudioMixer.cpp`); a test that a parameter change at a cell boundary takes effect at the next cell.
- Registry entry, effect count, Essential Sound presets (`ui/AudioWorkflow.cpp`) only if the effect belongs in a role chain.

**UI**: the Audio Essentials/Mixer panels list effects from the registry; confirm the new effect can be added to a track's effect list from the mixer strip (`AudioMixerPanel.qml` — see how EQ is added) and shows its parameters.

**Check by hand.** Add Delay to a track with the demo's tone: the tail is audible and equal in the exported WAV (export `audio.wav24`, open it in an audio editor and look at the repeats). Loudness meter reads sensible values.

**Pitfalls.** Never allocate in the realtime path: effects run inside `AudioMixer` on the producer thread (a lock-free ring feeds the device) — allocation there is tolerated by the existing design only because the work is done per block on a worker, but do not add locks or file I/O. Read the header of `audio/WasapiSink.cpp` before changing anything about threading.

---

## WP-C5. Colour wheel and curve widgets

**Problem.** The effects `color_wheels`, `curves`, `hue_curves`, `hsl_secondary` exist on CPU and GPU; the inspector shows them as rows of numbers.

**Files to read first**: `app/qml/ParameterRow.qml` and `InspectorPanel.qml` (how a parameter row is built from `Session::inspector`), `Session::setParameter`, `toggleKeyframe`, the descriptors in `effects/EffectRegistry.cpp` (`color_wheels` has `lift`, `gamma`, `gain` as Vec4 = red, green, blue, master; `shadows`, `midtones`, `highlights` as Vec3; `curves` has `master`, `red`, `green`, `blue` as Vec3 — three interior points at x = 0.25, 0.5, 0.75).

**Steps**

1. **Pure mapping functions** in `ui/ColorWidgets.h (new)` so the maths is tested without Qt:
   - `std::array<double,3> WheelOffset(double x, double y)` maps a point in the unit disc to an RGB offset that sums to zero (hue = angle, saturation = radius): `r = s*cos(h)`, `g = s*cos(h - 2π/3)`, `b = s*cos(h + 2π/3)`, scaled by the parameter's range.
   - `std::array<double,2> WheelPoint(r, g, b)` the inverse (least-squares angle/radius), for drawing the puck from the stored values.
   - `CurvePointsToValues`/`ValuesToCurvePoints` for the three-point curves.
2. **Tests** (`ui_tests.cpp`): round trip `WheelPoint(WheelOffset(p)) == p` over a grid; origin → zero offset; offsets sum to zero; clamps at the disc edge.
3. **`ColorWheel.qml (new)`**: a `Canvas` drawing an HSV disc (draw once to an offscreen image and reuse), a puck, a master slider on the right, double-click resets. Properties: `value` (Vec4 list), signals `valueEdited(list)` while dragging (use `session.setParameter(effectId, name, components)` on `onPositionChanged` throttled to 30 Hz, and one final call on release) — **the undo concern**: each `setParameter` is one undo step. Add `Session::beginParameterDrag()` / `endParameterDrag()` that wrap the intermediate edits in a single group (look at how `rampDrag(bool)` and `RampDialog` avoid a step per mouse move; the doc says "live preview writes every edit as its own undo step", which is the thing to improve).
4. **`CurveEditor.qml (new)`**: a square `Canvas` with a diagonal; draggable points at x = 0, 0.25, 0.5, 0.75, 1 (the ends fixed for now); channel tabs (Master/R/G/B). Edits write the three interior values. If you want **free** x positions you need a new parameter layout (arbitrary point count) and therefore a new render version, because existing projects' curves must keep meaning the same thing — see `core/model/RenderVersion.h` and rule 6 of the guide. Do that as a separate package.
5. **Use them**: in `InspectorPanel.qml`, when `effect.type` is `color_wheels` show three wheels (Lift, Gamma, Gain) and, collapsed, the numeric rows; for `curves` show the curve editor; `hue_curves` and `hsl_secondary` stay numeric (later).
6. **Keyframes**: each wheel has the keyframe diamond from `ParameterRow` (copy its `toggleKeyframe` hook).
7. **App test**: drag a puck programmatically via a test hook (`Session::testSetWheel(effectId, name, x, y)`), assert the stored parameter matches `WheelOffset`; undo restores in one step.

**Check by hand.** Select a clip, add Color Wheels, drag the Gain puck to orange: the picture warms; scopes (Color workspace) show the vectorscope blob move toward orange; undo once returns it. Take a screenshot of the Color workspace.

**Docs/parity**: `color_wheels`, `lumetri_style_curves` → `implemented` only when wheels *and* curves are interactive; COLOR-002 limit text.

---

## WP-C6. Titles: roll, crawl and on-canvas text

**Problem.** Titles are drawn by `render/Graphics.cpp` from a versioned document (`effects/GraphicsDocument.h`) with per-element keyframes and entrances. There is no rolling/crawling title (text moving across the whole clip), and text is edited in a field *beside* the canvas.

### C6.1 Roll and crawl

1. **Design.** A roll is vertical movement of the whole document's content from fully below the frame to fully above it over the clip's length; a crawl is horizontal. It is a property of the **document** (so templates can carry it), not an effect keyframe, because it must scale with the clip length chosen on the timeline. Add to the document: `"motion": {"kind": "none|roll|crawl", "speed": "fit|pixels_per_second", "value": 120, "pre_roll": 0.0, "post_roll": 0.0, "ease_in": 0.0, "ease_out": 0.0}`.
2. **Schema/versioning.** `GraphicsDocument` has a schema number (3 today: "written only when used; an older build refuses it"). Writing `motion` raises the document schema to 4 **only when used**. Update the parser, the writer, validation (unknown kind refused, negative values refused), and the "needs newest schema only when it uses it" test pattern (`AGraphicElementTurnsAboutItsMiddleAndTheDocumentNeedsTheNewestSchemaOnlyWhenItUsesIt`).
3. **Clip time into the drawing.** The graphic is drawn with a time (read `render/Graphics.h` for the draw signature and `Compositor.cpp` for how the `graphic` effect is drawn — search `HasGraphic`/`DrawGraphic`). It needs the clip's **local time and duration**. Add them to the sampled effect as implicit parameters (the same implicit `clip_time`/`clip_duration` mechanism used by the strobe effect in C3 #14 — implement it once, in `TimelineCompiler`'s effect sampling, and use it for both).
4. **Drawing.** Measure the content's bounding box (union of element rectangles after layout); for `fit`, the offset at progress `p` is `lerp(frame_height, -content_height, p)`; for `pixels_per_second`, `offset = start - speed * t`. Elements keep their own entrances on top.
5. **Designer.** `GraphicDesignerPanel.qml` gets a "Motion" group in the properties list (kind, speed, pre/post-roll); `GraphicsDesigner` (`ui/GraphicsDesigner.h`) gets `SetMotion`. Save writes it; the Titles panel preview shows a mid-roll frame.
6. **Tests**: `render_tests.cpp` — `ARollMovesTheContentFromBelowTheFrameToAboveItOverTheClipsLength` (draw at p=0, 0.5, 1; check which rows are lit); `ACrawlMovesSideways`; `AnOlderDocumentWithoutMotionIsUnchangedAndKeepsItsSchema`. `ui_tests.cpp` — designer plan sets it and undoes. App — create a title, set roll, scrub: the picture moves.

### C6.2 On-canvas text editing

1. In `GraphicDesignerPanel.qml`, on double-click of a text element (the designer already reports the hit via `designerPress`), overlay a `TextArea` positioned and sized to the element's rectangle in canvas coordinates, with the element's font size scaled to the canvas; Enter commits (`session.designerSetProperty("text", value)`), Escape cancels. `Shift+Enter` inserts a line break (the document supports `\n`; check `render/TextRaster.cpp` wrap rules).
2. Keep the old text field (it remains the way to edit templates' text controls).
3. App test: set the overlay's text through a test hook, commit, the document's text changes in one designer undo step.

### C6.3 Also on the list
- Multi-select and grouping (`TODO.md` GFX-001): selection becomes a set in `ui/GraphicsDesigner`; align/distribute apply to the set; grouping adds a `group` element holding children (document schema bump, again only when used).
- Gradients as fills (`fill: {"kind": "linear", "stops": [...]}`); SVG import is a separate package (needs a path rasteriser — the mask code in `effects/MaskDocument` has Bézier flattening you can reuse).

**Check by hand.** Graphics workspace: new title from the "Quotation" template, set motion = roll, "fit"; add to the timeline, set its length to 8 s, play: text rolls through the frame and finishes as the clip ends. Export and play.

**Docs/parity**: GFX-001 limits, `UI-GRAPHICS` row, `graphics` domain.

---

## WP-C7. Export features

The export code is `exporter/ExportWorker.cpp` (render and write), `exporter/ExportPresets.cpp` (what a person chooses), `exporter/ExportQueue.cpp` (order and persistence), `exporter/ExportValidation.cpp` (check the finished file), `media/ffmpeg/FFmpegWriter.cpp` (encode and mux), and in the application `app/SessionExport.cpp` plus `app/qml/ExportDialog.qml`. Read `exporter/ExportWorker.h`'s header comment first.

### C7.1 Export at a different size

1. **Today** `Export` refuses a frame size other than the sequence's (`ExportAtAnotherSizeIsRefusedBeforeAnythingIsWritten` is the test that pins it).
2. **Policy.** Render at sequence size, then scale the finished frame to the delivery size with a documented filter, before `WriteVideo`. (Re-rendering at the new size is *not* equivalent: effect parameters are authored in sequence pixels.) The scaling must be done in a function of its own so tests can check it.
3. **Steps**: add `media::ScaleFrame(const VideoFrame&, int w, int h, ScaleFilter)` in `media/VideoFrame.cpp` — bilinear first (box filter when shrinking by more than 2x to avoid aliasing; use area averaging), keep the pixel format, copy colour tags; in `ExportWorker.cpp`'s `FramePipeline` render lambda, call it when `request.video.width/height` differ from the sequence's; remove the refusal; add the fit rule to `ExportRequest` (`fit`: `stretch`, `letterbox` (keep aspect, pad with black), `crop`) and the dialog (`ExportDialog.qml` size fields with a link-aspect checkbox).
4. **Tests**: media — `ScalingAConstantPictureKeepsItsColourAndAGradientKeepsItsEnds`, `DownscalingAChessboardByTwoGivesGrey`, aspect fits (the letterbox bars are black and exactly sized); export — `AnExportAtHalfSizeDecodesToHalfSizePicturesOfTheSameScene` (decode the file with `media::SourceRegistry`, compare the centre pixel to the scaled monitor frame within tolerance). Replace the old refusal test with a refusal for **invalid** sizes (zero, odd sizes for 4:2:0 codecs: pad or refuse with a message).
5. **Check by hand**: export the demo at 640x360 and at 1920x1080 letterboxed into 2000x1080; look at both.

### C7.2 Image sequences and stills
1. Presets `images.png`, `images.tiff`, `images.jpeg`, and "Export current frame" (a one-frame sequence). Container `image2` with the pattern `name_%05d.png`. Look at how a preset names its encoder chain in `ExportPresets.cpp` (e.g. `Soft("png", "rgb24")`, `Soft("tiff", ...)`, `Soft("mjpeg", ...)` — check which are in the FFmpeg build with the compatibility test you will write in C10).
2. **The temporary-file rule.** `ExportWorker` writes to a unique temporary file and renames it over the destination. A sequence is many files. Give `ExportSettings` an `output_kind {File, Sequence}`; for a sequence write into `<destination>.partial/` and rename the folder when complete; a cancelled export deletes the partial folder.
3. **Validation** (`ExportValidation.cpp`): the number of files equals the frame count; the first and last decode.
4. **Tests**: export 10 frames of a lossless PNG sequence, files exist and are named correctly, decoding frame 7 equals the monitor's frame 7 exactly; cancel mid-way leaves no folder; refuse when the destination folder exists and overwrite is off.
5. **Dialog**: pattern field with a live preview of the file names; number start (default 0 or 1).

### C7.3 Animated GIF (optional)
Needs a palette: implement median-cut or use the `palettegen`/`paletteuse` filters if the FFmpeg build has `libavfilter`. Dithering optional. A GIF has no sound; the preset disables audio. Tests: frame count and palette size ≤ 256; looping flag.

### C7.4 Render and replace
Replace a section of the timeline by a rendered file, keeping the ability to restore.
1. **Command pair (R1, R2):** table `replacements(id, new_clip_id, original_json, created_at)`; commands `RenderReplace {range, tracks, media_id (the rendered file already imported), replacement_id}` that (a) stores the original clips' payloads and effects as JSON, (b) deletes them, (c) inserts one clip referencing the rendered media across the range; and `RestoreReplaced {replacement_id}` that reverses it (re-inserts the stored clips and removes the stand-in). Both validate locks.
2. **Job**: render the range with a mezzanine preset (ProRes 422 HQ) into `<project>/renders/`, import it (`IngestFile`), then run `RenderReplace`. The render uses `ExportClone` (originals, full quality) — so the replacement is *better or equal* to the preview.
3. **UI**: right-click clip > "Render and Replace…" with preset chooser; stand-in clips are drawn with a badge; "Restore Unrendered".
4. **Tests**: command tests; round trip: replace then restore gives a sequence identical to before (compare snapshots, including effects and keyframes); the stand-in plays the same picture as the original (mean pixel difference small for a lossless preset).

### C7.5 Smart render (XL — needs a design review first)
Copy compressed packets when nothing changes. Minimal version for review: a sequence that is one unedited clip (or concatenated clips of the *same* codec parameters) with no effects/transitions: re-mux packets on GOP boundaries and re-encode only the partial GOPs at cuts. Needs a packet-level `Source` interface (`ReadPacket`) and a `Writer::WritePacket`. Define the exact eligibility rule and a fallback to normal export with a message.

### C7.6 Watch folders
A list of `{folder, preset, output folder}` kept in the configuration. A `QFileSystemWatcher` plus a 2-second "size stable" check queues a **transcode** of each new file: build the one-clip source graph from B1 and run the normal `Export` with the chosen preset. Tests: drop a fixture in a temp folder, a file with the preset's extension appears in the output folder; a half-copied file is not picked up (size changes during the check).

### C7.7 Metadata, HDR flags, burn-in
HDR metadata (mastering display, MaxCLL) for PQ/HLG deliveries: set the colour tags on the stream and side data in `FFmpegWriter`; a timecode burn-in option reuses the caption rasteriser (`render/TextRaster.h`); tests read the tags back with a probe.

**Docs/parity (C7 overall)**: `export_dialog` rows in `IMPLEMENTATION_STATUS.md` (EXPORT-001 limits), `render_and_replace`, `smart_render`, `image sequences`.

---

## WP-C8. Media path aliases

**Problem.** When a drive letter or folder changes, every clip goes offline and each media item must be relinked by hand.

**Design.** Two layers: **per-media alternates** (extra known paths for one media) and **project path rules** (a prefix mapping applied to all media).

**Steps**

1. **Schema (R2)**: `media_aliases(media_id REFERENCES media ON DELETE CASCADE, path TEXT, PRIMARY KEY(media_id, path))` and `path_rules(id, from_prefix, to_prefix, sort_order)`; both in `RecordedTables()`.
2. **Commands (R1)**: `AddMediaAlias`, `RemoveMediaAlias`, `SetPathRules {rules}` (replace all).
3. **Locator**: `Session::StartEngine`'s `MediaLocator` (the lambda in `app/Session.cpp`) currently returns `original_path`. Make a testable function `ResolveMediaPath(original, aliases, rules, exists)` in `media/PathResolver.h (new)`: try the original if it exists; else apply each rule (longest prefix first) and test; else each alias; else return empty (offline). Do the file-existence checks through an injected function so the test needs no disk.
4. **Offline detection**: `media.missing` is a stored flag set by relink commands, not by a scan. Add `Session::scanOffline()` (job): for each media run the resolver; for hits run `RelinkMedia` with the found path (fingerprint check as in B6.4) and clear `missing`.
5. **UI**: Project panel "Path rules…" dialog (table of from/to prefixes with a "Test" button listing how many media each rule fixes), media Properties shows aliases.
6. **Tests**: `media_tests.cpp` resolver cases (rule precedence, alias, nothing); `store_tests.cpp` commands; `app_tests.cpp` change a rule, `scanOffline`, the item becomes online and the monitor shows its picture.

**Check by hand**: import from `D:\Footage`, move the folder to `E:\Footage`, open the project (all offline), add the rule `D:\Footage` → `E:\Footage`, scan: online.

---

## WP-C9. GPU kernels

**State today.** `render/D3D11Compositor.cpp` runs compute shaders written as one HLSL source string (`kShaderSource`): draw a layer with transform/crop/opacity/grade/colour tools/LUTs, composite, mix a dissolve, convert to output. Any plan containing a filter, key, blend mode, mask, graphic, caption, noise reduction or optical flow falls back to the software compositor for the **whole frame** (`Supports` returns a reason, the engine counts it). The finished picture is read back to memory for the monitor.

This is the largest piece of engineering left on the render side. Do it in this order; stop after any step and the product is better.

**Rules for every kernel**
1. The software code stays the definition. The shader must match it within the tolerance the existing GPU tests use ("at least 99.8% of pixels within two levels of 255 and a mean difference under half a level"; read the helper in `gpu_tests.cpp`).
2. A kernel is added together with: its eligibility rule in `Supports` (parameters in range, sizes within limits), its parity test, and a benchmark line in `PERFORMANCE_LOG.md` (`scripts\bench.bat --gpu`).
3. Use the shared parameter readers in `render/CompositorParams.h` and `render/ColorOps.h` (`DescribeColorOp`) so CPU and GPU read parameters identically; add a `Describe...` function beside the software code for each new kernel.
4. A device error must still switch the engine to software for good and say so (`gpu_failures`).

**Steps**

1. **Layer effect passes (infrastructure).** Today the pipeline is `DrawLayer → (ops on canvas) → CompositeOver`. Add a `LayerEffectPass` stage between draw and composite for effects that need neighbours: ping-pong between the layer texture and a scratch texture of equal size (allocated on demand like the transition layers: `EnsureTargets` allocates lazily — follow that pattern and keep the memory accounting the audit found).
2. **Separable Gaussian blur** (`gaussian_blur`, `blur`): horizontal then vertical passes with a radius-dependent kernel computed on the CPU into a constant buffer (cap the radius, refuse above the cap in `Supports`); groupshared memory for the tile; match `ApplyGaussianBlur`'s edge handling exactly (clamp).
3. **Sharpen/unsharp mask, vignette, lens correction** (inverse-mapping gather with bilinear), **bulge/wave warp** (gather), **mesh warp** (4×4 grid → bilinear gather), **posterize/mosaic/find edges/etc.** (per-pixel or small neighbourhoods).
4. **Blend modes**: replace `CSDrawOver`'s source-over with a switch on mode; the CPU formulas are in `render/BlendModes.cpp` (12 modes, premultiplied); port them exactly; test each mode against `CompositeBlend`.
5. **Masks**: evaluate the mask shape on the GPU — upload the mask as a signed-distance or coverage texture generated on the CPU for the current time (small: render at the layer size once per mask per frame with the existing CPU rasteriser in `effects/MaskDocument`/`Compositor::BlendMaskedEffect`) and blend before/after exactly as `BlendMaskedEffect` does. This reuses CPU code and still saves the whole-frame fallback.
6. **Keys** (chroma/luma/ultra): per-pixel plus a matte cleanup (shrink/feather = small blurs) → needs step 2.
7. **Track mattes** (C1): read a second layer texture.
8. **Hybrid execution.** Instead of all-or-nothing, let the engine split a plan *per layer*: layers whose effects the card supports are composed there; unsupported layers are composed in software, uploaded as a texture and composited on the card in order. Requires `Supports` to return per-layer decisions and `Compose` to accept pre-rendered layers (`FrameResolver`-like callback for "software layer for track X").
9. **No read-back for the monitor.** Today the picture is copied back to memory and then into a `QImage`. With Qt 6.8's RHI on Direct3D 11, share the output texture with the Qt Quick scene graph (`QQuickWindow::createTextureFromNativeObject` / `QSGTexture` from a shared handle): the compositor creates its output texture with `D3D11_RESOURCE_MISC_SHARED`, the `MonitorItem` becomes a `QQuickItem` using a `QSGSimpleTextureNode`. Keep the CPU path for tests (`QT_QUICK_BACKEND=software`) and scopes (scopes need a CPU copy of a *downsampled* picture; do that with a small GPU downsample + read-back).
10. **Export on the GPU** (`ExportClone` disables it deliberately: "a delivery is rendered by the reference path"). Only after the parity tolerance is *proved acceptable for delivery* by a documented comparison (PSNR vs CPU on a corpus), add `ExportRequest::allow_gpu` default false.

**Tests per kernel** (`tests/native/gpu_tests.cpp`): `BlurOnTheCardMatchesTheSoftwareCompositorAtSeveralRadii`, edge cases (radius 0, huge radius refused), a plan with the effect and an unsupported one falls back whole (until step 8), then splits. These tests run on Direct3D's software rasteriser when no adapter exists; keep that property.

**Performance gates** (record in `PERFORMANCE_LOG.md`): 1080p with one blur ≤ 8 ms on the reference adapter; four layers each with a blur ≤ 20 ms; no more than 5% regression of the existing numbers.

---

## WP-C10. Decode validation and the codec matrix

**Problem.** HEVC decode and still-image decode are `partial` because nothing exercises them; the codec compatibility list is hard-coded in the documents.

**Steps**

1. **Generate the matrix from the real build.** A tool in `app/` or `scripts/`: for each registered FFmpeg decoder and encoder that Cutline's presets or ingest can use, report name, whether this build has it, and hardware variants. Output to `compatibility.md` (generated, committed, with the date and FFmpeg version string). The test `TheCompatibilityMatrixListsEveryPresetEncoderThatWorks` checks that every encoder named in `ExportPresets.cpp` appears with its availability.
2. **Fixtures.** `cmake/Fixtures.cmake` generates test media with the bundled `ffmpeg` binary at configure time. Add: HEVC 8-bit and 10-bit (if the build has an encoder; else skip with `SKIP_INAPPLICABLE`), PNG/JPEG/TIFF stills (including one with EXIF rotation), VFR, interlaced metadata, 10-bit, HDR-tagged (PQ), alpha (ProRes 4444 or PNG sequence), portrait and anamorphic pixel aspect. Add the fixtures required by `TODO.md`'s "Resolution and codec follow-up".
3. **Tests** in `tests/native/media_tests.cpp`: for each fixture — probe reports the right codec/size/rate/colour tags; the first, a middle and the last frame decode to known values (the generator makes frames identify themselves, see the existing `EveryFrameOfAReferenceFileIdentifiesItself`); stills decode with orientation applied. Document in `IMPLEMENTATION_STATUS.md` what is *measured* versus *assumed*.
4. **Hardware decode of HEVC** on the GPU path (`media/ffmpeg/FFmpegSource.cpp` `ReadDeviceVideo`) is validated per adapter — record the adapter and result in the matrix; never claim support for an adapter you did not run.

**Parity**: `hevc_decode`, `still_image_decode`, `probe_diagnostics`, `codec_license_reporting` (add a generated list of codecs with their licence class, LGPL vs GPL build flags from the FFmpeg `configuration` string).

---

Continue with [D-large-programmes.md](D-large-programmes.md).
