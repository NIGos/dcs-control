# Fixation depth

Depth boost needs to know how far away the thing the pilot looks at is. A single depth sample at the gaze point is not
reliable enough. The eye tracker's error (about 0.5–1°) and overlapping geometry make it jump: a wingman seen past the
canopy frame, the glareshield edge against the sky, or an MFD bezel in front of the ground. Each jump would make the
render IPD pump. This document describes how the fixation depth is measured, combined and filtered so that the effect
is calm and never surprising. Status: the pupil shift layer's side is implemented and validated offline (unit tests and
the end-to-end harness, below); the engine module's side (native/dcsqvcull) is built separately against the interface
below. Not yet flown.

Code: `native/pupil_shift/pupil_math.hpp` (`DepthMatcher`, `FixationDecider`, `far_scale`, the controller),
`native/pupil_shift/perception_shm.h` (the shared-memory blocks, shared with the engine module),
`native/pupil_shift/pvr_gaze.hpp` (Pimax PVR per-eye gaze), `native/pupil_shift/predictive_depth.hpp` (the predictive
transition), `native/pupil_shift/pupil_shift_layer.cpp` (the wiring).

## Sources

1. **Scene depth at the gaze (primary).** The engine module (native/dcsqvcull, inside DCS, Direct3D 11) reads DCS's
   depth buffer of a focus view in a cone around the gaze. It converts the samples to metres with the view's projection
   and publishes a summary each frame (see "Interface"). It runs whenever the profile uses Depth boost: the app
   writes `[Perception] FixationDepth=1` in `DcsQvCull.ini` (`VrProfile.UsesFixationDepth`), with Engine Optimizations or,
   without them, in the module's Perception-only mode (`[General] PerceptionOnly=1`).
2. **Binocular vergence (secondary).** Each eye's gaze, triangulated (`inverse_fixation_distance`). Vergence is too
   coarse to tell 50 m from 500 m, but it separates the cockpit (under 1.5 m) from outside, and it covers what the depth
   buffer gets wrong near the pilot (glass, symbology that is drawn far but seen near). It only counts once it has proven
   to be per-eye (it has seen both a cockpit and an outside fixation; PERCEPTION.md). The per-eye gaze comes from Pimax
   PVR when available (below); the bundled Quad Views' export and the OpenXR eye gaze combine both eyes into one gaze, so
   they never prove vergence.
3. **Fallback.** Without either source, Depth boost stays at ×1.

Without the engine module (a failed load; since 0.5.9 the app installs it with every Depth boost profile) Depth boost runs on the vergence alone: Enhanced works as Depth
boost v1 did (the maximum while the vergence says far), Realistic stays at ×1, because vergence cannot tell whether a
far object is nearer or farther than 150 m.

## Modes

Two curves turn the far fixation's distance into the scale (`far_scale`, linear in log distance between the two
distances). Realistic is the default.

| Mode | ×1 up to | Maximum from | MaxScale (default, range) |
|---|---|---|---|
| Realistic | `FarStartM` = 150 m (50–1000) | `FarFullM` = 400 m (≥ 1.2 × FarStartM, ≤ 3000) | 1.25 (1.0–1.5) |
| Enhanced | 3 m | 50 m | 1.4 (1.0–2.0) |

Why Realistic starts at 150 m. A binocular disparity is `IPD / Z` radians. A headset resolves about one pixel of it: at
46 px/deg one pixel is 0.022° (3.8·10⁻⁴ rad), which a 64–65.5 mm IPD reaches at Z ≈ 170 m (about 190 m with a little
more pixel density; sub-pixel anti-aliasing carries disparity somewhat farther). Up to there the headset's stereo is
already comparable to real stereo, and more IPD (hyperstereo) only falsifies distances: a wingman, a rejoin, a tanker
look nearer than they are. Real stereoacuity is about 20–30 arcsec (1–1.5·10⁻⁴ rad), so in reality stereo keeps working
to about 450–650 m. Realistic adds disparity only in that band, where the headset has lost it and the eyes would still
have it, up to ×1.25 from 400 m: the aim is to bring disparity back above the display's threshold where reality would
have it, not to add depth in general. The cockpit logic, overlap handling, vetoes and rate limits are the same in both
modes.

Enhanced is the v1 curve: more depth from 3 m and the maximum from 50 m. It is not realistic: distant objects look nearer
and the world smaller. The app describes it that way.

For realism the base IPD should be the one the headset measures (65.5 mm on the development PC), not DCS's custom IPD.
Today the app hands the layer DCS's custom IPD when one is set (`Ipd=` in PupilShift.ini, PUPIL_SHIFT.md), so Realistic
scales that value.

## From samples to a fixation (as built)

Per display time the layer pairs the newest engine summary with the gaze it was taken for (`DepthMatcher`), classifies
it, and runs the rules below (`FixationDecider`). The controller (`DepthBoostController`) then moves the scale.

