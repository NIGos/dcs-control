# One geometry pass per eye (periphery + focus together)

Design, first stage and live gates. Written 2026-11-10, offline: DCS, DCS Control and the headset software were
not started; nothing was written to Saved Games or the DCS install. Stage 1 (probe P1, census) and the stage 2
capture code were added on 2026-11-11, also offline: sections 10 and 11, and the notes marked "2026-11-11".

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
     Needs a view index in the pixel shader. **Settled 2026-11-11 [V, probe P1]: it has one** (section 10.1).
  3. The clone must cover at least 90 % of the focus triangles (4.1). The patcher exists and is exact on test
     shaders [V]. **2026-11-11 [V, partial cache]: 67 of 67 terrain-family vertex shaders clone exactly** (10.2).
- **New limit, 2026-11-11.** Instancer draws (trees, buildings) can be exact only up to the order of an object's
  instances: DCS's own culling chain already orders them differently on every run (10.3). Capture C7's "same order"
  can therefore not pass; the merged pass is "indistinguishable from stock", not bit-identical, wherever two
  instances of one object tie in depth or blend over each other.
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
  pixel shader input under single-pass stereo. **Verified 2026-11-11** [V, probe P1, section 10.1]: both arrive, even
  when the vertex shader does not write them, and a pixel shader that picks its constants by the index equals two
  stock passes byte for byte.
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
  **2026-11-11:** FocusTwin was shelved on the same day (R151, `FOCUS_TWIN_RETEST.md`: it never engages live, and
  listening costs about 0.42 ms per periphery pass). Until a cheaper skip exists, plan with the GPU half only
  (section 11.5).

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
| **1 offline (done 2026-11-11, section 10)** | P1 probe; census on a real cache | P1: the pixel shader can read its view: **GO**. Census: clones cover >= 90 % of terrain-family shaders, with or without `viewVaryings` counted separately: **100 % of a partial cache (67 shaders); repeat on a full one** | P1, census |
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
| C7 | yes | instancer instance buffers after the periphery chain and after the focus chain, per (type, LOD) | focus = periphery filtered, same order, over 100 frames. **2026-11-11: the order half cannot pass (10.3); the capture checks the counts half only** |

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

## 10. Stage 1 results (2026-11-11, offline)

### 10.1 Probe P1: the pixel shader knows its view [V]

`test/pep_p1_probe.cpp` (CMake target `DcsQvCullPepP1Probe`, never run by ctest), RTX 5090, driver 32.0.16.2114,
feature level 11_0 like DCS. NVAPI through `src/nvapi_sps_min.h` (declarations from NVIDIA's public SDK; no driver
setting is read or written). `NvAPI_D3D_QuerySinglePassStereoSupport`: status 0, single-pass stereo 1, XYZW 1.

One indexed, instanced draw into three render targets and a D32S8 depth-stencil, all two-slice arrays; depth
GREATER_EQUAL, stencil REPLACE, scissors on, two viewports of different sizes at the origin, RT index offset 1,
independent viewport masks. Reference: the plain shaders, one view at a time, into a second set of arrays.

| Variant | Slice 0 reads | Slice 1 reads | Stereo slices = reference (coverage, attributes, SV_Position, derivatives, depth, stencil) |
|---|---|---|---|
| pixel shader input `SV_RenderTargetArrayIndex`, vertex shader does not write it | 0 | 1 | 0 differing texels of 137,316 + 236,032 |
| pixel shader input `SV_ViewportArrayIndex`, vertex shader does not write it | 0 | 1 | 0 |
| vertex shader writes either or both as 0 | 0 | 1 | 0 |
| `NV_PACKED_EYE_INDEX` custom semantic | the written value | the written value | 0 (no use as a view index) |

- **Gate: GO.** The cheapest form works: the pixel shader clone only declares one more input. The vertex shader
  clone stays as the patcher makes it.
- **The merged pass in small** (`patched_clone`): a DCS-shaped vertex shader (TerrainContext at b11, position from
  rows 20-23, a `surfaceLod` row picked by a clamped index, instances) cloned by `pep::PatchVsDualPosition`, created
  with `NvAPI_D3D11_CreateVertexShaderEx`, drawn once with the periphery's TerrainContext and the census focus rows
  injected at b13, and a pixel shader that picks its view's constants by `SV_RenderTargetArrayIndex`. Against two
  stock passes (stock shaders, periphery / focus TerrainContext): **0 differing texels** in all three targets, depth
  and stencil (44,896 + 262,118 covered). This is the stage 3 mechanism end to end, on the real driver, without DCS.
