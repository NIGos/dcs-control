# Perception

Features that work with how the eye sees rather than with how the image is rendered. They share a few helpers in the bundled Quad Views compositor:

- `native/quadviews/dcsvr_perception.h`: one header for C++ and HLSL. It holds the eccentricity of a view point from the gaze, the gaze and saccade state of each display time (one record for both eyes), the bundled Quad Views' gaze export for the pupil shift layer (`DcsVr_QuadViewsGaze`, below), and the perceptual models used to set defaults (flicker fusion, acuity).
- `native/quadviews/dcsvr_gaze.h`: the saccade detector the focus area already uses. Every perception feature reads the same detector, with the same thresholds.

`scripts/build-quadviews.ps1` copies both headers into the Quad Views source and builds it. `tests/perception` checks them offline.

Pupil shift has its own document: [PUPIL_SHIFT.md](PUPIL_SHIFT.md). Depth boost lives in the same pupil shift OpenXR layer (`native/pupil_shift`) and shares its gaze input; Periphery stability and Far depth ([FAR_DEPTH.md](FAR_DEPTH.md)) live in the bundled Quad Views compositor. A shared Perception page in the app comes later; for now their settings are on the **Pupil shift** page.

## Periphery stability

### What it does

The periphery is rendered at about 15 to 20 % of the headset's resolution per axis. Quad Views then enlarges it about six times. A thin high-contrast line that moves by a fraction of a periphery pixel between frames therefore pumps across several headset pixels. Canopy frames, power lines, runway edges and the horizon are typical examples. The shimmer comes from DCS's DLSS residue at this small size and from the periphery contrast pass.

Periphery stability blends each periphery frame with the previous result, outside the focus area only. It runs once per rendered frame inside the bundled Quad Views, so frame generation receives the steadier image without extra work.

Off by default. The setting is on the Pupil shift page, in the **Periphery stability** card: one switch and one **Strength** slider. It works with a fixed or an eye-tracked focus. It runs in the bundled Quad Views only. With Pimax's own quad views, or without Quad Views, it is not applied, and Checks and the page say so.

### Why there

Krajancich, Kellnhofer and Wetzstein (ACM TOG 40(4), 2021) measured flicker fusion across the visual field. For content at 0.5 to 2 cycles per degree, which is where the enlarged periphery shimmers, the fusion limit is 30 to 75 Hz out to about 35°. It peaks between 10 and 30°, right outside the focus area.

DCS renders at 45 FPS in 2X and 30 FPS in 3X, so shimmer changes at most at 22.5 or 15 Hz. That is below fusion everywhere that matters: the eye sees it, most of all just outside the sharp area. Only content near 2 cycles per degree beyond about 40° (2X) or 46° (3X) is fused. The far fade-out below is computed from the same model.

### How it works

`native/quadviews/dcsvr_temporal_cs.hlsl` runs per eye at the periphery's own resolution. It runs after the flatten and before the periphery contrast pass.

1. **Reprojection.** The previous result is sampled where the same direction was in the previous frame, from the rotation between the two eye poses DCS rendered with. This is a rotation only, with the plane at infinity. The sample is Catmull-Rom (five bilinear taps), so a turning head does not soften the history frame after frame.
2. **Clamp against trails.** The history is clamped in YCoCg to the current 3×3 neighbourhood's mean ± 1 standard deviation. The box is widened to hold the pixel itself, so a still thin line is never dimmed. A history that had to be clamped by more than one standard deviation is dropped (confidence). Content that moves on its own, which has no motion vectors here, therefore falls back towards no filtering instead of a trail.
3. **Time-based blend.** The history weight is `exp(-dt / tau)`, where `dt` is the time between displayed frames. The same strength therefore gives the same response in 2X, 3X and Auto, and after a late frame. Strength 0.5 is `tau` = 40 ms: a first-order low-pass at about 4 Hz, which leaves about 18 % of 22.5 Hz shimmer.
4. **Weight by eccentricity.** `tau` is scaled by a weight:
   - 0 inside the focus area. The focus area is the edge the composition draws, where the focus view is this frame: the superellipse of `focus_view_shape` inscribed in the focus view (the ellipse for shape 2, so an elongated focus view keeps its own width on each axis), or the focus view's rectangle without a shape.
   - Strength 0 does not run the pass at all, and a second projection layer in the same frame is left unfiltered (one history per eye).
   - A scene cut that the head pose does not show (DCS switching view or camera, a respawn) can reset the history: another module in DCS's process calls the layer's `DcsVr_QuadViewsTemporalReset()` export (no arguments, any thread). Without it the neighbourhood clamp limits the old scene to a 1-3 frame smear.
   - Ramps up over 8° outside it, so no lag shows where the sharp focus fades into the periphery.
   - Fades out where shimmer is fused anyway. The start and end of that fade come from the flicker fusion model at 1.5 cycles per degree and 100 cd/m², against the measured frame rate: 39.5 to 48° at 45 FPS, 46 to 53.5° at 30 FPS.

   Eccentricity is measured from the cyclopean gaze, the same world direction for both eyes, or from the focus centre with a fixed focus.
