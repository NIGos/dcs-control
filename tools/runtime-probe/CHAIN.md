# Runtime probe: chain mode

Tests the whole layer chain **without DCS**. The probe behaves like the game: it loads the OpenXR API layers itself,
chains them in front of the runtime, enables `XR_VARJO_quad_views` and submits **one projection layer with four views**
(periphery left, periphery right, focus left, focus right). The Quad Views layer (and, under it, the frame-generation
layer) do the composition, and the Pimax mirrors show their result.

State: the lattice cases ran on the headset on 2026-11-15 (`artifacts\foveated\chain-analysis.md`). The scene motion
pattern added after that **compiles and has never run on the headset**: only the analysis self-test on synthetic
images (CPU only, see the end). Everything about the layers below comes from reading their sources in
`artifacts\foveated\qv-work` and `ofxr-work`.

The direct mode (`README.md`, `tools\runtime-probe\run-runtime-probe.ps1`) is unchanged.

## How it works

### The loader's part, by hand

`probe_run` still loads the runtime DLL and negotiates with it directly. With `--layer` (repeatable, **first = closest
to the application**) it also does what the OpenXR loader does for API layers:

1. Each layer DLL is loaded and `xrNegotiateLoaderApiLayerInterface(loaderInfo, layerName, request)` is called. The
   layer returns its `xrGetInstanceProcAddr` and its `xrCreateApiLayerInstance`.
2. An `XrApiLayerNextInfo` list is built: entry *i* carries layer *i*'s name and the `xrGetInstanceProcAddr` /
   `xrCreateApiLayerInstance` of what is **below** it: layer *i+1*, or for the last layer the runtime's
   `xrGetInstanceProcAddr` and a small terminator that calls the runtime's `xrCreateInstance`.