**Summaries (DepthMatcher).** A summary counts for at most 3 frames after it arrived (a new `seq`). When its
`displayTime` is one the layer wrote, that gaze must have been valid, not in a saccade (depth sampled mid-flight
describes no fixation), at most 8 frames old, and within 2° of the current gaze (otherwise it describes what the eyes
looked at before). A `displayTime` the layer never wrote (the engine had none for its frame and used the newest GazeOut)
is accepted on arrival alone. A summary with `valid = 0` or a torn read is no summary.

**Classes.** The engine counts each sample in the 1° cone as *near* (under 1.5 m), *far* (over 3 m) or *between*. The
cone is *near* with 70 % or more near samples, *far* with 70 % or more far, otherwise *ambiguous* (also for nonsense
values). These thresholds are fixed on the engine's side; the layer's `NearM` applies to the vergence only.

**Rules**, with the reason each exists:

| Rule | As built | Why |
|---|---|---|
| Ambiguous holds | an ambiguous cone never changes the state by itself | a mixed cone says nothing |
| Edges hold | the 1.5° ring with ≥ 30 % near and ≥ 30 % far: no change, a change being confirmed starts over, and the scale does not move towards the boost | canopy frame, glareshield, bezels; gaze jitter across an edge must not pump |
| Vergence veto | vergence proven, settled, near and nearer than **1.0 m**, while the depth says far: near | HUD symbology, near things seen through glass |
| Depth over vergence | depth near, vergence far: near (depth is trusted) | vergence is the noisier measure at a distance |
| Vergence gap | a frame without a fresh summary within 0.25 s of a near depth: a far vergence there is no evidence (pauses) | the vergence lags a fixation change; one stale frame must not re-arm |
| Landing | the end of a saccade (or of a gap in the gaze: a blink) arms a landing; the first class after it is the candidate (ambiguous counts as near); it becomes the state after 120 ms of agreement; the opposite class re-arms | changes land with saccades, where they are least visible |
| Landing waits for the veto | with vergence proven, a landing on a *far* depth waits until the vergence has settled after the saccade (about 55 ms) | so a veto acts on the landing, not 300 ms later |
| Landing expiry | a landing not confirmed within 0.4 s (it rested on an edge) is a fixation: the pursuit rule applies | a landing on an edge would otherwise switch on 120 ms of tracker drift seconds later |
| Pursuit | without a landing, a different class must hold 300 ms (ambiguous pauses, edges reset) | smooth pursuit and slow drift |
| No source, no gaze | neither source, or no gaze, for `HoldMs` (500 ms): near | the safe side |
| Far target | median of the last 9 far medians (`farMedianM`) of this fixation, through the mode's curve; it moves only by 0.02 of scale or more; without a summary for 2 s, Enhanced uses MaxScale on vergence, Realistic 1 | no pumping from a jittery median |

**Controller.** The rate limits are the v1 ones: towards the boost at most `UpMmPerS` (5 mm/s), back at most
`DownMmPerS` (40 mm/s), and never towards the boost while a change is being confirmed or the gaze is on an edge. In the
far state a lower target (a nearer far object, a pursued object coming in) also moves at `UpMmPerS`: only the return
to the cockpit is fast. The saccade step (`SaccadeStepMm`, 4 mm, granted once per saccade of 4° or more) is spent when
the landing's candidate is near while the state is still far, at once, within 80 ms of the end of the saccade flag (the
estimator holds that flag 50 ms after the eyes stop): the tail of saccadic suppression, before the 120 ms confirmation.
A gap in the gaze clears it.

