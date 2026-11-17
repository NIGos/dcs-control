# Foveated frame generation: the unattended test rig (2026-11-15)

> **A record, not a manual.** This file was written on 2026-11-15/16 while the rig lived in a git-ignored scratch
> folder, and it is kept as written: how the capture works, what each verdict measures, what the first flights
> changed. For how to run the promoted tools, their parameters and what is not promoted yet, read `README.md`.
> Where this file and `README.md` differ, `README.md` is right. Paths below are the scratch ones:
> `artifacts\foveated\rig\` is now `tools\rig\`; `rig\runs\` is now `artifacts\rig\runs\`; the capture layer
> (`capture-layer\`), `cursor_calib.py` and `cursor_verdict.py` were promoted in a second step (1cc03cd);
> `gaze_sheets.py` and the scenarios other than `hud`, `motion`, `gaze`, `cursor`, `cursor-calib` were not. Section 6 (the guards) was brought up to date with
> the promoted runner.

Test-only tooling of RIG-RECON.md's build-order steps 2 and 3. Everything is under `artifacts\foveated\rig\`; no
shipped component (`external\`, `patches\`, `native\`, `scripts\`, `src\`) was touched, nothing is registered or
installed, nothing was committed. Sections 1-8 are the state before any flight; **section 9 is what the first two
flights (hud ON + twin, gaze ON; flown by the main session, 2026-11-15 17:17 and 17:28) changed**, and it overrides
the earlier sections where they differ (capture keys, the twin's capture, the verdict's interpolation measure, the
hud scenario).

| Piece | File | State |
|---|---|---|
| Capture layer ("rig tap") | `rig\capture-layer\rigtap.cpp` -> `bin\XR_APILAYER_RIG_capture.dll` + `.json` | built, tested offline (section 7) |
| Its harness, chain shim, byte check | `capture-layer\rigtap_harness.cpp`, `rig_chain_shim.cpp`, `check_harness.py` | built, run |
| Offline composer | `rig\compose.py` (+ `riglib.py`) | self-tested |
| Verdict tools | `rig\verdict.py` | self-tested |
| Self-test of both | `rig\selftest.py` | run: 26 checks, 0 failed (24 before the flights + the two-stream twin and the HUD locator) |
| Scenario runner | `rig\run-scenario.ps1`, `rig\scenarios\*.json` | flown twice by the main session (hud ON + twin, gaze ON); the changes of section 9 (recenter, HUD test, twin plan) only `-DryRun` |
| HUD test / locator, gaze sheets | `rig\hudfind.py`, `rig\gaze_sheets.py` | section 9.5, 10 |

## 1. Build

```powershell
pwsh -File artifacts\foveated\rig\build-capture-layer.ps1          # layer, harness, shim, manifest
pwsh -File artifacts\foveated\rig\build-capture-layer.ps1 -Test    # + the two offline tests of section 7
py artifacts\foveated\rig\selftest.py                              # composer and verdict tools on generated data
```

The build reads nothing outside `rig\`: `capture-layer\msvc.cmd` is a copy of `scripts\msvc.cmd`, `capture-layer\third_party\openxr-include` a copy of the OpenXR headers of `external\ofxr-djules75`, and `capture-layer\test-data` holds copies (2026-11-15 15:28 build, patches up to 0081) of OFXR's test program, its layer DLL and the test's ini, so a branch switch or a rebuild of the components does not change the rig or its tests.
The build script waits while `artifacts\.lane\quiet.lock` exists. The manifest holds the DLL's absolute path (as the
A2 observer's): rebuild after moving the folder.

## 2. The capture layer

**Where it sits.** LAST in `XR_ENABLE_API_LAYERS` = closest to the runtime, below OFXR, so it sees exactly what OFXR
submits. **Graphics binding there: D3D12.** With `d3d11_bridge=1` OFXR opens the runtime session with an
`XrGraphicsBindingD3D12KHR` on its own device and queue (`external\ofxr-djules75\src\layer\openxr_layer.cpp`
6530-6536) and bridges DCS's D3D11 above it; the twin with the option off has the same binding (frame generation is
still on). The layer reads pixels on D3D12 only; on a D3D11 session (frame generation off) it records the metadata and
writes `"noPixels":"session is not D3D12"`.

**Hooks.** `xrCreateSession` (device, queue), `xrCreateSwapchain` (+ its own `xrEnumerateSwapchainImages` for the
textures), `xrAcquireSwapchainImage` / `xrReleaseSwapchainImage` (which image was released last, and a release counter
per swapchain), `xrCreateReferenceSpace`, `xrEndFrame`, the destroy calls. Everything else is not hooked at all.

**Idle cost.** `xrEndFrame`: one atomic load and a tick comparison; four times a second one file-attribute query for
the trigger file. Acquire / release: one mutex and a map lookup. Measured in the harness: 107 ns per call against 10 ns for the fake runtime's own call; 4.2 us mean over a stretch that held the file queries.
Without `RIGTAP_DIR` the trigger is never looked for.

**Arming.**

| How | Meaning |
|---|---|
| file `<RIGTAP_DIR>\arm.txt` | `key=value` lines, all optional: `frames` (default 40, hard cap 120), `delay_ms` (start that long after the file is seen), `start_display_time` (XrTime in ns: start at the first submission at or after it), `scale` (integer box downscale), `crop` (`x0,y0,x1,y1` fractions of every sub-image), `tag`. Write it under another name and rename it. It becomes `arm.taken-<n>.txt` |
| `RIGTAP_FRAMES=N`, `RIGTAP_AFTER_S=T` | one capture of N submissions starting T seconds after the session's first `xrEndFrame` |
| `RIGTAP_SCALE`, `RIGTAP_CROP` | defaults for every capture; `RIGTAP_MAX_MB` disk budget of the process (default 6144) |
| `RIGTAP_EXTENSIONS` | see "null-instance question" below |

A capture is N **consecutive** `xrEndFrame` calls. Several captures per session are possible (one at a time); each
goes to `<RIGTAP_DIR>\cap<nn>[_tag]\`.

**What a capture holds.** `capture.jsonl`: a header (settings, QPC frequency, every swapchain seen so far: id in
creation order, handle, size, array size, format asked and the texture's own format, usage), then one line per
submission: `n`, QPC, `displayTime`, `layerCount`, and per layer its type and flags; per projection view the pose, the
field of view (radians), the swapchain (handle, creation id, format, size), the sub-image rectangle and array index,
the image index and the release counter, `depthInfo`, and the pixel file; `copyUs` = what the capture cost inside that
`xrEndFrame`; quad layers with pose and size (no pixels); then an end line with the reason. Pixels: one file per view,
`s<n>_L<layer>_v<view>.rgba`, raw 8-bit x 4, rows top-down, colour **and alpha** as stored (`bgra:true` for BGRA
swapchains). `done.txt` appears when everything is on disk. `<RIGTAP_DIR>\rigtap.log` is the layer's own log.

**Real or synthetic: what the layer can and cannot tell.**
- *Repeat* (the same images submitted again, nothing released in between): exact, from the release counter.
- *Real against synthetic*: the layer does not know. It records what lets the tools decide:
  1. the display time: OFXR's flight log has one `op=presenter_submission` per runtime frame with b = that display
     time and c = 1 real / 2 synthetic / 0 repeat (`openxr_layer.cpp` 12737-12742). `verdict.py --flight-log` joins on
     it: **this is the reference**, and the runner always passes it;
  2. the swapchain creation ids: per application swapchain OFXR creates a "current" ring of two and then the
     "synthetic" ring (`openxr_layer.cpp` 4030-4043, 2697), so real frames come from two alternating swapchains and
     synthetic ones from the later ones. Read in the code, **never checked on the headset**; `verdict.py` uses it only
     without a flight log and prints how often it agrees with the log when it has both;
  3. the pose: a synthetic carries the pose of the real frame after it (`openxr_layer.cpp` 15954-15967), so the real
     frame after a synthetic does not change the pose. Useless with a still, untracked headset (the pose never moves).
- Not visible from here: which real frames a synthetic was made from, and whether a "real" image was touched by OFXR
  (HUD removal and re-adding, cursor) before it went down.

**Robustness.** The application's call is always forwarded and its result returned unchanged. Any failure (no image
known, format not 8-bit x 4, GPU copy not finished in 2 s, device removed, writer queue over 3 GB, disk budget, a file
that cannot be written, a C++ exception) ends the capture, is written as the end reason and to `rigtap.log`, and the
layer goes back to idle. A hook is only handed out when its next function exists.

**Null-instance question.** Before the instance exists OFXR asks the layer below, with a null instance, for
`xrEnumerateInstanceExtensionProperties` (does the runtime list D3D12 and the visibility mask). There is no next entry
yet, so the layer answers from the loader in the process (as OFXR does for the layer above it); with no loader (the
chain probe) from `RIGTAP_EXTENSIONS`, else the Pimax runtime's own list.

**Disk use and cost.**

| Sizes (two slices each) | Bytes per submission | 40 submissions | hard cap 120 |
|---|---|---|---|
| periphery 1076x1060 + focus 1952x1928 (the flown test profiles) | 39,232,128 (37.4 MiB) | 1.57 GB | 4.71 GB |
| periphery 1032x1020 + focus 3480x2048 (the user's profile, INTEGRATION 9a) | 65,437,440 (62.4 MiB) | 2.62 GB | 7.85 GB (over the default budget: the capture stops at 6 GB) |
| the first, `scale=2` | 9,808,032 | 0.39 GB | 1.18 GB |
| the first, `crop=0.3,0.3,0.7,0.7` | about 6.3 MB | 0.25 GB | 0.75 GB |

`scale` shrinks the files only (the box average runs on the writer thread); `crop` also shrinks the GPU copy and the
time inside `xrEndFrame`. Pixels wait in memory for the writer thread (limit 3 GB). The copy is synchronous: the image
is copied to a readback buffer on the session's queue and the call waits for it. Measured with the harness on this PC's graphics card with nothing else running, at 1076x1060 + 1952x1928 (778,027,776 bytes compared, all equal): **median 6.3 ms, worst 10.1 ms per submission** inside `xrEndFrame`; with `crop=0.25,0.25,0.75,0.75` 1.8 ms. Under DCS's load it is unmeasured and will be longer.
**Pacing numbers inside a capture window are void, and at 90 Hz the stall may itself cause repeats**: capture windows
and pacing windows must not overlap; use `crop` when the cadence inside the window matters.

**To check on first live use** (cannot be known offline): that the image is complete at `xrEndFrame` on the binding
queue (OFXR joins its synthesis queue before the release, `openxr_layer.cpp` 11945-11960); that Pimax's images are in
`RENDER_TARGET` state when released (OpenXR's D3D12 rule; the copy moves them to `COPY_SOURCE` and back); the real
cost of the copy under DCS's load.

## 3. Offline composer

```powershell
py artifacts\foveated\rig\compose.py <capture dir> --n 3 --layers        # composed PNG per eye + both layers + alpha
py artifacts\foveated\rig\compose.py <capture dir> --n 3 --roi full --ppt 600 --blend linear
```

Port of `Compose()` (`tools\runtime-probe\probe_run.cpp` 334-357) with the rule the runtime probe measured on Pimax:
periphery enlarged bilinearly over its field of view, focus placed by its own field of view (tangents) inside it,
`out = focus * a + periphery * (1 - a)` on the **encoded** bytes with the focus layer's unpremultiplied alpha.
`--blend linear` is the probe's original (linear light). Layer flags are honoured (no `BLEND_TEXTURE_SOURCE_ALPHA` =
opaque; no `UNPREMULTIPLIED_ALPHA` = added as premultiplied). A submission with one projection layer (the twin with
the option off) is resampled onto the grid as it is. Default grid: the focus field of view widened by 15 % at the
focus layer's own pixel density. Not modelled: the runtime's reprojection, lens distortion, panel colour.

## 4. Verdict tools

```powershell
py artifacts\foveated\rig\verdict.py <capture dir> --flight-log <run>\ofxr-bridge-flight-*.log `
   --crop hud:focus:-0.08,0.08,0.08,-0.08 [--band-from <the option-on capture, for a twin>] [--eyes 0,1]
```

