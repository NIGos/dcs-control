# Pupil shift (experimental)

## What it is

`XR_APILAYER_DCSVR_pupil_shift` is an OpenXR API layer. Its source is in `native/pupil_shift/`. It renders each eye from where the pupil really is for the current gaze, instead of from the single fixed eye point the runtime reports.

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

**Gaze.** The gaze comes from the focus views of a foveated quad-view configuration: their FOV is centred on the gaze. Pimax native Quad Views and the bundled Quad Views both provide this.

There is no gaze in two cases:

- stereo profiles;
- fixed focus.

Without a gaze, the layer does nothing.

**`xrLocateViews`.** For each eye:

- `pose += R_view * radius * (gaze - rest)`, where `rest` is straight ahead for the head: the mean of the two primary views' forward axes, expressed in this eye's frame. Headsets whose views are canted outwards (Sboys measures about 6° on the Crystal Super QLED) would otherwise read a straight gaze as turned towards the nose, a fixed shift of about 1.1 mm per eye (IPD 2.2 mm smaller). The gaze is clamped to `MaxGazeDeg` around `rest` too. The cant is logged.
- the FOV tangents become `(tan - d.xy/D) / (1 + d.z/D)`, where `D` is the virtual image distance.

The same offset in world space is applied to the eye's focus view.

**DCS's custom IPD.** DCS (Visualizer.dll 2.9.30, per-frame view loop) replaces each view's sideways eye position with ±custom_IPD/2 when `VR.custom_IPD_enable` is on, which would discard the pupil's sideways shift: only the vertical and depth parts and the FOV change would reach DCS. When the profile uses Pupil shift and DCS's custom IPD is on, the app writes `Ipd=<custom_IPD>` in `PupilShift.ini` and turns `VR.custom_IPD_enable` off in options.lua (an owned key, restored by Back to stock DCS). The layer then moves both eyes symmetrically along the line between them to that distance (focus views follow their eye) before the pupil shift, whatever the in-flight switch says, so the switch compares the pupil shift alone at the same IPD. `xrEndFrame` still hands the runtime its own poses back. Without DCS's custom IPD nothing changes (`Ipd=0`).

**`xrEndFrame`.** Projection views that still carry the values the layer handed out get the runtime's original pose and FOV back. The match is by display time and value, within 1e-4. The compositor and every layer below see the frame exactly as if the layer were absent; only the rendered content differs.

**Position in the chain.** The layer is first, next to DCS (`layerNames.Insert(0, …)` in `DeploymentPlanner`).

**Settings.** `PupilShift.ini` sits next to the DLL and is re-read about once a second:

- `Enabled`
- `EyeRadiusMm` (default 10.5)
- `VirtualImageM` (default 1.5)
- `MaxGazeDeg` (default 35)
- `SimulateGaze`, `SimulateGazeXDeg`, `SimulateGazeYDeg` (testing)

**Hotkey.** `Toggle` in `PupilShift.ini` (virtual-key:modifiers, Ctrl 1, Alt 2, Shift 4; default 121:5, Ctrl+Shift+F10; 0:0 off) turns the correction off and on in flight. The modifiers must match exactly. A low beep means off; a high beep means on. The app records it like its other in-flight keys and refuses a combination another switch uses.

**Status.** `PupilShift_Status()`, exported by the layer, is read by OFXR's diagnostic panel (patch 0015): bit 0 running, bit 1 correction on, bit 2 a gaze applied in the last second, bit 3 simulated gaze, bit 4 runtime poses given back, bits 8-15 the largest shift in 0.1 mm. The panel shows "Pupil shift: ON, 3.2 mm", "no gaze" or "OFF (Ctrl+Shift+F10)"; `DCSVR_PUPIL_HOTKEY` names the key.

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
| Profile (`VrProfile`) | `PupilShift` (off by default), `PupilShiftEyeRadiusMm` (5–15), `PupilShiftVirtualImageM` (0.5–5). `UsesPupilShift` needs a VR flight with Quad Views and an eye-tracked gaze. |
| Validation | `pupil-radius` and `pupil-image` are errors. `pupil-gaze` is a warning: the layer is on but the profile has no gaze, so it is not installed. |
| Deployment | `profiles/<id>/pupilshift/` holds the DLL, `PupilShift.ini` and the log. The manifest is `layers/pupil-shift.json`. `ControlService` verifies `components/pupilshift/XR_APILAYER_DCSVR_pupil_shift.dll` against its `.sha256`. |
| Release | `scripts/build-pupil-shift.ps1` builds the layer and runs the geometry tests. `build-release.ps1` copies the DLL and its hash into `components/pupilshift`. `verify-web-release.py` treats it as a rebuilt component. |
| UI | Its own page, **Pupil shift**, marked experimental: toggle, eye radius, image distance, in-flight switch; a note with a link when the profile has no eye-tracked gaze; the status from `PupilShift.log` (`PupilShiftStatus`): gaze range over the flight, largest shift, seconds with a gaze, whether the runtime's poses were given back, and warnings (gaze that barely moves; a runtime that moves the eye itself). |
| Tests | "Pupil shift is deployed first in the layer chain …" in `tests/DcsVr.Tests`, and `pupil_shift_tests` (CTest). |

## Open points

- **Real-headset check.** Not yet tested in DCS. To check:
  - the Pimax focus FOV follows the gaze without too much lag or clamping;
  - `end_frame: restored N projection views` appears in the log.
- **Compositor reprojection.** Checked statically, with a low risk: the compositor cannot undo the correction.
  - It receives the very poses it reported, so its timewarp does exactly what it does without the layer.
  - Its timewarp is the Oculus-style rotational warp (`EyeRenderD3D11PoseWarp`, `NdcToTimewarpLerpScale`). A rotational warp has no positional or depth term, so it cannot reverse a translation in any case.
  - Smart Smoothing works on the images themselves, so it carries the corrected content along.

  Quick sanity check: set `SimulateGaze=1`, `SimulateGazeXDeg=25` and `EyeRadiusMm=30` while DCS runs. The near cockpit should visibly shift against the outside. Restore the values afterwards.
- **Gaze lag.** The focus area may be smoothed by Pimax (`eye_tracking_low_pass_alpha`).
- **Beyond 25 degrees of gaze.** The lens part is not determinable there, so only geometry is corrected; `MaxGazeDeg` clamps at 35.
