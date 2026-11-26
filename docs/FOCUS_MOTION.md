# Focus-view motion and per-pixel depth (motion contract revision 4)

This is the export contract for items Q1 and Q2 of the motion study (`artifacts/motion-study/PLAN.md`, with the
critic's corrections). It is the input that M1 needs: the depth-ordered, one-sided composition at disocclusions.

- **Q1:** DCS's own motion vectors and class atlas for the two focus views, 2 and 3.
- **Q2:** a per-pixel linear view depth for all four views. The previous frame's depth comes from the slot ring.

The contract is `native/dcsqvcull/src/dcsvr_motion.h` at revision 4, plus one hint flag in `dcsvr_cockpit_depth.h`
(revision 3). The OFXR tree takes byte-identical copies through patch `0127-motion-contract-r4.patch`.
`scripts/check-contracts.ps1` checks that the copies match.

## Status and gates

The engine writer is implemented (`cockpit_mask.h` item 15, offline tested on the fake pass sequence; not flown yet).
OFXR 0128 reads the focus records and the depth plane (below); 0130 reads them for the aircraft cover too. The engine
and OFXR work is split out in the last section.

Engine details a reader may rely on:

- `CockpitMotionFocus=1` makes the set's layout include views 2 and 3 for the rest of the payload (the focus MVs come
  from the same per-view runs). Its switch-off then keeps the larger set, as the held-set rules below require.
- Review MEDIUM-1 (fg-cover): a focus view that is in the layout only for its MVs (not in `CockpitMaskViews`, which
  the app writes as 3) is captured for the motion copy alone: no mask or depth dispatch for it and no mask or depth
  record. The mask and depth blocks publish exactly the configured views, so OFXR 0034's moving-focus-edge band and
  every other reader of the mask see the same image with the key on or off (engine test 8 of the revision 4 block).
- A focus view's unknown DLSS jitter never resets the pair. Its own `jitter_proven` (and `true_motion_view`) gate it;
  views 0 and 1 keep the revision 2 reset rule.
- The app writes the keys only when the profile turns them on (`FgFocusMotionVectors`, `FgPerPixelDepth`, profile
  only, off). OFXR's consumer setting `FrameGenFocusMotion` (NVIDIA frame generation) also writes
  `CockpitMotionFocus=1`, since the consumer has nothing to read without it; it does not turn on the depth plane.
- Integration branch `feat/fmv` (contract + engine + OFXR 0128) is built and tested offline only. The live A/B plan is
  in "Live A/B plan (validation-08)" below.
