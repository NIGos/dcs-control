# Far depth

Far depth is a depth-selective disparity warp for the bundled Quad Views compositor. It is anchored at infinity, so the
sky does not move. It adds stereo disparity only in a far band, about 40 m to 20 km. Distant aircraft, ships and
vehicles then separate from the terrain behind them. The cockpit, the near field and the sky do not move.

Depth boost scales the rendered IPD instead. That changes near-field disparity a lot, so the cockpit "breathes", but it
barely changes a far aircraft's disparity: +16 mm of IPD at 300 m adds 0.18 arcmin, which is under a pixel.

Status: built into the bundled Quad Views by `scripts/build-quadviews.ps1` (section 2a), off by default
(`dcsvr_far_depth=1` turns it on, section 4). The maths, the field shader, the composition, the gain controller and the
import of the engine's depth export are tested offline (`tests/depthwarp`), and the composition with Far depth off is
tested to be the one without it, bit for bit, on the DLL's own shader (`tests/quadviews-composition`). Since 0.5.8 it
runs live on the engine's depth export (DcsQvCull's `FarDepthExport`, section 5). By default it also needs the engine's
before/after copies of the symbology passes (export v1.1, section 5, duty 8): HUD and MFD symbology are drawn after the
depth capture without depth, and the engine rule is that the warp must not move them (section 2). Without depth, or
without those copies, Quad Views composes exactly as without Far depth and its status reads "waiting for engine".

## 1. The curve

