# One geometry pass per eye (periphery + focus together)

Design, first stage and live gates. Written 2026-11-10, offline: DCS, DCS Control and the headset software were
not started; nothing was written to Saved Games or the DCS install.

**Tags.** [V] checked for this document (source, census bytes, or a test that runs in the build). [D] documented
in the project's research notes (R-numbers, kept in the engine's analysis tree outside this repository), not re-measured. [H] hypothesis: needs
the named capture before anyone builds on it.

## 0. Verdict

- **Feasible, but not yet provable as bit-identical.** One mechanism can be exact: NVIDIA single-pass stereo with
  a full second position (R120a "route S"), fed by a vertex-shader clone that computes the focus position with
  DCS's own focus rows. Everything else is either lossy by a few floats or slower than two passes.
- **Three conditions decide exactness, and none is proven live.**
  1. The focus view's projection must be known, to the bit, while the periphery pass runs (section 4.2). New
     offline result: the rows the shaders read are an exact function of rows DCS shares between both views and four
     scalars [V]. Whether the four scalars are available early is open (capture C5).
  2. The G-buffer pixel shader must compute motion vectors and the mip bias with its own view's constants (4.3).
     Needs a view index in the pixel shader. Open (probe P1, offline on this GPU).
  3. The clone must cover at least 90 % of the focus triangles (4.1). The patcher exists and is exact on test
     shaders [V]; DCS's own shaders can only be counted once its shader cache exists again (census, section 8).
- **Fail-safe by construction.** The merged focus content is used only when the predicted focus rows equal DCS's
  real upload byte for byte; otherwise the focus pass runs stock. A wrong prediction costs time, never pixels.
- **Size of the prize** [D, estimates]: GPU 0.35-1.15 ms and render thread 1.0-1.5 ms per frame; 6-10 weeks; high
  risk. The live captures in section 7 were never run and come first.
- **This commit** is stage 0: the exact-row math, the shader clone patcher, a shader-cache census behind
  `[Suite] PerEyePassCensus` (default 0), and tests. No draw path changes.

## 1. What DCS does today

Source for this section: R120b (census of every D3D11 call over 4 frames, F-4E Germany F2), R53, R38, and the
shipped hooks that depend on these facts.

- **Order** [D; V in `vrs_probe.h`, `focus_twin.h` which identify views this way]. Per frame: shared work (4 shadow
  cascades, height map, atmosphere), then four complete view pipelines: periphery L, periphery R, focus L, focus R.
  Each runs its own culling chain, G-buffer, lighting, transparents and DLSS. In F1, MFD sensor views render first
  on alternate frames, so views are identified by their TerrainContext, never by ordinal.