**Vergence filter changes** (`FixationEstimator`): its IIR restarts with each fixation (it no longer blends the previous
fixation's vergence into the first estimate after a saccade), and a gap in the gaze clears a saccade in progress.

**Deviations from the design, and why.**

- "A 4 mm step during the saccade towards near": during the saccade the target is unknown (the summaries lag by 2–3
  frames and are rejected while the gaze is in flight). The step is taken at the landing instead, within 80 ms. The
  predictive transition (below) does take a step during the saccade, from a predicted landing and the engine's grid.
- Edges reset a pending confirmation and hold the scale, not only the state: an edge interleaved with a few near frames
  would otherwise add up to 120 ms of "agreement".
- Landings expire after 0.4 s, and a landing on a far depth waits for the vergence: both found by the harness's noisy
  runs (1.0° noise + 0.7° drift per eye), where a landing on an edge switched seconds later, and a lagging vergence
  re-armed a near landing.
- The veto needs the vergence nearer than 1.0 m (not 1.5 m): a 1500 m fixation read as near with 2.4° of vergence error
  in the noisy runs; 1.0 m needs 4.2°. HUD and glass are within reach.
- Summaries sampled during a saccade are rejected (the design matched by `displayTime` only).

## Predictive transition

Two transitions, switchable live for A/B comparison: `[DepthBoost] Transition=gradual|predictive` in `PupilShift.ini`
(re-read about every 90 frames) and the in-flight key `TransitionToggle` (legacy path only since 0.5.9: the app writes
`Transition=gradual` and `TransitionToggle=0:0`; e.g. Alt+Shift+F9, `120:6`; one beep
gradual, two beeps predictive; a changed ini value replaces the key's choice). **Gradual** (default) is everything
above. **Predictive** adds one capped step *during* a long saccade, towards the scale of where the eyes will land; the
rest of the change follows the gradual rates, and the landing's cone confirms or reverts it. Code:
`native/pupil_shift/predictive_depth.hpp` (pure: detector, grid lookup, step, gate), the wiring in
`pupil_shift_layer.cpp`.

**High-rate gaze.** `xrLocateViews` runs at the render rate (about 45 Hz in DCS), too slow for a 60–90 ms saccade.
While Depth boost is on, `PimaxPvrGaze=1` and predictive is selected, a small thread queries PVR's
`getEyeTrackingInfo` at `PredictPollHz` (500; high-resolution waitable timer) under the same shared lock as the
per-frame query, so it never calls PVR with a destroyed handle (destroyHmd takes the lock exclusively and forgets the
handle first). New sample times go into a lock-free single-producer ring that each `xrLocateViews` drains; while the
thread runs it is the only caller of PVR (the per-frame vergence path reads its latest answer). It stops when no longer
wanted and is joined at `xrDestroyInstance`. Without PVR's per-eye gaze (no client, a copied table, `PimaxPvrGaze=0`, no
samples for 300 ms) predictive falls back to gradual and the log says why (`predict: transition predictive selected,
gradual in effect: <reason>`).

**Saccades** (`SaccadeDetector`), on the cyclopean gaze of the stream: speed is the slope of a least-squares line
through yaw and pitch over the last 8 ms. Onset at ≥ 100 °/s on two samples in a row, end below 70 °/s on two samples
(the hysteresis band); onset position the fitted position before the first fast sample. A gap or blink drops a saccade
in flight, and a new one may start only once the stream has run 30 ms again. The **peak** is the vertex of a parabola
fitted to the last 12 ms of speeds (at least 6 samples) once it lies 3 ms or more in the past and the speed is 3 % under
it; a slow tracker without the samples uses a 7 % drop of the 3-sample mean speed. A peak is accepted only where the
eyes have done 30 % of the path it predicts (a noise bump on the rising flank is not the peak).

**Amplitude from the main sequence.** Vp = Vmax (1 − e^(−A/C)) (the saturating exponential of Baloh et al. 1975,
Neurology 25:1065, and Bahill, Clark & Stark 1975, Math. Biosci. 24:191), inverted: A = −C ln(1 − Vp/Vmax), with
Vmax = 600 °/s, C = 15° (inside the 500–700 °/s plateau later fits report; not one study's constants). Duration
D = 21 ms + 2.2 ms/° · A (Carpenter 1988). Above about 20° the curve saturates and a small speed error is a large
amplitude error, so the prediction is bounded to 1.4–3 times the path travelled at the peak (the peak comes at
35–60 % of the path), and at most 45°. An online gain (EWMA of the log ratio of the actual to the main-sequence
amplitude over landed saccades of 3° or more, clamped 0.7–1.4) absorbs a systematic error: an individual, or a tracker
whose filtering lowers the peaks. Direction: from the onset to the peak position; each eye's landing is its onset
rotated by the predicted amplitude about that axis. Predicted end = onset + D(A).

**Landing depth** (`classify_grid`): the left eye's predicted landing, from the head frame through this frame's head
basis to world space and into view 0's frame *at the grid's display time* (the layer keeps the views' orientations of
the last 16 display times), projected with the grid's own `fovTan` (asymmetric: live Pimax values
−1.53/1.03/1.27/−1.26). Outside the grid: no step. The landing cell is the cone, its 8 neighbours the ring, with the
cone's rules: near or far with a 70 % majority of the centre cell, an edge when the neighbours hold ≥ 30 % near and
≥ 30 % far, the 1.5–3 m dead band neither. Per cell, near = nearShare/255; far = 1 when even the nearest point is
beyond 3 m, else the part that is not near when `typicalM` is beyond 3 m (true for a log mean and for a median), else 0;
a cell with a distance ≤ 0 (empty) is no evidence. The far distance is the median `typicalM` of the far cells among the
nine; the near distance is the centre's `nearestM`. The grid must have arrived within 4 frames, have a `displayTime` the
layer located at most 4 frames ago, and not 0 (the engine had no gaze).

**The step** (`step_mm`, `PredictiveGate`): towards 1 (near) or the mode's far curve at the landing distance (far), at
most `PredictMaxStepMm` (12 mm of IPD) and at most what changes the landing target's disparity by `PredictDisparityDeg`
(0.4°: an IPD change d moves a point at Z by d/Z radians; 4.9 mm at 0.7 m; no limit at 20 km). No step when the
saccade is predicted shorter than `PredictMinDeg` (8°: the gradual path), the grid is missing or stale, the landing is
outside, on an edge or mixed, the step would be under 0.5 mm, or the predicted saccade has less than
`PredictLeadFrames` frame periods left at this `xrLocateViews` (the frame would not be rendered in flight).

**After the step** the scale holds against the step's direction (after a step up it does not move down, and the
reverse) until the eyes have landed and the decider's state is the predicted class (confirmed), or its frame class is
the opposite one (contradicted), or 0.6 s pass after the predicted end, a gap in the gaze or the next prediction arrives
(expired). Released, the controller moves at the normal rates (fast down, slow up): a contradicted step is reverted at
40 mm/s, never by a second jump, and the gradual 4 mm saccade step is withheld for that saccade and 0.9 s after it. In
predictive mode the stream's saccade flag also counts as a saccade for the vergence filter.

