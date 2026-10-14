# Pupil shift (experimental)

## What it is

`XR_APILAYER_DCSVR_pupil_shift` is an OpenXR API layer. Its source is in `native/pupil_shift/`. It renders each eye from where the pupil really is for the current gaze, instead of from the single fixed eye point the runtime reports.

The same layer carries **Depth boost** (the render IPD rises while the eyes fixate far), described in [PERCEPTION.md](PERCEPTION.md). The two are independent: `[PupilShift] Enabled` and `[DepthBoost] Enabled`; the layer is deployed when either is on.

## Why

When the eye turns, the pupil moves on a sphere around the eye's rotation centre. That sphere has a radius of about 10.5 mm. At 25 degrees of gaze the pupil sits 4.4 mm sideways and 1 mm back.

The runtime renders from a fixed point, so two errors appear when the eyes move without the head:

- Near objects (the cockpit) show a perspective error: about 0.2° at 0.7 m.
- The whole image shifts against the lens's virtual image, which sits at a finite distance: about 0.15° at 25 degrees of gaze.

Together they are the part of "pupil swim" that does not depend on the lens.

## Is the lens itself part of the problem?

`native/pupil_shift/virtual_lens/README.md` studies this with an ensemble of pancake lenses fitted to Pimax's own data. The data was read from the compositor: the distortion curve, a focal length of 16.8 mm, and the chromatic aberration.

If the lens is sharp when the eye turns, its own swim within 25 degrees of gaze is 0.01° ± 0.075°. The geometric correction is therefore the part worth doing, and no lens measurements are needed for it.

Pimax does not do this already:

- Its compositor uses eye tracking only for foveated rendering, the IPD motor and gaze filtering.
- Its distortion is one static object per eye.

The layer's log can confirm this at runtime (see "How to verify in the headset").

## How it works

**Gaze.** The layer picks one gaze source per frame and logs it (`gaze source=` in the stats line):