Writes `verdict.json` (every number per triple) and `verdict.txt` (the table). Per triple real(n), synthetic,
real(n+1); at 3X two synthetics with fractions 1/3 and 2/3.

| Check | Measure | Threshold (**all guesses** until the twin gives the floor) |
|---|---|---|
| (a) `seam.gradient` | composed image: mean gradient in the focus fade band / mean of just inside and just outside; synthetic against its real neighbours | synthetic / real <= 1.25 |
| (a) `seam.layerMismatch` | the two layers' disagreement in the band, both low-passed to the periphery's resolution; synthetic / real | <= 1.5 (recon) |
| (b) `interp*.ratio` | per layer, 4x4 regions: phase correlation real->real and real->synthetic, corrected for a moved field of view; ratio = projection of the synthetic's motion on the real one's. Regions that moved < 0.75 px are left out | abs(median - fraction) <= 0.15 (recon) |
| (b) `interp*.notACopy` | byte-equal share of the synthetic with each neighbour | no synthetic >= 0.98 equal (scene must move: mean abs difference real-real >= 0.5) |
| (c) `alpha.equalsNext` | synthetic focus alpha against the next real frame's (resampled when the fields of view differ), flags equal | mean abs <= 2 of 255 |
| (c) `alpha.noHoles` | pixels whose alpha fell below half where the next frame's is >= 128 | share <= 0.0005 |
| (c) `alpha.uncovered*` | the zone of the synthetic's focus image outside the previous real frame's focus rectangle: black pixels with alpha > 8; layer mismatch there against the interior | black share <= 0.01; mismatch <= 2 x interior (recon) |
| (d) `crop.<name>.sharp` | tangent box in the focus or periphery layer: mean squared gradient, synthetic / mean of neighbours | >= 0.9 (recon) |
| (d) `crop.<name>.position` | centroid of the bright mark holding the crop's brightest pixel: fraction of the neighbours' displacement; or the error in px when they are still | abs(fraction error) <= 0.2; <= 2 px |
| (e) `duplicates.none` | repeats (exact) and new images with >= 0.999 byte-equal pixels to the submission before; display-time steps; the cadence string (`R`, `S`, `r` repeat, `?`) | 0 |