- **The merge target** is the main G-buffer pass (`SM_GBUFFER_PBR`). Its draws: terrain patches (`DrawIndexed`),
  instancer trees and buildings (`DrawIndexedInstancedIndirect`), cluster (`DrawInstancedIndirect`), grass (one
  tessellated draw), models (the G-buffer recorder's command lists).
- **Focus is a subset replay of the periphery** [D, R120b 0.2]. Instancer: 601 of 601 focus draws use a periphery
  argument slot, same LOD mesh in 100 %. Patches: byte-identical draw in 99.6-100 % (R123 later: about 84 % for
  patches once vertex stream 0 is compared). Per-draw constant buffers are identical across views.
- **Only four pass-level buffers differ per view**: b5 (304 B), b7 `cPerView` (2416 B), b10 (1280 B), b11
  `TerrainContext` (2176 B). Each is uploaded once per pass; b11 as the first event inside the pass.
- **TerrainContext rows that differ between the two views of an eye** [V, `focus_twin_math.h` `PerViewRow`; census]:
  16/17 (projection x/y), 20/21 (view-projection x/y), 24/25 (inverse projection), 28-31 (side planes), 44 (render
  size in z, w), 132. Every other row is byte-equal.
- **What the shaders read** [D, R38 4, 4,054 archive blobs]. Terrain, instancer and cluster vertex shaders: rows
  20/21 and the flag `cb11[44].x` (2,016 of 2,020). G-buffer pixel shaders read per-view data only for motion
  vectors (b10 rows 18/19/21, b7 `gPrevFrameTransform`, `gTargetDims`) and the mip bias (-1.7776 against -1.7862).
- **Culling** [D]. Models: Scene.dll per view, probably a finer LOD in the focus. Forest and buildings: one GPU
  chain per view, LOD by camera distance only, so the focus list is the periphery list cut by the focus planes.
  Patches: per-view CPU lists, same LOD. Grass: per view.
- **Targets** [D; memory note dcs-depth-targets]. Each view has its own G-buffer array and depth
  (R32G8X24, reverse-Z, GREATER_EQUAL), bound only inside the pass. With DLSS: periphery 476x468, focus 1176x1164
  internal, 1 sample. DCS pops to a dummy frame buffer at the pass end.
- **Cost** [D, R120b 2]. Focus main G-buffer 0.6-1.2 ms per view, 74-76 % of it front-end (fetch, vertex shader,
  clip, setup). Its triangles: patches 44 %, models 37 %, instancer 15 %, cluster 3 %, grass 1 %. About 935 terrain
  family draws per focus view on the render thread.

## 2. What "bit-identical" demands

A merged pass equals stock only if all of these hold. Each later section names how.

| # | Condition | Why it can break |
|---|---|---|
| E1 | Same triangles reach the focus rasteriser with the same clip positions | float order of the vertex math; a predicted projection |
| E2 | Same rasterisation | atlas offsets move depth by an ulp (R120a 3.5); MSAA |
| E3 | Same pixel shader inputs | interpolants and constants of the periphery leak into the focus |
| E4 | Same winner per pixel | draw order and depth ties (GREATER_EQUAL: the later draw wins a tie); blended draws |
| E5 | Same content drawn | per-view LOD or culling (models, grass) |
| E6 | Everything downstream sees stock targets | DLSS, exports, fixation depth, colour, frame generation |

## 3. Mechanisms considered

| Mechanism | Verdict | Exact? | Reason |
|---|---|---|---|
| **S: single-pass stereo, second position = focus position from a vertex-shader clone, RT-array offset, independent viewport masks** | **target** | yes for E1/E2 [D toy: 0 pixels differ]; E3 needs 4.3 | 100 % of the ideal saving, 0 broadcast overhead (R120a 3.4) |
| Sa: same, second position as an affine of the periphery position | no | 0.05 % pixels differ | one more rounding than stock |
| B: FastGS viewport broadcast, affine in viewport 1 (R53's machinery, whole pass) | prototype only | 0.31 % pixels differ | viewport transform is not the stock arithmetic |
| C: vertex shader viewport mask into an atlas | no | atlas is not exact even for two separate passes | dominated by S |
| D: instancing x2 with `SV_ViewportArrayIndex` from the vertex shader | no | near exact | +0.68 ms per eye: the vertex shader runs twice |
| E: plain geometry shader x2 | no | no | +0.64 ms per eye |
| Stream-out and replay | no | no | +1.9 ms per eye |
| Periphery depth as focus early-Z | no | lossy (0.46 % wrong pixels) | saves 0.012 ms |
| Shared culling + per-view CPU replay (FocusTwin, R123/R125) | separate item, in another worktree | exact by design | CPU only; costs 0.1-0.2 ms GPU per focus view. It is also this design's skip layer (4.6) |
| Cross-eye stereo, 4-view MVR | no | no | different positions and culled sets; 4-view MVR crashes on D3D11 |
| Re-running the recorder's periphery command lists for the focus | no | lossy | they carry the periphery's targets and model LOD |

## 4. The design

### 4.1 Sequence per eye

1. **Before the periphery main pass**: build the focus rows (4.2). No rows: stock frame for this eye.
2. **Periphery main pass**: bind two-slice arrays (slice 0 periphery, slice 1 focus), two viewports at the origin,
   single-pass stereo on (immediate context only). For each terrain, instancer and cluster draw whose vertex shader
   has a clone: bind the clone and the injected buffer. Every other draw renders view 0 only, as stock (R120a T13).
   Models and grass are not merged (E5).
3. **Pass end**: stereo off, copy the periphery slice back to DCS's targets.
4. **Focus main pass**: compare the rows used with DCS's real upload, bytes only. Equal: skip the merged families
   and bring slice 1 in (4.4). Different: drop the slice, DCS draws everything (stock).
5. Everything after the G-buffer is untouched (E6).

The clone is made by `pep::PatchVsDualPosition` (`src/per_eye_pass.h`) [V]:

- It finds every instruction whose result depends on a per-view row, in program order and through branches, and
  repeats exactly those instructions on twin registers with the row operands pointed at an injected buffer. The
  second position is therefore computed by the same opcodes in the same order as a separate focus pass would run.
- It adds two outputs, named for `NvAPI_D3D11_CreateVertexShaderEx`: `NV_XYZW_RIGHT` (float4) and
  `NV_VIEWPORT_MASK` (uint, read from the injected buffer so the focus view can be masked off at run time).
- It refuses, with a reason, anything it cannot prove: a branch that depends on a per-view value, a computed row
  index, an unknown instruction, a per-view write it cannot mirror. A refused shader's draws stay stock in both
  passes.
- It reports outputs other than the position that depend on a per-view row (`viewVaryings`). Those are an E3
  problem: single-pass stereo shares every attribute but the position.
- Test result [V, WARP in the build; also run once on this PC's RTX 5090]: for five shader shapes, the clone's own
  outputs equal the stock periphery run and its second position equals the stock focus run, byte for byte, over
  384 vertices and four census view pairs.

### 4.2 The focus rows, to the bit (E1)

The vertex shader needs focus rows 20/21 during the periphery pass, before DCS uploads them.

- **New, offline** [V; a scratch script over the R26 census's `gdd_cb.csv`, four view pairs repeated in
  `per_eye_pass_test.h`]: on all 16 distinct
  TerrainContexts of the census, row 20 = `float(float(sx * row0) + float(cx * row2))` and row 21 =
  `float(float(sy * row1) + float(cy * row2))`, 128 of 128 components byte-equal. A fused multiply-add matches only
  88-118. Rows 0-2 are shared by both views of an eye. So **four floats (sx, cx, sy, cy) fix the focus rows
  exactly** (`pep::BuildVpRows`, `PredictFocusRows`).
- **The jitter** [V for x]: centre = `c0 + 2 j / size`, with the same jitter in pixels in both views. Byte-equal
  in 8 of 8 x cases of the census (j = -1/16 and 7/16 px). The y axis uses a base-3 sequence; two phases cannot pin
  its float form (capture C5).
- **Why FocusTwin's prediction is not enough**: it fits the focus projection through float camera matrices and
  accepts a relative tolerance of 5e-5 (`focus_twin_math.h` `ValidateFocus`, `kRowNear`) [V]. That is fine for a
  replay at focus time, which copies DCS's real bytes. A merged pass rasterises before those bytes exist.
- **Open** [H]: where to read sx, sy and the unjittered centre of the focus early. Fixed foveation: last frame's
  values are this frame's. Eye tracking: the centre moves every frame; candidates are the collect-time camera
  (`CollectionInfo`, which FocusTwin already publishes), DCS's per-view context object, or the OpenXR view FOV with
  DCS's own formula. Capture C5 logs each candidate next to the real upload, in hex.
- **Guarantee**: `pep::VerifyFocusRows` compares bytes at the focus pass. A miss means a wasted slice and a stock
  focus pass. The hit rate is a performance number, not a correctness one.

### 4.3 Pixel shader inputs (E3)

Inside the merged pass the focus fragments run the periphery's pixel shader state. Three leaks:

| Leak | Size if ignored | Exact remedy |
|---|---|---|
| Motion vectors (b10 rows 18/19/21, b7 `gPrevFrameTransform`, `gTargetDims`) | 0.004-0.055 px after a full-screen fix-up: not exact | pixel shader clone that selects per-view constants |
| Mip bias (b7 / b10) | 0.0086 LOD step; R53 saw 0.85-1.36 % of G-buffer pixels differ | same clone |
| Vertex outputs that depend on per-view rows | unknown; the census counts them | recompute in the pixel shader clone from position and per-view constants |

- The remedy needs the view index in the pixel shader: `SV_RenderTargetArrayIndex` or `SV_ViewportArrayIndex` as a
  pixel shader input under single-pass stereo. **Not verified** [H]. Probe P1 (section 7) answers it in a toy app on
  this GPU, no DCS.
- If the pixel shader cannot learn its view: no exact G-buffer colour merge. Merging depth only would stay exact
  but saves no vertex work, so it is not worth building; the project would stop at stage 1.
- A pixel shader clone needs the same patcher machinery for pixel shaders (select between two injected constant
  sets). Not in stage 0.

### 4.4 Depth, early-Z and order (E4)

- **Whole pass, not one family.** The focus slice must receive every occluder in stock order, or pixel work
  explodes (section 5). Terrain, buildings, trees and cluster go in together.
- **Order inside the merged families** is the periphery's order, which is the focus's own order for the shared
  draws [D, inferred from identical sort keys]. Capture C4 confirms.
- **Order against the unmerged draws** (models, grass, refused shaders). With GREATER_EQUAL the later draw wins a
  depth tie. `pep::ClassifyOrder` [V] gives the only exact cases:
  - merged run first: copy slice 1 in, then DCS draws the rest;
  - merged run last: DCS draws the rest, then slice 1 is composited with the pass's own depth test and stencil
    state;
  - mixed: no exact composite; the focus pass stays stock that frame.
- **Blended instancer draws** (buildings use `BlendEnable`, R53): exact only if the instance order inside each
  draw is the same in both chains' outputs. Open (capture C7). Until then they stay unmerged, which makes the order
  "mixed" wherever they interleave. This alone can sink the gain.
- **F1 cockpit**: the cockpit pass writes depth and stencil before the main pass and hides 65-75 % of the focus.
  The slice needs that depth first or it shades hidden pixels. Exact route: merge the cockpit pass too (models,
  same clone technique, plus CockpitTrueIpd's camera shift). Until then F1 is excluded.

### 4.5 Targets (E2)

- Two-slice arrays per channel at the larger size per axis, both viewports at the origin (`pep::PlanSlices`) [V
  pure]. Arrays are exact; an atlas is not [D].
- Copies: colour with `CopySubresourceRegion`, depth through an `SV_Depth` pass, stencil through 8 bit-plane passes
  (no `PSSpecifiedStencilRef` on D3D11). R53 measured 0.06 + 0.05 ms per frame for these. A depth copy through a
  shader must return the same bits: checked in stage 3's same-frame verify.
- Not first: making DCS's render graph allocate inside our arrays (no copies, unknown pooling).
- MSAA targets: no plan (`kPlSamples`). The design covers the DLSS path, which is the only one Quad Views users
  with this app run.

### 4.6 Culling, content, CPU (E5)

- Instancer draws in the merged pass read the periphery's instance lists. The focus list is that list cut by the
  focus planes with the same LOD [D, R38 3.2], and the cut instances lie outside the focus frustum, so the focus
  slice gets the same coverage. Needs C7 for order (4.4).
- Patches: the periphery's list is a superset; extra patches clip away. The 16 % of patches whose vertex stream
  differs per view (R123) are a refusal per draw.
- Models keep their own focus LOD; grass its own distance. Both stay per view.
- **CPU half**: the gain on the render thread comes from not running DCS's terrain items in the focus pass. That
  skip layer is FocusTwin's item-level takeover. This design reuses it and does not build a second one.

### 4.7 DLSS, motion vectors, exports, Quad Views (E6)

- DLSS inputs stay per view. The jitter is applied by the focus rows themselves, so focus samples land where stock
  puts them.
- Motion vectors: see 4.3. DCS's are jitter-free [D, R69].
- Our consumers of per-view targets (fixation depth, far-depth and cockpit exports, colour, GazeDof, frame
  generation exports) sample after the focus G-buffer. They see stock content if the slice is in before the pass
  ends. Each gets a regression check in stage 3.
- Quad Views layer: untouched. It composites four swapchain images at `xrEndFrame`. Eye tracking only moves the
  focus projection (4.2).

### 4.8 Driver state rules and fallbacks

From R120a 1.3 [D]:

- A clone must never be drawn with stereo off: the driver then stops multicasting it for good. Clones are bound
  only inside the block.
- Stereo mode is set on the immediate context only; it survives `ExecuteCommandList`.
- RT offset must be at least 1. One mask word: low 16 bits view 0, high 16 bits view 1 (`pep::PackViewportMasks`).
- NvAPI object creation only after the start gate (10 s of steady frames): creation during a mission load hung
  DCS once (R53 6). A start-up self-test on DCS's device decides whether the driver still supports the path;
  otherwise the feature stays off.
- Any unexpected state (target mismatch, device proxy, kill switch, F10 map, MFD views): stock.

## 5. Lessons from R53

- R53 broadcast **trees only** into a slice with no occluders. Periphery pixel shader invocations rose from 1.25 M
  to 10 M; trees were a small part of the focus vertex work (7.99 M to 7.32 M). Result: +0.10 ms. That closes
  "trees only", not the idea. R120a reproduced the failure in a toy and showed a whole-pass broadcast keeps pixel
  work equal (12.28 M against 12.37 M).
- R53 was FLOAT-class by design (affine in the viewport, periphery mip bias). Its verify failed its own 1 % gate
  in F2. Route S with exact rows removes the first cause; 4.3 must remove the second.
- Facts R53 paid for and this design keeps: identify views by TerrainContext; capture it through the upload
  observer; DCS pops its frame buffer at pass end; `gPrevFrameTransform` is jitter-free; the NvAPI start gate.
- R53's prediction ran with a fixed focus only. Eye tracking was never tested.

## 6. Expected gain

Unchanged from the notes; nothing here was measured again.

| | Per frame | Source |
|---|---|---|
| GPU, route S | 0.35-1.15 ms | R120a 5.3 |
| Render thread | 1.0-1.5 ms | R120b 5 (estimate; no per-view CPU measurement exists) |
| Vertex shader overhead of the clone | small: only position-dependent instructions repeat (test shader: under half) | stage 0 [V]; real shaders: census |

Things that shrink it: refused shaders, blended buildings, "mixed" order frames, F1 until the cockpit pass is
merged, prediction misses under eye tracking.

## 7. Stages, gates and captures

Each gate is GO / NO-GO. A NO-GO at any stage leaves everything before it shippable as measure-only code.

| Stage | Work | Gate | Decided by |
|---|---|---|---|
| **0 (this commit)** | exact rows, clone patcher, census, tests | build and tests green; clone bit-exact on WARP | done [V] |
| **1 offline** | P1 probe; census on a real cache | P1: the pixel shader can read its view. Census: clones cover >= 90 % of terrain-family shaders, with or without `viewVaryings` counted separately | P1, census |
| **2 live, measure only** | captures below | all of C1, C2, C4, C5, C7 pass | C1-C7 |
| **3 live prototype** | merged pass for one eye, verify every frame against a stock focus render in the same frame, then both eyes | 0 differing texels over 10 minutes in F2, fixed and tracked gaze; then GPU >= 0.3 ms or render thread >= 0.5 ms in the matching scene | same-frame verify; GpuPassTiming ABBA |
| **4** | cockpit pass, WaterWaveMask, flat shadows; default-off opt-in | same verify in F1; soak | verify, soak |

**Captures.** C1-C6 are R120b 7's (never run). C5 is tightened; C7 and P1 are new.

| Id | Needs DCS | What it measures | Pass |
|---|---|---|---|
| P1 | no (toy app on this GPU; needs NVIDIA's public NVAPI header for the stereo entry points) | pixel shader reads `SV_RenderTargetArrayIndex` / `SV_ViewportArrayIndex` under single-pass stereo with RT offset; clone drawn with DCS-like state | the value differs per view and equals the slice |
| Census | cache only: `[Suite] PerEyePassCensus=1` in a flight, or the test binary with `QV_PEP_ARCHIVE` on a copy | shaders cloned exactly / with view-dependent outputs / refused, by reason | >= 90 % cloned; refusals named |
| C1 | yes | per-view GPU and render-thread ms per pass (Germany F2, airfield F2) | focus G-buffer render thread >= 1.0 ms per frame at the airfield |
| C2 | yes | front-end share of the focus main pass (FocusGeomProbe from the R53 archive) | >= 0.6 |
| C3 | yes | model triangles and LOD factor per view | confirms models stay per view |
| C4 | yes | item order of the focus main pass: where merged families, models, grass sit | `ClassifyOrder` is first or last in >= 95 % of frames |
| C5 | yes; eye-tracked part needs the headset worn | focus sx, cx, sy, cy from each early candidate against the real upload, in hex; the jitter value DCS holds | byte-equal in >= 99 % of frames fixed, >= 95 % tracked |
| C6 | yes (optional) | model GPU per view | sizing only |
| C7 | yes | instancer instance buffers after the periphery chain and after the focus chain, per (type, LOD) | focus = periphery filtered, same order, over 100 frames |

Run order: P1 and the census first (no DCS). Then one live session for C1, C2, C4, C5 fixed gaze, C7. C5 tracked
needs the user.

## 8. Stage 0 as built

- `src/per_eye_pass.h` (namespace `pep`), included last in `src/main/features.inc`:
  - exact rows: `BuildVpRows`, `JitterCentre`, `PredictFocusRows`, `VerifyFocusRows`;
  - rules: `PackViewportMasks`, `PlanSlices`, `ClassifyOrder`;
  - DXBC: container parse and build, checksum, instruction walker, `PatchVsDualPosition`;
  - census: `CensusFolder`, `CensusText`, `SuitePhase`.
- `[Suite] PerEyePassCensus` (default 0). With 1, the suite thread reads
  `Saved Games\DCS\metashaders2\*.meta2` and logs lines starting `per-eye pass census:`. File reads and CPU only: no
  hook, no device object, no NvAPI call. With 0 nothing runs. DCS Control does not write the key.
- `test/per_eye_pass_test.h`: see its header. `QV_PEP_ONLY=1` runs only these; `QV_PEP_HW=1` uses the GPU instead of
  WARP; `QV_PEP_ARCHIVE=<folder>` prints the census of a folder.
- **Not known yet**: the census on DCS's real shaders. This PC's `metashaders2` is empty since the reformat; DCS
  refills it while missions run.

## 9. Risks, ranked

1. Pixel shader view index (4.3). No exact colour merge without it.
2. Early, byte-exact focus projection under eye tracking (4.2).
3. Blended buildings and item order (4.4): frames that fall back to stock.
4. Clone coverage on DCS's real shaders, and `viewVaryings` among them.
5. Depth and stencil copy fidelity; render-graph target identity.
6. NVIDIA's single-pass stereo is a legacy VRWorks path: a driver update can remove it. Start-up self-test, then
   off. NVIDIA GPUs only.
7. F1 needs the cockpit pass merged before it gains anything.
8. Interplay with the G-buffer recorder, FocusTwin, CockpitTrueIpd, VRS probe, device proxies (ReShade).