- **DCS's own clones are accepted by the driver:** with `QV_PEP_ARCHIVE` set, 67 of 67 clones of the cache copy
  were created by `NvAPI_D3D11_CreateVertexShaderEx` (status 0).

### 10.2 Census on DCS's cache (partial) [V]

`QV_PEP_ARCHIVE=<copy>` on a copy of `Saved Games\DCS\metashaders2`: 16 files, 276 blobs, 88 duplicates, 68 distinct
vertex shaders, 67 with the per-view buffer. **This cache is partial**: it holds what DCS compiled since the
reformat (R38 counted 2,020 terrain vertex shaders in a full one). The gate needs a rerun on a full cache; the key
`[Suite] PerEyePassCensus=1` does it inside a flight.

| | Stage 0 patcher | With the bounded-row rule (this stage) |
|---|---|---|
| cloned exactly | 5 (7.5 %) | **67 (100 %)** |
| cloned, view-dependent value to the pixel shader | 0 | 0 |
| refused: computed row index | 62 | 0 |
| instructions repeated by the clones | 20 of 259 | 284 of 6,226 (4.6 %) |

- All 62 refusals were one pattern: `cb11[r + 45]` with `r = imax(imin(i, 3), 0)`, DCS's
  `TerrainContext_surfaceLod[4]` (rows 45-48). None of those rows is per view (R123 list).
- **The rule** (`per_eye_pass.h`, `Scan::lo / hi`): a computed row is accepted when its index register holds the
  result of `imin` / `imax` with literals made in the same straight-line run (the range is forgotten at every other
  write and at every branch), the range is finite and non-negative, the index is not itself per view, and no row of
  `base + [lo, hi]` is a per-view row. Anything else computed stays refused. Tests: a clamped index onto rows 45-48
  clones and is byte-exact on WARP; a range reaching row 44, and an index without a lower bound, are refused.
- **No clone hands a view-dependent value to the pixel shader** in this cache. If that holds on a full cache, the
  pixel shader clone needs the constants remedy only (4.3 rows 1 and 2), not a recomputation of varyings.
- **Live check added** (capture C5): rows that differ between a periphery and its focus TerrainContext outside the
  patcher's per-view list are counted. It must stay 0, or the list (and the bounded-row rule's premise) is wrong.

### 10.3 C7 decided in part offline: the instance order is not stock-stable [D, R34 4.1]