5. **Resets.** One decision per frame covers both eyes, so one eye is never filtered while the other is not. The filter starts over in these cases:
   - the first frame;
   - a saccade (dcsvr_gaze.h's detector; vision is suppressed then, so the reset is free);
   - more than 100 ms between frames (a pause, a loading screen, a scene change);
   - a head turn of more than 10° in one frame;
   - a new periphery size or a new layer space;
   - the focus debug view, or an alpha-blended layer.
6. **The contrast pass never feeds back.** The periphery contrast pass reads the stabilised periphery and writes its own texture, so sharpening is never stored as history.

Each eye uses two RGBA16F textures at periphery size, written in turn. At 814×804 that is 5.2 MB each, about 21 MB for both eyes.

### Settings

| Profile | Quad Views key (`settings.cfg`, `[exe:DCS]`) | Default | Range |
|---|---|---|---|
| `PeripheryStability` | `dcsvr_temporal` (1 only with the bundled Quad Views in a VR flight) | off | on/off |
| `PeripheryStabilityStrength` | `dcsvr_temporal_tau_ms` = 80 ms × strength | 0.5 (40 ms) | 0–1 |
| — | `dcsvr_temporal_ramp_deg`: from the focus edge to full weight | 8 | 1–30 |
| — | `dcsvr_temporal_clip`: clamp box half-width in standard deviations | 1 | 0.25–4 |
| — | `dcsvr_temporal_debug`: tints where and how strongly it acts (red), without filtering | 0 | 0/1 |

The advanced keys are written at their defaults and are meant for testing only. A lower `dcsvr_temporal_clip` trails less on scenery that moves on its own but steadies less. Offline, 0.75 with a tighter confidence cut the remaining roll lag over the horizon by about a fifth. It also left more of the shimmer at 28–35°: 63 % instead of 53 %, both measured in the harness's smaller `--quick` run.

### Live changes

The bundled Quad Views re-reads its `settings.cfg` files while DCS runs (`native/quadviews/dcsvr_live.h`, tested by `tests/quadviews-live`). At most once a second a thread-pool poll compares each file's write time and size, and parses only when one changed. It uses the same files, order and section rules as at start. The next frame applies these keys together, between two frames: `sharpen_focus_view`, `smoothen_focus_view_edges`, `dcsvr_sharpen_taper`, `dcsvr_periphery_sharpen`, `dcsvr_temporal`, `dcsvr_temporal_tau_ms`, `dcsvr_temporal_ramp_deg` and `dcsvr_temporal_clip`. A change shows within about one second. Every other key keeps its start value until DCS restarts: sizes, multipliers, sections, `focus_view_shape`, turbo, eye tracking and Far depth. A reload sets each live key to its file value, even one that Quad Views' debug keys changed since. Turning periphery stability off frees its history; turning it on again starts it over.

### In the log

`%LOCALAPPDATA%\Quad-Views-Foveated\Quad-Views-Foveated.log` (or the profile's own log) shows these lines:

- On each reload of `settings.cfg`: `DCSVR live settings: settings.cfg reloaded: sharpen_focus_view 0.70 -> 0.50; kept until the next start: focus_multiplier`.
- At the first frame (and again after a live change to its keys): `DCSVR periphery stability: on, 40 ms, ramp 8 deg, clip 1.00`.
- Every 30 s: `DCSVR periphery stability: N frames, M filtered at 45.0 FPS, R resets (saccade a, gap b, turn c, size d, other e), fades out 40-48 deg`.

Many resets from turns or gaps mean the filter is idle much of the time.

### Offline validation

`tests/perception/perception_test.cpp` is built and run by `scripts/build-quadviews.ps1`.

It first checks the shared maths on the CPU:

- the eccentricity;
- the weight, which is monotonic and 0 inside the focus;
- the blend, which keeps the same history share after 100 ms in 2X, 3X and Auto;
- the flicker fusion model against the paper's values, and the acuity model;
- every reset reason;
- the reprojection direction;
- the gaze brought into canted eyes.
- the gaze export (`DcsVr_QuadViewsGaze`): the valid, saccade and no-gaze flags, nothing for a display time more than 100 ms after the last sample, and one view-space gaze 10° right reading 13° and 7° in eyes canted 3° outwards.

It then runs the real compute shader on a D3D11 device, the GPU or WARP, over synthetic periphery sequences. The scene is defined in world directions, so every frame has an exact ground truth. It is shown on a periphery of ±50° at 407×402 (half the real 814×804 per axis, same field), with a centred focus of 20° radius and the default settings. There are two scenes:

- **Lines**, the worst case: thin bright lines every degree or so, with a fine checker above the periphery's Nyquist frequency.
- **Horizon**: sky over ground, a road every 6° and a few thin wires.

Results from 2026-10-11 on an RTX 5090, with DCS running on the same GPU at the time.

**(a) Still scene, per-frame sub-pixel jitter (Lines).** Temporal flicker is the mean per-pixel standard deviation of luma.

| Frame rate | Band (deg) | Off | On | On / Off |
|---|---|---|---|---|
| 45 FPS (2X) | 0–20 (focus) | 0.2106 | 0.2106 | 1.00 |
| 45 FPS (2X) | 20–28 (ramp) | 0.2016 | 0.1561 | 0.77 |
| 45 FPS (2X) | 28–35 | 0.1935 | 0.1146 | 0.59 |
| 45 FPS (2X) | 35–45 | 0.1782 | 0.1167 | 0.65 |
| 45 FPS (2X) | 45–70 (fused, fades out) | 0.1536 | 0.1528 | 0.99 |
| 30 FPS (3X) | 0–20 (focus) | 0.2130 | 0.2129 | 1.00 |
| 30 FPS (3X) | 20–28 | 0.2063 | 0.1752 | 0.85 |
| 30 FPS (3X) | 28–35 | 0.1982 | 0.1386 | 0.70 |
| 30 FPS (3X) | 35–45 | 0.1829 | 0.1276 | 0.70 |
| 30 FPS (3X) | 45–70 | 0.1579 | 0.1351 | 0.86 |

Against the 4×4 supersampled truth at 28–45°, 2X, the error drops from 0.242 (off) to 0.173 (on).

**(b) Steady head turn, explained by the reprojection.** The input is anti-aliased (the truth itself), so any error is what the filter adds. The table gives RMS error against the truth at 28–45°.

| Scene | Turn | On | On, not reprojected | Previous frame shown (1-frame ghost) |
|---|---|---|---|---|
| Horizon | 37°/s | 0.0103 | 0.0201 | 0.0644 |
| Horizon | 113°/s | 0.0092 | 0.0199 | 0.0639 |
| Horizon | 211°/s | 0.0091 | 0.0200 | 0.0640 |
| Lines | 37°/s | 0.0575 | 0.0953 | 0.2454 |
| Lines | 113°/s | 0.0545 | 0.0946 | 0.2438 |
| Lines | 211°/s | 0.0535 | 0.0931 | 0.2314 |

What remains is the resampling of detail finer than a periphery pixel, not a trail. With the clamp disabled (113°/s, Horizon), the error is 0.0092 with the reprojection, 0.0239 without it, and 0.0245 when reprojected the wrong way (the negative control). With jittered aliased input during a 100°/s turn, the error against the truth drops from 0.256 to 0.179.

**(c) Content that moves on its own.** This is a horizon roll with the head still, which the reprojection cannot explain because there are no motion vectors. The input is anti-aliased, so the error is the lag.

| Scene | Roll | On | On, clamp disabled | Previous frame shown (1-frame ghost) |
|---|---|---|---|---|
| Horizon | 10°/s | 0.0220 | 0.0236 | 0.0355 |
| Horizon | 30°/s | 0.0279 | 0.0342 | 0.0720 |
| Horizon | 90°/s | 0.0269 | 0.0401 | 0.0915 |
| Lines | 10°/s | 0.0868 | 0.0945 | 0.1673 |
| Lines | 30°/s | 0.1080 | 0.1177 | 0.2793 |
| Lines | 90°/s | 0.1036 | 0.1197 | 0.2936 |

The lag stays under 0.7 of a one-frame ghost: 0.29 to 0.62 on the Horizon, 0.35 to 0.52 on the Lines. On the Horizon, the clamp cuts it by a third at 90°/s.

Dense line patterns keep the history inside the neighbourhood's range, so there the clamp helps little. That is the documented worst case: a faint trail of about a third to a half of a one-frame ghost.

**(d) Resets.** While filtering, the largest |output − input| is 0.531. It is 0.0002 (the fp16 rounding) on a saccade frame, after a 150 ms gap and on a new periphery size. Filtering resumes on the next frame.

**(e) Frame generation timing.** This is a +0.03 step on a textured still scene, inside the clamp box, at 28–45°. The table gives the share of the step still missing.

| Time after the step | 2X (45 FPS) | 3X (30 FPS) | Auto (45/30 alternating) | exp(−t / 40 ms) |
|---|---|---|---|---|
| 67 ms | 0.073 | 0.072 | 0.100 | 0.189 |
| 100 ms | 0.038 | 0.036 | 0.049 | 0.082 |
| 133 ms | 0.023 | 0.023 | 0.029 | 0.036 |
| 200 ms | 0.019 | 0.015 | 0.015 | 0.007 |

2X, 3X and Auto agree within 0.03 at the same time. The step settles faster than the pure exponential because of the clamp's confidence and the far fade-out in that band. The tail of about 0.015 is one fp16 step of the history at that brightness.

**Cost.** The median GPU time of the pass at 814×804 is 0.014 ms per eye, about 0.03 ms for both. That was measured with timestamp queries on the RTX 5090 while DCS was running. The composition at 5424×5356 per eye stays by far the larger cost.

### Limits and open points

- **The benefit in DCS is not measured yet.** DCS's DLSS may already remove most shimmer, and what is left may be aliasing that stays put rather than flicker. A flight A/B decides whether the feature stays. Look at an instrument and judge the canopy frame, power lines and horizon at 15 to 35°. Also check fast head turns, low-level rolls, a rotor in the periphery and night, at 2X, 3X and Auto.
- **No in-flight switch yet.** Quad Views reads its settings at start, so an A/B needs two flights. A shared Perception switch, in the style of Pupil shift, is the planned way to compare.
- **No motion vectors, and the reprojection is rotation only.** Translation and the view changes made by Pupil shift are left to the clamp. These are small, and they happen mostly during saccades, when the filter resets anyway.
- **Not taken into account:** brightness, the headset's luminance (not measured; 100 cd/m² is assumed) and attention (Krajancich et al. 2023: peripheral sensitivity drops while the eye works on a task). The latter is why the default strength is moderate.

## Depth boost

Status: implemented, off by default, validated offline only (unit tests and an end-to-end harness, below). Not yet
flown. The fixation depth (where the eyes look and how far) has its own document, [FIXATION_DEPTH.md](FIXATION_DEPTH.md):
the scene depth from the engine module, Pimax PVR's per-eye gaze, the rules for overlapping geometry and their
validation. This section keeps the mechanism, the vergence source and the control law.

### What it does

While the eyes fixate far, the render IPD (the distance between the two cameras DCS renders from) is scaled up, so
distant objects get that many times more binocular disparity: more stereo depth on and around them. While the eyes
fixate the cockpit the scale returns to exactly 1, so the cockpit keeps its true size and matches head motion and the
pilot's hands.

Two modes set how much, from the distance looked at (FIXATION_DEPTH.md, "Modes"). **Realistic** (default): ×1 up to
150 m, where a headset's stereo is already about as good as real stereo (one pixel of disparity at ~46 px/deg falls
near 170–190 m), then a log ramp to ×1.25 at 400 m and beyond, the band where real stereo still works (stereoacuity of
20–30 arcsec reaches 450–650 m) and the headset's pixels no longer resolve it. A wingman or a rejoin keeps its true
distance. It needs the scene depth (the engine module, installed with Depth boost); on the eyes' vergence alone it stays at ×1. **Enhanced**: the
v1 behaviour, more depth from 3 m and ×1.4 from 50 m. More depth, not realistic: distant objects look nearer and the
world smaller. For realism the base IPD should be the headset's measured one (65.5 mm on the development PC); today the
app hands the layer DCS's custom IPD when one is set.

The cost: while the scale is above 1 the whole world looks somewhat smaller (a larger render IPD reads as a larger
head), and the first fraction of a second of a glance back at the instruments happens at the raised scale. It does not
know the height above the ground yet, so keep it off for landings (the ground relief would be exaggerated during the
flare).

It is a camera-based simplification of GazeStereo3D (Kellnhofer, Didyk, Myszkowski, Hefeeda, Seidel, Matusik; ACM TOG
35(4), 2016). GazeStereo3D remaps the disparity of a rendered image with a non-linear curve around the fixation and
applies it by warping. Here only its linear special case is used, a uniform disparity scale, produced by the cameras
themselves: DCS renders true geometry at the new IPD, so there are no warping artifacts. Convergence (a disparity
shift) is never changed: in a headset it would only move infinity to a finite distance. Points at infinity keep zero
disparity at any scale, so the horizon and sky do not move in depth; only finite depths change, in proportion to 1/Z.

### Mechanism

- The pupil shift layer already moves both eyes symmetrically along the line between them to apply DCS's custom IPD
  (`Ipd=`, see PUPIL_SHIFT.md). Depth boost applies `IPD_render = s × IPD_base` with the same move every frame:
  `IPD_base` is `Ipd=` (DCS's custom IPD, taken over by the app when either feature is on) or the headset's own eye
  distance. The move happens before the pupil shift; the FOV is shifted only for the pupil, never for the IPD (the IPD
  change must reach the display as disparity).
- `xrEndFrame` gives the runtime its own poses and FOVs back, as for the pupil shift: the compositor, OFXR and every
  layer below see the frame as if the layer were absent; only the rendered content differs.
- Pupil shift and Depth boost are independent: `[PupilShift] Enabled` and `[DepthBoost] Enabled` in `PupilShift.ini`.
  The layer is deployed, and DCS's custom IPD is taken over, when either is on (`VrProfile.UsesPerceptionLayer`).

### Transition: gradual or predictive (Legacy=1 only)

Only the legacy path (`[DepthBoost] Legacy=1`, diagnostics) uses this: with the engine's dual camera the scale changes
only smoothly, so there is nothing to predict. The app writes `Transition=gradual` and `TransitionToggle=0:0` and no
longer offers the choice, and the engine's depth grid (`DepthGrid`) is read only by the predictive transition.

`[DepthBoost] Transition` (in flight: `TransitionToggle`, e.g. Alt+Shift+F9, one beep gradual, two beeps
predictive). Gradual (default) changes the scale at the rates below, confirmed after the eyes land. Predictive
samples Pimax PVR's gaze at about 500 Hz, predicts a long saccade's landing at its velocity peak from the main sequence,
looks the landing up in the engine module's coarse depth grid, and makes one capped step (12 mm of IPD, 0.4° of
disparity at the landing) while the eyes are still in flight; the rest follows the gradual rates, and the landing's
cone confirms the step or reverts it at the normal rates. Without PVR's per-eye gaze or the grid it works as gradual.
Details, numbers and risks: FIXATION_DEPTH.md, "Predictive transition".

### Fixation depth

The primary source is the scene depth at the gaze, measured by the engine module and published in shared memory; the
binocular vergence below is the secondary one (vetoes, and the only source when no scene depth arrives). How the two
are combined, and the rules for overlapping geometry, are in [FIXATION_DEPTH.md](FIXATION_DEPTH.md).

### The vergence source

**Gaze source.** Pimax PVR's per-eye gaze when the layer has found the runtime's PVR session (FIXATION_DEPTH.md,
"Pimax PVR per-eye gaze"; `[DepthBoost] PimaxPvrGaze=1`). Otherwise each eye's gaze from the same source as the pupil
shift (PUPIL_SHIFT.md), without `SimulateGaze`:

- with the bundled Quad Views, from its export `DcsVr_QuadViewsGaze(displayTime, left[3], right[3])`. Flags: bit 0 a
  sample for that display time (or the latest one within 100 ms before it), bit 1 a real eye-tracker gaze, bit 2 a
  saccade (dcsvr_gaze.h's detector). The gaze is Quad Views' one combined gaze, given to each eye in that eye's own
  view frame, so both eyes get the same direction. Quad Views records it for every located display time, whether
  Periphery stability is on or not (one `GazeHistory` for the process, `SharedGazeHistory()`). Without a valid gaze
  Quad Views shows a fixed focus (the resting gaze plus a per-eye horizontal offset, about 2.4° outwards on the Crystal
  Super). The focus-view reading took that for a real gaze; the export says it is none, and Depth boost gets **no
  sample** for that frame (`no_gaze` in the `depth:` line). Quad Views' saccade flag counts as a saccade for the
  estimator (the sample is rejected, as with its own speed rule);
- without the export (Pimax native Quad Views), from the runtime's eye gaze through `XR_EXT_eye_gaze_interaction`,
  which the layer enables and reads with its own action (PUPIL_SHIFT.md). The location is in the base space of
  `xrLocateViews`, at its display time. The gaze is one combined direction, given to each eye in its own view frame (as
  with the export). It counts only with `ORIENTATION_VALID` and `ORIENTATION_TRACKED`. Otherwise the frame has no gaze
  and Depth boost gets **no sample**, although Pimax then places the focus views at a fixed spot about 2.4° outwards
  per eye. The extension has no saccade flag;
- only when neither exists (the runtime does not offer the extension or reports no eye-gaze support, DCS attached no
  action sets, `OpenXrEyeGaze=0`), from the focus views: the left eye from focus view 2, the right eye from focus view 3
  (their FOV is centred on the gaze). The log warns once that a fixed focus would then read as a gaze.

**Vergence.** In the head's frame (right = from the left eye to the right
eye, forward = between the two canted views), each gaze has a horizontal tangent `tx`. The two rays meet at depth
`z = ipd / (tx_left − tx_right)`, at a distance `r = z · sqrt(1 + tx² + ty²)` with the mean tangents. The estimator
works with the inverse distance `w = 1/r` (0 = infinity): diverging or parallel rays read as infinity, rays nearer than
0.2 m as 0.2 m (`inverse_fixation_distance` in `pupil_math.hpp`).

Filtering (`FixationEstimator`):

- **Saccades** are rejected: the mean gaze turns faster than 120 deg/s, measured between the mean of the last three
  samples and of the three before (this divides the apparent speed of tracker noise by about five; single 90 Hz samples
  with 1° of noise read as 100+ deg/s). Samples during a saccade and 50 ms after it are dropped and the median restarts.
- A 9-sample **median**, then an 80 ms **IIR**, restarted with each fixation (the first estimate after a saccade or a gap
  is the median itself, so the previous fixation's vergence never lingers).
- **Hysteresis**: near (cockpit) when `w > 1/NearM`, far when `w < 0.5/NearM`, otherwise the class is kept. With the
  default `NearM` = 1.5 m: cockpit nearer than 1.5 m, outside farther than 3 m.
- **No fresh gaze**: a gaze identical to the previous frame's (the same focus FOVs, or the same exported directions) is
  not a new sample: the provider held or cached its gaze, e.g. during a blink, or (read from the focus views) fell back
  to its fixed focus. Neither is a gaze Quad Views reports invalid, a NaN FOV or lost tracking.

**Accuracy limits.** For a 64 mm IPD the vergence angle is 5.2° at 0.7 m, 2.4° at 1.5 m, 1.2° at 3 m, 0.37° at 10 m
and 0.18° at 20 m. Eye trackers are accurate to about 0.5–1° per eye, so the vergence error is about as large as the
whole signal beyond a few metres: `w` is off by about 0.2–0.3 /m (an error of 0.25 /m reads a target at infinity as
4 m). That is enough to separate the cockpit (`w` ≈ 1.4–2 /m) from outside (`w` ≈ 0), which is all the control law
needs, but it cannot grade 50 m against 500 m. A slowly drifting per-eye error can make an outside fixation read as
near for a moment: the scale then goes back towards 1 (the safe direction). The opposite error, a cockpit fixation read
as far, needs a convergent error of the full cockpit vergence (5°) and was not seen in the harness.

**The source must prove itself.** Vergence is used only after the estimator has seen both a cockpit and an outside
fixation (`source=vergence`). Until then (`probing`) the scale stays 1. If the vergence stays constant to 0.002 /m for
two half-second blocks while the gaze sweeps 2° or more, the focus views do not come from two eyes (`constant`): the
scale stays 1 and the source must prove itself again.

This matters: **the bundled Quad Views derives both focus views from one combined gaze.** Verified in its source
(`openxr-api-layer/layer.cpp`): `getEyeGaze` takes the single `XR_EXT_eye_gaze_interaction` pose, or the average of the
two eyes of `XR_FB_eye_tracking_social`, and `ProjectPoint` projects that one direction as a point 1 m in front of the
head through each eye. Both focus views then converge at 1 m whatever the pilot looks at; the export gives both eyes
the same direction (parallel, infinity). Either way Depth boost stays at 1 (`probing`, or `constant` once the gaze has
moved) with the bundled Quad Views, and the OpenXR eye gaze is one combined direction too. Pimax PVR is the per-eye
source on Pimax; the log line `vergence=` shows in the first minute of flight whether per-eye vergence reaches the layer
(`vergence`), or not (`probing`, `constant`). Without it Depth boost runs on the scene depth alone (no veto).

### Control law

**Stepped (the default, `[DepthBoost] Stepped=1`).** The first in-headset tests (2026-10-12/13) showed the ramps below
changing the eye distance during 71 % of far fixations, about 18 reversals a minute as the far target followed each
glance across terrain. A changing eye distance moves the near field's disparity by ΔI/Z: 5 mm/s is 25-34 arcmin/s at a
0.5-0.7 m canopy, against GazeStereo3D's ~6 arcmin/s for depth scaling, and it was seen as the foreground "breathing".
So the eye distance now changes in steps hidden by saccades and blinks, and stays still while the eyes fixate:

- while fixating at most `DriftMmPerS` (0.5) either way, about 3 arcmin/s at 0.5 m;
- per saccade (and its landing window), or per blink, up to `SaccadeStepMm` (4) upwards and `StepDownMm` (8)
  downwards towards a lower far scale;
- back to the cockpit in one go (`FullNearReturn=1`, the default): x1 inside the saccade or landing window that brings
  the eyes there, as soon as the scene depth at the landing says near, ahead of the confirmation that follows about
  100 ms later. The 2026-10-13 flights showed why: with 8 mm steps the cockpit stayed small until the next saccade, and
  that step was seen as a jump ("very tiny" for half a second). If the cockpit is only confirmed after the landing
  window, the scale returns at `NearReturnMmPerS` (80) and the next saccade finishes it. `FullNearReturn=0` restores the
  steps of at most `StepDownMm` (range 0-40);
- upwards only towards the median of the last three fixations' targets (one far glance does not start it, two of three
  do), downwards towards the current fixation's.

There is no published threshold for disparity steps hidden by saccades. The step sizes come from the
displacement-suppression literature (Bridgeman 1975; Li & Matin 1990; Bolte & Lappe 2015; Sun et al. 2018) and must be
confirmed in the headset. `Stepped=0` restores the ramps described next.

A disparity change at the cockpit (0.6 m) cannot be made invisible by rate alone: 26 mm of eye distance is about 150
arcmin there, some 8 s at the ~18 arcmin/s shift threshold. The one-go return hides it in saccadic suppression as far as
possible. Two later designs avoid the change altogether: Far depth's `near` preset (the world as if seen with 1.4 times
the eye distance, the cockpit pinned by its stencil, FAR_DEPTH.md) and the engine's dual camera (DcsQvCull renders the
cockpit at the true eye distance from the layer's `Local\DcsVrDepthBoost_v1` record; opt-in, not in 0.5.8).