3. The instance is created with the first layer's `xrCreateApiLayerInstance(createInfo, XrApiLayerCreateInfo{nextInfo},
   &instance)`. From then on every function comes from the first layer's `xrGetInstanceProcAddr`.

`--layer` takes a manifest (`name` and `library_path` of its `api_layer` object; a relative path is relative to the
manifest) or a DLL (the layer's name is then the file name without `.dll`, which is how both of our layers are named).

Consequences of having no real loader:

- **Layers registered in the registry are not loaded.** Only the `--layer` ones are in the chain: no installed Quad
  Views, no installed frame generation, no OpenXR Toolkit. Nothing is registered or installed either.
- No `enabledApiLayerNames`, no `disable_environment` handling, `loaderInstance` is null and `settings_file_location`
  is empty in `XrApiLayerCreateInfo`. Neither of our layers reads them.
- The frame-generation layer resolves the global functions "from the loader" (`openxr_loader.dll` in the process) when
  a layer above asks for them with a null instance. There is no loader here; Quad Views does not ask that way (it
  creates a throw-away instance instead), so the path is not used.

### Four views

`--quad` (D3D11 only):

- instance extensions `XR_KHR_D3D11_enable` + `XR_VARJO_quad_views`; session begun with
  `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO`;
- four swapchains, one per view, at the sizes `xrEnumerateViewConfigurationViews` recommends for the quad
  configuration (so they follow the Quad Views settings: multipliers and sections);
- every frame `xrLocateViews` for four views; the four views are submitted **with the located poses and FOVs**. The
  focus FOV is the layer's: with no eye tracker it is the fixed section.

The application name stays `dcsvr_runtime_probe`. Quad Views special-cases the name `DCS World`: for DCS it ignores
the focus FOV the game submits and uses the one it handed out in `xrLocateViews` for that display time. For the probe
it uses the FOV the probe submits, which is the located one of the same frame, so the values are the same; the
quirk's own code path (the per-display-time table) is **not** exercised. For the same reason `[app:DCS World]` and
`[exe:DCS]` sections of a settings file do not apply to `probe_run.exe` (the script renames `[exe:DCS]`).

`--quad` with no `--layer` asks the runtime itself for quad views (a runtime that has the extension).

### Cases

Same files as the direct mode (`caseNNN.txt` + `caseNNN.png`, kinds unchanged), plus `chain=quad`, `views=4` and
`focus_drift` in every case. `fovA` = the eye's FOV, `fovB` = **the located focus FOV**, `aw/ah/bw/bh` = the real
swapchain sizes. `run.txt` gets `chain`, `layers`, `layer_paths`, `recommended_quad`.

| Kind | Periphery views | Focus views | Metric |
|---|---|---|---|
| `lattice` | green dots on the tangent lattice, white anchor at tangent (0,0) | **opaque**: red dots on the half-step lattice, and a white dot at (0,0) because the focus covers the periphery's anchor | position of the red dots against the green lattice, in focus pixels |
| `gray` | one grey level | the same level | luminance deviation in the fade band the layer makes |
| `two` | one level | another | linear-light or encoded-value fade |
| `edge`, scale 1 | slanted edge half way between the eye's left border and the focus area | slanted edge a little right of the focus centre | 10-90 % rise of both edges (compare runs: composed against two layers) |
| `motion` | the lattice, moving | the same, same world motion | see below |

Not in chain mode: `sentinel` (the layer owns the alpha), the 2.6x scale model (the layer fixes the periphery FOV), the
rotated-pose reprojection cases, the timing block, array swapchains, D3D12.

### Motion (`--motion`, `--motion-only`)

A frame-generation check that needs no game.

- The whole lattice, periphery and focus alike, moves right by `step / phases` of tangent per **submitted** frame
  (`step` 0.06, `--motion-phases` 8 by default: 0.0075 per frame, about 0.43 degrees). The pattern repeats every
  `phases` frames (the anchor jumps back one step), so the images are made once and one set is copied per frame.
- After `--motion-warmup` frames (300) the undistorted mirror is read `--motion-grabs` times (2) per application
  frame, from **both** of its buffers (`undistorted_mirror_handle_0` and `_1`). Every image that differs from the last
  one read from that buffer is kept, until `--motion-frames` (24) are. They are written after the capture, one
  `motion` case each, with `submitted` = index of the last frame sent before the read, `buf`, `t_ms`, `phases`, `vtan`.
- `probe_analyze`, per kept image: fits the green lattice, places its origin with the mirror geometry taken from the
  still `lattice` case, and gets the content's **phase** in frames. `lag = submitted - phase` is whole for a real
  frame and ends in .5 for a frame interpolated at 2X (.33 / .67 at 3X). Reported per image: phase, lag, type (real,
  half, third, off the trajectory), the distance from the nearest expected place in focus pixels (2X and 3X), the
  **seam** (red dots against the green lattice: mean, maximum, x, y in focus pixels), the lattice fit's residual and
  left eye against right eye.

To compare "composed" against "two layers" under 2X: run both with `-Motion` and the frame-generation layer and read
the two motion tables side by side (trajectory error of the interpolated images, and the seam).

`--motion-only` does the still lattice (needed for the geometry) and the motion capture, nothing else.
Each kept image is 16.6 MB in memory until it is written.

### Motion with the scene pattern (`--motion-pattern scene`)

The lattice is periodic: under frame generation a dot can be matched to its neighbour, so a damaged interpolated
image says little (`artifacts\foveated\chain-analysis.md`, section 3). The scene pattern is the same capture with a
world that **does not repeat** (`scene_pattern.h`). `lattice` stays the default and is unchanged.

**The world** (tangent space, made from one seed, the same for periphery and focus views, each at its own
resolution):

- background: three octaves of value noise, luminance 0.10 to 0.26 (linear), slightly tinted;
- discs: one per 0.045 cell in three out of four cells, radius 0.004 to 0.020, darker or brighter than the
  background, grey, green, yellow or cyan. Background and discs always have G >= max(R, B);
- **markers**: bullseyes on black (disc radius 0.008, black gap to 0.012, white ring to 0.016, black rim to 0.019),
  13 per unit of tangent area, each placed as far as possible from the others (at least 0.15 apart = 9 disc
  diameters; about 0.19 with the default seed). About 60 per eye in the Crystal Super's mirror. The disc has G = 0
  and is **red in the focus views and blue in the periphery views**: nothing else in the image has R or B above G,
  and in the fade band the red part of a marker is the focus layer's rendering and the blue part the periphery's.

**The motion.** 0.0075 of tangent per submitted frame to the right (the lattice's default speed, 15.9 focus px with
the neutral settings); with `--motion-diag` also 0.0045 upwards. A world without a period cannot loop, so the
`--motion-phases` pre-made images (default 32, at most 64) are **swept there and back**: frame *n* shows phase
0, 1, .. 31, 30, .. 1, 0, 1, .. Every pair of consecutive frames is still a pure translation by one step; its sign
changes every 31 frames. The motion cases carry `turn=1` when the image is within one frame of a turning point.

Cases: one `scene` case (`tag=still`, phase 0 held) before the motion, then the `motion` cases as before with
`pattern=scene`, `sweep=pingpong`, `seed`, `phases`, `vtan`, `vtan_y`, `fg`, `markers`, `marker_min_dist`. The still
`lattice` case is still taken first: the mirror's geometry comes from it.

`--motion-seed S` changes the world (default 20261115). `--motion-fg 1|2|3` tells the analysis how many frames the
chain shows per application frame (the script sets it: 1 without `-FrameGen`, 2, or 3 with `-Triple`); without it
the analysis accepts whole, half and third steps alike, as for the lattice.

Video memory: phases x (the four views' pixels x 4 bytes). 32 phases are 3.4 GB with `chain-settings.cfg` (2712x2678
and 2712x2410), 1.3 GB with 1040x1028 + 2028x2004. The probe logs the figure and uses fewer phases (not below 8)
when it is more than 70 % of what DXGI says is free. Making the images takes about 0.25 s per phase.

**What the analysis reports** (`report.md`, two tables per image and one summary; `results.csv` keys in brackets):

- *Fase letta* (`phase`): each marker disc is located (search radius 0.06 of tangent around its place, 0.4 of the
  least distance between markers) and the median along the motion gives the phase, in frames. Every submitted frame
  shows a whole phase, so the fraction is where the image sits between two real frames.
- *Tipo* (`class`: 0 real, 2 half, 3 third, 9 off): the nearest slot the declared frame generation can show; more
  than 0.12 frame from it is off the trajectory. *Errore sulla traiettoria* (`traj_err`) is that distance in focus
  pixels. `lag` is only informative here (each phase is passed twice per round; the smaller lag is given).
- Per region, periphery / fade band / focus interior (`mk_per_*`, `mk_band_*`, `mk_foc_*`): markers found / expected
  (`_n`, `_exp`), median and maximum distance of a marker from its nominal place in focus pixels (`_med`, `_max`).
  Band = superellipse radius 0.55 to 1.05 of the focus FOV. A marker that the interpolation moved, dimmed or lost
  shows here even if the image as a whole is on time.
- *Doppi* (`mk_dup`): extra discs near a marker already found. *Allungamento* (`mk_elong`): the disc's extent
  along the motion over its extent across it; 1.00 clean, 1.08 with a 30 % ghost half a step away.
- *Giunzione* (`seam_n`, `seam_med`, `seam_max`, `seam_dx`, `seam_dy`): for the band markers where the fade shows
  both layers (alpha 0.10 to 0.90), red disc minus blue disc in focus pixels. The analysis finds by itself whether
  the band was blended in linear light (composed) or on encoded values (two layers; `blend` 0 / 1) and undoes the
  fade's alpha per pixel with the model of `common.h`.
- *Qualita* (`q_mae`, `q_psnr` outside the band; `q_mae_per`, `q_mae_foc`, `q_mae_band`, `q_psnr_band`; `q_ref`,
  `q_shift`): the image against the real image nearest in phase, shifted by the measured phase difference
  (Catmull-Rom). Mean absolute error in 8-bit codes and PSNR. Ghosting and doubling show here where no marker sits.
  **Read it against the real rows of the same run** (real against real is the method's floor), not as an absolute.
- Summary table: real, interpolated and off-trajectory images, medians and maxima of the above.

To compare composed against two layers: the "Interpolati" row of each run against its own "Reali" row, then the two
runs side by side (markers lost, band and interior error, seam, quality).

Limits:

- The sweep turns back every `phases - 1` frames. An interpolation between two frames is not affected; anything in
  the layer that uses more than two frames (prediction, a pipeline of several pairs) may be, for the images marked
  `turn`. They are in the tables, not excluded.
- The seam needs the fade model (rounded, exponent 8, smoothing 0.2) and a fixed focus; 8 to 20 markers per image
  qualify. With alpha near 0.1 the weaker disc is noisy: read the median, not the maximum.
- "Expected" markers assume the mirror shows the whole eye FOV inside its 1440 px (12 px off its border, 0.03 of
  tangent off the FOV's). The still case gives the count to hold the moving ones against.
- The quality floor depends on the fraction of a mirror pixel in the shift, so a half step and a whole step have
  different floors; the frame-rate overlay of the frame-generation layer is in every image and adds to the band's
  number in real and generated images alike.
- A mirror pixel is about 3.6 focus pixels: marker positions are centroids over about 80 mirror pixels (0.05 focus
  px on synthetic images; the still case gives the real floor).
- Colour is restricted (no red, blue or magenta outside the markers) and the periphery and focus renderings of a
  marker differ in colour on purpose.

### Analysis changes

`probe_analyze` is the same program; for `chain=quad` cases:

- the variant is named "catena a quattro viste";
- `lattice`: only the red dots inside the part the fade leaves whole (superellipse radius below 0.55) are measured,
  because the focus image is opaque and the layer fades it;
- the verdict of the variant needs position and corona, not the sentinel alpha;
- the edge table says "catena a quattro viste, scala 1"; there is no reference inside one run;
- a new section for the motion cases; the header lists the chain.

The ring metrics model the fade as `common.h` does: rounded, exponent 8, smoothing 0.2. A settings file with another
`focus_view_shape` or `smoothen_focus_view_edges` makes `gray` / `two` read wrong.

## Command lines

From the repo root, PowerShell 7. **The caller owns Pimax**: Pimax running, headset awake on its stand in a lit room,
screen saver off (as `tools\runtime-probe\run-runtime-probe.ps1` arranges for the direct mode). `run-chain-probe.ps1` does not
start, stop or configure Pimax, refuses to run while `DCS.exe` runs or when `pi_server` is not there, and writes into
`artifacts\runtime-probe\<timestamp>-<label>\`. The DLL in `artifacts\foveated\qv-build\bin` does both modes.

```powershell
$qv = 'artifacts\foveated\qv-build\bin'
$fg = 'artifacts\foveated\ofxr-build'
$cfg = 'tools\runtime-probe\chain-settings.cfg'   # or the profile file DCS Control writes