**Lead: half a frame, not one (adapted with evidence).** The design asked for about one frame of saccade left. A 20°
saccade lasts about 68 ms and its peak is known about 46 ms after the onset, so about 22 ms are left, exactly one
45 fps frame; the step then waits for the next `xrLocateViews`. The harness's sweep (below) gives steps on 19 of 40
saccades of 15–35° with one frame and 27 of 40 with half a frame, and at one frame almost none under 25°. The frame
located in flight is displayed about two frames later (after the landing, inside post-saccadic suppression) in either
case. `PredictLeadFrames=1` restores the stricter rule live.

## Interface (shared memory, both directions)

`native/pupil_shift/perception_shm.h` holds the three structures (gaze, depth summary, depth grid), the names, the flags
and the seqlock, for both sides. All three blocks are named file mappings in the DCS process's session (`Local\`),
little-endian, `#pragma pack(8)`, sizes 128, 96 and 9264 bytes (static_asserts pin every offset). Either side may create either mapping; a block never written reads as all
zero (no data). A sequence number guards against torn reads: odd while writing, even when done; the reader copies
between two reads of `seq` and keeps the copy only when both are the same even value. One writer per block. The names
take an optional suffix from the environment variable `DCSVR_PERCEPTION_SHM_SUFFIX`, read when the OpenXR instance is
created: the harness uses it so a test run never touches the blocks of a DCS that runs at the same time.

**View order** in both blocks, as in `XR_VARJO_quad_views`: 0 left periphery (primary), 1 right periphery (primary),
2 left focus, 3 right focus. With two views only 0 and 1 exist.

### `Local\DcsVrPerceptionGaze_v1` (written by the pupil shift layer at each new display time in xrLocateViews)

```
struct GazeOut {
  uint64 seq;            // even = stable
  int64  displayTime;    // XrTime of the located views
  uint32 flags;          // bit0 valid gaze, bit1 saccade, bit2 per-eye gaze (vergence available)
  uint32 viewCount;      // 2 or 4
  float  gazeTan[4][2];  // per view: gaze as tangents (x right, y up) in that view's own frame
  float  fovTan[4][4];   // per view: left, right, up, down tangents of the FOV DCS renders with (after pupil shift)
  float  vergenceM;      // binocular estimate, metres, while per-eye vergence is proven; <= 0 unknown
};
```

It is written after the views are final, so `fovTan` is exactly what DCS renders with. The gaze is Depth boost's: Pimax
PVR per eye when valid (bit 2), otherwise the source the pupil shift selected (Quad Views' export, the OpenXR eye gaze,
the focus views). With `SimulateGaze=1` it carries the simulated direction in each eye's frame (valid, no saccade, not
per-eye), whether or not Depth boost is on, so the engine side can be tested live with nobody in the headset. Without a
valid gaze `gazeTan` is zero (straight ahead) and bit 0 is clear.

### `Local\DcsVrFixationDepth_v1` (written by the engine module once per rendered frame)

```
struct DepthIn {
  uint64 seq;            // even = stable
  int64  displayTime;    // the GazeOut.displayTime the samples were taken for (or the newest GazeOut read)
  uint32 flags;          // bit0 valid, bit1 reverse-Z detected, bit2 view used was a focus view
  uint32 view;           // which view was sampled
  float  coneDeg;        // radius actually used
  float  nearShare, farShare, edgeNearShare, edgeFarShare;   // 0..1, cone and 1.5° ring
  float  medianM, nearestM, farMedianM;                      // metres
  uint16 histogram[16];  // log-distance bins, 0.2 m .. 20 km
  float  latencyFrames;  // frames between the depth read and its publication
};
```

The engine module projects `gazeTan` into the viewport of the view it samples, using `fovTan`. The window it reads must
hold the 1.5° ring at the focus view's resolution (at about 2028 px for 34°, 33×33 texels do not; about 65×65 do): the
layer never assumes a window size and uses only the published shares and `coneDeg`. It maps the window 2–3 frames later
with no stall and converts the samples with the projection DCS used for that view (reverse-Z and an infinite far plane
detected). A frame that cannot be sampled publishes `valid = 0`. When the engine does not know the `displayTime` of the
frame it rendered, it publishes the newest GazeOut's; the layer then accepts the summary on arrival (up to 3 frames).