`NOT JUDGED` is printed with the reason (still scene, single layer, no triple, nothing to tell real from synthetic).
Limits: phase correlation measures translation per region, not occlusion or deformation; the true in-between frame
does not exist in DCS (only the chain probe has it); the blob measure needs the mark to be the brightest thing in its
crop; the seam measure on a twin needs `--band-from` (the band's place comes from the option-on capture, valid while
the gaze is still).

## 5. Scenario runner

One command per scenario (flies the option ON, then the twin with the option OFF, same mission and schedule):

```powershell
pwsh -File artifacts\foveated\rig\run-scenario.ps1 -Scenario hud    -Both -Prep
pwsh -File artifacts\foveated\rig\run-scenario.ps1 -Scenario motion -Both -Prep [-Arm 2x|3x|auto]
pwsh -File artifacts\foveated\rig\run-scenario.ps1 -Scenario cursor -Both -Prep [-Arm 2x|3x]
pwsh -File artifacts\foveated\rig\run-scenario.ps1 -Scenario gaze   -Both -Prep [-Arm fill2|fill1|nomarker]
pwsh -File artifacts\foveated\rig\run-scenario.ps1 -Scenario head   -Both -Prep -Force     # blocked, see below
pwsh -File artifacts\foveated\rig\run-scenario.ps1 -Scenario hud -Both -DryRun             # plan and checks only
```