For range R along the ray and rendered IPD I (pupil shift's rendered IPD, Depth boost included):

- natural disparity: `I / R`
- target disparity: `Dt = (I / Z0) (Z0 / R)^gamma`
- extra disparity: `dd = max(0, Dt - I/R) * (1 - smoothstep(5 km, 20 km, R))`

`dd` is 0 in these cases:

- `R <= Z0`
- inside the 30 m near guard
- the sky (reverse-Z depth <= 1e-7)
- anything not finite
- the cockpit stencil composition (`(stencil >> 3) & 7 == 5`)

Each eye's image turns by `a = min(3', 0.5 g w dd)` along its epipolar line. The left eye's content moves toward the right
eye and the right eye's toward the left. This is crossed disparity, so the content looks nearer. The cyclopean
direction does not change, so HUD aiming does not change either.

The gain g is a blend in [0, 1], never a multiplier above 1. Total disparity `I/R + g dd` is then strictly decreasing in
R, so depth order is kept (test M2). A multiplier above `1/(1 - gamma)` would reverse it.

| Preset | gamma | Z0 | Peak (both eyes, 64 mm) |
|---|---|---|---|
| Subtle | 0.5 | 40 m | 1.38' at 160 m |
| Default | 0.35 | 40 m | 2.03' at 201 m |
| Strong | 0.25 | 35 m | larger, peak nearer |

Default, both eyes: 1.79' at 100 m, 1.98' at 300 m, 1.56' at 1 km, 1.14' at 3 km, 0.57' at 10 km. Per eye at the
focus density (29.5 px/deg) that is 0.39-0.49 px between 100 m and 1 km.

## 2. Where it runs

- **Engine (DcsQvCull).** At the end of each focus view's WaterSeaPass it copies that view's depth+stencil raw, as a
  whole subresource, into its own texture. It then pushes a descriptor (`native/quadviews/dcsvr_depth_export.h`) to
  Quad Views. It never changes DCS's image or bindings.
- **Quad Views, once per frame.** `DepthImport::Validate` (`dcsvr_depth_import.h`) takes both focus views of one engine
  frame, or neither. `DisparityController::BeginFrame` (`dcsvr_depthwarp.h`) decides the gain, and whether a frame
  without depth is bridged by the held fields.
- **Quad Views, per eye.** `FieldMain` (`dcsvr_depthwarp_cs.hlsl`) builds the backward displacement field b. It runs
  after the periphery's contrast pass and before the composition draw. The field is R16G16_FLOAT, in focus uv, at the
  depth viewport's size. Each eye has its own field texture, kept until the next one is built, so a frame without
  depth can reuse it.
- **The jitter centre.** DLSS output is de-jittered, the depth is not: gProj's centre is the unjittered centre plus
  the jitter. Quad Views' focus frustum follows the gaze, so the unjittered centre moves every frame and no average of
  gProj's centre can stand for it (a plain 32-frame mean lags a 10 deg/s pursuit by about 190 depth texels).
  `JitterCentreEstimator` takes the focus FOV Quad Views handed DCS for that frame as the reference and averages only
  the residual (the jitter) over 32 frames. Without a reference it averages gProj's centre but restarts whenever the
  centre leaves the mean by more than 1.25 texels, so it lags by about a texel at most.
- **HUD symbology and canopy glass.** They are drawn after the depth capture, without depth. Without a mask, every
  colour pixel takes the shift of the depth behind it, so a collimated HUD line crossing the horizon would get about
  +-0.28 px over terrain at 3 km and 0 over the sky: a disparity step, the symbology no longer at infinity. The
  engine copies the focus view's colour before and after the passes that draw symbology, both into one texture (export
  v1.1, `PRE_POST_OVERLAY`). `CoverageMain` (`dcsvr_coverage_cs.hlsl`) marks every field texel whose pixels changed
  between the two, dilates the marks by 2 texels and FieldMain pins them, as it does for the cockpit stencil. The
  comparison is in display steps for 8-bit copies (more than 2 sRGB steps) and relative for float copies (more than
  1e-4 + 1 %), because DCS draws the F-16 HUD after DLSS but before the tonemap, into the HDR buffer. Measured live: the
  diff against the submitted swapchain image does not work (CopyToHMDPass converts UNORM to sRGB), hence the pair.
  By default Quad Views treats depth without the copies as no depth (`dcsvr_far_depth_allow_unmasked=1` lifts this,
  for A/B only). Canopy glass is not pinned: the terrain seen through it moves, the canopy frame (stencil) does not.
- **The composition (`ProjectionPS`, `dcsvr_depthwarp_ps.hlsli`).** It subtracts b from the focus sample position and
  `b * J` from the periphery sample position. J is the ratio of the focus field of view to the periphery's. Both
  layers of an eye move alike, so no seam can show in the blend band, and there is no extra resample and no extra
  target. The field is 0 at the focus edge (edge window 0.85-0.97 of the shape metric) and beyond 25 degrees from the
  gaze.
- **Compute fallback (`WarpMain`, mode compute).** It warps the focus image alone with a clamped Catmull-Rom fetch. Its
  field is built 0 wherever the composition's alpha is below 1. It is kept compiled in case the fused route is
  rejected.

`FieldMain` runs in two steps:

1. **Forward shift.** For each texel it takes the nearest depth of a 3x3 footprint, so every nearer object is dilated by
   one texel on both sides. Any cockpit stencil or overlay coverage in the footprint pins the texel. It then computes
   the range, `dd`, the weights and the epipolar turn. The shift is clamped to 0.9 field texel (0.6 in the debug
   exaggeration), which keeps the map monotonic: no holes and no fold-over.
2. **Backward solve.** Fixed-point iterations solve the backward map until a step is under 1/128 texel, at most 16.
   Each step shrinks the error by the field's slope, which reaches the clamp at a silhouette against the sky: there 3
   steps stop up to 0.34 texel short, 16 within 0.17 (0.09 measured). If the solve has not settled, the farther
   candidate wins.
3. **A frame without depth.** The engine skips pushes it is unsure of. Up to 2 such frames reuse each eye's held
   field: the composition reads it through a 3x3 map from this frame's focus NDC to the field's
   (`ReuseHomography`: both eye orientations, both focus frustums), so the head's turn and the gaze's move since are
   followed. A longer gap, a scene cut or a change too large to reproject shows the unwarped image.

The composition is injected at three exact-once anchors of the shipping `ProjectionPS.hlsl`: the cbuffer gets
`float4 dcsvrWarp` and `float4 dcsvrReuse[3]`, `t2` gets the field, and the stereo sample moves after the focus NDC. With
`dcsvrWarp.x == 0`, or with a field of zeros, the composition is bit-identical to the one without Far depth (tested,
section 2a).

## 2a. In the bundled Quad Views

`scripts/build-quadviews.ps1` copies `dcsvr_depthwarp.h`, `dcsvr_depthwarp_cs.hlsl`, `dcsvr_depthwarp_ps.hlsli`,
`dcsvr_depth_import.h` and `dcsvr_depth_export.h` into the layer, adds `FieldMain` (the entry point of
`dcsvr_depthwarp_cs.hlsl`, compiled as `g_dcsvr_depthwarp_cs`) to the project, and patches the sources at anchors that
must match exactly once (the build stops otherwise):

- **`ProjectionPS.hlsl`**: the three anchors above, the same text as the test's `InjectProjectionPS`. The build keeps the
  shader from before as `ProjectionPS.nofardepth.hlsl`; `tests/depthwarp` checks the built shader is exactly its own
  injection into that copy.
- **`layer.cpp`**:
  - the includes and `g_dcsvr_depthwarp_cs`;
  - `ProjectionPSConstants` grows to 96 bytes (`dcsvrWarp`, `dcsvrReuse`; a static_assert ties it to the cbuffer);
  - the settings, in the block that parses `dcsvr_temporal`;
  - at `xrCreateSession`, with Far depth enabled only: the device pushes must come from, and `DepthWanted` = `0b1100`;
  - per projection layer, before its eyes are composited: `dcsvrFarDepthFrame` (below);
  - in the composition: the eye's constants and the field at `t2`, unbound after the draw;
  - the field pass and its constant buffer, created only when enabled;
  - at `xrDestroySession`: `DepthWanted` back to 0, every held texture released, the controller, the jitter estimates
    and the fields reset, the status 0;
  - the exports (section 5) and `DcsVr_QuadViewsDisparityStatus`.

`dcsvrFarDepthFrame`, once per display time (another projection layer of the same frame gets the same answer):

1. The thread submitting frames becomes the render thread: pushes from any other thread are refused (code 6).
2. Each eye's orientation (the pose DCS submitted) and focus frustum (Quad Views' FOV for that display time, the DCS
   quirk's record, as the composition uses).
3. `DepthImport::Validate` (focus aspect from the focus image, coverage required unless `allow_unmasked`) for an opaque
   layer outside the focus debug view; with valid depth, each eye's `JitterCentreEstimator` takes gProj with Quad
   Views' focus frustum as the reference.
4. `ReuseHomography` for both eyes, from the held fields' orientation and frustum to this frame's.
5. `DisparityController::BeginFrame`: the gaze record of the display time (valid, saccade), the tracker, the pupil shift
   layer's event counters when it sends them, the `SceneCuts` counter.
6. A fresh field: both eyes' depth views (cached per texture, dropped 8 frames after their last use, so a released
   generation does not stay alive) and field textures first, then both `FieldMain` dispatches; anything that fails
   stops the warp for both eyes. The inputs: the centre from `jitterPx` when `JITTER_KNOWN`, else the estimate; the
   rendered IPD from `ChooseRenderedIpd` (the pupil shift layer's push for this display time, else the engine's camera
   separation from `viewInv`, else the submitted eye poses); each eye's baseline (the push, else the eyes' separation in
   that eye's frame); the gaze in each eye's frame, or the focus centre without a tracker. A held field: no dispatch,
   the composition reprojects it with `dcsvrReuse`.
7. Logs: one line at the first frame (the settings) and every 30 s the controller's counts and the engine's pushes and
   rejections (`DCSVR Far depth: ...`); a missing field texture is logged once and the frame goes on unwarped.

Off (disabled, an alpha-blended layer, the focus debug view, no valid depth, gain 0): nothing is bound at `t2` and every
Far depth constant is 0. Proof, run by every `build-quadviews.ps1`:

- `tests/quadviews-composition`: the layer's shader (the source, and the DLL's own bytecode read from the generated
  `ProjectionPS.h`) with the constants at 0 and a field of 0.047 uv bound at `t2`, against
  `ProjectionPS.nofardepth.hlsl`: 0 differing pixels in all 8 cases (round, rectangular and rounded-square focus, off
  centre, sharpened, unpremultiplied).
- `tests/depthwarp`: `warp.x = 0` with a non-zero field, the near field, the sky and gain 0 are bit-identical to the
  shader without Far depth.

VRAM: two R16G16_FLOAT fields of the depth viewport's size (5.5 MB each at 1176x1164), only while enabled.

## 3. Measured offline (RTX 5090; `scripts/build-depthwarp-test.ps1`)

The tests use synthetic scenes with an exact ground truth. A fake engine renders reverse-Z, infinite-far D32S8 depth
at 1176x1161 inside a larger texture, with Halton jitter. The colour is anti-aliased and de-jittered at 2028x2003, and
the composition runs through the shipping ProjectionPS with the hook injected.

### Image correctness

- **Near field, sky, gain 0.** Near field (< 40 m, with and without the cockpit stencil), sky and terrain beyond 20 km,
  and gain 0: the field is exactly 0 and the composition is bit-identical.
- **Far aircraft.** 6x3 px aircraft at 300 m, 600 m and 1 km. The left/right offset added between the two eyes'
  composited images is 0.975, 0.864 and 0.766 px. The curve predicts 0.976, 0.869 and 0.769 px; the worst per-eye error
  is 0.004 px. The sign is crossed.
- **Fovea.** On a plane at 1 km the field equals the CPU reference within 0.0006 px. The weight is 1 to 10 degrees, 0.55
  at 17.5 degrees, 0.29 at 20 degrees and 0 from 25 degrees. The field is exactly 0 at the focus edge. The fovea
  follows the gaze, and without a tracker the focus centre is used.
- **Canopy frame (0.8 m, cockpit stencil).**
  - Over terrain at 2 km, the frame's pixels are untouched and its edges move 0 px. The terrain beside it moves
    0.322 px (expected 0.326). The disocclusion is filled from the terrain, over 1 field texel.
  - Against the sky, no pixel changes.
- **Colour without depth.**
  - A cloud at 4 km over terrain at 3 km moves with the terrain (+0.274 / -0.272 px, terrain +-0.281 px). Over the sky
    it does not move.
  - Without coverage, an additive collimated HUD glyph over far terrain moves +0.285 / -0.281 px, and over the sky
    not at all. This is why coverage is required by default.
  - With the engine's coverage the same glyphs move at most 0.0011 px, over terrain and over the sky.
- **Jitter (16 Halton frames).**
  - With the jitter in gProj (Quad Views' FOV plus the mean residual as the centre), without it in gProj, and with
    jitterPx known: the field changes <= 0.0007 px away from edges, the aircraft's position spreads 0.011 px, and the
    frame never changes.
  - The focus frustum following the gaze (10 deg/s pursuit and a 4 degree saccade): the field matches the unjittered
    field of the same frustum within 0.0008 px away from edges. A plain 32-frame mean of gProj's centre misplaces it by
    0.29 px on the same frames (the error before the fix).
  - The field texel grid is the depth texel grid, so a jitter under half a texel never changes the 3x3 footprint. The
    answer to measurement request (i) therefore matters little for the default footprint.
- **A frame without depth.** With the head turned 1 degree and the focus frustum moved 2 degrees since the held field
  was built, the reprojected field moves a 300 m aircraft by 0.481 px (the fresh field: 0.487 px) and leaves the
  pinned canopy frame as the fresh field does. Read without reprojection it moves the aircraft by the terrain's
  0.281 px.
- **Seam.** In the blend band (alpha 0.5-0.84, rectangle and round shape, exaggerated x4), the focus layer and the
  periphery layer move within 0.015-0.033 px of each other.
- **Robustness.** NaN, Inf, negative and > 1 depth stay finite. The 0.9 texel clamp holds at about x40 the curve, and
  the backward map stays monotonic.
- **Formats.** D24S8 is within 0.0007 px of D32S8. R32 without stencil gives the same field. A viewport depth range
  of 0.1-0.9 is undone.
- **Compute fallback.** It keeps 97% of a 2x2 px aircraft's contrast and moves it by 99% of its shift at full
  resolution. Unmoved pixels pass through bit-exact.

### Cost

| Pass | Size | GPU ms per eye |
|---|---|---|
| FieldMain | 1176x1164 | 0.017 (fovea 25 deg), 0.021 (worst case: whole view) |
| FieldMain | 816x804 | 0.010-0.013 |
| Composition with the field fetch, 5424x5356 sRGB8 | | +0.016 over the shipping 0.223 |
| WarpMain (fallback only) | 2028x2004 | 0.070 |

Per frame, both eyes, fused route: about 0.08 ms of GPU. A frame bridged by the held fields skips FieldMain. Render
thread: a constant-buffer update and a dispatch per eye. The test reports these numbers; `-Perf` asserts the limits
(0.06 ms each) and is meant for an idle GPU, so `build-quadviews.ps1` runs the test with `-Quick` and without it.

## 4. Settings (the Quad Views settings file)

Read at start, like every Quad Views setting. Values outside the range are clamped; a word that is not one of the
listed ones is a parsing error in the log, and the setting keeps its value.

| Key | Default | Values |
|---|---|---|
| `dcsvr_far_depth` | 0 | 1 on |
| `dcsvr_far_depth_preset` | default | `subtle`, `default`, `strong` (or 0-2): gamma and Z0 (section 1); `near` (3): the world as if seen with 1.4 times the eye distance from 4-12 m out, the cockpit untouched (the world-IPD curve below) |
| `dcsvr_far_depth_world_ipd` | 0 | 1.2-2.0: the `near` curve's factor (0 or 1: the infinity-anchored curve) |
| `dcsvr_far_depth_toggle` | 0:0 | in-flight A/B switch, `virtual-key:modifiers` (1 Ctrl, 2 Alt, 4 Shift; e.g. 119:6 = Alt+Shift+F8): off shows the unwarped image at once, on brings the full effect back at once; one low beep off, two high on |
| `dcsvr_far_depth_z0_m` | 40 | 5-2000; wins over the preset, whichever line comes first |
| `dcsvr_far_depth_gamma` | 0.35 | 0.05-0.95; wins over the preset, whichever line comes first |
| `dcsvr_far_depth_fade_start_m`, `dcsvr_far_depth_fade_end_m` | 5000, 20000 | 100-100000, 100-200000 |
| `dcsvr_far_depth_max_arcmin` | 3 | 0-10, per eye |
| `dcsvr_far_depth_inner_deg`, `dcsvr_far_depth_outer_deg` | 10, 25 | 0-60, 0-90: the fovea weight |
| `dcsvr_far_depth_saccade_step` | 0.25 | 0-1 per saccade onset |
| `dcsvr_far_depth_drift_per_s` | 0.05 | 0-1 |
| `dcsvr_far_depth_gating` | gated | `gated` (1) or `immediate` (0, live A/B only) |
| `dcsvr_far_depth_hold_frames` | 2 | 0-8 frames without depth bridged by the held fields |
| `dcsvr_far_depth_allow_unmasked` | 0 | 1: depth without the engine's symbology copies is used (A/B only: the HUD moves) |
| `dcsvr_far_depth_coverage` | 1 | 0: ignore the engine's copies (no CoverageMain; with allow_unmasked, A/B only) |
| `dcsvr_far_depth_coverage_sample` | 45 | frames between two diagnostic samples of the coverage share (0: none). Never every frame: a Map on DCS's context stalls the driver |
| `dcsvr_far_depth_coverage_dump` | 0 | 1: diagnostics, writes the left eye's copies and mask as BMPs next to settings.cfg, 450 frames in and 90 frames after each switch back on |
| `dcsvr_far_depth_mode` | fused | `fused` (0); `compute` (1) is accepted but not built into the layer: the fused route is used and the log says so |
| `dcsvr_far_depth_debug` | 0 | 3: exaggerate (sign and seam checks) |
| `dcsvr_far_depth_debug_scale` | 4 | 1-16 |

The app does not write these keys yet (P10).

### How the gain changes

The gain changes only where a change cannot be seen:

- up to 0.25 per saccade onset, from pupil shift's 500 Hz events or else Quad Views' saccade flag;
- all the way on a blink (the gaze lost for 80-300 ms) or on a scene cut;
- at most 0.05/s otherwise, and drift only without a tracker;
- up to 2 frames without depth reuse the held fields, reprojected, at the held gain, so a skipped push does not
  flicker the far disparity;
- a longer gap, a scene cut during it, or a change too large to reproject shows the unwarped image, so the gain is 0
  from then on and climbs again under the rules above once depth is back. A scene cut during the gap (a mission load
  stops the pushes around its cut) is applied on the first frame with depth;
- the gain is what the viewer sees: disabling sets it to 0, and enabling starts from 0.

## 5. Engine export v1 (specification for the DCS Optimization session)

The header is `native/quadviews/dcsvr_depth_export.h`, to be copied verbatim. It holds the `DcsVrDepthView` struct,
the flags, the Quad Views exports and the rejection codes.

### Engine duties

1. Once per frame at the first pass boundary, call `mask = DcsVr_QuadViewsDepthWanted()` (resolved with
   GetModuleHandle + GetProcAddress, as scene_cut.h does). If the export is missing or the mask is 0, do nothing, and
   free the copies after 120 unwanted frames. Phase 1 wants `0b1100` (both focus views).
2. At each wanted view's WaterSeaPass end, record `CopySubresourceRegion(dst = engine texture, 0, 0, 0, 0, src = DCS
   depth, srcSubresource of the view, NULL)`.
   - The source box must be NULL: a source box is illegal for depth-stencil resources.
   - The engine texture has the same typeless format and size, ArraySize 1 and MipLevels 1 (the view's subresource
     is copied into subresource 0, which D3D11 allows between different array sizes), DEFAULT usage,
     BIND_SHADER_RESOURCE, and is single-sample. `arraySlice` is then 0. Quad Views reads it as a Texture2D and
     rejects anything else (code 7).
   - The format is R32G8X24_TYPELESS, R24G8_TYPELESS or R32_TYPELESS. No 16-bit depth: raw reverse-Z D16 with a 0.02 m
     near plane has a single step for all of 0.9-2.6 km and reads 0 (the sky) beyond, the band Far depth is for.
   - The copy must come before the next view overwrites a shared target.
   - Fallback when there is no WaterSeaPass: copy at GBufferLightingPass, the next view or the frame end, with
     `FALLBACK_CAPTURE` set and without `AFTER_WATER`.
3. Fill the descriptor and call `DcsVr_QuadViewsDepth(&d)` right after recording the copy, on the render thread and
   DCS's immediate context.
   - The descriptor holds `proj` (gProj as cPerView holds it at byte 256, this frame), the viewport including
     MinDepth/MaxDepth, `engineFrame`, `generation` and the flags.
   - Set `viewInv` with `VIEWINV_VALID` when available; it gives the left/right check.
   - Set `jitterPx` with `JITTER_KNOWN` only if it is read from NGX.
   - Set `displayTime` only to the displayTime of the xrLocateViews call whose FOV DCS rendered this view with, when
     the engine knows that pairing; otherwise 0. Never the newest GazeOut record: a mismatch is rejected.
   - `generation` is an opaque id: change it on payload load, device change and texture recreation. It need not grow
     (a reloaded payload may restart it); both focus views of a frame must carry the same one.
   - `fovTan` is DcsFov, computed from gProj: it carries the jitter and is not used as the unjittered centre.
4. Keep a ring of 2 slots per view, alternating by engine frame; 1 slot is acceptable under VRAM pressure. The copy
   and Quad Views' read are recorded on the same immediate context in order, so no fence is needed.
5. Never push a view whose identity is uncertain: no learned CopyToHMDPass ordinal, a MatchFov mismatch, a missed arm,
   the size-rank fallback, or MSAA. Not pushing skips the frame for both eyes: Quad Views bridges up to 2 such frames
   with its held fields, so an occasional skip does not show.
6. On uninstall or device change, optionally call `DcsVr_QuadViewsDepthRelease(generation)` (it drops the textures of
   that generation), release only your own references, and change `generation`. Quad Views' references keep the
   textures alive.
7. Save or restore no state: copies touch no bindings.
8. **Symbology copies (export v1.1, `HAS_PRE_OVERLAY` 512 and `PRE_POST_OVERLAY` 1024).** Far depth runs by default
   only with them. Per wanted view, the engine copies the view's colour box (x, y, w, h) before and after the passes
   that draw symbology without depth into one texture of at least 2w x h: before at (0, 0), after at (w, 0). Fields:
   `preOverlay` (the texture, the render target's typeless family or its own format), `preOverlayFormat` (the view
   format the target is drawn with), `preOverlaySize` (w | h << 16). The struct stays 264 bytes, version 1. Quad Views
   compares the halves (CoverageMain), dilates by 2 field texels and pins. Where DCS draws the symbology decides the
   interval: the F-16 HUD with `HUD_MFD_after_DLSS` is drawn after DLSS and before the tonemap, so the engine brackets
   that (DcsQvCull's HUD mode `hdr`). With only `HAS_PRE_OVERLAY`, Quad Views compares against the submitted swapchain
   image instead (works only when CopyToHMDPass copies 1:1).

### Quad Views' checks per frame (tested, M8)

Quad Views requires all of the following, otherwise it skips the warp for both eyes (or bridges the frame with the
held fields) and counts the reason:

- both focus views pushed since the last frame;
- the same `engineFrame`;
- the same `generation`;
- `VALID` and `REVERSE_Z` set;
- a reverse-Z perspective gProj;
- a viewport aspect within 1% of the focus image's, and the same viewport size for both eyes;
- the engine's `displayTime`, if not 0, equal to the frame's;
- the right camera to the right of the left camera;
- overlay coverage on both views, unless `dcsvr_far_depth_allow_unmasked`.

`Validate` runs once per display time; a second projection layer of the same frame gets the same answer.

At push time it rejects with a code:

- 0: null or a short struct, or `HAS_COVERAGE` without a coverage texture;
- 2: a foreign device (a device proxy such as ReShade's is recognised: Quad Views also accepts the device that its own
  resources report);
- 3: the format (depth or coverage);
- 4: MSAA;
- 5: not wanted;
- 6: not the render thread;
- 7: the layout (`arraySlice` not 0, ArraySize or MipLevels not 1, a coverage of another size).

### Budget

Render thread at most 8 us per frame: 2 copies, 2 pushes and 1 Wanted call. GPU at most 40 us: 2 x 11 MB at
1176x1164, or 2 x 5.2 MB at 816x804. VRAM 22 MB per slot.

### Tests asked of the engine owner

- The copied texels (depth and stencil) equal the source.
- Bindings are bit-identical around the copy.
- The ordinal-to-view mapping holds over 1000+ frames.
- Nothing runs when Wanted() returns 0.
- A hot reload while Quad Views holds the texture causes no crash and no debug-layer error.
- The coverage marks the HUD and glass texels of a frame and none of the opaque scene.

### Measurement requests (read-only, the engine owner's tooling)

- **(i)** Is DLSS's jitter in gProj's P20/P21 (32 frames with DLSS on)? Can jitterPx be read from the NGX evaluate
  parameters, and with which sign? `JitterNdc` assumes +x right, +y down.
- **(ii)** What is the real per-view depth size and viewport with DLSS: 1176x1164 focus, or the shared 816x804 target?
- **(iii)** Which pass draws HUD symbology and canopy glass, and does any stencil composition mark them? Far depth
  needs this coverage by default (duty 8).

### Status for the in-headset panel

`uint32_t DcsVr_QuadViewsDisparityStatus()` returns `dcsvr::DisparityStatus()`: bit 0 on (enabled, an opaque layer),
bit 1 warped, bit 2 the engine's depth valid, bit 3 held fields bridged the frame, bits 8-15 the gain x 255, bits 16-23
the last skip reason (`DisparitySkip`: 1 off, 2 depth, 3 gain), bits 24-31 an update counter. The panel shows:

- **on**: bit 0 with bit 2 or 3 (the gain may still be climbing: bits 8-15);
- **waiting for engine**: bit 0 without bits 2 and 3;
- **off**: bit 0 clear (also 0 before the first frame, when disabled, and after the session).

## 6. Not done yet

- **QV integration (P7): done** (section 2a), except the compute fallback (`WarpMain`), which stays in the offline
  test, and the exports of P9.
- **The engine's depth export and symbology copies (duty 8): done** (DcsQvCull, 0.5.8). The copies' interval for the
  F-16 HUD (after DLSS, before the tonemap) is verified offline and in the engine's draw trace; a live check with the
  HUD in the fovea is still open.
- **Pupil shift events and rendered eyes (P9).** The exports `DcsVr_QuadViewsGazeEvent` and
  `DcsVr_QuadViewsRenderedEyes` are not in the layer yet. The layer already reads their stores (`GazeEvents`,
  `SharedRenderedEyes`); until the pupil shift layer fills them it uses Quad Views' saccade flag and gaze-loss blinks,
  the engine's camera separation or the submitted eye poses for the IPD, and the eyes' separation for the baseline.
- **App settings and UI (P10).**
- **Live checks:**
  - HUD and glass behaviour in the F-16, F/A-18 and F-4;
  - visibility (blind A/B);
  - the gated gain with the Pimax tracker;
  - cost in the GPU-bound F-4 scene;
  - interaction with OFXR frame generation.