### `Local\DcsVrDepthGrid_v1` (written by the engine module with `[Perception] DepthGrid=1`)

```
struct DepthGrid {        // pack(8), 9264 bytes
  uint64 seq;             // even = stable
  int64  displayTime;     // the newest GazeOut.displayTime at the capture; 0: no gaze (the layer discards the grid)
  uint32 flags;           // bit0 valid, bit1 reverse-Z, bit3 water in at least one cell, bit4 read after the water
  uint32 view;            // 0: the left periphery (the whole field)
  uint32 w, h;            // 32 x 32
  float  fovTan[4];       // left, right, up, down tangents of the part of the view covered (signs as XrFovf)
  float  nearestM[1024];  // per cell (row * w + col, row 0 at the top): the nearest distance
  float  typicalM[1024];  // the log mean (geometric) of the cell's distances; sky 20 km; 0 = empty cell (not valid)
  uint8  nearShare[1024]; // the share nearer than 1.5 m, 0..255
};
```

Published about 2 frames after the capture (GPU readback without a stall), each cell about 15 × 15 pixels of the
periphery. `DepthGrid` is independent of `FixationDepth`; with both on they share the capture. Since 0.5.9 it is off
(`DepthGrid=0`, the module's default; since 0.5.11 the app leaves the key out): only the predictive transition reads the
grid, and with the dual camera the transition is always gradual. Measured live it costs
about 14 µs of GPU and 5 µs of render thread per frame.

## Pimax PVR per-eye gaze

Pimax's OpenXR runtime runs on its PVR client (`LibPVRClient64.dll`, loaded in DCS's process). PVR's
`getEyeTrackingInfo` gives each eye's gaze (`GazeTan[2]`) and a convergence distance; the OpenXR eye gaze and Quad Views
give one combined gaze, which carries no vergence. The query needs the runtime's own HMD handle, which nothing exports.