**Depth boost with the engine's dual camera (the shipped design after 0.5.8).** The user's in-headset A/B (2026-10-14)
preferred it, with no flicker: Depth boost now runs only with DcsQvCull's dual camera (`CockpitTrueIpd=1`), which renders
the cockpit at the true eye distance while the world gets the boosted one. The layer writes `Local\DcsVrDepthBoost_v1`
(BoostOut) for every display time once the block exists (flags 0 and scale 1 while Depth boost is off, and a final
record when the session ends), and reads the engine's acknowledgement `Local\DcsVrCockpitStereo_v1` (CockpitStereo;
perception_shm.h). While it is fresh and active:

- the world's scale follows the usual far target and curve (Enhanced or Realistic), but a cockpit fixation keeps the last
  far target, so glances at the instruments do not move the world;
- the scale changes only smoothly, at `DualCameraMmPerS` (20, range 2-60) both ways, in equal steps per frame (the mean
  frame period, not each display time's delta): no saccade or landing steps, no predictive jumps. A one-frame
  mis-pairing in the engine then moves the cockpit by a fraction of a millimetre;
- the cap is `MaxScale` itself (no fast return is needed, so `DownMmPerS x RecoverMs` no longer limits it);
- it is left only after 300 ms without an active record, or at once on an explicit stop (the engine's key pause).

Without a fresh acknowledgement Depth boost holds or slides back to x1 at the same rate. `[DepthBoost] Legacy=1`
(diagnostics only, default 0) restores the gaze-driven path above, with the cockpit returning to x1 in saccades. The log
names the path (`depth: path ...`) and, once a second in the dual-camera path, the target and the scale. Harness
scenarios: `flight_dual_camera` (the acknowledgement every frame, with a 170 ms gap every 300 frames) and
`flight_no_dual_camera` (Legacy=0 without it: x1 throughout).

`DepthBoostController` (pure, in `pupil_math.hpp`), once per display time:

- **Target**: from `FixationDecider` (FIXATION_DEPTH.md): in the far state the mode's curve of the far distance, on
  vergence alone `cap` (Enhanced) or 1 (Realistic); 1 otherwise. `cap = min(MaxScale, 1 + DownMmPerS · RecoverMs /
  IPD_base)`: GazeStereo3D's quick-recovery guarantee, the return to 1 never takes longer than `RecoverMs`.
- **Towards the boost** at most `UpMmPerS` of eye distance per second, and only on a settled estimate (fixation samples
  measured since the last saccade or gap; the eyes may be landing in the cockpit).
- **Back towards 1** at most `DownMmPerS`; in the far state (a nearer far object) at most `UpMmPerS`, so only the
  return to the cockpit is fast. Never towards the boost while a change is being confirmed or the gaze is on an edge.
- **After a saccade** of 4° or more that lands on something near while the state is still far, `SaccadeStepMm` at once,
  within 80 ms of the end of the saccade (the tail of saccadic suppression), before the confirmation; once per saccade.
- **No fresh gaze**: the scale holds (a blink changes nothing) for `HoldMs`, then returns to 1 at `DownMmPerS`. This
  deviates from "never change while the gaze is invalid": a tracker that stops for seconds while the pilot may be
  looking into the cockpit must not keep the world small.
- **Switched off** (in flight or in the ini): 1 at once.
- Frame gaps count at most 50 ms, so a hitch never produces a jump.

Rates in terms of disparity: changing the IPD by `v` mm/s changes the disparity of a point at distance `Z` by `v/Z`
rad/s. GazeStereo3D measured a disparity-shift detection threshold of 17.64 arcmin/s (median, at fixation; scaling is
more visible, `0.1992 + 1.787·ln(s+1)` arcmin/s for a corrugation of amplitude s arcmin), and its validation found no
loss of stability at twice the thresholds.

| Key | Default | Range | Why |
|---|---|---|---|
| `Enabled` | 0 | 0/1 | Off by default. |
| `Mode` | realistic | realistic, enhanced | FIXATION_DEPTH.md, "Modes". |
| `MaxScale` | 1.25 (Realistic), 1.4 (Enhanced) | 1.0–1.5, 1.0–2.0 | 64 mm renders as 80 mm, 89.6 mm. Kim & Interrante (2017) found a 38% size change at 150 mm (×2.3). |
| `FarStartM`, `FarFullM` | 150, 400 (Realistic); 3, 50 (Enhanced) | 1–5000, ≥ 1.1 × FarStartM | ×1 up to `FarStartM`, `MaxScale` from `FarFullM`, linear in log distance between. |
| `NearM` | 1.5 m | 0.5–5 | Cockpit below, outside beyond twice this. Vergence cannot do better (see accuracy). |
| `UpMmPerS` | 5 | 0.5–20 | At the nearest cockpit content (0.5 m) the disparity changes by 34 arcmin/s, twice the shift threshold: the paper's recommended ×2. At fixation (far) it is far below every threshold. 1 → 1.4 at 64 mm takes 5.1 s. A strict ×1 would be 2.6 mm/s. |
| `DownMmPerS` | 40 | 5–100 | About 15× the threshold at 0.5 m: probably visible. The trade-off is deliberate: a small world in the cockpit is worse than a visible return. 1.4 → 1 in 0.64 s. |
| `SaccadeStepMm` | 4 | 0–10 | Hidden by saccadic suppression; GazeStereo3D found changes timed to saccades less stable with real tracker latency, so the saccade is a bonus, not the mechanism. |
| `RecoverMs` | 800 | 200–3000 | Caps `MaxScale` so the return takes at most this long. |
| `HoldMs` | 500 | 0–2000 | A blink keeps the scale; a lost gaze returns to 1. |
| `Toggle` | 121:6 | key | Alt+Shift+F10, not a DCS default binding, outside the engine optimizations' Ctrl+Alt keys, apart from Pupil shift's Ctrl+Shift+F10. |
| `PimaxPvrGaze` | 1 | 0/1 | Look for Pimax PVR's per-eye gaze (four interface-table slots, no code patched). |
| `Trace` | 0 | 0/1 | One `dtrace:` line per display time in the log (diagnostics only). |

Deviations from the design study (`feature_disparity` study): `NearM` is 1.5 m instead of 3 m; the study's `FarM`
smoothstep over 3–30 m became the two modes' log ramps on the scene depth (3–50 m Enhanced, 150–400 m Realistic);
the study's peripheral multiplier is folded into `UpMmPerS`; the 90 mm render-IPD clamp of the study is not used (it was
the sanity range of DCS's custom IPD input; the scale's own range 1–2 applies); `MinAglM` is not implemented (no
altitude source yet).

The app writes `Enabled`, `Toggle`, `Mode`, `MaxScale`, `FarStartM`, `FarFullM` and `NearM` from the profile
(`DepthBoost`, `DepthBoostToggleKey`, `DepthBoostMode`, `DepthBoostRealisticMax` or `DepthBoostMax`,
`DepthBoostFarStartM`/`DepthBoostFarFullM` for Realistic, 3/50 for Enhanced, `DepthBoostNearM`) and the rates at their
defaults; the rates can be tuned in the deployed ini while DCS runs (re-read about once a second). It also writes
`[Perception] FixationDepth=1` in `DcsQvCull.ini` (`VrProfile.UsesFixationDepth`), which turns on the engine module's
depth sampling; without Engine Optimizations the module runs in its Perception-only mode.

### Status and log

`PupilShift_Status()` (64 bits, layout in `pupil_math.hpp`, namespace `status`): bits 0–4 and 8–15 are the pupil
shift's as before; bit 5 Depth boost on, bit 6 per-eye vergence proven, bit 7 the pupil correction off in the profile,
bits 16–23 `round((s − 1) · 100)`, bits 24–25 the fixation (1 cockpit, 2 far), bit 26 Depth boost enabled, bit 27 one
shared gaze (no depth), bits 28–29 the fixation depth's source (0 none, 1 vergence, 2 scene depth), bit 30 the gaze on
an edge, bit 31 a vergence veto, bits 32–47 `round((s − 1) · 10000)`, bits 48–59 the fixation distance in cm (4095 =
40.95 m or farther). An older panel ignores the new bits. OFXR patch 0017 shows Depth boost on the pupil shift line ("Pupil shift:
ON, 3.2 mm; Depth x1.32 far") or alone ("Depth boost: x1.32 far", "waiting", "no depth (shared gaze)", "OFF
(Alt+Shift+F10)"); `DCSVR_DEPTH_HOTKEY` names the key. One line either way, so the panel keeps its height.

`PupilShift.log` gets a `depth settings:` line on every settings load and, once a second, a `depth:` line: on/off, the
source (depth, vergence, none), the vergence's state, the fixation and its estimated distance (range over the second),
the scale (range and target), frames near, far, unknown and without a fresh gaze, frames boosted, saccades and rejected
samples, the scene segment (mode, fresh, stale and absent summaries, shares, edges, vetoes, state changes) and the PVR
segment (FIXATION_DEPTH.md, "Logging"), and the base and rendered IPD.
The pupil shift's `stats:` line ends with the gaze source both use: `gaze source=<qv|xr|focus|simulated|none> (qv N,
xr N, focus N, simulated N, none N; qv invalid N, xr invalid N, saccade N)`. Pimax native Quad Views: `xr`, and `none`
with `xr invalid` counting the frames whose OpenXR eye gaze was not valid and tracked (`focus` only without the OpenXR
eye gaze). The bundled Quad Views: `qv`, and `none` with `qv invalid` counting the frames it had no real gaze.
`PupilShiftStatus` reads it, and the older line, for the Pupil shift page (`DepthBoostStatus`: working, probing,
no-depth, no-scene (Realistic without the scene depth), no-gaze, off).

### Offline validation

`scripts/build-pupil-shift.ps1` runs both through CTest.

**Unit tests** (`pupil_shift_tests`): vergence distance exact for rays that meet from 0.4 m to 300 m at ±30° yaw and
±20° pitch, also in a turned head with canted views; parallel and diverging rays read as infinity; the estimator
classifies a 0.7° noisy cockpit and far trace, freezes during a 25° saccade, keeps its class without a gaze and ignores a
repeated display time; a combined gaze projected at 1 m or at infinity never proves vergence (and the parallel case is
flagged constant); ramp-up and return rates per frame; 1 → 1.4 in exactly 25.6 mm / 5 mm/s; return to exactly 1 within
`RecoverMs`; the recovery cap; the saccade step bound; no boost on a stale estimate; hold through a blink, return after
`HoldMs`; 1 without a proven source, at once when switched off, NaN-safe, at most 50 ms per frame gap; the scaled IPD
move keeps the centre; every status field round-trips. The fixation depth's rules each have their own test
(FIXATION_DEPTH.md, "Offline validation").

**End-to-end harness** (`pupil_shift_harness`): loads the built `XR_APILAYER_DCSVR_pupil_shift.dll` through
`xrNegotiateLoaderApiLayerInterface` and its `xrCreateApiLayerInstance`, with a fake runtime below it: four
`PRIMARY_QUAD_VARJO` views, the primary views canted 3° outwards each (6° between them), 65.5 mm apart, the head turning
±15° and swaying a few cm, focus views centred on each eye's scripted gaze. A 49 s scripted flight: cockpit fixations
at 0.6–0.7 m and outside fixations (2 km, 300 m, 50 m) with 45 ms saccades, vergence that settles about 200 ms after
each landing, a 10 s far fixation, a 1.5 s glance at the instruments, blinks where the provider holds or drops its gaze,
0.3 s of lost tracking, a NaN focus FOV, 1.5 s without a fresh gaze. Per-eye noise: 0.5° white plus a 0.5° drifting
error (2 s time constant); the noisy run 1.0° white plus 0.7° drift. "DCS" locates every fifth frame twice, resubmits
every seventh frame with values rounded differently (1e-6), changes one view itself every 53rd frame, and every 11th
frame locates the next frame before ending the current one.

Three more runs behave like the bundled Quad Views (`qv_*`): one combined gaze, its focus area 1° outwards from it,
and its fixed focus (rest + 2.4° outwards per eye) whenever the gaze drops (blinks, a NaN gaze; a held gaze is its
blink cache, valid). A fake `XR_APILAYER_MBUCCHIA_quad_views_foveated.dll` (`fake_quadviews.cpp`), loaded before the
layer, exports `DcsVr_QuadViewsGaze` with the scripted flags and directions. With it, the expected pupil shift comes from
the export's gaze (not the offset focus centre), and every frame without a valid gaze must keep the runtime's FOVs and
render at exactly `Ipd` (64.000 mm); the log must name the export, count `qv` frames, invalid frames and saccade frames,
and never read the focus views. Without the export (`qv_no_export`), the old reading is kept and reproduces the live
bug: 64.879 mm on the fixed-focus frames (live: 64.9 mm).

Four more runs (`xr_*`) give the fake runtime `XR_EXT_eye_gaze_interaction`, as Pimax's runtime has it (Pimax native
quad views: the same combined gaze and fixed focus, without the export). The harness's "DCS" creates its own action
set, suggests a controller binding, creates a session, attaches its set and calls `xrSyncActions` with it before every
`xrLocateViews` (in every scenario, so the pass-through without the eye gaze is checked too). The fake runtime keeps the
last suggestion per profile and refuses a second attachment and syncs of sets that are not attached. It locates the
layer's gaze space only in the views' base space at their display time: valid and tracked when the scripted gaze is,
nothing otherwise. Checked: the extension is enabled exactly once, by the layer or by the application; DCS's set is
attached once and is in every sync, the layer's set is attached with it and active in every sync; the eye-gaze profile
carries the layer's binding (and, in `xr_app_gaze`, the application's own), while the controller suggestion is unchanged;
the pupil shift follows the OpenXR gaze; and every frame without a valid gaze keeps the runtime's FOVs and renders at
exactly `Ipd`, although the focus views sit 2.4° outwards: 64.000 mm in `xr_gaze` and `xr_app_gaze`, and the runtime's
65.500 mm in `xr_never_valid` (`Ipd=0`, nobody wearing the headset; nothing is changed, every view passes through
bitwise). The log has to show `xr` and `xr invalid` frames, no `focus` frame and no fallback warning. In
`xr_offered_qv_export` the export wins and the extension is not added. Without the extension (all other runs) the
layer leaves input alone and logs the fallback warning.

Checked on every frame: the runtime receives bitwise its own poses and FOVs at `xrEndFrame` (views the layer did not
change, and the one DCS changed itself, pass through as submitted); each located view is runtime pose + IPD move
(`Ipd=64` or the runtime's 65.5 mm, times the scale) + that eye's pupil shift with straight ahead between the canted
views, within 10 µm, and its FOV is shifted for the pupil only; orientations untouched, no NaN; a second locate for the
same time returns the same views and does not advance the scale; the scale's rate stays within `UpMmPerS` and
`DownMmPerS` (plus `SaccadeStepMm` within a saccade); the scale is exactly 1 in the cockpit once the return had time and
whenever the source is not proven; the status word decodes consistently; the log has its lines.

The runs above use Enhanced (the v1 curve on vergence alone); the scene-depth runs, Realistic, Pimax PVR and the
GazeOut block are described in FIXATION_DEPTH.md. Results (default settings; CSV and SVG of scale and estimated
distance against time in `artifacts/native/pupil-shift/harness_out/`):

| Run | Reaches ×1.4 in the 10 s far fixation | Glance at the instruments: back to ×1.00 | Scale in the cockpit once settled | Cockpit frames above ×1.00 | Fastest change (mm/s, up / down) | Near/far right | Source |
|---|---|---|---|---|---|---|---|
| Pupil shift + Depth boost, `Ipd=64` | 4.1 s | 0.77 s | ×1.0000 | 13.5% | 5.2 / 40.3 | 99.7% | vergence |
| Depth boost alone, runtime IPD 65.5 mm | 4.2 s | 0.78 s | ×1.0000 | 13.7% | 5.3 / 40.1 | 99.7% | vergence |
| Noisy tracker (1.0° + 0.7° drift) | 5.3 s | 0.77 s | ×1.0000 | 13.5% | 5.2 / 40.3 | 98.1% | vergence |
| One shared focus gaze, Pimax PVR per eye (`pvr_per_eye`) | 4.1 s | 0.77 s | ×1.0000 | 13.5% | 5.2 / 40.3 | 99.8% | vergence (PVR) |
| The same, the runtime calls its own copy of PVR's table (`pvr_copied_table`) | never (×1.00) | — | ×1.0000 | 0% | 0 / 0 | — | constant |
| Realistic without the engine module (`realistic_vergence_only`) | never (×1.00) | — | ×1.0000 | 0% | 0 / 0 | 99.7% | vergence |
| Combined gaze at 1 m (bundled Quad Views) | never (×1.00) | — | ×1.0000 | 0% | 0 / 0 | — | constant |
| Combined gaze, parallel | never (×1.00) | — | ×1.0000 | 0% | 0 / 0 | — | constant |
| Pupil shift alone | — | — | ×1.0000 | 0% | — | — | — |
| Depth boost switched off in the ini during the far fixation | ×1.00 within one reload | — | ×1.0000 | 7.3% | — | — | — |
| Bundled Quad Views with the export (`qv_export`, and `qv_export_depth_only`) | never (×1.00) | — | ×1.0000 | 0% | 0 / 0 | — | constant |
| Quad Views-style focus without the export (`qv_no_export`) | never (×1.00) | — | ×1.0000 | 0% | 0 / 0 | — | constant |
| OpenXR eye gaze (`xr_gaze`, `xr_app_gaze`, `xr_offered_qv_export`) | never (×1.00) | — | ×1.0000 | 0% | 0 / 0 | — | constant |
| OpenXR eye gaze never valid (`xr_never_valid`) | never (×1.00) | — | ×1.0000 | 0% | 0 / 0 | — | probing |

Quad Views runs: 4324 frames with a valid gaze, 63 without (rendered IPD 64.000 mm with the export, 64.879 mm without),
37 flagged as a saccade. OpenXR runs: the same frames, 64.000 mm without a gaze (`xr_gaze`, `xr_app_gaze`). In
`xr_never_valid`, 4387 frames without a gaze, all at 65.500 mm.

Rates above 5 and 40 mm/s are the status word's 0.01 mm resolution over one frame. "Cockpit frames above ×1.00" are
the returns after each look outside: a cockpit fixation is confirmed 120 ms after the landing is measured, the 4 mm
saccade step is taken then, and the return from ×1.4 takes 0.54 s more. A short look outside (2.5 s) reaches about ×1.17.

### In-headset test, in order

1. Sources, in the first minute (Pupil shift page, or the `depth:` line): `source=depth` means the engine module's
   scene depth arrives (the engine module is loaded); `vergence=vergence` means per-eye vergence arrives (on Pimax, through
   PVR: `pvr: the runtime's PVR session found`, `pvr valid=`). Compare the `pvr ... convergence` column with the
   estimated distance on a cockpit fixation. Realistic with neither scene depth nor a boost: the page says none arrived
   and points to the Engine Optimizations page.
2. Phase 0 of the study, a fixed scale: with Depth boost off and Pupil shift on, set `Ipd=80`, then `Ipd=90` in the
   deployed `PupilShift.ini` while DCS runs (the app writes it again at the next launch) and compare a wingman at 50–150 m, a tanker at 50 m, terrain at 300 m. No benefit at 90 mm: stop.
3. Depth boost on (Alt+Shift+F10 to compare): formation, rejoin, tanker; look in and out; watch the panel's scale.
   Realistic: a wingman inside 150 m must stay at ×1.00; a target beyond 400 m reaches ×1.25. Rest the gaze on the canopy
   frame next to the wingman and on the glareshield edge: the scale must not move.
4. Tune `UpMmPerS` and `DownMmPerS` until the change is just not noticed; report the values.

### Risks

- Visible "breathing" of the cockpit during the ramps (peripheral sensitivity is not modelled by the paper); the return
  at 40 mm/s is probably visible.
- The glance cost: the first 0.2–0.8 s of a look at the instruments happen at the raised scale.
- Distance judgement: relief is exaggerated ×s while looking out; closure is judged mainly by looming, which is
  unchanged. Landing and low level: ground relief exaggerated; keep it off (no altitude gate yet).
- Vergence quality on the real tracker is unknown; any failure falls back to ×1. Whether Pimax's runtime lets the layer
  see its PVR session (interface table) is unknown; without it there is no per-eye vergence and no veto.
- The scene depth depends on the engine module's latency and window (FIXATION_DEPTH.md, "Open risks").
- Sickness: IPD changes that do not match head motion; keep `MaxScale` modest and the switch at hand.

## Far depth

Far depth adds stereo disparity only to far content (about 40 m to 20 km), so a distant aircraft separates from the
terrain behind it, while the cockpit, the near field and the sky stay where they are. Depth boost cannot do that: it
scales the IPD, which moves the near field a lot and a far aircraft by less than a pixel. The design, the maths, the
engine contract and the offline measurements are in [FAR_DEPTH.md](FAR_DEPTH.md).

Status: built into the bundled Quad Views (`dcsvr_far_depth=1`, off by default). It runs only with the engine module's
depth of both focus views, which DcsQvCull does not push yet, and by default also with the engine's overlay coverage
(HUD and canopy glass). Until then the compositor waits: the in-headset panel can read "waiting for engine" from
`DcsVr_QuadViewsDisparityStatus`. Off, or without depth, the composition is the one without Far depth, bit for bit (tested
on the DLL's own shader). Not flown.

It shares this page's gaze record and saccade detector: the gain changes only during saccades, blinks and scene cuts
(the same `DcsVr_QuadViewsTemporalReset` counter Periphery stability uses).