- Review MEDIUM-4: `FrameGenFocusMotion` turns the engine's focus export on without `[Dev]
  CockpitMotionFocusAssumeJitterFree`, so until gate P1 passes the engine publishes views 2 / 3 unproven, OFXR refuses
  them (`motion_focus_refused`) and the pilot pays the focus copies, the larger atlases and OFXR's fine grids for no
  image change. It is a research key (profile only, off) for that reason; the app should write it only together with
  the proven flag once P1 has passed live.
- `[Perception] CockpitMotionDump=N` writes the focus views' records too ("view 2" / "view 3" lines; they live in
  `Header::focusView[slot]`, not in the `MotionSlot`), and, when the slot publishes the depth plane, the plane as raw
  R16_FLOAT rows (`cockpit_motion_<seq>.r16f`, same size and rects as the MV atlas).
- Diagnosis (dev only, branch `dev/fg-diag`): OFXR patch 0129 dumps, per pixel of chosen pairs, what the MV
  composition decided (path, cell flags, class, scatter candidate, weights, colour-check error), the resampled grid,
  the scatter and the frame's MV / class atlases. Off unless the process has `XRFG_DEV_FG_DIAG=1` and a folder
  (`XRFG_DEV_FG_DIAG_DIR` or the capture layer's `RIGTAP_DIR`); armed by `<folder>\fgdiag.txt` (the rig writes it with
  the capture's own delay). The patch header has the record layout.

Before the work started, the motion study measured two things:

- **M0a:** ghosting does not follow aircraft size in MV texels, and it does not depend on whether an MV texel row
  crosses the fuselage.
- **M0b:** in the 0120 `cell` fixtures, finer MVs (d2, d1) do not shrink the halo. With today's consumer, at headset
  scale, they make the model band worse (5.38 at d10 against 9.67 at d2, day x4).

So **Q1 alone is not a fix**. Its value is as the input for M1, and for the small-aircraft case that the fixtures did
not cover. For that reason:

- Both writer options are off by default (`[Perception] CockpitMotionFocus=0`, `CockpitMotionDepth=0`).
- The OFXR consumer of the focus MVs is off by default too.
- OFXR uses 4 px cells in the focus rect only together with the retuned edge band and scatter, and later M1. Today's
  `dcs_motion.hlsl` edge check is tuned to the 10 px periphery texel.
- Before any live A/B, three things must pass:
  - step 0c: DCS's MVs are right for other aircraft;
  - a small-aircraft fixture (4-8 px aircraft, 2 px light) at d10 vs d2;
  - the jitter-free check on the focus views (below).

## Data flow

```
DCS render thread, one frame (view order: periphery L 0, periphery R 1, focus L 2, focus R 3)
  view v: DepthPass -> GBufferPass (MV target SV_TARGET5, per-view DSV, stencil)
          -> RiversPass / WaterSeaPass                <- sample point S_v (pass end, targets still bound)
             engine CS at S_v: MV.xy -> MV atlas rect v      (R16G16_FLOAT, verbatim)
                               stencil + depth -> class rect v (R8_UINT, as revision 3)
                               depth -> depth plane rect v    (R16_FLOAT, w = near / d, metres)
          -> GBufferLightingPass, TransparentPass, ... -> DLSSPass (DCS, or Cheeky's private focus SR) -> CopyToHMDPass
  xrEndFrame (DCS's own call, before any API layer): commit slot s
          companion headers (motion: focus records, viewMask bits 0-3, flags; depth) before the mask header
          one Signal(fence, maskSeq) on DCS's immediate context
Layers below (Quad Views, OFXR) at the same xrEndFrame:
  OFXR: find_slot(T) in the mask, motion and depth blocks; paired_with_mask; one Wait(fence, seq)
        copy the rects it uses (MV, class, depth) into its own per-frame copy; keep the last copy as "previous"
  OFXR pair (A, B): usable_for_pair(B, A.displayTime); per view: view_usable, true_motion_view, depth_usable
```

## Sample point and timing per view

| View | Render size (2.9.31, 20261125-1851 log) | Sample point | Camera |
|---|---|---|---|
| 0 periphery L | 624 x 616 | its WaterSeaPass end (`after_water`), else its GBufferPass end | world; cockpit texels by the cockpit camera |
| 1 periphery R | 624 x 616 | same | same |
| 2 focus L | 1132 x 1120 (1176 x 1164 at other DLSS modes) | its own WaterSeaPass / GBufferPass end, inside the pass | same |
| 3 focus R | same as 2 | same | same |

- **Inside the pass.** Each view's DSV and MV target are bound only inside that view's passes. Between passes, DCS
  keeps a dummy 816x804 frame buffer bound (`dcs-depth-targets`). So the focus capture must arm inside the focus
  passes, as the mask already does for its focus views (mask and depth companion carry views 2/3 today), and never
  read OM at a pass boundary. The g_gbBegun guard of R69 applies: never AddRef a DSV or RTV from an earlier install.
- **One frame, one pose.** All four views come from the same frame and the same xrLocateViews pose. `slotTime` is per
  slot, so a pair's content fraction is the same for every view.
- **Jitter.** R70 T1 proved that MVs are jitter-free for views 0/1 only. Views 2/3 get `view_flags::jitter_proven` only
  after the same still-frame check (mv = 0 ± 0.001 px across DLSS jitter steps) passes live on the focus views.
  `true_motion_view()` refuses a focus view without it.
- **Three views (0.7).** The rule that the right periphery is unproven while culled (`TvRightMvUnproven`) stays for
  view 1. The focus views are not culled.
- **F10, menus and loading screens.** No MV target, so no slot, as today. The held set stays (R88).

## Not disturbing Cheeky's private focus SR

In two-layer mode, Cheeky keeps the focus views' DLSS on a private D3D12 SR (deferred focus features, never
materialized before the QV proof or with DLSS 5 off; `deferred-focus-no-materialize`). The capture is compatible with
that only under these rules:

- It reads DCS's MV target, stencil and DSV at a point before DLSSPass. It writes only our own atlases. It never writes
  a DCS target.
- It saves and restores every binding it touches: CS shader, SRVs, UAVs and constant buffers, the same as the existing
  mask CS. It leaves OM as DCS set it.
- It makes no NGX call, does not touch Cheeky's transport, and does not change any resource's state or lifetime
  (no extra references held past the pass).
- If the focus views have no MV target this frame (stereo mode, Cheeky routes without one), the focus rects are not
  valid. There is no fallback copy from elsewhere.

## Formats, sizes and VRAM

All three atlases share one size (`atlasW` x `atlasH`) and the same per-view rects (divisor 1, render resolution).

| Plane | Format | Content |
|---|---|---|
| MV | R16G16_FLOAT | DCS's MV of the view in render pixels of that view, B -> A (prev = cur + mv), jitter-free per view flag |
| Class | R8_UINT | stencil composition bits 0-2 and sky bit 7, as revision 3; bit 6 still never set |
| Depth | R16_FLOAT | linear depth along the view axis in metres (w = near / d, reverse-Z, near 0.02, viewport depth range undone); 65504 = sky or beyond (class bit 7); cockpit texels at the cockpit camera's depth |

The depth plane steps are about 1 m at 1-2 km and 2 m at 2-4 km. That is enough to order an aircraft against the
terrain or sky behind it, which is all M1 needs.

**Recommended layout:** periphery 0 above 1 in a column at x = 0, focus 2 and 3 side by side to its right.

At 624x616 and 1132x1120 that gives 2888 x 1232 = 3.56 Mpx. Today's atlas is 1248 x 616 = 0.77 Mpx. MB means
10^6 bytes.

| Configuration | Per slot | 3 slots | Extra vs today |
|---|---|---|---|
| Today (revision 3: MV + class, views 0/1) | 3.8 MB | 11.5 MB | - |
| + focus views (Q1) | 17.8 MB | 53.4 MB | +41.9 MB |
| + depth plane, periphery only | 5.4 MB | 16.1 MB | +4.6 MB |
| + focus views + depth plane (Q1 + Q2) | 24.9 MB | 74.7 MB | +63.2 MB |

The critic's estimate of about 7 MB per slot for the depth plane holds (7.1 MB here).

On the OFXR side, each per-frame copy entry is sized from `atlasW`/`atlasH` today, and an older OFXR would size it the
same way. A revision 4 OFXR should size its copies to the rects it uses. VRAM is near the budget (`ofxr-0009`), so both
options stay off unless the measurement shows a gain.

**GPU cost (estimate, to be measured with the pass timer):** the focus capture moves about 2.5 Mpx x roughly 15 B of
traffic, which is 30-60 us per frame for the writer. The OFXR copy adds roughly the same again. This is within the
PLAN's 15-50 us estimate for the writer only.

## Slots, previous frame and sync

- **Slots, seq and generation.** Unchanged: 3 slots, seq == maskSeq, one fence Signal per commit covering mask, depth,
  motion and the new planes. A D3D12 reader does one `Wait(fence, seq)`.
- **Previous frame.** For a pair (A, B), A is `prev_slot(h, B)`: the slot whose seq is B.prevSeq and whose displayTime
  is B.prevDisplayTime. The engine reuses a slot only after `slot_count - 1` newer frames, so at B's xrEndFrame both A
  and B are intact. There are two ways for a reader to keep A:
  - **Recommended:** keep its own copy, as OFXR already copies each frame's slot at that frame's xrEndFrame. Copy the
    depth rects too, and keep the last entry as "previous". There is no cross-queue hazard and no extra hold.
  - **Or:** hold A in the shared ring with the mask block's `releaseFence` (signal A.seq - 1). The engine then skips
    writing a frame rather than overwrite A. It never waits.
- **Reset.** As before, `slot_flags::reset` on B (cut, scene change, dropped frame, unknown jitter) means "no MVs for
  this pair". M1 must then also skip the depth test, because A's depth belongs to another scene.

## Held set and generation (R85, R88)

- **First switch-on.** Turning on `CockpitMotionFocus` or `CockpitMotionDepth` for the first time is a layout change
  (a companion that the held set lacks), so it makes one new generation.
- **Switch-off.** The larger set is kept, with the same generation and the same handles. The flag clears, the focus
  records and viewMask bits 2/3 are 0, and the plane is not written.
- **Back on.** The same generation again, so nothing new is imported.
- **No other bumps.** Do not add any generation bump beyond these (`r84-r1b-depth-vram`: PDH counts every NT-handle
  open).

## Failure and fallback

| Condition | Writer | Reader (OFXR) |
|---|---|---|
| Option off | revision 3 records (flags clear, tail zero) | today's path: periphery MVs mapped by tangent over the whole image |
| Larger atlas cannot be created (VRAM) | logs once, publishes revision 3 records under the existing set, no new generation loop | today's path |
| Focus view not rendered or not captured this frame | viewMask bit clear, `slot_flags::focus_partial` if the mask slot has the view | that view's region falls back to periphery MVs by tangent (or flow) for this pair |
| Focus jitter not proven | `jitter_proven` clear on 2/3 | `true_motion_view` is false, so periphery MVs or flow in the focus region |
| Depth plane missing in A or B | `view_flags::depth_plane` clear | M1 off for that pair and view (today's composition) |
| `reset`, generation change, late commit, idle | as revision 3 | as revision 3 |
| Fault in the capture | existing fault path, the option goes off for the session | today's path |

Every fallback is per pair and per view. None of them waits on the GPU or the engine.

## How OFXR uses it

- **Focus rect at 4 px cells.** In the stereo image, OFXR finds the focus inset by tangent: the focus view's
  `fovTan`, which matches Quad Views' composition. Inside the inset it resamples the focus MVs at 4 px cells; outside
  it keeps today's 8 px cells from the periphery MVs.
  - Unit change: a focus MV in focus render pixels becomes a tangent delta, (mv.x · (r - l) / w, -mv.y · (u - d) / h),
    and then output pixels through the output's own tangent scale.
  - Seam: a one-cell feather inside the inset edge blends toward the periphery field, so there is no step at the
    border.
  - The class comes from the focus class rect on the same cells.
  - All of this ships default off, together with the retuned edge band and scatter (M0b).
- **Per-pixel depth for M1.** At each disocclusion candidate, M1 takes the farthest depth in 3x3 as the background.
  It then z-tests each endpoint against A's depth (previous copy) and B's depth, and takes the visible endpoint alone.
  It blends only where both endpoints are visible, and uses flow only where there is no G-buffer data. This replaces
  0121's 3-rank class ordering on the 10 px grid.
- **Previous depth.** OFXR's own copy of A (above). If that is missing, it uses no depth test for that pair.

### OFXR consumer: patch 0128 (`[ofxr] dcs_motion_focus`, app setting `FrameGenFocusMotion`, off)

What is built (offline only; no producer writes revision 4 yet, nothing flown):

- **Import.** With the key, the consumer takes views 2 / 3 when the producer publishes them (`view_usable`,
  `true_motion_view`: without `jitter_proven` on the view they are refused and counted) and copies their rects in the
  frame's one MV / class copy. The depth plane is opened with the generation and copied in the same list under the one
  fence wait. A plane shared after the generation was opened reopens that generation once. The copy is still the
  union box at atlas size, not "sized to the rects used": the focus views widen it to the whole atlas.
- **Grid.** A pair whose current frame has focus MVs resamples on a 4 px grid (a target and scatter of its own where
  the image's cell is 8). The periphery outside the inset keeps today's lattice: each fine cell takes the value of
  the coarse cell it lies in, computed by the old code. The composition's interpolation between cells then ramps over
  4 px instead of 8. The sky-occlusion rings keep today's reach in pixels.
- **Focus cells.** These use an edge-aware filter: bilinear weights over the texels of the nearest texel's surface
  only (same class, depth within 3 % + 0.25 m, motion within about one output pixel). The one-surface test follows the
  same rules, and there is a one-cell feather at the inset edge.
- **Fallback.** With the key off, an older producer, unproven focus jitter or F10, nothing above runs. The keys-off
  output is byte-identical to the build before 0128: 144 of 144 compositions of `XRFG_TEST_DCS_MOTION` match
  (`XRFG_TEST_OUTPUT_HASHES`). Within one build, key off with focus data and key on without focus data each equal
  today's output in every fixture scene.
- Review LOW-4: on a pair resampled on the fine grid the periphery outside the inset is not byte for byte today's: its
  cells are duplicated on the 4 px grid, but `dcs_motion_field` interpolates bilinearly on that grid and the scatter
  works per fine cell, so the periphery's field and occlusion differ slightly (every fixture within 0.01). Byte identity
  holds for the pairs that are not fine.
- **Moving focus (review HIGH-1, OFXR 0130).** With eye-tracked Quad Views the focus projection follows the gaze, and
  DCS makes each view's MVs with that view's own previous camera (`gPrevFrameViewProj`), so a focus MV's previous end
  is in the previous frame's focus projection. Not verified live yet (a gaze-sweep still-scene check belongs to gate
  P1). Until then the consumer is safe under both readings: an eye's focus MVs are used only where the pair's previous
  frame has that eye's focus record and its fovTan moved by at most a quarter focus texel; the previous end is mapped
  with the previous frame's tangents. Otherwise that eye's focus region takes the periphery's MVs, as without the key
  (counted: `motion_focus_moved`, OFXR log `cockpit_mask result=12` c bits 16-31). The focus import also checks the
  jitters are finite, as the periphery's (review LOW-3).

Fixtures (`XRFG_TEST_FG_FOCUS_MOTION`, `tests/fg_focus_motion.inc`): periphery MVs d10, cell 8; focus MVs d2 over the
middle half, fine cell 4. The numbers are MAE in sRGB units, today -> focus.

| Scene | 640 x 480: model band (inset) | x4 (2560 x 1920, headset-scale motion): model band (inset) | x4 other regions |
|---|---|---|---|
| orbit day (aircraft still, world sliding) | 2.23 -> 1.79 | 5.39 -> 3.09 | all 0.28 -> 0.28 |
| orbit dark | 1.28 -> 1.02 | 1.93 -> 1.18 | all 0.11 -> 0.11 |
| small aircraft day (12-30 px) | 9.36 -> 10.11 | 12.46 -> 8.58 | ahead 5.14 -> 6.50, trailing 2.91 -> 3.55 |
| small aircraft dark | 2.52 -> 2.85 | 4.00 -> 2.46 | ahead 2.52 -> 4.29, behind 0.74 -> 0.18 |
| flight day (0120 scene) | 8.43 -> 8.11 | 10.64 -> 11.57 | wires 21.44 -> 20.86 |
| flight dark | 1.99 -> 1.98 | 2.90 -> 3.08 | wires 5.41 -> 5.23 |

- Outside the inset, every scene matches today to within 0.01.
- With focus MVs at d1, the 640 x 480 small-aircraft day case goes from 9.36 to 7.67 and orbit day from 2.23 to 1.64.
  Flight day gets worse, 8.43 to 8.96.
- In these scenes the depth plane changes nothing beyond class and motion: focus with depth and focus with class only
  give identical output. A checkerboard depth plane does change the output, so the plane is read. Depth matters only
  where two surfaces of one class move alike, and there the motion is the same anyway. Its real use is M1.
- Reading: in the focus region the change helps the orbited aircraft and the small aircraft at headset scale. It moves
  error into the ahead and trailing bands of small aircraft and slightly worsens the flight scene's model band. This
  matches M0b: finer MVs alone do not fix the halo, and the cross-fade at disocclusions (M1) remains.

Cost (`XRFG_TEST_DCS_MOTION_BENCH` with `XRFG_TEST_DCS_MOTION_BENCH_FOCUS=1`, 5424 x 5356 x 2, focus d2 over the
middle half; 2 runs): the composition takes 1201-1202 us today and 1354-1355 us with focus, about +153 us per pair
for both eyes; the whole pair is +155 us. The consumer's larger copy (the full atlas plus the depth plane, about
25 MB per frame at 2.9.31 sizes) is not in that number and is not measured. VRAM (key on only): each synthesizer gets a fine
grid target and scatter. At 5424 px those are 1356 x 1339 x 6 slices x 8 B plus x 4 slices x 4 B, about 116 MB
together. Each ring entry's copies grow to the full atlas plus the plane. VRAM is already near the budget
(`ofxr-0009`), which is one more reason the key stays off.

## Aircraft cover: patch 0130 (`[ofxr] dcs_motion_cover`, profile `FrameGenCover`, off)

The fix the fg-diag live diagnosis found (`artifacts\motion-study\fg-diag`, run 20261126-0237). DCS's MVs of the AI
aircraft are right (0c: at most 0.17 texel error on model texels), but its class lands on the MV texels' centres for
only 15-46 % of what is drawn of it at 450-1800 m. The rest reads as the sky with the sky's vector; the gather starts
from the pixel's own (sky) cell and stays on that class; the scatter wins on 4-20 %; and the colour check accepts the
sky's vector because a faint aircraft differs from the sky by less than 16/255. The generated frame was about 0.9 of a
cross-fade of the aircraft's two places, though composing with the true motion passes the colour check (0.89-1.00).

- **Lever 1, cover (resample).** A cell whose nearest texel is the sky takes the vector and class of the model texel
  (DCS's model class) among the eight around it nearest the sample point. In the focus view with its depth plane a
  farther surface gives way too, to the nearest model texel by depth. A covered cell is an edge cell (never
  interpolated; flags bit 8), so the scatter carries it as well.
- **Lever 2, the sky gather checked (composition).** After the scatter, `DcsMotionNearCS` sets bit 31 of both outputs'
  scatter keys on a sky cell with a model cell within about 16 output px. A sky gather with that bit whose two samples
  differ at all (encoded, above 2/255), where the two frames do not agree at the very pixel (HUD strokes stay where
  they are), first tries the aircraft's own vector (the current end and rings 1-3 cells out, the fixed-point test of
  `dcs_motion_sky_occlusion`). Where none holds but a surface was seen, the frame whose end DCS's class map calls sky
  is taken alone. It runs before `dcs_motion_sky_occlusion`, which is unchanged.
- **Lever 3, far aircraft.** Under one periphery texel the cover has nothing to spread (M0b below). With
  `dcs_motion_focus` the focus view's MVs (d2 at 2.9.31's sizes) go through the same cover.
- **Key and cost.** A quality key (live in OFXR, kFgQualityKeys bit 16). A pair whose only quality key is the cover
  binds a `_cover` twin of the MV compositions (only this code compiled in: 27-28 registers as the plain composition,
  the `_extras` twin has 34). The app writes the key at the start (an older OFXR would not read it live).
- **Diagnosis records.** 0129's records are 16 bytes (json `recordStride`): the vector each pixel was composed along
  and the cover's bits. `fgdiag_analyse.py` reads both sizes. With `XRFG_DEV_FG_DIAG=1` the twins are built from the
  first pair, so the first dump session has records too (fg-diag cap00 had none).

Fixtures (`XRFG_TEST_FG_COVER`, `tests/fg_cover.inc`, 640 x 480, periphery d10 at the headset's 8 px cell, sRGB view,
the class sampled at texel centres; "d2": focus MVs over the middle half with the depth plane). MAE in sRGB codes of
the true middle frame; xfade = |out - truth| over the aircraft, trail and lead regions against the plain cross-fade
(1 = a cross-fade); within1 = share of aircraft pixels whose value is the composition along the true motion to within
1 px. Off -> cover:

| Scene | obj (aircraft) | model band | trail / lead | halo band (1-6 px) | xfade | within1 | must not regress |
|---|---|---|---|---|---|---|---|
| M0b small (4-8 px, 2 px tall), d10 | 21.97 -> 21.97 | 6.21 -> 6.21 | 3.17 / 14.68 same | 0.15 same | 0.81 same | 0.16 same | lights 17.46 same |
| M0b small, d2 | 15.99 -> 8.45 | 5.28 -> 2.87 | 3.17 -> 2.12 / 14.68 -> 4.83 | 0.15 -> 0.14 | 0.69 -> 0.35 | 0.45 -> 0.76 | lights 14.47 -> 9.06 |
| crossing day 450-1800 m, d10 | 0.98 -> 0.24 | 0.81 -> 0.23 | 3.03 -> 0.71 / 1.61 -> 0.28 | 0.00 | 0.38 -> 0.09 | 0.93 -> 0.98 | all 0.02 -> 0.01 |
| crossing day, d2 | 0.68 -> 0.06 | 0.59 -> 0.06 | 3.02 -> 0.50 / 1.03 -> 0.01 | 0.00 | 0.30 -> 0.04 | 0.96 -> 1.00 | |
| crossing dusk, d10 | 0.29 -> 0.07 | 0.24 -> 0.06 | 1.00 -> 0.19 / 0.53 -> 0.07 | 0.00 | 0.48 -> 0.10 | 1.00 | |
| orbit (F2), d10 | 1.45 -> 0.05 | 1.40 -> 0.61 | | 1.12 -> 1.86 | 6.30 -> 0.98 | 0.80 -> 1.00 | far 0.18 same |
| orbit, d2 | 0.81 -> 0.05 | 0.99 -> 0.26 | | 1.19 -> 0.23 | 3.93 -> 0.98 | 0.89 -> 1.00 | |
| HUD over the sky, d10 | 3.20 -> 7.42 | 3.53 -> 2.30 | 15.56 -> 0.19 / 5.30 -> 0.27 | 0.33 -> 0.34 | 0.36 -> 0.23 | 0.72 -> 0.78 | HUD 2.31 -> 2.16 |
| wires, d10 | 6.18 -> 6.62 | 6.68 -> 2.95 | 17.07 -> 0.46 / 11.55 -> 0.63 | 0.12 -> 0.50 | 0.52 -> 0.22 | 0.89 -> 0.90 | wires 13.09 -> 13.04 |
| smoke, d10 | 9.06 -> 6.82 | 5.80 -> 4.18 | 14.22 -> 14.19 / 11.26 -> 2.01 | 0.56 -> 1.10 | 0.45 -> 0.31 | 0.85 -> 0.90 | smoke 1.51 -> 1.52 |

Per crossing aircraft at d10, xfade off -> cover: 450 m 0.32 -> 0.04, 900 m 0.48 -> 0.00, 1200 m 0.73 -> 0.73 (no
texel centre on it in this placement), 1800 m 0.25 -> 0.35; at d2: 1200 m 0.33 -> 0.00, 1800 m 0.26 -> 0.20.

- The M0b aircraft (2 px tall) never land on a d10 texel centre: lever 1 has nothing to spread, as the diagnosis said
  for 1800 m. At d2 the 6 and 8 px aircraft are fixed (xfade 0.18-0.20, within1 0.92-0.94); the 4 px one is still not
  on a d2 centre.
- The halo band grows where the sky beside the aircraft has texture of its own and moves differently (orbit d10 +0.74,
  smoke +0.54, wires +0.38 codes): the covered texel moves that sky with the aircraft. The strict colour check on
  covered cells lowered the orbit band (1.86 -> 1.54) but made the smoke trail worse than without the key (14.2 ->
  21.2) and was dropped. DCS's sky near an aircraft is smooth; the live A/B judges it (ringErr, rearW).
- The HUD fixture's "obj" (aircraft pixels at the middle instant, HUD strokes over them included) goes up 4.2 while the
  aircraft band, its trail and lead and the HUD region itself go down.
- With `dcs_motion_sky_occlusion` too the numbers are the cover's (the cover runs first). Without MVs the key changes
  nothing (asserted). With every key off the compositions are byte for byte 0129's: 144 / 144 hashes of
  `XRFG_TEST_DCS_MOTION` and every `XRFG_TEST_FG_FOCUS_MOTION` output; the six `XRFG_TEST_DCS_MOTION` compositions that
  differ are the "quality 15" ones, which now run kFgQualityAll = 31 (the cover included): dark occlusion aircraft
  0.44 -> 0.04, leading 3.02 -> 0.36, trailing 0.08 -> 0.23.

Cost (`XRFG_TEST_FG_TRIPLE_BENCH`, 5424 x 5356 x 2, composition median): keys off 2X 1238 us / 3X 2080 us; cover alone
(the `_cover` twin) 1112 / 2101 us; sky occlusion alone (the `_extras` twin) 1535 / 2886 us; both 1551 / 2897 us.
`XRFG_TEST_DCS_MOTION_BENCH` with the sky behind the aircraft (`XRFG_TEST_DCS_MOTION_BENCH_SKY=1`, sRGB view): cover
1196 -> 1257 us (+61 us); cover + sky occlusion 1206 -> 2227 us.

Live (2026-11-26, local-only package from feat/fg-cover fac4f8b, headset untracked: the capture layer's head):

- **Diagnosis flights** (`artifacts\motion-study\fg-diag\runs\20261126-0507`, `scenarios\fgcover.json`; the dev ini
  issue fixed: the scenario needs `developerOptions: true`, else the app's ForPilots drops Developer mode and the
  deployed ini has no `[Dev] IniPath`). Arm cover (periphery MVs) and arm coverfocus (`frameGenFocusMotion`, `[Dev]
  CockpitMotionFocusAssumeJitterFree=1`). obj = the aircraft at the synthetic's instant; within1 = share of its pixels
  composed along a vector within 1 px of the measured motion (now the recorded vector itself); xfade = the captured
  synthetic against a cross-fade (recomposite median):

  | Crossing | off (run 0237) within1 / xfade | cover | cover + focus MVs |
  |---|---|---|---|
  | 600 m (x3) | 0.50 / 0.53 | 0.18 / 0.39 | 0.72 / 0.36 |
  | 1200 m (x1) | no records (twins not built yet) | 0.50 / 0.68 | 0.59 / 0.57 |
  | 1800 m (x2) | 0.12 / 0.94 | 0.47 / 1.32 (obj MAE 0.5 vs 0.3: almost invisible) | 0.03 / 1.31 |
  | 450 m (x4) | 0.18 / 0.92 | not located | not located |

  The 0129 off numbers came from the grid and the scatter (the vector used was not recorded then), so within1 is not
  strictly comparable. The cover's aircraft-vector path (lever 2's fixed-point composition) fired on 0 % of the
  aircraft pixels; only its one-frame rule did (2-15 % of obj / trail / lead). Lever 1 lowered the sky gather on the
  aircraft at 1200 m (0.29) but not at 600 m (0.74).
- **Ghostfree A/B** (`artifacts\validation-08\runs\20261126-0526`, `scenarios\keys-cover.json`: three ABBA groups of
  `dcs_motion_cover` on vs off, every other key off; `results\ghostfree\stats.txt`). ghostW ON - OFF (t CI):
  - day: -0.128 [-0.205, -0.051] BETTER, -0.078 [-0.145, 0.009] SAME, -0.143 [-0.206, -0.080] BETTER (OFF mean
    0.775); leadW -0.11..-0.21, rearW -0.30..-0.44, objErr -0.6..-5.8 (all down); ringErr +0.04..+0.06 (CIs touch 0).
  - night: -0.056, -0.005, -0.017, all SAME (OFF mean 0.994).
  - validation-08's own image verdict (fg_ghost trailShare / ghostIndex): day g1 WORSE (maeSky 0.74 -> 1.52,
    trailShare 0.017 -> 0.034), day g2 NO GAIN, day g3 WORSE (tileMcResidual), night g3 PASS, night g1 / g2 NOT JUDGED.
  - pacing: composition + pack +20..+24 us (fast regime), +79..+98 us (slow), all within the gate; late frames per
    minute FAIL in day g2 (7.1 -> 34.0, with the optical flow itself 1706 -> 2328 us) and night g2 (0 -> 18.7), better
    in day g1, night g1 and g3; 3X share 0 %.
- **Verdict.** Lever 1 + 2: day ghosting clearly lower by the ghostfree rule, night unchanged; the diagnosis targets
  (within1 > 0.8, cross-fade < 0.4) are met only at 600 m; two late-frame FAILs and validation-08's trail / sky
  measures worse in two day groups. NO-GO as a default: the key stays off. Lever 3 (focus MVs through the cover):
  better than the periphery cover at 600 / 1200 m in the diagnosis, worse at 1800 m; not A/B flown, and it needs gate
  P1 (focus jitter) first. Next: why the aircraft-vector path never holds live (the fixed-point test: the class and
  vector at the current end within 1 px) and the late frames of the g2 blocks.

### Aircraft cover, round 2 (2026-11-26, OFXR 0130 e7a1c51)

- **Why lever 2 never fired live** (`analysis\lever2_why.py` on the 16-byte records of run 20261126-0507): of the
  sky gathers with an aircraft within reach, 22 % passed lever 2's gates; their aircraft candidates' fixed points fell
  on a cell DCS calls the sky in 99.97 % of cases (the vector itself was right: 0.07 px median where the class
  matched), and 65 % of those ends were within two cells of the aircraft's cells (85 % within three). DCS's class and MVs
  cover only part of what DLSS draws of a distant aircraft, so the exact class test at the current end failed on drawn
  aircraft. Fix: the fixed point counts within two cells of a cell of the candidate's class and vector, where the two
  frames' samples agree (8/255 encoded); the cover reaches two MV texels. Live the aircraft-vector path now takes
  8-12 % of the trail (was 0 %).
- **Live-condition fixture** (`XRFG_TEST_FG_COVER_LIVE=<dir>`, pairs exported by `export_live_fixture.py` from the
  dumps: the frame's own MV / class atlases, the real frames in the synthesizer's target pixels, the measured
  motion and the colour-located aircraft): 1200 m pairs of the focus layer (1952 px), off -> cover, cross-fade
  0.35-0.62 -> 0.12-0.28, within 1 px 0.58-0.83 -> 0.86-0.97; periphery layer (1076 px), 0.42-0.99 -> 0.23-0.78,
  0.79-0.93 -> 0.92-0.98. The flown round-1 shader on the same pairs: 0.17-0.34. Pairs where the aircraft moves under
  4 px are not meaningful (their cross-fade is the reference).
- **Diagnosis flights round 2** (`runs\20261126-0747`), periphery cover, within1 / cross-fade: 1200 m 0.64 / 0.36
  (round 1 0.50 / 0.68), 600 m 0.93 / 0.42 (0.18 / 0.39), 1800 m 0.19 / 1.00 (an aircraft of about one texel, contrast
  0.1 code: nothing to measure or to cover). Cover + focus MVs: 0.40 / 0.53, 0.33 / 0.40, 0.30 / 0.99: worse than the
  periphery cover.
- **Gate P1** (`analysis\p1_check.py`, engine dumps of a flight with the simulated gaze still, then saccading 8-14
  degrees): with the gaze still DCS's focus MVs of the sky match the periphery's MVs mapped with the CURRENT focus fovTan
  at both ends exactly (0.000 px median and p90): jitter-free. The fovTan itself moves 0.3-1.0 focus texel every frame:
  it is DCS's jittered projection's. When the gaze moves (the inset shifted 28-428 px between frames) DCS's focus MVs
  carry that shift (|MV| 28-374 px on still sky) and match the PREVIOUS frame's fovTan (residual 0.4-1.1 px, the jitter
  difference), not the current one (residual = the shift): `gPrevFrameViewProj` of a focus view is that view's previous
  camera, projection included (review HIGH-1 confirmed). OFXR therefore keeps the current fovTan (exact while the
  gaze holds) and refuses a pair whose fovTan moved more than 1.5 texels (a gaze move); the refused pairs take the
  periphery's MVs. `[Dev] CockpitMotionFocusAssumeJitterFree` could become the default (the jitter check passed).
- **Ghostfree A/B re-flown with the lock held** (`validation-08\runs\20261126-0806`; go-v8.ps1 now takes
  `quiet.lock` itself for the whole round): ghostW ON - OFF by day -0.176 [-0.386, 0.034], +0.004 [-0.198, 0.207],
  -0.033 [-0.235, 0.169], all NOT JUDGEABLE (round 1: -0.128, -0.078, -0.143); at night +0.004 / -0.113 / not
  judgeable. Pooled over the two rounds the day effect is about -0.09: lower, not clearly.
- **Late frames**: round 1 failed in day g2 and night g2, round 2 in night g3 and day g1 (slow regime), while round 2's
  day g2 had fewer with the key (23.2 -> 0.0 fast, 50.3 -> 3.7 slow); the optical flow's own time, which the key does
  not touch, swings with them (1706 -> 2328 us, 7856 us in one off block). Noise of the scene and the GPU, not the key.
  composition + pack: +20..+30 us fast, -63..+116 us slow (one slow group over the 20 % gate: 202 -> 318 us).
- **Night** (the pilot's reference, the F-14B(U) "Night Intercept" instant action, Kola 2001-04-05 01:40 moonlight,
  clean sky; a read-only copy with the fg-diag crossings and the player's autopilot engaged, `missions\
  qvbench_v8_kolanight2.miz`: the unpiloted F-14 of the first copy wandered off course and crashed at about FLY + 61 s,
  so its windows were invalid; with the autopilot every crossing passed in front at the planned time, the ownship alive
  to the end, all four windows valid in both arms). Located from DCS's class atlas, not from colour
  (`analysis\class_ghost.py`), in the swapchain's sRGB codes: the moonlit sky 3.3-5.3 codes, the silhouette 0.3-0.7
  code DARKER than the sky. Cross-fade fraction off -> cover on the same crossings: 1800 m 0.79 -> 0.50, 450 m 0.96 ->
  0.70 (codes; log luminance and local-contrast-normalised give the same within 0.04). The validation-08 night
  captures (I23 base, same date and weather, 4 km): the located "object" is the nav light and its renderer trail; the
  silhouette is not above the sky's noise either. What the swapchain holds of the night silhouette is below 1 code, so
  the headset's own brightness / gamma (Pimax brightness, display) or the pilot's DCS gamma must be what lifts it to the
  5-20/255 the pilot sees; lever 2's 2/255 gates cannot see sub-code differences, lever 1 (class-based) acts anyway.
  Offline, a moonlit fixture ("crossing night": sky 3 codes, silhouette 6-14 codes above it, a 2 px nav light at each
  nose): cross-fade 0.56 -> 0.17, within 1 px 0.90 -> 0.98, lights 14.3 -> 12.3, with the focus MVs 0.49 -> 0.11. A
  silhouette the swapchain holds at 5-20 codes is handled: the cover does not go through the 16/255 colour check (its
  own gates are 2/255 encoded), so no lower or relative check is needed for it.

Verdict, against the targets (within 1 px > 0.8, cross-fade < 0.4, ghostW clearly lower, no pacing regression):
- Lever 1 + 2 (periphery): 600 m meets the image targets, 1200 m meets the cross-fade target (within1 0.64), 1800 m
  cannot (an aircraft of one MV texel, below one code of contrast); ghostW lower by day but not clearly; pacing within
  the gate except one slow-regime group. NO-GO as a default; useful as an option.
- Lever 3 (focus MVs through the cover): worse than the periphery cover in both diagnosis rounds. NO-GO.
- Limit: DCS's class and MVs cover a fraction of a distant aircraft's drawn pixels, and the farthest aircraft are
  smaller than one MV texel; no composition rule can move pixels the G-buffer does not hold.

### Aircraft cover, round 3 (2026-11-26, the night F2 orbit and the slow-group repeat)

- **Night F2 orbit** (the pilot's complaint as a scenario): the Kola Night Intercept autopilot copy, F2 with the camera
  orbiting the player's F-14B(U) at the rig's closed-loop 20 deg/s (`orbit\rig_orbit.lua`, dev/rig-orbit dd65789
  cherry-picked onto the TEST branch `test/fg-cover-rigext` only), `dcs_motion_cover` ABBA x3 live, captures at
  bearings +-90 degrees and 0129 dumps across each capture window. The rig orbits the ownship only: there is no
  orbit around another aircraft. Three flights: `fg-diag\runs\20261126-1052` (dumps finished before the captures:
  no overlap), `20261126-1114` (the capture layer's 6 GB disk budget stopped captures after block 4) and
  `20261126-1151` (budget raised, dumps spread over the window: 18 of 18 captures and 11 of 12 blocks measured, ownship
  alive to the end, all windows valid).
- Located from the class atlas (`analysis\orbit_band.py`: the ownship's model texels in B, a band 2-16 px outside
  the silhouette, the reference along DCS's own MV), periphery layer, off -> cover: band error 0.30 -> 0.28 codes
  (log luminance 0.020 -> 0.021, local-contrast-normalised 0.055 -> 0.062), cross-fade fraction 0.38 -> 0.47 (sums over
  the synthetics; 0.41 -> 0.47 log luminance). The silhouette is 2.0-2.7 codes DARKER than its band (band level
  3.3-3.7 codes): the whole error is under one code in the swapchain. Validation-08's `fg_orbit.py` (colour-located)
  keeps too few captures at night to judge (captures 1 / 4, 3 / 1, 4 / 0 off / on per group: NOT JUDGED).
- Pacing (fast regime only: the orbit holds 2X): comp + pack 211 -> 212 / 213 / 215 us, late frames per minute 9.6 ->
  1.8, 8.2 -> 6.4, 8.5 -> 3.2, all groups PASS. The second flight's g2 had 1.1 -> 19.1 late per minute with the dumps
  and captures written in the same blocks; the third flight writes them in every block and has none.
- **Slow-group repeat** (validation-08 `runs\20261126-1131`, fgday ABBA under the lock): the slow-regime g2 that went
  over the 20 % gate (202 -> 318 us) is 220 -> 248 us now (limit 320, PASS); slow comp + pack 308 -> 249 (g1), 198 ->
  228 (g3); fast 198 -> 226-229 us. The late-frame failure moved again (slow g1 0.0 -> 7.2; g2 14.4 -> 14.1): noise.
  ghostW ON - OFF +0.104 [-0.223, 0.431], -0.050 [-0.323, 0.222], -0.113 [-0.327, 0.102]: NOT JUDGEABLE, as in round
  2; objErr lower in g1 / g2 (-4.1 / -3.7 codes), ringErr +0.06..+0.09 higher in every group.

- **Close F2 orbit by day and dusk** (offline the cover's best case: orbit d10 cross-fade 6.30 -> 0.98, d2 3.93 ->
  0.98; `fg-diaguns61126-1259`, validation-08's orbitday / orbitdusk missions, same 20 deg/s orbit, cover ABBA x3
  live, 0129 dumps across the captures; 18 + 16 captures, every one complete, no crash in either dcs.log). Validation-08's
  `fg_orbit.py` (ORBIT-STATUS.md rules, ratio ON / OFF [95 % CI], BETTER = CI hi < 0.95, WORSE = CI lo > 1.05):

  | Scene | Group | errBand OFF -> ON, ratio | trailing | leading | errFar (control) | Verdict band / trail / lead |
  |---|---|---|---|---|---|---|
  | day | g1 | 13.5 -> 13.9, 1.28 [1.04, 1.50] | 1.25 [0.98, 1.52] | 1.26 [1.09, 1.44] | 1.34 [1.10, 1.62] | SAME / SAME / WORSE |
  | day | g2 | 14.1 -> 11.5, 0.77 [0.70, 0.85] | 0.78 [0.72, 0.86] | 0.77 [0.70, 0.87] | 0.87 [0.68, 1.08] | BETTER / BETTER / BETTER |
  | day | g3 | 14.7 -> 16.0, 1.14 [1.04, 1.24] | 1.16 [1.06, 1.26] | 1.12 [1.03, 1.22] | 0.87 [0.63, 1.14] | SAME / WORSE / SAME |
  | dusk | g1 | 3.6 -> 2.8, 0.73 [0.61, 0.90] | 0.77 [0.66, 0.94] | 0.68 [0.54, 0.89] | 0.87 [0.80, 0.94] | BETTER / BETTER / BETTER |
  | dusk | g2 | 4.3 -> 3.2, 0.72 [0.61, 0.84] | 0.59 | 0.88 | 1.01 | NOT JUDGED (captures 1 / 3) |
  | dusk | g3 | 2.7 -> 3.1, 1.40 [1.21, 1.60] | 1.38 | 1.37 | 0.94 | NOT JUDGED (captures 1 / 3) |

  Class-located band (`orbit_band.py`, periphery, off -> cover): day band error 8.3 -> 10.4 codes, cross-fade
  fraction 0.45 -> 0.68 (silhouette +54 codes over its band); dusk 3.6 -> 4.0 codes, 0.59 -> 0.62. Pacing PASS in every
  group of both flights: comp + pack day fast 280 -> 278 / 286 / 272 us, slow 331 -> 346 and 360 -> 291 us; dusk fast
  218 -> 231, 229 -> 234, 218 -> 219 us; late frames per minute no higher with the cover (day 17.2 -> 15.1, 23.5 -> 1.7,
  8.1 -> 5.4; dusk 8.2 -> 6.8, 14.7 -> 8.4, 10.9 -> 3.2). One group better and one group worse in each scene, and the
  control area away from the aircraft swings as much (day g1 errFar 1.34): the close orbit is NOT clearly better with
  the cover by day or by dusk, so the option's description does not claim it.

Final verdict (targets: within 1 px > 0.8, cross-fade < 0.4, ghostW clearly lower, no pacing regression):
- Lever 1 (spreading the aircraft's MV / class over its edge): offline it meets the targets (fixtures and the
  live-condition pairs); live, 600 m and 1200 m crossings by day improve, but ghostW is not clearly lower in three A/B
  rounds, in the night orbit the cover is not better (cross-fade 0.38 -> 0.47, error under one code either way), and the
  close orbit by day and dusk is mixed (one group BETTER, one SAME / WORSE per scene, the far control moving as much).
  Pacing within the gate (the 318 us group was noise). NO-GO as a default; stays an EXPERIMENTAL option, off.
- Lever 2 (the aircraft vector before a sky gather): fires live since round 2 (8-12 % of the trail); no separate live
  gain beyond lever 1's. NO-GO as a default (part of the same option).
- Lever 3 (focus-view MVs through the cover): worse than the periphery cover in both diagnosis rounds. NO-GO.
- What the night pilot sees: the swapchain holds the orbit silhouette at 2-3 codes against a 3-4 code sky; the grey
  ghost the pilot reports must be lifted by the headset's brightness / gamma. Nothing in the composition can tell
  sub-code differences apart, and DCS's G-buffer does not hold more of a distant aircraft than it draws.

## Live A/B plan (validation-08)

Nothing below has been flown. The harness is `artifacts/validation-08` (`go-v8.ps1`, `scenarios/make_scenarios.py`,
`tools/run-scenario-v8.ps1`); its sessions, metrics and verdict rules are the ones in `ORBIT-STATUS.md` (orbit) and
`ghostfree/stats.txt` (distant aircraft), unchanged. Only the keys and the gates are new.

### What is toggled, and how

`[ofxr] dcs_motion_focus` is **start-only** (read at `xrCreateSession`, 0128: "fixed for the session"), so it cannot be
the live toggle. The live toggle is the engine's export instead. With the OFXR key on, an engine that stops publishing
the focus views (`header_flags::focus_views` clear) gives exactly today's output (0128's fallback tests), so:

| Where | Key | Both arms | Toggled |
|---|---|---|---|
| Draft profile (start) | `frameGen` NVIDIA, `frameGenFocusMotion: true` (-> `[ofxr] dcs_motion_focus=1`), `fgPerPixelDepth: true` | yes | no |
| Draft profile (start) | `engineDevMode: true`, `engineDevIniPath` = the round's engine ini (below), `freeVram: false` | yes | no |
| Engine ini `[Dev]` | `CockpitMotionFocusAssumeJitterFree=1` (only after gate P1 passed) | yes | no |
| Engine ini `[Perception]` | `CockpitMotionFocus`, `CockpitMotionDepth` | launched at 1 / 1 | live, per group |

- **Engine ini.** The round copies the `DcsQvCull.ini` that the package's CLI writes for the draft (so every other
  setting is the pilot's) to `<run>\live\engine\DcsQvCull.ini`, adds the `[Dev]` line, and points `engineDevIniPath`
  at it. The module reloads it on its write time (live mode, R143); `[Perception]` and `[Dev]` keys do not reset the
  engine kill switch (R142).
- **Harness change needed (not done):** `copyini` resolves `file` under `<run>\live`, so the copy above must exist
  before launch, and `make_scenarios.py` must write the groups to `engine\DcsQvCull.ini` section `Perception` instead
  of `ofxr\ofxr_bridge.ini` section `ofxr`. The rest of the schedule (ABBA, toggle >= 26 s before the changed
  crossing / at the orbit block start, toggle + 5 s left out) stays as it is.
- **Launch at 1 / 1.** The first switch-on of either key makes one new generation (held-set rule); launching with both
  on makes that generation at load, so no toggle in the round re-lays out the set or makes OFXR reopen it.
- **Package.** Orbit sessions need the rig extension hook of `dev/rig-orbit` (`DCSC_RIG_EXT`). Build the test package
  from a throwaway merge of `feat/fmv` and `dev/rig-orbit` (`build-release -LocalOnly`), as the 0.7.1 rig-orbit
  package was built; do not merge `dev/rig-orbit` into `feat/fmv`. ghostfree sessions can use the `feat/fmv` package.

`scenarios/keys-fmv.json` (A = `off` in every group, as today's keys.json):

| Group | On | What it isolates |
|---|---|---|
| `off` (A) | `CockpitMotionFocus=0`, `CockpitMotionDepth=0` | today's image (revision 3 records + revision 4 marker) |
| g1 | `CockpitMotionFocus=1`, `CockpitMotionDepth=0` | focus MVs (the 0128 resample, class-only surface test) |
| g2 | `CockpitMotionFocus=0`, `CockpitMotionDepth=1` | depth plane alone: **image must not change**; cost of the plane (engine dispatch + OFXR copy) |
| g3 | `CockpitMotionFocus=1`, `CockpitMotionDepth=1` | the configuration a pilot would get |

### Gates before the A/B (short flights, stop at the first failure)

- **P1 focus jitter (R70 T1 on views 2 / 3).** Still frames with `[Suite] HoldPose`, `CockpitMotionFocus=1`,
  `CockpitMotionDump` on, `motion_check.py` T1 on views 2 / 3. Pass: |mv| <= 0.001 px at p99 on still frames, as views
  0 / 1 in R70. Fail: no A/B (the consumer refuses unproven views; forcing them would test jittered MVs).
- **P2 writer cost.** Engine 30 s log line, focus + plane on: capture GPU time <= 60 us per frame; no "the larger
  motion atlases (...) could not be made; revision 3's motion set is used" line (review LOW-2: the engine's own text);
  `motion contract revision 4` line shows the keys as written. A failed larger set is retried only when a key turns on
  from both off or the layout changes (review LOW-1), so an A/B toggle under VRAM pressure does not rebuild it each
  time.
- **P3 OFXR import.** OFXR log: focus views imported (no `true_motion_view` refusals after P1), no generation reopen
  loop, `dcs_motion_focus=1` in the session's settings line.
- **P4 Cheeky (only if the draft uses Cheeky / DLSS 5).** No "materialized" line, Cheeky log unchanged.

### Sessions

```powershell
$v8 = 'E:\Storage\Code Projects\DCSControl\artifacts\validation-08'
pwsh -NoProfile -File "$v8\go-v8.ps1" -Cli <fmv+rig-orbit package>\files\DcsVr.Cli.exe -Sessions orbitday,orbitdusk -NoRegress
pwsh -NoProfile -File "$v8\go-v8.ps1" -Cli <fmv package>\files\DcsVr.Cli.exe -Sessions fgday,fgnight -NoRegress
```

- **orbitday / orbitdusk** (F2 silhouette band): `qvbench_ff_land.miz` and `missions\qvbench_v8_orbit_dusk.miz`; F2
  orbit at a measured 20 deg/s; blocks of 26 s in group order g2, g1, g3 (ABBA each); captures at bearings +90 / -90.
  Night is left out (every band error < 1 luma code in round 20261125-1851; ratios not meaningful).
- **fgday / fgnight** (distant aircraft, ghostfree): `qvbench_v8_day.miz` / `qvbench_v8_night.miz`, 12 crossings = three
  ABBA groups, captures timed to the AI crossings; analysis `ghostfree` as in `stats.txt` (covariates logRange,
  speedPxS; aircraft-located gate).

### Expected effect (from the 0128 fixtures; the live picture may differ)

| Session | Measure | Expected g1 / g3 | Why |
|---|---|---|---|
| orbit day | errBand, trailing, leading | ratio about 0.6-0.8 | fixture "orbit day" x4: 5.39 -> 3.09 in the inset |
| orbit dusk | errBand, trailing, leading | ratio about 0.6-0.8 | fixture "orbit dark" x4: 1.93 -> 1.18 |
| orbit both | errFar (control far from the aircraft) | ratio 1 | outside the inset every fixture is within 0.01 |
| fgday | objErr | lower (about -30 %) | fixture "small aircraft day" x4 model band 12.46 -> 8.58 |
| fgday | leadW | possibly higher | fixture ahead band 5.14 -> 6.50 |
| fgday / fgnight | ghostW | SAME | M0b: finer MVs do not remove the cross-fade halo (that is M1) |
| fgnight | leadW, ringErr | SAME or slightly worse | fixture "small aircraft dark" ahead 2.52 -> 4.29; lights have no MVs |
| all | g2 (depth only) | identical to A | 0128 reads the plane only for focus cells |

Cost: composition +12.7 % per pair (bench +153 us on 1201 us at 5424 px x 2); the consumer's larger copy (about 25 MB
per frame) and the engine capture (estimate 30-60 us) are not measured yet. VRAM with g3 on: engine +63 MB (3 slots,
Q1 + Q2) and OFXR about +116 MB per synthesizer plus the larger ring copies.

### Pass criteria

The focus MVs may become a default candidate only if **all** of these hold; otherwise they stay off and the round's
numbers go into this file as the input for M1.

1. **orbit day and dusk, g1 and g3:** errBand verdict BETTER (ratio CI hi < 0.95) in at least one of the two scenes and
   not WORSE (CI lo > 1.05) in either; trailing and leading edge not WORSE; errFar CI includes 1.
2. **fgday and fgnight, g1 and g3:** not WORSE by the ghostfree rule (ghostW ON-OFF CI hi < +0.15 and no secondary
   WORSE: leadW, rearW, ringErr CI entirely above 0); objErr not higher (CI hi <= 0) in fgday. NOT JUDGEABLE in a
   scene counts as a re-fly of that scene, not as a pass.
3. **g2 everywhere:** image SAME (orbit ratios' CI include 1; ghostfree ghostW inside +-0.15 and no secondary
   difference). A difference here is a bug, not a result.
4. **Pacing, every group, both frame regimes:** composition + pack per pair <= OFF + max(100 us, 20 %); late frames per
   minute not FAIL by `ofxr_metrics.py`; 3X share not up; no KEY NOT LIVE line (the engine keys must show as reloaded in
   DcsQvCull.log, the OFXR key is start-only by design). Review MEDIUM-3: `ofxr_metrics.py` takes liveness from OFXR's
   own `live_settings` records only, so an engine-file group reads KEY NOT LIVE as it is; for those groups liveness has
   to come from DcsQvCull.log's reload line and its "motion contract revision 4: CockpitMotionFocus=..." line (not
   changed in the harness yet; OFXR-key groups such as 0130's `dcs_motion_cover` are unaffected).
5. **VRAM** (`VramIntervalSec=10`, DXGI): measured against a separate launch with today's shipped configuration (keys
   absent), not against arm A of the same launch (review MEDIUM-2): launched at 1 / 1, arm A already holds the larger
   engine set, OFXR's fine grid and scatter (about 116 MB per synthesizer, made at session start with
   `dcs_motion_focus=1`) and the full-atlas ring copies, so g3 - A is about 0. Budget: shipped + 250 MB; no OFXR
   VRAM-budget skip that the shipped launch does not have.
6. **Restore** as in `ORBIT-STATUS.md` (pilot's profile re-applied, `engineDevMode` off, user state files identical,
   quiet.lock removed, PimaxClient closed).

## Task split

### Engine agent (DcsQvCull)

**Owns:**

- `native/dcsqvcull/src/cockpit_mask.h`: the motion writer, layout, records and held set.
- `fixation_depth.h`: arming at the focus passes' sample point.
- `main/config.inc`, `main/config_load.inc`, `main/perception_tick.inc`: the keys.
- `status_word.h`: optional new bits.
- `test/cockpit_mask_test.h`, `test/ini_keys_test.h`, `test/invariants_test.h`.
- The app's DcsQvCull.ini defaults (keys absent or 0).

**Must not edit:** `dcsvr_*.h`. A contract change goes back through a new revision in both trees, with
`check-contracts.ps1 -Update` once both sides agree.

**Keys:**

- `[Perception] CockpitMotionFocus=0` (needs `CockpitMotionExport=1`).
- `[Perception] CockpitMotionDepth=0` (needs `CockpitMotionExport=1`).
- `[Dev] CockpitMotionFocusAssumeJitterFree=0`: sets `jitter_proven` on views 2/3 before the live proof, for the
  OFXR bring-up only.

**Interfaces it implements:**

- `Header.focusView[s][0..1]`, viewMask bits 2/3, `view_flags::focus` / `after_water` / `jitter_proven`, and
  `slot_flags::focus_partial`.
- `header_flags::focus_views`.
- `depthFormat = 54`, `depthTextureHandle[s]` (shared NT, same device and fence), `header_flags::depth_plane`,
  `slot_flags::depth_plane` and `view_flags::depth_plane`.
- `dcsvr_cockpit_depth::header_flags::per_pixel_in_motion` while the plane is published.
- `revision = 4` (already from the constant).

**Tests to add (offline):**

1. Revision 3 byte identity with both options off: the tail is zero and records match today's (extend
   `TestMotionContract`).
2. A focus capture on the fake pass sequence pL, pR, fL, fR: rects, fovTan, after_water, focus flags. Bindings are
   restored after the CS (state snapshot before and after), and nothing is written to DCS targets.
3. Depth plane values against the depth companion: w = near / d, the viewport range undone, 65504 for d = 0, and
   per-pixel depth between the companion's block .x/.y extrema for the same texels (the PLAN's offline check).
4. MV/class/depth texel alignment: the same rect in all three textures.
5. Held set: off/on keeps the generation and handles, the first on bumps once, and the R85 harness with the new
   textures (every release path, both reader policies).
6. Idle without a heartbeat: no focus or depth work.
7. A VRAM failure falls back to revision 3 without a generation loop.
8. Ini keys and invariants with the options on and off.

**Live (Dev, short):**

- The R70 T1 still-frame jitter check on views 2/3. If it passes, set `jitter_proven` by default.
- A GPU pass timing of the capture (budget 60 us).
- The Cheeky two-layer run: no "materialized" line and no change in its log.

### OFXR agent (patches/ofxr-djules75, after 0127)

**Owns:**

- New patches 0128 and later.
- In the OFXR tree: `src/d3d12/cockpit_mask.cpp/.hpp` (pairing, copies, the previous copy),
  `src/d3d12/dcs_motion.hlsl` (focus-inset 4 px cells, feather, retuned edge band and scatter), the M1 composition
  shaders when M1 starts, and `tests/cockpit_mask_tests.cpp` plus the fixture tests (`XRFG_TEST_FG_ENGINE_DATA`).

**Must not edit:** `include/xrfg/dcsvr_*.h` (contract copies; they come only from a contract-revision patch such as
0127).

**Keys:** the OFXR ini keys next to `[ofxr] dcs_motion`, default off: focus MVs, the focus cell size (4), and the depth
plane / M1.

**Interfaces it consumes:** `view_record`, `view_usable`, `true_motion_view`, `depth_usable`, `prev_slot`,
`usable_for_pair`, `slot_time_usable`, and `header_flags::focus_views` / `depth_plane`, `depthFormat` and
`depthTextureHandle`.

**Tests to add:**

1. The fake writer at revision 4 (focus and depth on): focus and depth rects copied in the same list under one wait,
   and copies sized to the rects used.
2. A revision 3 writer: the periphery-only path, unchanged output.
3. Focus without `jitter_proven`, focus_partial, depth missing in A: each falls back per view or per pair.
4. Previous copy pairing across a reset, a generation change and a skipped frame.
5. Held set: active 1 -> 0 -> 1 with the same generation means no re-import (count opens).
6. Fixtures:
   - `cell` with a focus inset at 4 px against the 8 px periphery (no seam at the inset border);
   - the M0b small-aircraft fixture (4-8 px aircraft, 2 px light) at d10 vs d2;
   - the model, trailing, behind and ahead bands must not regress against d10 before the focus key may default on.

**Later (M1):** the offline gate in PLAN M1 (behind/ahead/model-band MAE at d2 with depth, against 0036 and 0121),
then the live F2 orbit and day4/night4 ghostfree ABBA with the validation-08 pacing gates.