# 1. Quad Views composed, no frame generation
tools\runtime-probe\run-chain-probe.ps1 -QuadViews $qv -Settings $cfg -Label qv-composed -Motion
# 2. Quad Views two layers (dcsvr_two_layers=1), no frame generation
tools\runtime-probe\run-chain-probe.ps1 -QuadViews $qv -Settings $cfg -Label qv-two -TwoLayers -Motion
# 3. Quad Views composed + frame generation (2X)
tools\runtime-probe\run-chain-probe.ps1 -QuadViews $qv -Settings $cfg -Label qv-composed-fg -FrameGen $fg -Motion
# 4. Quad Views two layers + frame generation (2X, foveated_layers=1)
tools\runtime-probe\run-chain-probe.ps1 -QuadViews $qv -Settings $cfg -Label qv-two-fg -FrameGen $fg -TwoLayers -Motion
```

Other switches: `-MotionOnly`, `-MotionPhases`, `-MotionFrames`, `-Quick` (three grey levels), `-Format 28` (UNORM
swapchains instead of sRGB), `-QuadSet key=value,...` (replaces the keys the script appends to the Quad Views
settings), `-FrameGenSet key=value,...` (keys for `[ofxr]`), `-OutRoot`; `-Triple` = `[ofxr] triple_frame_gen=1` (3X) and
`-AsyncArm` = `[ofxr] async_arm=1` (both key names checked in `ofxr-work\src\common\implicit_layer.cpp`; both need
`-FrameGen`); `-MotionPattern scene`, `-MotionDiag`, `-MotionSeed`.

The scene pattern, composed against two layers under 2X, then the same at 3X and with diagonal motion:

```powershell
tools\runtime-probe\run-chain-probe.ps1 -QuadViews $qv -Settings $cfg -Label qv-composed-fg-scene -FrameGen $fg -MotionOnly -MotionPattern scene
tools\runtime-probe\run-chain-probe.ps1 -QuadViews $qv -Settings $cfg -Label qv-two-fg-scene -FrameGen $fg -TwoLayers -MotionOnly -MotionPattern scene
tools\runtime-probe\run-chain-probe.ps1 -QuadViews $qv -Settings $cfg -Label qv-two-fg3-scene -FrameGen $fg -TwoLayers -Triple -MotionOnly -MotionPattern scene
tools\runtime-probe\run-chain-probe.ps1 -QuadViews $qv -Settings $cfg -Label qv-two-fg-diag -FrameGen $fg -TwoLayers -MotionOnly -MotionPattern scene -MotionDiag
# without frame generation: the floor of every number (all images real)
tools\runtime-probe\run-chain-probe.ps1 -QuadViews $qv -Settings $cfg -Label qv-two-scene -TwoLayers -MotionOnly -MotionPattern scene
```

By hand: `probe_run <out> --quad --layer ... --motion-only --motion-pattern scene [--motion-diag] [--motion-phases 32]
[--motion-seed S] --motion-fg 2 --motion-frames 24`. With `-MotionPattern scene` the script passes `--motion-phases`
only when `-MotionPhases` is given. More kept images (`-MotionFrames 48`) give more interpolated ones: the mirror
showed 6 in 24 on 2026-11-15.

What the script does besides calling the two programs:

- **Quad Views settings**: copies `-Settings` to `<out>\quad-settings.cfg`, renames `[exe:DCS...]` headers to
  `[exe:probe_run]`, appends an `[exe:probe_run]` section with `turbo_mode=0`, `force_no_eye_tracking=1`,
  `dcsvr_far_depth=0` (the `-QuadSet` default) and `dcsvr_two_layers=0|1`, and gives the probe process
  `DCSVR_QUAD_SETTINGS=<that file>`. The variable is set on the child process only.
- **Frame generation**: copies the layer DLL and `ofxr_bridge.ini` to `<out>\framegen\`, removes `control_event`,
  sets `enabled=1`, `d3d11_bridge=1`, `foveated_layers=0|1`, `[diagnostics] logging_enabled=1` and the `-FrameGenSet`
  keys in the copy, and loads the copy. The build folder is not written to; the layer's flight log lands beside the
  copy.
- Afterwards copies the Quad Views log (`quad-views.log`) and the newest Pimax server log, runs `probe_analyze`.

The same by hand (what the script runs), for configuration 4:

```powershell
$env:DCSVR_QUAD_SETTINGS = '<out>\quad-settings.cfg'
tools\runtime-probe\bin\probe_run.exe <out> --quad `
  --layer artifacts\foveated\qv-build\bin\openxr-api-layer.json `
  --layer <out>\framegen\XR_APILAYER_XRFrameBridge_diagnostic.dll `
  --motion --motion-phases 8 --motion-frames 24 --format 29
Remove-Item Env:DCSVR_QUAD_SETTINGS
tools\runtime-probe\bin\probe_analyze.exe <out>
```