`-Twin` = only the twin; neither = only the option on; `-Prep` = start Pimax, screen-off key 0, headset restart (as
fly4.ps1 -Prep); `-KeepPimax` = leave PimaxClient open at the end. Output: `rig\runs\<scenario>_<arm>_<MMdd-HHmm>_<on|off>\`
(`run.json`, `profile.json`, `live\` = the profile copy, `tap\cap*\` = captures with `verdict.txt` / `verdict.json`,
logs, `ofxr-captures\`), `rig\runs\steps.log`, `rig\runs\state-before-<stamp>\`.

Per flight: profile = `user-state-backup\draft.json`'s profile + the scenario's and the arm's overrides +
`frameGenFoveated` on or off, applied with the **installed** CLI; a copy of the applied profile folder with the
scenario's `[ofxr]` / `[diagnostics]` keys, Quad Views `[exe:DCS]` keys and the capture layer appended last to
`XR_ENABLE_API_LAYERS` / `XR_API_LAYER_PATH` in the **copy's** `launch.json` (plus `RIGTAP_DIR`); DCS from that
`launch.json` into the mission; boost helper; FLY click confirmed by `WWT (Main):` in dcs.log; the schedule; normal
close; logs; `verdict.py` per capture with the flight log (after DCS has closed).

| Scenario | Measures | From | Status |
|---|---|---|---|
| `hud` (1) | Sharp HUD on two layers: HUD crop sharpness and position on synthetics, seam, alpha; `cockpit_mask` counters; OFXR `hud_capture` triples | capture at FLY+30 s (40), flight log, `hud-*.bmp` | ready, never run. The HUD crop is a guess until the first picture |
| `head` (2) | yaw per real frame against the commanded rate, synthetic pose = next real pose, per-layer equality | capture x2 | **blocked**: the pose hook does not match any Quad Views DLL since 0.6.2 (recon finding 1); needs the resolver in the peer payload + developer mode, and a runner action that writes the dev ini (not written) |
| `gaze` (3) | focus field of view after a jump, synthetic alpha, uncovered zone (black, mismatch), saccade counters; arm `fill1` | captures of 90 at scale 2 around +-8 and +-15 degree jumps, Quad Views log | ready, never run. Uses `debug_simulate_tracking=1` (copy only): the mouse is confined to the desktop's top-left 1000 px while DCS runs, the cockpit cursor moves with the gaze, `smooth_cursor=0`, the first run draws the gaze marker. Mapping sign and scale unverified |
| `cursor` (4) | cursor position on synthetics in both layers, `smooth_cursor` counters against the twin; arms 2x / 3x | centre-crop capture of 60 during a 12 s mouse circle, flight log, `cursor-capture-*.bmp` | ready, never run |
| `motion` (5) | interpolation ratio 1/2 (1/3, 2/3) in both layers, layer mismatch, 1X during the F10 map, duplicates; arms 2x / 3x / auto | captures at FLY+30, +70 (map), +107 s | ready, never run |

## 6. Guards on the machine's state, and the risks

| Risk | Guard |
|---|---|
| Stock not restored, DCS left running | `finally` of every flight and of the run: DCS closed (90 s, then killed), `DcsVr.Cli restore`, status printed; a non-clean state is said in `steps.log` |
| The app's files edited | never: only the installed CLI applies; all edits are in the copy under `rig\runs\`; the copy is checked (no path into the source, every layer library exists, `DCSVR_QUAD_SETTINGS` in the copy) |
| The user's draft / presets changed | the five state files are copied to `runs\state-before-<stamp>\` first and compared by hash at the end, with that copy and with `user-state-backup`. A difference is reported; **nothing is written back automatically** |
| Mouse left confined (`gaze`) | `ClipCursor(NULL)` after every flight and at the end. If the script is killed: the clip ends when DCS exits or on Ctrl+Alt+Del |
| Screen-off key left at 0, Pimax left on | the key's value is read before `-Prep` sets it to 0 and put back at the end (a value that cannot be read is not changed); PimaxClient is closed only when it was not open before the run, and not with `-KeepPimax` |
| Quiet lock left behind | taken after waiting for another holder and for running builds; removed in `finally` (only if this run took it) |
| Disk filled | per capture: frames cap 120, `RIGTAP_MAX_MB` 6144, writer queue 3 GB; the runner checks free space before starting |
| DCS Control open, DCS running, layer not built, backup missing, scenario blocked | refused before anything is applied |
| The capture layer breaks the session | if `rigtap.log` shows no D3D12 session at the briefing the flight is abandoned before FLY. A crash inside the layer would take DCS down (it is in the process): then the `finally` restores as above |
| The script itself is killed (window closed, power) | nothing restores: run `DcsVr.Cli.exe restore`, set the key back to the value `steps.log` shows as "screen-off key before" with `pimax_ctl.exe set runtime_screen_off_timeout_second <value>`, delete `artifacts\.lane\quiet.lock` |

## 7. What was tested offline, and what has never run

**Capture layer, test 1: harness** (`rigtap_harness.exe` = loader + fake runtime + OFXR's part, D3D12 on the WARP
software device; `check_harness.py` recomputes the pattern). Run 2026-11-15 16:01:
- three captures from the trigger file (12 full; 6 with `scale=2` + `crop` + `delay_ms`; a request of 500 cut to the
  hard cap of 120): **552 views, 321,556,224 bytes of colour and alpha compared, all equal**; sub-image rectangles
  with an offset, array slices 0 and 1, odd sizes (row pitch), a TYPELESS texture (format 28) and an sRGB one (29);
  pose, field of view, flags, swapchain ids, formats, display times as submitted; consecutive submissions;
- 18 repeated submissions recognised, none missed or invented (the first submission of a capture has nothing before
  it and is never called a repeat);
- a quad layer on a swapchain the layer never saw: recorded without pixels, no error;
- every `xrEndFrame` reached the runtime once with the application's own pointer; a function the runtime lacks stays
  unsupported; the null-instance extension question is answered before the instance exists;
- idle `xrEndFrame` through the layer **107 ns** against 10 ns for the fake runtime's own call (120 calls);
- copy inside `xrEndFrame` at the harness sizes (2.4 MB per submission, WARP): median 0.47-0.56 ms.

**Capture layer, test 2: below the real OFXR** (`rig_chain_shim.dll` puts the capture layer between
the copy of `artifacts\native\ofxr-djules75\XR_APILAYER_XRFrameBridge_diagnostic.dll` (sha 806d079e) and the fake runtime of OFXR's own
`xrfg_layer_call_chain.exe`, scenario `two:dcs`, `foveated_layers=1`, WARP):
- idle and armed: the call-chain test still passes (exit 0; its own two-layer checks: 56 runtime frames, 23
  synthetic, 0 wrong shapes, 0 wrong focus alpha);
- armed from the environment (40 submissions, 0.25 s after the first frame): 40 of 40 with two projection layers,
  flags 0 and 6, 160 of 160 views with pixels;
- real against synthetic: OFXR's flight log has the display time of 35 of the 40 submissions (the first five went
  down before its presenter wrote records); **the swapchain-order rule agrees with the log on 35 of 35**; the pose
  rule holds on every real-after-synthetic pair; the synthetic focus alpha equals the next real frame's in 18 triples.
  The images of that test are 4x4 and 8x8 pixels: it says nothing about image content.

Not exercised offline: the real OpenXR loader (manifest, `XR_ENABLE_API_LAYERS` order: the A2 observer flew that
way), the Pimax runtime (image state, typeless formats, queue ordering with OFXR's synthesis queue), DCS's process,
the D3D11 bridge above (the call-chain scenario used is D3D12 on both sides), sizes above 1952x1928, a session that
ends in the middle of a capture, device removal, a full disk, the memory limit of the writer queue.

**Composer and verdict tools: `selftest.py`** (generated scene with known motion, a quarter of the live sizes, files
in the layer's format), 24 checks, 0 failed:

| Case | Known | Read |
|---|---|---|
| vectorised composer against a literal port of `Compose()` | equal | max difference 0.0000 of 255 (linear-light rule and encoded rule) |
| flat layers, 200 over 100 at alpha 64 | 125.10 encoded (linear light 134.5) | 125.10; 134.52; opaque without the alpha flag 200; premultiplied 134.90 |
| focus placed by its field of view | world rendered on the same grid | mean error 0.249 of 255; with the field of view off by one focus pixel 3.018 |
| periphery enlargement | ramp, exact | max error 0.00001 |
| 2X, synthetic at the true half | ratio 0.5 | focus 0.5000 (residual 0.003 px of 12.65), periphery 0.4958 (0.012 px of 2.18); seam synthetic/real 1.007; layer mismatch 1.006; alpha difference 0; cursor fraction 0.5018; HUD sharpness 0.997, HUD position error 0.75 px |
| synthetic at 0.3 | 0.3, FAIL | focus 0.2996, periphery 0.2990, cursor 0.3029: FAIL |
| 3X | 1/3, 2/3 | 0.3327, 0.6677 |
| synthetic = copy of the previous frame | caught | equal share 1.000, ratio 0.0000: FAIL (and listed as same-pixel duplicates) |
| 40x40 alpha hole | 3200 px over two synthetics | 3200, FAIL |
| focus layer 12 % brighter on synthetics | seam FAIL | layer mismatch synthetic/real 4.47 (limit 1.5); the band gradient alone reads 1.014 and does not catch it |
| HUD blurred, sigma 2 px | FAIL | sharpness ratio 0.119 |
| cursor left at the previous place | FAIL, world unaffected | cursor fraction 0.0056, world 0.5000 |
| one repeat + one new image with the same pixels | both found | cadence `RSRrSRRSRSR`, both listed |
| gaze jump of 26 px, true fill | ratio 0.5 across the moved field of view, uncovered zone passes | 0.5000; black share 0.0000; uncovered / interior mismatch 0.88 |
| the same with a black uncovered strip | FAIL | black share 1.000, mismatch 28.9 x |
| composed twin (one layer, periphery enlarged 3x), `--band-from` | ratio 0.5, seam judged, alpha not judged | 0.4997; seam 1.000; NOT JUDGED |
| flight-log join | same classes as the swapchain order | 9 of 9 matched, agreement 1.0 |
| nothing to tell real from synthetic by | NOT JUDGED | NOT JUDGED, with the reason |

One finding of the self-test worth keeping: a fully whitened phase correlation read 0.41 instead of 0.5 on the
enlarged twin (it locks onto the enlargement's pixel grid) and 0.47-0.48 on the periphery; the measure now uses the
low frequencies only and whitens only where there is energy (`WHITEN_FLOOR` in `verdict.py`).
The self-test's scene is a smooth texture with rigid motion: real DCS frames (parallax, occlusion, transparency,
the 20 Hz HUD) have not been through these measures.

**Runner**: `run-scenario.ps1 -DryRun` on the five scenarios (parsing, plan, checks, disk estimate). Nothing else.

Never run: the capture layer under the real loader, in DCS's process, on the Pimax runtime, on a hardware-synchronised
queue under load, with OFXR's real pacing, with the user's layer chain above it (perception layer, Cheeky); the whole
of `run-scenario.ps1` beyond `-DryRun` (profile apply, copy, launch, FLY click, schedule actions, mouse and gaze
scripts, window move, collection, restore); `debug_simulate_tracking` on this build; every threshold on real content;
the swapchain-order rule for real against synthetic on the headset; the capture layer under the chain probe
(`probe_run --quad --layer <QV> --layer <OFXR> --layer rig\capture-layer\bin\XR_APILAYER_RIG_capture.json` with
`RIGTAP_DIR` and `RIGTAP_FRAMES` set: needs the headset).

## 8. First live trial

```powershell
pwsh -File artifacts\foveated\rig\build-capture-layer.ps1                       # if bin\ is missing
pwsh -File artifacts\foveated\rig\run-scenario.ps1 -Scenario hud -Both -DryRun  # read the plan
pwsh -File artifacts\foveated\rig\run-scenario.ps1 -Scenario hud -Both -Prep    # about 8 minutes, 3.1 GB
py artifacts\foveated\rig\compose.py artifacts\foveated\rig\runs\<run>_on\tap\cap00_hud --n 0 --layers
```

Close DCS Control first. Read in this order: `runs\steps.log` (capture layer loaded with a D3D12 session, FLY
confirmed, capture complete, restore clean, state same), `tap\rigtap.log`, `tap\cap00_hud\verdict.txt` (first line:
did the swapchain order agree with the flight log), then the composed picture (is the HUD inside the focus rectangle,
is the image whole: this answers "complete at xrEndFrame"), then set the HUD crop in `scenarios\hud.json`.

## 9. After the first flights (2026-11-15, 17:17 hud ON + twin, 17:28 gaze ON)

What the flights settled: the layer loads last in the real chain (5 layers, D3D12 session), the pictures are whole at
`xrEndFrame`, the textures are TYPELESS (27) for every swapchain, FLY / schedule / collection / restore / state
hashes work, 40 full submissions of the two layers = 1497 MB with 9.5 ms inside `xrEndFrame` each (under DCS).
What they broke, and what was changed (tools only):

### 9.1 Size-aware capture (the twin overflowed: 5424x5356 per eye = 232 MB per submission)

New `arm.txt` keys (the old ones stay):

| Key | Meaning |
|---|---|
| `budget_mb` (default 2048) | size of the whole capture. On its first submission the layer adds up what one submission will put on disk and raises the first stream's `scale` until `frames x bytes` fits. Logged, and `autoScale` in that submission's line |
| `window0`, `window1` | tangents `l,r,u,d` kept of view 0 / 1 of every projection layer, instead of `crop`: the same angular window whatever the image size (clamped to the view) |
| `scale2` (+ `crop2`) | a second file per view (`s<n>_L<l>_v<v>b.rgba`; fields `file2`, `w2`, `h2`, `cropOffset2`, `scale2`): the whole sub-image at that scale |

**A scale is now done on the GPU** (a pixel shader that sums the s x s texels as integers and rounds as the CPU path
does) whenever the texture takes a raw UNORM shader view (TYPELESS or UNORM: every Pimax swapchain flown), so only
the small image is read back: the copy, the memory and the time inside `xrEndFrame` all shrink with the scale. A
fully typed sRGB texture still goes the old way (full read-back, average on the writer thread), which is what filled
the 3 GB queue in the twin flight.
Tested (harness, WARP and this PC's card at the live sizes): 5 captures, 680 files, 996,621,896 bytes, all equal to
the exact box average, 304 files scaled on the GPU and 304 on the writer thread; the window geometry to the pixel;
the budget (20 submissions asked into 16 MB: scale 1 -> 2). Inside `xrEndFrame` on the card: full two layers 6.5 ms,
scale 4 of the same 2.0 ms, window + second stream 7.0 ms. **Not flown.**

The runner sets it per flight (`Get-TwinPlan`): the option-ON flight captures both layers whole with the 2 GB budget
(1.5 GB flown). For the twin's capture of the same tag it reads the ON capture and asks for
`window0/1` = the ON focus layer's field of view + 15 % on each side at full resolution, and `scale2` = composed
width / ON periphery width (5 here) for the whole image: 35.3 + 9.3 MB per submission, 1.78 GB for 40. Without an ON
capture of that scenario, arm and tag (twin flown alone) it captures the whole image and the budget picks the scale
(4 at 5424x5356). A scenario's own `crop` / `scale` is kept as written.

### 9.2 Like with like (`verdict.py --like <the ON capture>`; the runner passes it for the twin)

The twin's composed image is resampled onto the ON capture's grids before anything is measured: "periphery" = the
ON periphery layer's field of view at its pixel size (from the whole-image stream, low-passed when that reduces it),
"focus" = the ON focus layer's field of view at its pixel size (from the full-resolution window stream). So
`interpPeriphery`, `interpFocus` and every crop are taken over the same tangent window at the same pixels per
tangent in both arms; the seam uses the ON capture's band on the same composed grid (as before); alpha stays NOT
JUDGED. Limit: the reference grids come from the ON capture's first two-layer submission, valid for a still gaze.
Self-test `twin in two streams`: periphery +0.002, focus window +0.000 off 0.5 (the ON capture: -0.004, +0.000).

### 9.3 The interpolation measure (the 0.40 / 0.455 of the hud run was the estimator)

Evidence, from the flown captures: (1) with the same pictures shifted rigidly the estimator reads 0.51; (2) the
per-triple medians came from 2-5 regions each with real-to-real motion of 1-2 periphery pixels and a spread of
0.1-0.96; (3) regions of flat fuselage gave a correlation peak anywhere and so a ratio of 0 or 1 (18-31 % of regions
below 0.15, 20-30 % above 0.85); (4) with regions of quality >= 0.10 only, the twin reads **0.51 (p10 0.47, p90
0.55, 34 regions)** on the whole image and 0.49 on the focus window. Timing was checked too: inside the capture
window the display steps alternated 11.1 / 22.2 ms (ON) and were 44-78 ms (twin, the 232 MB copies), and the
application's own frame steps were uneven (15 of 45 off the 22.2 ms median), but the ratio does not follow the
display fraction (gaze run: 0.61 / 0.65 / 0.60 for triples shown at 1/3, 1/2, 2/3).
Changed: 8x8 regions; a region needs quality >= 0.10 and >= 1 px of motion; the verdict is on the **pooled** regions
of the capture (median of ratio minus cadence fraction; fewer than 8 regions = NOT JUDGED); the table also gives the
numbers split by "field of view still / moved" and by motion, the estimator's own reading on the same content
shifted rigidly, the display fraction of the triples and the application's frame steps from the flight log
(`op=app_end_frame`); every region is in `verdict.json` (`regionList`). Motion is measured in view space: a frame
whose focus box moved or changed size is resampled onto the previous real frame's tangent grid first.
Remaining limit: the periphery layer moves 0.5-2 of its pixels per frame in these scenes, so its ratio stays noisy
(p10-p90 about 0.6 wide); the twin is the reference, not 0.5.

### 9.4 Real against synthetic without the flight log

The creation-order rule of section 2 was wrong for the flown session (Quad Views makes its swapchains more than
once; with the Smoothness buffer the ring is current, current, synthetic, synthetic): 75 % agreement in the gaze
run. Now: OFXR's private swapchains ask for another usage (49 against 33; 17 against 1 in its test), they are taken
by usage and split into rings of four or three. On the four flown captures: 90/90, 90/90, 40/40, 14/14 (the twin:
one ring per eye). The flight log remains the reference and was right every time.

### 9.5 Eye not in the cockpit: recenter, the HUD test, the HUD locator

- Tracked headset on its stand = eye under the aircraft (`runs\hud_2x_1115-1717_*\cockpit.png`); untracked = identity
  pose = eye in the cockpit. Both are handled the same way: recenter, then test.
- **DCS has no default key for "recenter VR Headset"** (`iHeadTrackerPosReset`, listed without combos in the
  installed `Config\Input\UiLayer\keyboard\default.lua`; `Num5` is "View Center", the 2D view). The binding made in DCS
  is read (read only) from `Saved Games\DCS\Config\Input\UiLayer\keyboard\Keyboard.diff.lua` (on the test machine a
  lock key). New schedule action `{"at": 8, "do": "recenter"}`: DCS to the foreground, the bound key
  with its scan code, twice for a lock key so Scroll Lock ends as it was (the second press recenters to the same
  pose). `"key"` / `"mods"` in the action override the lookup. No binding found = the action fails with that text.
  Not flown: whether DCS takes the synthetic key event, and whether recenter levels the pitch of the stand.
- `{"do": "shot", "name": "cockpit", "require": "hud"}`: the picture of DCS's window goes through `hudfind.py`
  (saturated green thin strokes: G > 140 and > 1.8 x R and B, what a k x k opening removes, specks dropped, a minimum
  count); none = the flight stops there with **"HUD not in view"** and the usual restore. `hud.json` now does
  recenter at +8 s, this test at +13 s, the capture at +30 s; the other scenarios recenter and take the picture
  without the test.
- `--crop hud:focus:auto` (and `:periphery:auto`): the same locator on the first real submission of that layer gives
  the crop box in tangents; nothing found = that crop NOT JUDGED "HUD not found". The guessed crop of the first
  flight judged a piece of fuselage: its 0.73 / 3.48 mean nothing.
- Locator tested on a drawn HUD (box within 0.03 tangents) and, as negatives, on the flown pictures and captures
  (0 green pixels each). **Never on a captured real HUD**: the thresholds are guesses until one is flown.

### 9.6 Also changed

`alpha.uncoveredAgreesWithPeriphery` divides by max(interior, 2.0 of 255) (see 10); crops and cursor positions are
compared in view space (a moved or resized box cancels out); `gaze_sheets.py` (contact sheets of the gaze jumps).

### 9.7 Commands

```powershell
pwsh -File artifacts\foveated\rig\run-scenario.ps1 -Scenario hud    -Both -Prep            # recenter, HUD test, ON then twin
pwsh -File artifacts\foveated\rig\run-scenario.ps1 -Scenario motion -Both -Prep -Arm 2x    # or 3x, auto
pwsh -File artifacts\foveated\rig\run-scenario.ps1 -Scenario cursor -Both -Prep -Arm 2x    # or 3x
py artifacts\foveated\rig\verdict.py <twin cap> --flight-log <log> --like <ON cap> --crop hud:focus:auto   # by hand
py artifacts\foveated\rig\gaze_sheets.py <gaze cap> --flight-log <log>
```

`-Both` matters for the twin: its capture plan and its `--like` come from the ON flight of the same command (or the
newest ON run of that scenario and arm).

### 9.8 Flying another OFXR build, and the cursor scenario in three stretches (written 2026-11-15 evening, not flown)

`run-scenario.ps1 -OfxrDll <file> [-TwinOfxrDll <file>]`: after the installed CLI has applied the profile and the
folder has been copied, `rig\runs\<run>\live\ofxr\XR_APILAYER_XRFrameBridge_diagnostic.dll` of the option-ON flight
(the twin's with `-TwinOfxrDll`) is replaced by the given file. `steps.log` and `run.json` carry both SHA-256
(`ofxrInstalledSha256`, `ofxrFlownSha256`, `ofxrFlownFrom`); the copy is checked against the file, the applied folder
against its hash from before. The installed app is not touched. Without the switch the installed DLL flies.

`cursor.json`: the mouse now moves in three stretches (action `mouse-zone`): **inside** the focus image (FLY+30-40 s,
a circle of 0.4 of the box), in the **periphery** (42-52 s, the same circle 2.3 half-boxes to the side), and to and
fro across the box's right edge (**border**, 54-66 s). Each stretch's start and end go to `run.json`. Where the box
is in DCS's window comes from the newest fixed-gaze ON capture (left eye: centre 0.58, 0.50, half size 0.15 of the
window) and **assumes the window shows the left eye's whole image: not verified**. Captures: the centre of both
layers during "inside"; a tangent window on the box's right edge during "border".

`cursor_verdict.py <run> [--on <ON run>]` (the runner calls it for this scenario): the flight log's
`op=smooth_cursor` per second with the stretch that second belongs to (log time = the log file's start second + ms,
good to about a second): `result=1` (judged, drawn, views disagree, not found, low score, idle) and `result=6` of
the OFXR build that writes it (per layer: judged, accepted; on the focus swapchain the views drawn from its own
detection, from the periphery's record inside the focus image, cut by the border, outside the focus image), totals
per stretch with accepted / judged, and for the twin (`result=1` only) drawn / judged per stretch beside the ON
run's. The focus columns of `result=6` are also the check of the stretch names: "inside" must show own or inside,
"periphery" outside, "border" cut. Tested on a made-up log (decoding) and on the flown twin's log (`result=1`).

```powershell
pwsh -File artifacts\foveated\rig\run-scenario.ps1 -Scenario cursor -Both -Prep -Arm 2x `
  -OfxrDll artifacts\native\ofxr-djules75\XR_APILAYER_XRFrameBridge_diagnostic.dll
```