- `qv`: the bundled Quad Views exports `DcsVr_QuadViewsGaze(displayTime, left[3], right[3])` (`native/quadviews/dcsvr_perception.h`, applied by `scripts/build-quadviews.ps1`). At instance creation (and once more at the first `xrLocateViews`) the layer looks for `XR_APILAYER_MBUCCHIA_quad_views_foveated.dll` with `GetModuleHandleW` and the export with `GetProcAddress`. Quad Views sits below the layer, so when the layer's `xrLocateViews` returns from the chain, Quad Views has just recorded that display time. The export returns flags (bit 0 a sample for that display time or the latest one within 100 ms before it, bit 1 a real eye-tracker gaze, bit 2 a saccade) and the gaze as a unit vector in each eye's view frame. Quad Views has one combined gaze, so both eyes get the same direction, each in its own (canted) frame. When there is a sample, the export decides: a valid gaze is used as it is (the raw gaze, without the focus area's offsets, saccade lead or deadzone), and an invalid one means **no gaze**.
- `xr`: without the export (Pimax native Quad Views, or an older bundled Quad Views), the layer reads the runtime's eye gaze itself through `XR_EXT_eye_gaze_interaction`. DCS does not enable that extension, but Pimax's runtime offers it, so the layer adds it at instance creation (if the runtime refuses it, the instance is created as DCS asked). It is not added when the bundled Quad Views' export is loaded, or with `OpenXrEyeGaze=0`. The layer creates its own action set (`dcsvr_pupil_gaze`) with one pose action bound to `/user/eyes_ext/input/gaze_ext/pose`, and a gaze space when `xrGetSystemProperties` reports eye-gaze support. It never attaches or syncs input on its own: its set rides on DCS's own calls (the design of Cheeky's layer, copied and adapted). Its binding is merged into DCS's suggestion for the eye-gaze profile, or suggested alone just before DCS attaches. Its set is attached together with DCS's sets in DCS's `xrAttachSessionActionSets` and made active in each of DCS's `xrSyncActions`. If a merged call fails, DCS's call is made again exactly as it was. After 3 failed syncs in a row the layer stops adding its set. Each frame it locates the gaze space in the base space of DCS's `xrLocateViews` (`info->space`, at its display time) and turns the one combined gaze direction into each primary view's frame. The location decides: with `ORIENTATION_VALID` and `ORIENTATION_TRACKED`, the gaze is used; without them it is **no gaze**, whatever the focus views show.
- `focus`: only when neither the export nor the OpenXR eye gaze exists (the runtime does not offer the extension, the system reports no eye-gaze support, or DCS never attached action sets), the gaze comes from the focus views of the foveated quad-view configuration: their FOV is centred on the gaze. The log warns once (`gaze: WARNING reading the gaze from the focus views' centres …`, with the reason), because a fixed focus without a gaze then reads as one.
- `simulated`: `SimulateGaze=1`.
- `none`: no gaze. The eyes stay at rest (no pupil offset; the views keep the runtime's poses plus the `Ipd` move), and Depth boost gets no sample.

There is no gaze with stereo profiles (unless the OpenXR eye gaze gives one), with a focus FOV that is not finite, whenever the OpenXR eye gaze is not valid and tracked (nobody wearing the headset, the tracker lost the eyes), and whenever the bundled Quad Views reports its gaze invalid (the tracker lost the eyes, a blink longer than its 600 ms cache, nobody wearing the headset, foveated rendering off). In that case Quad Views shows a fixed focus area: the resting gaze plus `fixed_horizontal_offset` per eye, outwards. Read from the focus views, that fixed area looks like a gaze about 2.4° outwards per eye; before the export the layer moved each pupil about 0.44 mm outwards, and the log of a live test (Crystal Super, nobody wearing the headset, `Ipd=64`) showed `gaze x[-2.4..-2.4]` and `rendered IPD 64.9 mm`. With the export the same frames log `shifted=0 (no valid gaze)` and `rendered IPD 64.0 mm`. Pimax's native quad views do the same without a gaze: on 2026-10-11 (Crystal Super uOLED, nobody wearing the headset) the log showed `gaze x[-2.4..-2.4]`, `runtime IPD 65.6 mm, rendered IPD 66.4 mm; gaze source=focus`. With the OpenXR eye gaze those frames have no gaze (`xr invalid`), and the rendered IPD is exactly `Ipd`, or the runtime's own IPD with `Ipd=0`.

**`xrLocateViews`.** For each eye:

- `pose += R_view * radius * (gaze - rest)`, where `rest` is straight ahead for the head: the mean of the two primary views' forward axes, expressed in this eye's frame. Headsets whose views are canted outwards (Sboys measures about 6° on the Crystal Super QLED) would otherwise read a straight gaze as turned towards the nose, a fixed shift of about 1.1 mm per eye (IPD 2.2 mm smaller). The gaze is clamped to `MaxGazeDeg` around `rest` too. The cant is logged.
- the FOV tangents become `(tan - d.xy/D) / (1 + d.z/D)`, where `D` is the virtual image distance.

The same offset in world space is applied to the eye's focus view.

**DCS's custom IPD.** DCS (Visualizer.dll 2.9.30, per-frame view loop) replaces each view's sideways eye position with ±custom_IPD/2 when `VR.custom_IPD_enable` is on, which would discard the pupil's sideways shift: only the vertical and depth parts and the FOV change would reach DCS. When the profile uses Pupil shift or Depth boost (`UsesPerceptionLayer`) and DCS's custom IPD is on, the app writes `Ipd=<custom_IPD>` in `PupilShift.ini` and turns `VR.custom_IPD_enable` off in options.lua (an owned key, restored by Back to stock DCS). The layer then moves both eyes symmetrically along the line between them to that distance (focus views follow their eye) before the pupil shift, whatever the in-flight switch says, so the switch compares the pupil shift alone at the same IPD. Depth boost multiplies that distance (or the headset's own when `Ipd=0`) by its scale. `xrEndFrame` still hands the runtime its own poses back. Without DCS's custom IPD nothing changes (`Ipd=0`).

**`xrEndFrame`.** Projection views that still carry the values the layer handed out get the runtime's original pose and FOV back. The match is by display time and value, within 1e-4. The compositor and every layer below see the frame exactly as if the layer were absent; only the rendered content differs.

**Position in the chain.** The layer is first, next to DCS (`layerNames.Insert(0, …)` in `DeploymentPlanner`).

**Settings.** `PupilShift.ini` sits next to the DLL and is re-read about once a second:

- `Enabled`: the pupil correction. With `Enabled=0` the layer still runs when `[DepthBoost] Enabled=1`; it then applies `Ipd` and Depth boost's scale only, and logs `shifted=0 (pupil shift disabled)`.
- `EyeRadiusMm` (default 10.5)
- `VirtualImageM` (default 1.5)
- `MaxGazeDeg` (default 35)
- `SimulateGaze`, `SimulateGazeXDeg`, `SimulateGazeYDeg` (testing). The simulated gaze is also published in the
  `Local\DcsVrPerceptionGaze_v1` block (FIXATION_DEPTH.md), so the engine module's depth sampling can be tried live with
  nobody in the headset.
- `OpenXrEyeGaze` (default 1): set 0 to keep the layer from enabling and reading `XR_EXT_eye_gaze_interaction`. The gaze then comes from the focus views, as before. Read once, at instance creation.
- `[DepthBoost]` keys (PERCEPTION.md), among them `Mode`, `FarStartM`, `FarFullM`, `PimaxPvrGaze` (Pimax PVR's per-eye gaze for Depth boost; it is read only while Depth boost is enabled) and `Trace`.

**Shared memory.** While Depth boost (or `SimulateGaze`) is on, the layer writes its gaze and the FOVs DCS renders with to
`Local\DcsVrPerceptionGaze_v1` at every new display time, and reads the engine module's depth summary from
`Local\DcsVrFixationDepth_v1` (`perception_shm.h`, FIXATION_DEPTH.md). Pimax PVR's per-eye gaze is used for Depth boost
only; the pupil shift keeps the gaze sources above.

**Log.** `PupilShift.log` sits next to the DLL; it is opened shared for reading (`_wfsopen`, `_SH_DENYWR`), so the app reads it while DCS runs. A `gaze:` line says whether the Quad Views export was found. `eye gaze:` lines follow the OpenXR eye gaze: whether the extension was enabled by DCS, added by the layer, refused or not offered; the action set; the session (system support, gaze space); the binding; and the attachment. Then `gaze: OpenXR eye gaze in use`, or the fallback warning. Once a second a `stats:` line gives the frames, the shifted frames, the gaze range, the largest shift, the runtime's eye position, the cant, the runtime and rendered IPD. It ends with `gaze source=<last frame's> (qv N, xr N, focus N, simulated N, none N; qv invalid N, xr invalid N, saccade N)`: the frames per source, the frames Quad Views reported without a valid gaze, the frames whose OpenXR eye gaze was not valid and tracked, and the frames Quad Views flagged as a saccade; then `qv valid N, refused as implausible N, last refused x .. y .. deg`: Quad Views' valid gazes and those the plausibility check refused (more than 30° up or 50° off the eye's axis), with the last refused direction. Without a shifted frame the line says why: `pupil shift disabled`, `correction off`, `no valid gaze` (Quad Views or the OpenXR eye gaze said so) or `no gaze source`.

**Hotkey.** `Toggle` in `PupilShift.ini` (virtual-key:modifiers, Ctrl 1, Alt 2, Shift 4; default 121:5, Ctrl+Shift+F10; 0:0 off) turns the correction off and on in flight. The modifiers must match exactly when the key itself goes down; changes of the modifiers while the key is held do not count (Windows' Alt+Shift input-language switch flickers them), and the switch fires at most once per 300 ms. A low beep means off; a high beep means on. The app records it like its other in-flight keys and refuses a combination another switch uses.

**Status.** `PupilShift_Status()`, exported by the layer, is read by OFXR's diagnostic panel (patch 0015): bit 0 running, bit 1 correction on, bit 2 a gaze applied in the last second, bit 3 simulated gaze, bit 4 runtime poses given back, bits 8-15 the largest shift in 0.1 mm. The panel shows "Pupil shift: ON, 3.2 mm", "no gaze" or "OFF (Ctrl+Shift+F10)"; `DCSVR_PUPIL_HOTKEY` names the key. Depth boost uses bits 5-7 and 16-59 (PERCEPTION.md); bit 7 set means the pupil correction is off in the profile, and patch 0017 then shows Depth boost alone on that line.

**Log.** `PupilShift.log` sits next to the DLL. Once a second it writes:

- frames and how many were shifted;
- the gaze range;
- the maximum shift;
- how many projection views were given back to the runtime with its own poses (`restored=`);
- the views' cant, the runtime's eye distance and the eye distance DCS renders with;
- the runtime's own left-eye position relative to the head.

If that last value moved with the gaze, the runtime would already be shifting the eye itself.

## Profile and app

| Area | What exists |
|---|---|
| Profile (`VrProfile`) | `PupilShift` (off by default), `PupilShiftEyeRadiusMm` (5–15), `PupilShiftVirtualImageM` (0.5–5). `UsesPupilShift` needs a VR flight with Quad Views and an eye-tracked gaze. `DepthBoost` and its settings: PERCEPTION.md. `UsesPerceptionLayer` (either) deploys the layer and takes over DCS's custom IPD. |
| Validation | `pupil-radius` and `pupil-image` are errors. `pupil-gaze` is a warning: the layer is on but the profile has no gaze, so it is not installed. |
| Deployment | `profiles/<id>/pupilshift/` holds the DLL, `PupilShift.ini` and the log. The manifest is `layers/pupil-shift.json`. `ControlService` verifies `components/pupilshift/XR_APILAYER_DCSVR_pupil_shift.dll` against its `.sha256`. |
| Release | `scripts/build-pupil-shift.ps1` builds the layer and runs the geometry tests. `build-release.ps1` copies the DLL and its hash into `components/pupilshift`. `verify-web-release.py` treats it as a rebuilt component. |
| UI | Its own page, **Pupil shift**, marked experimental: toggle, eye radius, image distance, in-flight switch; a note with a link when the profile has no eye-tracked gaze; the status from `PupilShift.log` (`PupilShiftStatus`): gaze range over the flight, largest shift, seconds with a gaze, whether the runtime's poses were given back, and warnings (gaze that barely moves; a runtime that moves the eye itself). |
| Tests | "Pupil shift is deployed first in the layer chain …" and the Depth boost tests in `tests/DcsVr.Tests`; `pupil_shift_tests` and the end-to-end `pupil_shift_harness` (the built DLL under a fake runtime, PERCEPTION.md) in CTest. The harness's `qv_*` scenarios run a Quad Views-like runtime (combined gaze, fixed focus without a gaze) with a fake `XR_APILAYER_MBUCCHIA_quad_views_foveated.dll` exporting `DcsVr_QuadViewsGaze` (`fake_quadviews.cpp`): with it, every frame without a valid gaze renders at exactly `Ipd` (64.000 mm) with the runtime's FOVs; without it (`qv_no_export`) the fixed focus still reads as a gaze (64.879 mm). The `xr_*` scenarios add `XR_EXT_eye_gaze_interaction` to the fake runtime. With a valid gaze (`xr_gaze`), the shift follows it. Without one, the IPD is exactly 64.000 mm, and in `xr_never_valid` (`Ipd=0`) exactly the runtime's 65.500 mm, although the focus views sit 2.4° outwards. In `xr_app_gaze` the application enables the extension and binds its own gaze action, and both bindings reach the runtime. In `xr_offered_qv_export` the export wins and the extension is not added. In every scenario the harness's "DCS" gets its own set attached once and synced in every call, with the layer's set merged in only when it is used. `tests/perception` checks the export's flags and per-eye directions. The `pvr_*` scenarios load a fake `LibPVRClient64.dll` (`fake_pvr.cpp`) and the `depth_*` and `realistic_*` scenarios an engine-module stand-in (FIXATION_DEPTH.md). |

## Open points

- **Real-headset check.** Not yet tested in DCS. To check:
  - the Pimax focus FOV follows the gaze without too much lag or clamping;
  - `end_frame: restored N projection views` appears in the log.
- **Compositor reprojection.** Checked statically, with a low risk: the compositor cannot undo the correction.
  - It receives the very poses it reported, so its timewarp does exactly what it does without the layer.
  - Its timewarp is the Oculus-style rotational warp (`EyeRenderD3D11PoseWarp`, `NdcToTimewarpLerpScale`). A rotational warp has no positional or depth term, so it cannot reverse a translation in any case.
  - Smart Smoothing works on the images themselves, so it carries the corrected content along.

  Quick sanity check: set `SimulateGaze=1`, `SimulateGazeXDeg=25` and `EyeRadiusMm=30` while DCS runs. The near cockpit should visibly shift against the outside. Restore the values afterwards.
- **Gaze lag.** With Pimax native Quad Views the layer reads the runtime's eye gaze (`gaze source=xr`); whether Pimax smooths it as it smooths the focus area (`eye_tracking_low_pass_alpha`) is not known yet. With the bundled Quad Views the layer uses its raw gaze (`gaze source=qv`).
- **OpenXR eye gaze and DCS's input.** Checked with the harness only. Live, check: the log has `attached with the application's N action set(s): yes` and `gaze: OpenXR eye gaze in use`; controllers, hand tracking and the mouse work as before; `xr invalid` counts every frame while nobody wears the headset (rendered IPD exactly `Ipd`); `xr` counts frames while someone does. Cheeky (when it runs below this layer in quad focus mode) normally does not request the extension. It now finds it enabled, so it creates and attaches its own gaze action the same additive way. If anything misbehaves, `OpenXrEyeGaze=0` restores the previous behaviour.
- **Pimax PVR.** For Depth boost the layer patches four slots of PVR's interface table (never code) to find the runtime's
  session (FIXATION_DEPTH.md). Live, check `pvr: Pimax PVR client found` and `pvr: the runtime's PVR session found`, and
  that nothing else changes (tracking, frame submission, the runtime's own eye tracking). `PimaxPvrGaze=0` turns it off.
- **Quad Views' blink cache.** For up to 600 ms after the tracker loses the eyes, the bundled Quad Views keeps reporting the last gaze as valid; the export does the same, so a pupil stays where it was during a blink.
- **Beyond 25 degrees of gaze.** The lens part is not determinable there, so only geometry is corrected; `MaxGazeDeg` clamps at 35.