How the layer gets it, as Cheeky does in `libovr_gaze.cpp` (route 1 only): when Depth boost is on and
`[DepthBoost] PimaxPvrGaze=1` (default), at the first xrLocateViews and then every 2 s until it is found, the layer looks
for a `LibPVRClient64.dll` the runtime already loaded (it never loads PVR itself and never creates a PVR session), calls
`getPvrInterface` for interface 1.32 down to 1.23, and patches three slots of every interface table that shares the same
implementations: destroy first (the handle is forgotten under an exclusive lock before PVR frees it, and a freed handle
is never noted again), then end frame and submit frame (the handle is noted as the runtime passes it), only in a table
whose destroy slot took. Tracking state is never patched (passing it through moved DCS's head, 2026-10-12). Only table
entries are written (with `InterlockedExchangePointer`); the runtime's code is never patched. The layer's DLL is pinned
once a slot is patched, so a detour can never point at unloaded code, and the slots stay patched for the life of the
process: xrDestroyInstance stops the sampler and forgets the session, and the detours pass straight through. Putting
the slots back and patching them again for a later instance could take another patcher's detour, chained over the
layer's, for the original and call itself forever. Each frame the layer calls `getEyeTrackingInfo(hmd, getTimeSeconds())` under a shared lock.

Cheeky's second route, inline hooks on the implementations for a runtime that copied the table, is not used: it patches
the runtime's code, which this layer does not do. If the runtime calls through its own copy, the handle is never seen;
after 900 frames the log says so and Depth boost keeps the other vergence sources (in practice none per-eye on Pimax:
vergence unproven, so the scene depth alone). Whether Pimax's OpenXR runtime calls through the shared table is not known
yet: the log line `pvr: the runtime's PVR session found` answers it in the first seconds of a flight.

Validity: both eyes' tangents must be finite and within 84° (one bad eye is no sample), neither eye reported closed
(`blink` above 0.5, clients 1.30+), not both eyes at exactly (0, 0) (a tracker that lost the eyes), `TimeInSeconds` must be set and
advancing (a time that stops for 200 ms is invalid; a repeated time is valid but not a new sample). `GazeTan` is taken
as the headset's frame (x right, y up), not each canted display's: if it were per display, both eyes would read as turned
outwards by the cant and the vergence would read as infinity, so the source would never prove itself (×1, the safe
side). `ConvergenceDistance` (clients 1.30+) is logged for comparison, not used. Both are to be checked in flight.

## Logging

Once a second, the `depth:` line:

```
depth: on source=depth vergence=vergence fixation=far distance 500.00 m [480.00..520.00] scale 1.200 [1.180..1.200]
target 1.250; frames=90 near=0 far=90 unknown=0 no_gaze=0 boosted=90 saccades=1 rejected=4; scene mode=realistic
fresh=85 stale=3 moved=1 absent=0 shares near 0.02 far 0.97 ring 0.10/0.85 far_median 500.0 m edge=0 ambiguous=3
vetoes=0 depth_over_vergence=0 changes=0 landing_steps=0; pvr valid=88 invalid=2 eye_invalid=1 convergence 0.00 m;
IPD base 64.0 mm, render 76.8 mm
```

(one line in the log). `source`: where the fixation depth came from in most frames (depth, vergence, none); `vergence`:
whether per-eye vergence is proven (probing, vergence, constant); `fixation`: the state. The scene segment: the mode,
fresh, stale (moved: the gaze moved since), absent summaries, the mean shares of the fresh ones, their mean far median,
frames on an edge and ambiguous, vetoes, depth trusted over vergence, state changes and landing steps. The pvr segment:
valid and invalid queries, queries with a bad eye, mean convergence distance (`pvr off` when not wanted, `pvr none`
before the session is found). The `depth settings:` line has the mode, MaxScale, the far distances and `pvr=`.
`[DepthBoost] Trace=1` adds one `dtrace:` line per display time (gaze, vergence, summary, decider, target and scale), for
diagnostics only. `PupilShiftStatus` reads the new line and older ones; the Pupil shift page shows where the distance
comes from and, for Realistic without the scene depth, says that none arrived and points to the Engine Optimizations page
(where the module's state shows).

Status word (`PupilShift_Status()`): bits 28–29 the source (0 none, 1 vergence, 2 scene depth), bit 30 on an edge,
bit 31 vergence veto; the fixation (bits 24–25) is the decider's state; the distance (bits 48–59) the scene's median at
the gaze while it is recent, else the vergence's; bit 60 the predictive transition in effect, bit 61 predictive
selected but gradual in effect.

While predictive is selected, a `predict:` line follows the `depth:` line once a second:

```
predict: transition=predictive in effect samples=500 rate 500 Hz saccades=3 dropped=0 predicted=3 steps=2
mean_step 8.44 mm confirmed=1 contradicted=1 expired=0 skipped short=1 no_grid=0 edge=0 ambiguous=0 outside=0 late=0
same=0; gain 1.02; grid fresh=45 stale=0
```

(`fallback gradual: <reason>` instead of `in effect`). Each step also has its own line (`predict: step +12.00 mm (scale
1.0000 -> 1.1875, target 1.250) landing ok far 20000 m cell 21,12 ...; 46 ms after onset, 23 ms before the predicted
end; amplitude 20.1 deg (main sequence 20.1, travelled 10.3, peak 445 deg/s, gain 1.00) at display time ...`), with
`Trace=1` every prediction. The `depth settings:` line has `transition=` and `transition_key=`, the in-flight key logs
`hotkey: transition predictive`, the sampler `predict: gaze sampler started/stopped`. `PupilShiftStatus` reads them
(the selected transition, the last `predict:` second, steps, confirmations and reversions this flight); the Pupil shift
page shows them.

## Offline validation

`scripts/build-pupil-shift.ps1` runs both through CTest.

**Unit tests** (`pupil_shift_tests`, one per rule): the classes and the edge test; both curves (exactly 1 at 80 m and
150 m in Realistic, the cap at 400 m, half way at the geometric mean; Enhanced 3–50 m); the matcher (absent, fresh for 3
frames, invalid gaze, saccade gaze, gaze too old, moved 2.5° vs 1.5°, unknown time, invalid summary, torn read); pursuit
300 ms and landing 120 ms; an ambiguous landing is near with the step allowed; ambiguous holds 5 s; an edge holds 5 s
and holds the scale, leaving it needs the full 300 ms, edge frames interrupt a confirmation; landing expiry; the veto at
0.7 m but not 1.25 m, depth near over vergence far; a far landing waits for the vergence, a near one does not; the
vergence gap pauses; no source and no gaze for HoldMs give near; a blink arms a landing without the step; the far target
(80 m → 1 and 500 m → cap in Realistic, the deadband, the 2 s memory, Realistic on vergence → 1, Enhanced on vergence →
cap, the median ignores an outlier); the controller (far-state reduction at UpMmPerS, the landing step once, no step after
a gap); the status bits; PVR validity (time 0, repeated time, stall, one bad eye, an eye beyond 84°, head frame to world,
vergence from per-eye tangents); the seqlock (never written, round trip, sequence, odd, repaired writer).

**End-to-end harness** (`pupil_shift_harness`): the built layer under the fake runtime (PERCEPTION.md), plus an engine
module stand-in that reads the layer's GazeOut each frame (checking `displayTime`, `viewCount`, `fovTan` against the views
DCS got, the flags), samples a synthetic scene around the left focus view's gaze, from the left eye (parallax matters at
1 m), in a 1° cone (a 13×13 grid) and a 32-sample ring at 1.5°, and publishes the summary 2 frames later (5 in one run).
The fake runtime's per-eye gaze has 0.5° (overlap scenes 0.8°) white noise plus 0.5° drift per eye; the stress runs 1.0°
plus 0.7° with four seeds. A fake `LibPVRClient64.dll` (`fake_pvr.cpp`) exports `getPvrInterface` with two tables
sharing their implementations; the fake runtime calls its slots with its handle every frame, through the table or
through copies taken before the layer loaded.