- DCS's culling chain ranks the instances of an object with racing atomics (`instancemanager2.calccounters`; the
  stage before it appends with atomics too). Two stock runs on the same input differ in 106-168 k of 168 k rank
  words [D, measured in R34's harness]. The focus chain and the periphery chain are two such runs.
- So "focus list = periphery list, same order" has no meaning even between two stock frames. What remains testable
  is the set: per object, the focus chain's instance count must not exceed the periphery's (same LOD bucket, fewer or
  equal instances). `[Suite] PerEyeC7` reports exactly that from the measurement kit's chain census.
- **Consequence for 4.4.** A merged instancer draw gives the focus slice the periphery's order. The result differs
  from a stock focus pass only where two instances of the same object tie in depth on a pixel (GREATER_EQUAL) or
  blend over each other, and stock itself changes there from frame to frame. The app already ships this class of
  exactness (`[D3D] ChainFast` default 7). Stage 3's verify has to count instancer draws separately (11.6).

## 11. Stage 2 as built, and the stage 3 plan

### 11.1 Stage 2 capture code (`src/per_eye_live.h`, 2026-11-11; never run live)

All keys are in `[Suite]`, default 0, and run as one suite phase (Ctrl+Alt+F11 or `run_suite.flag`). Each key is a
duration in seconds.

| Key | Capture | Greppable line | Changes the image |
|---|---|---|---|
| `PerEyeC5=N` | TerrainContext of every G-buffer pass, read back without waiting | `pep C5:` every `PerEyeLogSec` (10) s, `pep C5 sample:` hex at the end | no |
| `PerEyeC4=N` | item vector of every G-buffer pass execution, classed by vtable | `pep C4:` every `PerEyeLogSec` s, `pep C4 class:` at the end | no |
| `PerEyeC7=N` | measurement kit chain census + the view pairs | `pep C7:` once | no |
| `PerEyeC2=N` | R53's FocusGeomProbe, restored: focus main G-buffer pass geometry only, five blocks | `pep C2 block`, `pep C2:` once | **yes, in its two ON blocks only** (the focus views lose their terrain) |

- Views are told apart by the TerrainContext, never by fixed ordinals; the main pass is the second to last of a
  view's three G-buffer passes (R44).
- C5 reports: (1) focus VP rows exact from the periphery's shared rows and the focus's own scalars (the live form of
  4.2's census result); (2) rows differing outside the per-view list; (3) the **early source for a fixed gaze**: a
  table from the periphery's jittered centre, known at the periphery pass, to the focus scalars last seen with it;
  (4) the jitter step in pixels in both views. The hex samples settle the y jitter's float form offline. The
  eye-tracked sources of 4.2 (OpenXR FOV, DCS's view object) are not captured: the payload has no hook that sees them.
- C4 classes pass items, not the draws inside the terrain item. If the terrain is one item (expected: Visualizer's
  TerrainObject), "first / last / mixed" tells where it sits among the models; the order of patches, instancers and
  grass inside it needs the draw-level tee of stage 3.
- No device object is created outside the phase; the phase waits for 3 s of steady frames first. No NvAPI call.

### 11.2 Stage 3: why there is no live skeleton yet

P1 passed, so stage 3 is open. A one-eye skeleton was **not** built in this round: it cannot be made exact and safe
without things only a live session provides, and a merged pass that cannot be verified is worse than none.

1. No vertex shader bytecode at run time (DCS keeps none, R45). The clone needs it per live shader object.
2. No pixel shader clone patcher yet (4.3), and no census of which G-buffer pixel shaders read per-view rows.
3. C2, C4, C5 have never run: the share, the order and the early source are unknown.
4. The skip layer of the focus pass does not exist (FocusTwin shelved).

What was built instead is the offline proof of the mechanism (10.1 `patched_clone`) and the capture code.

### 11.3 Build order for the skeleton (each step default off, each with its own check)

| Step | Work | Check before the next step |
|---|---|---|
| a | `CreateVertexShader` / `CreatePixelShader` hook from payload load: keep bytecode by shader object for blobs that declare b11 (about 16 MB for a full terrain set). Hot reload: no bytecode, feature off until the next mission load | log: shaders kept, bytes; G-buffer draws whose shader has bytecode >= 99 % |
| b | Pixel shader patcher: one more input (`SV_RenderTargetArrayIndex`), per-view rows of b7 / b10 / b11 read from an injected array indexed by it; refuse what cannot be proven, as the vertex patcher does. Offline: WARP + P1 rig | merged = two stock passes, 0 texels, for every pixel shader shape; census on a full cache >= 90 % |
| c | Clones created on a worker after the start gate (10 s steady frames), self-test on DCS's device first (P1 `patched_clone` in a 64 x 64 private target) | self-test exact, else off for the session |
| d | **Shadow mode, one eye, nothing skipped.** Periphery main pass of the left eye: private two-slice arrays, stereo on, `VSSetShader` / `PSSetShader` swapped to the clones as DCS binds them (chain_fast.h's pattern), injected buffers bound, stereo off and stock shaders back at the pass end; slice 0 copied to DCS's targets. Focus pass: stock | periphery slice 0 = stock periphery: verify by `GBufferRecVerify`-style same-frame compare on sampled frames; 0 texels |
| e | **Same-frame verify of slice 1.** In the stock focus pass every draw whose vertex shader has a clone is drawn a second time into a private target set T (same state, DCS's own focus constants). At the pass end slice 1 is compared with T: all G-buffer targets, depth, stencil, byte for byte | 11.6 |
| f | Skip: in the focus pass the draws e duplicated are dropped and slice 1 is composited (4.4: copy-in after DCS's clear when the merged run is first, depth-tested composite when it is last, stock when mixed) | same verify against a stock frame every Nth frame; then GPU ABBA |

### 11.4 State rules for step d (from R120a 1.3, R53 and split_filter.h)

- Stereo mode on the immediate context only, set at the first draw of the pass after DCS's binder ran, cleared
  before DCS pops its frame buffer.
- A clone is bound only inside the block; at the block end the stock shader DCS believes is bound is bound again
  (split_filter.h's shadow has it without a Get call).
- Draws without a clone inside the block render view 0 only (R120a T13). They are the unmerged families.
- The injected vertex buffer slot is the patcher's per shader (b13 when free); it is bound with the clone, not once
  per pass, because DCS may use b13 for other shaders.
- F10, MFD views, a device proxy, a target mismatch, MSAA, the kill switch: no block that frame.

### 11.5 Expected gain, revised

- GPU: 0.35-1.15 ms per frame as before [D]; C2 decides (share >= 0.6).
- Render thread: **not counted** until an item-level skip exists. A draw-level skip (step f) saves GPU work only;
  DCS still walks and binds its focus items. The clone swap costs no extra call per draw (it rides on DCS's own
  `VSSetShader`).
- So the route now has to pay for itself with the GPU half: GO needs >= 0.3 ms GPU in the matching scene.

### 11.6 The verify's rule (step e)

- Patches, cluster and every draw that is not an instancer draw: **0 differing texels** in T against slice 1.
- Instancer draws (`DrawIndexedInstancedIndirect` from IM2's argument buffer): compared in a second private pair,
  and reported, not gated at 0: stock orders instances differently on every run (10.3). Gate: differing texels <=
  the stock-against-stock floor. One frame runs each chain once, so the floor is T against T of the next frame with
  the sim paused and the pose held.
- A miss of the early focus rows (C5) is not a verify failure: the slice is dropped before any compare.

### 11.7 The live session for stage 2 (about 10 minutes of DCS, fixed gaze, headset not worn)

Common ini as in `LIVE_MEASUREMENT_PLAN.md` (`[Suite] Quick=1`, `BenchEngine=0`, `HoldPose=1`, `HoldPitchDeg=0`),
`[Model] FocusTwin=0`, `[Model] GBufferRecorder=0` for the C2 run (recorded model draws carry their own viewports).

| Run | Scene | `[Suite]` keys | Suite time | Grep | Pass |
|---|---|---|---|---|---|
| 1 | `qvbench_airfield`, F2, `HoldYawDeg=285`, paused | `PerEyeC5=60`, `PerEyeC4=60`, `PerEyeC7=30`, `PerEyePassCensus=1` | about 100 s | `pep C5:`, `pep C4:`, `pep C7:`, `per-eye pass census:` | C5 `PASS` (hit >= 99 %, rows exact 100 %, rows outside the list 0); C4 `PASS` (first or last >= 95 %); C7 `PASS`; census >= 90 % cloned |
| 2 | same | `PerEyeC2=50` (others 0) | about 60 s; the focus area loses its terrain twice for 10 s | `pep C2:` | share >= 0.60, periphery main unchanged between the arms |
| 3 | `qvbench_f4_germany`, F2, `HoldYawDeg=0`, parked | runs 1 and 2 again | about 3 min | same | same; C1's numbers come from `GpuPassTiming=1`, `PassTimelineSec=60` of the measurement kit |

`pep live:` lines carry the capture's own counters (snapshots, passes without a TerrainContext at b11, frames
dropped). A `NOT MEASURED` verdict means too few frames or an unusable block, not a failure of the route.


### 11.8 First live run (2026-11-11) and its fix

- Airfield F2, paused: census 67 of 67 again. C5, C4, C2 and C7 measured nothing ("frames 0", "-1 culling chains").
- **Cause:** frames were told apart by `ptiming::g_execFrame`, the second argument of the pass execute. Live it never
  changes (the same run's pass timeline says "1 frames" over 3,712 rendered ones), so the capture filled one set of
  24 snapshots and never read anything back; the chain census never rotated its set either ("sampled 1, read back 0").
- **Fix:** `mkit::FrameTrack`: a frame starts at the first top-level HeightMapPass or shadow cascade after a view
  was rendered (R53's rule). Used by the captures and by the chain census. C7 picks the view chains by their shared
  pass object. A `pep live diag:` line, once per capture, gives frame starts, G-buffer passes, snapshots, frames read
  back and paired, and names the cause when nothing pairs.
- Still open: `[Suite] PassTimelineSec` groups its events by the same constant value (one "frame"); not changed here.

## 12. Second live run, the stage 3 decision, and what was built for it (2026-11-11)

### 12.1 Results (airfield F2, yaw 285, paused, about 58 fps, Quad Views with 4 views)

| Capture | Result | Verdict |
|---|---|---|
| Census | 67 of 67 vertex shaders cloned (16 cache files: still partial) | GO |
| C5 | focus VP rows exact 100 %; early source (periphery centre -> focus scalars) hit 100 %; jitter step equal in both views | PASS, fixed gaze |
| C4 | the terrain is **one item**, the last one: item 640 of 640 in the focus main pass, 2,397 of 2,397 in the periphery's, in 100 % of the passes | PASS ("merged run last") |
| C2 | focus main G-buffer passes, both eyes: stock 1.283 ms GPU, geometry only 0.933 ms: front-end share 0.73. Periphery main 2.41 ms, unchanged between the arms | PASS |
| C7 | first eye 203 of 271 frames, second 271 of 271 | capture bug, 12.2 |

Context from the same night: the paused airfield is CPU-bound (render thread 17.3 ms per frame); in flight DCS is
GPU-bound (84 fps, 98 % GPU). The GPU half of this route is what counts in flight.

### 12.2 C7's failure was the pairing, not the chains

- The census compared chains **by index** over the frames with the modal chain count (7). The scene was paused, so
  every chain is the same in every frame; yet the per-index means are mixtures: #3 74,635 and #4 88,825 instances
  where a periphery chain has about 93,300, #5 9,385 where a focus has about 4,200. So the layout of a 7-chain frame
  is not fixed (which cascades update varies; 221 of 492 sampled frames had another chain count altogether), and
  index 3 is not always the left periphery.
- An independent full-record capture of the same night (the chain derive census) found 0 focus records outside the
  periphery's in every pair, both eyes.
- **Fix:** C7 now works per frame: the view chains are the four chains of one pass object (TerrainModule), the
  peripheries are the two with the most instances, each focus is paired with the periphery of the same rank in frame
  order. Every sampled frame counts, whatever its cascade count. `pep C7 layout:` lines list the layouts seen
  (for example `CCH|PPFF`), so a wrong assumption shows instead of failing one eye.

### 12.3 Stage 3: GO for the offline building blocks, one number still missing

- Gate: >= 0.30 ms GPU saved per frame. The focus main passes spend 0.93 ms in the front end (both eyes), the most
  a merged pass could save there.
- **But that front end is not all terrain.** C4 shows 639 model items before the terrain item in the focus pass.
  Models stay per view (4.6). Only the terrain item's front end is saved, minus the merge's copies (about 0.11 ms,
  R53 live). The split was not measured.
- **Decision:** GO on building the exact offline parts (cheap, each proven on its own). The live skeleton (11.3
  steps d-f) waits for `pep C2 terrain:`: the terrain item's own front end must be >= 0.41 ms per frame (0.30 +
  0.11). Below that the route is closed: there is no other GPU work it could save.
- C4's result fixes the composite: **merged run last**. DCS draws the focus models, then the slice is composited
  with the pass's depth test (4.4). The slice is rendered without the models' depth, so terrain pixels behind
  models are shaded in the slice and thrown away at the composite; in F2 that is a small area.

### 12.4 Built in this round (all offline, nothing bound in DCS)

- **Pixel-shader clone patcher** (`pep::PatchPsViewSelect`, `per_eye_pass.h`). The clone reads every per-view row
  from an injected buffer, row `2 i + view`, with `view` = `SV_RenderTargetArrayIndex` (one more input at the next
  free register, one dynamically indexed constant buffer, one temp). Same instructions on the same values: exact by
  construction. Refusals as the vertex patcher's; a shader that reads no per-view row needs no clone.
  - WARP test: the clone with the periphery's buffers and the injected rows equals the stock shader with each view's
    own buffers, byte for byte, in both views (rows of two buffers, a clamped `surfaceLod` read inside a branch, a
    branch on a per-view value).
  - GPU, under single-pass stereo (`pep_p1_probe`, `patched_clone`): the probe's merged draw now uses both patchers'
    clones (vertex: second position; pixel: view select) against two stock passes.
  - The range rule now holds into `if` arms (an `if` writes nothing). DCS's pixel shaders clamp the `surfaceLod`
    index, then branch, then read.
- **Census of the cache's pixel shaders** (TerrainContext rows only): 118 distinct, 74 read no per-view row, 40 get
  a clone (80 operands), 4 refused (`TerrainContext_flirFactor[i]`, row 77, unclamped: FLIR shaders, not the
  G-buffer's). Vertex shaders unchanged: 67 of 67.
- **Capture of the pixel stage's per-view rows** (`pep C5 ps rows:`): which rows of b7 (cPerView) and b10 differ
  between a periphery and its focus. The patcher's lists for those two buffers come from this line; until then the
  clone knows the TerrainContext only and must not be trusted for a real G-buffer shader.
- **C2 terrain split** (`pep C2 terrain:`): the terrain item timed as a nested pass in both arms.

### 12.5 What remains before a live skeleton, in order

1. Live: `pep C2 terrain:` >= 0.41 ms (else stop), `pep C5 ps rows:`, `pep C7:` with its layouts, the census on a
   fuller cache (fly two maps first).
2. Offline: the per-view row lists of b7 / b10 into `PsPatchOpts`; the pixel census again with them (gate >= 90 % of
   the G-buffer pixel shaders clone or need none); motion vectors need the previous frame's rows per view as well,
   which the capture will show as per-view rows of b7 / b10.
3. Then 11.3 a, c, d, e, f as written. Step b is done.
