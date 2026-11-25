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
No reader uses the new fields yet. The engine and OFXR work is split out in the last section.

Engine details a reader may rely on:

- `CockpitMotionFocus=1` makes the mask's views include 2 and 3 for the rest of the payload (the focus MVs come from
  the same per-view runs). Its switch-off then keeps the larger set, as the held-set rules below require.
- A focus view's unknown DLSS jitter never resets the pair. Its own `jitter_proven` (and `true_motion_view`) gate it;
  views 0 and 1 keep the revision 2 reset rule.
- The app writes the keys only when the profile turns them on (`FgFocusMotionVectors`, `FgPerPixelDepth`, profile
  only, off).

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