Scenes (Enhanced unless noted): a wingman at 80 m past a 0.6 m canopy strut, the gaze 0.2° from the strut's edge for 5 s;
the glareshield edge against the sky, landing on it from the sky and from the panel; an MFD bezel over the ground, from
the MFD and from the ground; the HUD with the combiner in the depth buffer and the eyes at infinity, and with nothing
near in the depth buffer and the eyes at 0.7 m; smooth pursuit of an object from 1 m to 300 m and back; a held 150 ms
blink while confirming far and while returning; the engine stopping for 2.5 s in a far fixation, with and without
per-eye vergence; 5 frames of latency; Realistic with the wingman at 80 m and with a target at 500 m.

Pass criteria, checked on every frame and per segment: no state change on an edge or an ambiguous cone and a render-IPD
oscillation under 0.5 mm there (a monotonic ramp is not an oscillation); back to exactly ×1 within 0.8 s of a near
fixation; no IPD step beyond `DownMmPerS`·11.1 ms outside a saccade, nor beyond that plus `SaccadeStepMm` within 200 ms
of one; the rates of PERCEPTION.md; Realistic exactly ×1 with the wingman, at most ×1.25 with the target.

Results (0 failures; numbers from `pupil_shift_harness`'s depth table):

| Scene | Hold segments / state changes in them | Render-IPD oscillation on edges | Slowest return to ×1 | Largest step outside / within a saccade | Edge frames / veto frames |
|---|---|---|---|---|---|
| Wingman 80 m at the canopy strut | 1 / 0 | 0.000 mm | 0.76 s | 0.448 / 4.000 mm | 103 / 0 |
| Glareshield edge vs sky | 2 / 0 | 0.000 mm | 0.61 s | 0.448 / 4.000 mm | 388 / 225 |
| MFD bezel vs ground | 2 / 0 | 0.000 mm | 0.50 s | 0.448 / 4.000 mm | 352 / 232 |
| HUD: depth near, eyes far | – | – | 0.60 s | 0.448 / 4.000 mm | 0 / 0 |
| HUD: depth far, eyes near (veto) | – | – | 0.61 s | 0.448 / 4.000 mm | 0 / 517 |
| Pursuit 1 m → 300 m → 1 m | – | – | within 1.0 s of 1 m (0.48 s after 1.5 m) | 0.448 / 4.000 mm | 14 / 0 |
| Blink while confirming and returning | – | – | 0.75 s after the blink | 0.448 / 4.000 mm | 0 / 0 |
| Engine stale 2.5 s (vergence proven) | – | – | 0.76 s | 0.448 / 4.000 mm | 0 / 0 |
| Engine stale 2.5 s, no vergence | – | – | 1.18 s (HoldMs + return) | 0.448 / 4.000 mm | 0 / 0 |
| 5 frames of latency | – | – | 0.63 s | 0.448 / 4.000 mm | 0 / 0 |
| 12 noisy runs (1.0° + 0.7°, edge scenes) | 20 / 0 | 0.000 mm | 0.76 s | 0.448 / 4.000 mm | 74–391 / 0–165 |
| Realistic, wingman 80 m | – | – | – | 0 / 0 (×1.0000 throughout) | 103 / 0 |
| Realistic, target 500 m | – | – | 0.52 s | 0.448 / 4.000 mm; reaches ×1.2500 | 0 / 0 |

0.448 mm is 40 mm/s over one 90 Hz frame; 4.000 mm is the saccade step. A 16-seed stress pass of the noisy runs (48 runs)
also had no failure; the slowest return there was 0.78 s. The vergence-only runs keep their v1 behaviour with the
improved filter (PERCEPTION.md); `pvr_per_eye` proves vergence from PVR with the focus views sharing one gaze, boosts to
×1.4 in 4.06 s and returns in 0.77 s, with the near/far class right on 99.8 % of settled fixations;
`pvr_copied_table` never sees the session, never queries PVR and stays at ×1; every patched slot is back after
xrDestroyInstance. CSV (with the source, edge and veto per frame) and SVG traces in
`artifacts/native/pupil-shift/harness_out/`.

**Predictive transition, unit tests** (`predictive_tests`): the main sequence and its inverse (20° peaks at 442 °/s,
saturation, Carpenter's duration); a clean 20° saccade at 500 Hz: one onset, one prediction 46 ms after the onset of a
67.9 ms saccade, amplitude 19.90°, landing 0.34° off, onset within 8 ms, end within 1 ms; 0.15° of white noise per
sample: no false onset in 5 s, 18 of 20 saccades (20° in 20 directions) within 3° (worst 4.6°); 120 Hz: predicted
(22.2°, 3.0° off, 58 ms after onset); 4°: under 8°; 30 % faster than the main sequence: over-predicted but bounded by
the path; peaks 15 % low (tracker filtering): the gain learns 1.32 and the error falls from 5.3° to 0.6° in 16
saccades; a blink in flight drops the saccade without touching the gain. The grid: sky far, panel near, the strut and
the panel's top edges, the dead band, outside, invalid flags and sizes, both `typicalM` definitions, the live asymmetric
FOV, an empty cell. The step: 12 mm cap, 4.89 mm at 0.7 m, small remainders. The gate: step and hold, a near frame before
the landing is no contradiction, confirmation, contradiction without a step back, an ambiguous landing holds then
expires, lead 1 frame vs half a frame at 45 and 90 fps, the skips, off. The controller: the step at once, held, then
back at DownMmPerS with no saccade step.

**Predictive transition, harness** (45 fps, DCS locating 2 frames before the display time, PVR as a 500 Hz stream with
0.15° per-sample noise that the layer's own sampler thread polls at 1000 Hz; the engine stand-in publishes the cone and
the 32 × 32 grid of view 0, 2 frames late; panel below −10°, sky above, a canopy strut at 9–13°; saccades follow the
main sequence). Steps are the scale changes beyond the rates that the layer logged as a step at that display time;
every other change stays within the rates plus the gradual 4 mm step. 0 failures:

| Scenario | Result |
|---|---|
| Cockpit → sky, 20° (68 ms) | one step +12.00 mm, located 46 ms after the onset, 22 ms before the eyes land (displayed 22 ms after the landing); confirmed; ×1.2 after 0.51 s (gradual 2.91 s), ×1.25 after 1.13 s (gradual: not within 3.5 s) |
| The same, `typicalM` as a median; `PredictLeadFrames=1` | the same step (this phase leaves 22 ms) |
| Sky → panel, 24° | one step −4.87 mm (0.4° at 0.7 m), 48 ms after the onset, 27 ms before landing; confirmed; ×1 after 0.65 s (gradual 0.67 s) |
| Onto the canopy strut's edge, 29° | no step (mixed landing cell); the cone 0.3° past the strut then ramps at the gradual rates |
| 4° from the panel's top into the sky | no step (short); gradual |
| Mispredicted: 18° on the panel, 30 % faster than the main sequence, predicted into the sky | one step +12.00 mm (4 ms before landing), contradicted, back to ×1 0.48 s after the landing; one reversal, no second step |
| Predictive selected without PVR | gradual, the reason logged, status bit 61 throughout |
| Ini switched gradual → predictive live | the sampler starts, the step follows |
| Sweep: 15/20/25/30/35°, up and down, 4 frame phases each, lead ½ frame | steps on 0+2, 3+2, 3+4, 3+3, 4+3 of 4+4 (27/40); 0 contradictions |
| The same, lead 1 frame | 0+1, 1+0, 1+3, 3+3, 4+3 (19/40) |
| The same, a 120 Hz tracker, lead ½ | 0+2, 2+2, 1+3, 3+3, 4+3 (23/40), but located only 1–7 ms before landing |

In every predictive run PVR is never called after the runtime destroyed its session, the sampler stops at
xrDestroyInstance and every patched slot is back. CSV (locate and display time, gaze yaw/pitch, scale, steps, status
bits) and SVG traces: `artifacts/native/pupil-shift/harness_out/predict_*` and `gradual_*`.

## Open risks

- The engine module's side is not built yet in this tree; the harness's stand-in follows this document. Its real
  latency, window size and reverse-Z handling decide how often summaries are fresh.
- Whether Pimax's OpenXR runtime calls PVR through the shared interface table is unknown; if not, there is no per-eye
  vergence on Pimax (no veto; the scene depth alone). The frame of `GazeTan` (headset vs display) is an assumption whose
  failure is safe (×1 via unproven vergence), but it should be confirmed with the log's convergence column.
- The depth buffer at the HUD and through the canopy glass: if DCS writes depth for the HUD glass, Depth boost reads it
  as near (×1) unless the eyes converge far, which the rules leave to depth by design.
- Predictive: PVR's real sample rate and filtering are unknown. If the tracker runs at 120 Hz or smooths its output,
  the peak is seen later and lower (the 120 Hz sweep steps only a few ms before landing); the online gain corrects a
  systematic amplitude error, not the delay. The synthetic saccades are symmetric (smoothstep); real ones peak somewhat
  earlier, which helps. Whether `getEyeTrackingInfo` tolerates 500 calls a second from a second thread is untested on
  Pimax's runtime (the layer then is its only caller).
- Predictive: a 12 mm step up during a saccade towards the sky also changes the disparity of the cockpit around the
  landing by about 1° (at 0.7 m); it is applied while the eyes move and displayed just after they land, inside
  post-saccadic suppression, but whether it is invisible in practice is for the headset A/B to tell (lower
  `PredictMaxStepMm` live if it shows).
- The 0.8 s return budget is tight: 50 ms saccade hold + 2–3 frames of summary latency + 120 ms confirmation + the
  return at 40 mm/s from ×1.4 (0.54 s after the step). Longer engine latency adds to it directly.