Without the frame-generation layer: drop the second `--layer`. Composed: `dcsvr_two_layers=0` in the settings file.

Exit codes of `probe_run`: as in `README.md`; in chain mode 2 also means a layer did not load or negotiate, the chain
refused the instance, or there is no quad view configuration (Quad Views missing or bypassed), and 3 also means the
layer never gave a valid focus FOV. `probe_run.log` says which.

## What each layer needs

### Quad Views (`XR_APILAYER_MBUCCHIA_quad_views_foveated`)

- Activates only for an application that enables `XR_VARJO_quad_views` **and** `XR_KHR_D3D11_enable`; otherwise it
  logs "layer will be bypassed" and passes everything through. The probe enables both.
- Settings, read at `xrGetSystem`, in this order (later wins): `settings.cfg` beside the DLL; then the file named by
  the environment variable **`DCSVR_QUAD_SETTINGS`**, or without it `%LOCALAPPDATA%\Quad-Views-Foveated\settings.cfg`.
  Sections: `[exe:<part of the exe name>]`, `[app:<part of the application name>]`, `[<runtime or system name>]`.
- Keys that matter here: `dcsvr_two_layers` (0 composed, 1 two layers; read at start only), `turbo_mode` (keep 0: with
  1 the layer makes up display times), `force_no_eye_tracking=1` (fixed focus: the test images are made for one focus
  FOV; `focus_drift` in the cases shows if it moved), `dcsvr_far_depth=0` (there is no engine to give depth),
  `smoothen_focus_view_edges=0.2` and `focus_view_shape=8` (what the analysis models), `sharpen_focus_view`,
  `dcsvr_periphery_sharpen`, `dcsvr_temporal` (pixel-changing passes: 0 to measure the composition alone; in two-layer
  mode the last two also decide whether the periphery gets a pass and a swapchain of its own),
  `peripheral_multiplier`, `focus_multiplier`, the sections (they set the recommended sizes and the focus FOV).