### 9.9 Start, pose, cockpit (flown by this session through the runner, 18:36 cursor ON + twin, 18:45 hud ON + twin)

- **"FLY not confirmed" was the foreground, not the pose.** In the failed flight every click was sent while DCS was
  not the foreground window (`DCS in the foreground: False` six times); the briefing ignores clicks then. The click
  that starts the mission is the same in both poses, **892,895 of 1076x1060**: the mouse drives DCS's interface
  cursor in interface coordinates, not the place where the picture shows the button (cockpit pose: button drawn at
  929,700, started by 892,895 in six flights). The runner now clicks 892,895 first, brings DCS to the foreground by
  stronger means in turn (Alt tap, attached input, switch, minimise / restore, a click on the title bar), logs the
  means or what holds the foreground, and falls back to Pause/Break with its scan code. Both flights of both runs
  started at the first click. The stronger means and the Pause key were not needed, so they are still unflown.
  Seen as foreground on this desktop at other times: Tobii's `PeekerWindow`.
- **Pose.** Untracked headset (`slam warning flags: 1`) = eye in the cockpit, FLY button visible in the briefing
  picture; tracked on the stand (`flags: 0`, the 17:17 hud run) = eye under the aircraft, no button in the picture.
  The runner writes `poseAtBriefing` (from `hudfind.py --fly` on `briefing.png`) and the tracking line.