- Writes its log to `%LOCALAPPDATA%\Quad-Views-Foveated\Quad-Views-Foveated.log`: **the same file an installed Quad
  Views uses**; a probe run replaces its content. Expected lines: `Session is using quad views`, the recommended
  resolutions, and in two-layer mode `DCSVR two layers: on, ...` then `... focus swapchain WxH format 28`.
  `... off for this session, composing as before` = a swapchain was refused and the run measured the composed path.

### Frame generation (`XR_APILAYER_XRFrameBridge_diagnostic`)

- No environment variable is needed. Everything is read from **`ofxr_bridge.ini` beside the DLL** (which is why the
  script stages a copy): `[ofxr] enabled=1`, `d3d11_bridge=1` (the probe is a D3D11 application; the layer adds
  `XR_KHR_D3D12_enable` and bridges), `backend=nvidia`, `foveated_layers=1` for the two-layer shape (missing = 0),
  `triple_frame_gen` for 3X. For the prototype keep `hud_symbology=0`, `smooth_cursor=0`, `cockpit_mask=0` (they are
  0 in the build's ini; nothing shares a cockpit mask here anyway).
- A `control_event` key (written by the tray for a managed install) switches the layer off when the tray's event does
  not exist. The build's ini has none; the script removes it from the copy.
- With `[diagnostics] logging_enabled=1` it writes `ofxr-bridge-flight-<date>-pid<pid>.log` beside the DLL (the staged
  folder). With `foveated_layers=1`: one `op=smooth_cursor result=5` record per swapchain naming the periphery layer.
- It paces the application to the display period times the frames per application frame, so under 2X the probe runs
  at half the refresh rate and one motion step is one real frame.
- Must be **below** Quad Views (second `--layer`), as in DCS Control's chain.

### The probe itself

- Sets `enable_undistorted_mirror` through the PVR client interface for the run and puts it back (as the direct mode
  does; recorded in `run.txt`). It changes nothing else of Pimax.

## Self-test of the analysis (CPU only)

```powershell
tools\runtime-probe\bin\probe_analyze.exe --selftest <new folder>
```

Writes eight synthetic quad-chain "mirror" images of the lattice (a still one, one with the red dots 0.5 focus pixels
off, six moving ones at known places), nine of the scene pattern, and analyses them. The scene readings are checked:
the program prints one line per image and ends with exit code 1 if one is wrong.

Scene images (mirror at 800 px per unit of tangent, 20.9 focus px per frame, `fg=2`), run on 2026-11-15, all `ok`:

| Image | Made as | Read |
|---|---|---|
| `still` | phase 0 | real, 78/78 markers, worst 0.08 px, seam 0.01 (9 markers) |
| `real3`, `real4` | phases 3 and 4 | real, 76/76, worst 0.08 px; quality floor 0.44 codes, 40.9 dB (reference one frame away) |
| `half` | phase 3.5 | half step, 76/76, worst 0.08 px, seam 0.02; 0.26 codes, 44.7 dB |
| `half_seam1.5` | phase 4.5, focus layer 1.5 focus px to the right, blended on encoded values | half step, blend read as encoded, seam +1.49, -0.00 (8 markers); focus interior 1.50 px, band 0.74 median / 1.53 max, periphery 0.04; band 0.54 codes |
| `half_ghost30` | phase 5.5 with 30 % of phase 5.0 | phase 5.35: **off the trajectory** at 2X (3.14 px), elongation 1.079, 1.65 codes, 29.8 dB (6 times the clean half step) |
| `diag_real2`, `diag_half`, `diag_real3` | diagonal motion, phases 2, 2.5, 3 | real, half step, real; 74-76 markers all found, worst 0.08 px, seam 0.01 |

Lattice images: Run on 2026-11-15: seam read +0.47 for +0.5; phases 2.00, 2.50, 3.00,
7.49, 5.33, 6.20 for 2, 2.5, 3, 7.5, 5.33, 6.2; types real, half, real, half, third, off. It proves the arithmetic of
the motion, lattice and scene metrics, nothing about the layers or the mirror.

The direct-mode analysis was checked for regressions on an existing capture (`results.csv` identical). After the
scene pattern was added: `results.csv` and `report.md` of `night-d3d12b` (direct) and `20261115-020720-qv-two-fg`
(lattice motion) are byte-identical to those of the build before it.

## Unverified

Nothing below has run. In the order to look at first:

1. **The hand-made chain against the real layers.** The negotiation and create-info checks were read from both
   sources (Quad Views wants `maxInterfaceVersion` exactly 1 and its own name in `layerName`; it creates and destroys
   a throw-away instance through the chain before the real one). Not executed.
2. **Quad Views with a non-DCS application on the Pimax runtime**: session begin, `xrLocateViews` for four views
   (the focus FOV stays empty while the pose is not valid: the probe then submits nothing and ends with exit 3),
   composition from the probe's `R8G8B8A8_UNORM_SRGB` swapchains.
3. **Frame generation under the probe**: whether it arms for this application (D3D11 bridge with the probe's
   `CopySubresourceRegion` + `Flush` frames, three or four more swapchains in two-layer mode), whether 300 warm-up
   frames are enough, and what it does with a perfectly periodic dot lattice (one period is 8 frames of motion, so
   the true displacement is an eighth of the dot spacing; a flow that locks onto the wrong dot shows as "off the
   trajectory").
4. **The mirror under frame generation.** The undistorted mirror is a ring of two buffers written at about half the
   refresh rate (design notes). At 2X, real and generated frames alternate at the full rate, so the mirror may show
   **only real or only generated** frames; the report says so when no interpolated image was seen. Reading
   `undistorted_mirror_handle_1`, reading a buffer while the compositor writes it (a torn image shows as a large fit
   residual), and whether two reads per frame catch both buffers are all assumptions.
5. **`lag`** depends on the pipeline depth (deep pipeline, display-time correction): its whole part is expected to be
   steady, not a particular number.
6. **The white anchor in the focus image** assumes tangent (0,0) is inside the part of the focus the fade leaves
   whole (true for a centred fixed section). With a fixed offset that moves the focus off the axis the lattice fit can
   fail ("calibrazione fallita").
7. **`edge`**: the rows between 40 % and 60 % of the mirror's height must cross the focus area; thresholds of the edge
   finder were tuned on the scale model.
8. **Two layers in the mirror**: the direct mode showed how Pimax blends two projection layers; that the mirror shows
   the layer's own focus swapchain (UNORM, fade in alpha, unpremultiplied) the same way is expected, not seen.
9. The fade model (exponent 8, smoothing 0.2) against a profile with other values; the 0.55 radius limit for the red
   dots; the "off the trajectory" limit of 0.12 frame.
10. `run-chain-probe.ps1` was parsed and its ini editing tried on a copy in memory; the script itself was not run.
11. **The scene pattern in `probe_run`** (compiled, never run): the sweep index, the still case, the video-memory
    check, and how long the layers take with 3.4 GB of source textures in the probe's process. The pattern's
    rendering and the whole analysis ran on the CPU (self-test); what the mirror does to the markers (its filter when
    it shrinks the focus 3.6 times, the real band blend against the model) is first seen in the still case.
12. What the frame generation does at the sweep's turning points (see the limits of the scene pattern).