- **Cockpit.** `requireCockpit: true` (cursor) or `require: "hud"` on the shot (hud): recenter at +8 s, picture at
  +13 s, HUD locator; no HUD = "HUD not in view" and the flight ends. Found in all four flights (517-522 px). The
  locator's colour rule was reset on the real HUD (G > 130 and 60 above R and B). **The recenter key itself is still
  unproven**: all four flights were already in the cockpit pose, so nothing shows whether DCS took the key.

### 9.10 Cursor scenario with the cursor in the captures (flown 19:12 calibration, 19:20 ON + twin)

- `cursor-calib` (ON only) + `cursor_calib.py`: the mouse at nine window places, both layers whole; the cursor found
  there is a **yellow cross about 48 px in the focus image**, not a white dot. Measured: cyclopean tangent of the
  cursor = 2.672 x (window fraction x - 0.5), -2.632 x (fraction y - 0.5) (`rig\cursor-calibration.json`, written by
  hand from four points: the fit needs three in the left eye's focus image and only two had it); on near surfaces the
  eyes see it +-0.08 apart. The 9.8 assumption (window = left eye's image) was wrong; `mouse-zone` is no longer used.
- Actions `cursor-stretch` (a path in tangents: slow approach or a marked jump, a capture window around the whole
  path in both layers and both arms at full resolution, 56 submissions) and `mouse-path`; every mouse step goes to
  `<run>\mouse.csv` with the capture's QPC clock. `cursor.json`: a-panel, b-hud, c-periphery, d-border, e-jump.
- `make_cursor_fixtures.py <ON run> <twin run> <out>`: 25 consecutive real frames per stretch, mode, layer and eye
  as PNG + `frames.json`. Result: `artifacts\foveated\port\cursor-fixtures\` (238 MB, README there).
- Flown result: two layers judged 448-450 per layer and stretch, accepted 0, drawn 0 in every stretch; the composed
  twin drew 2 / 0 / 0 / 4 and, after the deliberate jump, 433 of 855.

## 10. Findings of the gaze run `runs\gaze_fill2_1115-1728_on` (analysis only, no flight)

Outputs: `runs\gaze_fill2_1115-1728_on\analysis\cap00_jump8|cap01_jump15\verdict.txt|json`, `...\sheets\*.png` +
`jumps.json`, `analysis\match.json`. Native focus pixels unless "stored"; eye 0.

**CORRECTION (2026-11-15 18:45, after the motion and cursor flights): 10.1's conclusion does not hold. The
interpolation ratio cannot assign a defect when the capture itself disturbs the cadence.** With captures that are
shown evenly (the cursor run `cursor_2x_1115-1836`: centre crop / tangent window, 0-1 of 29 triples shown unevenly,
application frame steps even) the option-ON focus layer reads **0.46 (135 regions) and 0.50 (119 regions)** and the
twin 0.50 / 0.48. In the motion run (whole layers, 12-17 of 19 triples shown unevenly) both arms were off: ON focus
0.61-0.62, twin 0.56 and 0.72. So the 0.65 below belongs to capture windows whose submissions the capture delayed
(27 of 44 triples in the gaze run), in either mode: when OFXR's presenter is delayed, the real frame "before" a
synthetic is no longer the frame it was made from. Rule from now on: read `interp*.ratio` only when the verdict's
timing line says the triples were shown evenly (use `crop` / `window` captures for it); otherwise it is NOT a
measure. 10.1 is kept as written for the record.

**10.1 Focus interpolation: valid estimator, real finding (superseded, see the correction above).** In view space, quality >= 0.10: box still **0.655**
(101 regions) and 0.639 (96); box moved 0.641 (30) and 0.627 (14): the same, so the box is not the cause. By motion:
under 2 px 0.60, 2-5 px 0.70, over 5 px 0.78. The estimator on the same pictures shifted rigidly reads -0.04 / -0.01
off. A second method (the synthetic matched against the previous frame moved forward and the next frame moved back,
in 0.05 steps) gives 0.70 / 0.70 and 0.70 / 0.65 on the 10 + 7 regions with the most motion. Quality >= 0.3: 0.73 and
0.71. Not tied to the display fraction. The hud run agrees: focus layer ON 0.61 (69 regions), the composed twin
**0.51 (p10 0.47, p90 0.55)**. So **in foveated mode the focus layer's synthetic sits about 0.65-0.7 of the way to
the next real frame** (the next frame moved back by about a third of the motion); the periphery layer is near 0.5
within a wide scatter. Cause not established; where to look in OFXR: the focus layer's flow applied too short (it
is the `nvidia_input_scale=50` layer, and with `nvidia_bidirectional=0` a short flow leaves the synthetic near the
frame it is warped from), or a warp factor other than one half for that layer. Caveats: one flight, eye under the
aircraft, half-resolution captures, uneven application frame times inside the capture window (28 of 71): repeat
once with a crop-only capture. The 0.15 guess passes it by a hair (0.143 / 0.136); against the twin it fails.

**10.2 The jumps.** 2 jumps per capture; each lasts 4-5 real frames and the box **grows** x1.07-1.53 while it moves
(saccade widening), so the strips are 469-987 px (10-30 degrees):

| Sheet (under `analysis\`) | Strip px (deg) | Box size | Black | Mismatch strip / interior | Ratio | Strip against the next frame |
|---|---|---|---|---|---|---|
| `cap00_jump8\sheets\jump01_n027.png` | 637 (15.5) left | x1.30 | 0 | 3.45 / 2.43 | 1.42 | 81 % byte-equal |
| `cap00_jump8\sheets\jump01_n031.png` | 528 (10.6) left | x1.20 | 0 | 2.93 / 2.81 | 1.04 | 81 % |
| `cap00_jump8\sheets\jump02_n079.png` | 639 (16.5) right | x1.30 | 0 | 1.60 / 0.90 | 1.77 | 99.9 % |
| `cap00_jump8\sheets\jump02_n081.png` | 469 (10.3) right | x1.07 | 0 | 1.45 / 1.15 | 1.26 | 99.7 % |
| `cap01_jump15\sheets\jump01_n023.png` | 852 (22.8) left | x1.35 | 0 | 5.33 / 4.00 | 1.33 | 85 % |
| `cap01_jump15\sheets\jump01_n025.png` | 819 (16.7) left | x1.16 | 0.002 | 4.58 / 4.30 | 1.06 | 77 % |
| `cap01_jump15\sheets\jump02_n077.png` | 987 (30.1) right | x1.53 | 0 | 2.15 / 0.76 | **2.85** | 99.9 % |
| `cap01_jump15\sheets\jump02_n079.png` | 542 (11.0) right | x0.99 | 0 | 1.84 / 1.61 | 1.14 | 99.7 % |

In every sheet the synthetic has the next real frame's field of view and alpha (difference 0); the strip is as sharp
as the next frame's (1.00) and sits 0.0 px from it: it is the next real frame's pixels, byte-identical on the
right-hand jumps and moved by a fraction of a pixel on the left-hand ones (mean difference 0.06-0.2 of 255). That is
fill = 2 with a periphery flow near zero, which is the case here (0.1-1.6 focus px, low confidence): **this run
cannot tell "moved along the periphery flow" from "copied in place"**; a scene with motion under the strip is needed.
At the old box edge the synthetic differs from the next frame by 0.1-0.6 of 255 on the covered side, 0.00 in the
strip: nothing to see. **The 2.85 is the threshold**: the widest strip, an absolute mismatch (2.15) lower than six
of the other seven, divided by a flat interior (0.76). With the floor now in the check the eight read 0.73-1.42.
Worst sheet: `runs\gaze_fill2_1115-1728_on\analysis\cap01_jump15\sheets\jump02_n077.png`: the strip is clean; what
it does show is the **cockpit cursor drawn twice** on the synthetic (old place dim, new place full). The mouse is
the gaze in this scenario and `smooth_cursor=0` was set for it: a teleporting cursor through plain interpolation,
not a strip defect and not something a real eye tracker produces.

**10.3 Ring order 75 %**: the tool's rule, fixed (9.4). The flight log was right for all 180 submissions.

**10.4 Logs.** Quad Views, 30 s window with both scripts: 2646 eye frames, **36 saccades**, **464 widened (17.5 %)**,
held 93.9 %, peak 2867 deg/s; lead 55 ms (`dcsvr_saccade_lead_ms=55`, the app's value at fixed 2X), max extend 0.35,
hold 40 ms; the window before: 1 saccade, 8 widened. Commanded +-8 degrees moved the box 12.0 degrees, +-15 moved it
18-22 (step plus lead): sign and scale of `debug_simulate_tracking` confirmed. OFXR: one `foveated_mode result=1`
(2 layers, 2 swapchains, array 2, NVIDIA, keys 12 = fill at 2), no `result=2`. **The flight log has no record for a
per-frame fall-back of the fill** (2 -> 1 while the smooth cursor moves, or to the ramp), so it cannot show one;
`smooth_cursor=0` here, so the documented reason for 2 -> 1 did not exist.
