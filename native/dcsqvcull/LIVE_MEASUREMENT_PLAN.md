# Live measurement plan: performance audit 2026-11-10 (questions a-f)

One DCS session, two mission starts, about 45 minutes with DCS running (70 minutes including preparation and
restore). Everything below is measure-only: no step changes the image, and every key used here is off by default.
The parser is `scripts/parse-live-measurement.ps1`.

The audit (`artifacts/analysis/fable-perf-audit.md` in the main checkout) left six questions open:

| | Question | Answered by | Decision it feeds |
|---|---|---|---|
| a | Where does the render thread spend its time now (per pass, frame start, culling), paused, unpaused, cockpit? | `[Suite] PassTimelineSec`, `Quick=0` phases, `FrameStartGap`, `Profile` | A4 / B5 / B7: which CPU work is worth building next |
| b | Does the cockpit G-buffer recorder save render-thread milliseconds? | bench mode 35 (`BenchGBufferRecorderCockpit`) with `[Bench] PassMs=1` | B3: `GBufferRecorderCockpit` default |
| c | How many effect Applies repeat inside instancer runs on a dense map? | `[Suite] FxApplyCount` (existing) | A3: build the same-pass Apply fast path or not |
| d | Can the focus culling chain be derived exactly from the periphery chain? | `[Suite] ChainCensusSec` | A2 / B4: R38 rank 3 |
| e | What do the five small exact GPU items give as one bundle? | `[Suite] GpuBundleTiming` (A/A until the items are built) + `GpuPassTiming` / `GpuPassStats` | A6 |
| f | Does `[Prefetch] TableBits=20` hurt p95 in a CPU-bound scene? | bench mode 34 (`BenchPrefetchTableBits`) | A8: keep 20 or go back to 16 |

## What already existed, and what this kit added

Existing and reused:

- Suite (`Ctrl+Alt+F11`, or `run_suite.flag` in the dev folder) with alternating OFF/ON blocks (`bench.h`; each ON
  block compared with its OFF neighbours, Student's t interval, `bench_<time>.csv`, `report_<time>.txt`).
- `[Suite] Quick=0`: frame times, 5 s mean render-thread CPU per pass (`pass_timing.h`), culling timeline
  (`collect_timing.h`), culling tasks, per-model culling, shadow cascades, allocator.
- `[Suite] FrameStartGap` (`frame_start.h`): xrEndFrame return to first pass, OpenXR and driver waits.
- `[Suite] Profile` and `run_profile.flag` (`profiler.h`): sampled CPU profile, `profile_<time>.txt`.
- `[Suite] FxApplyCount` (`fx_apply_count.h`): same-pass Apply counter with a cost model and an estimate line.
- `[Suite] GpuPassTiming` / `GpuPassStats` (`gpu_pass_timing.h`): GPU ms per pass kind and per call.
- Bench mode 29 (`BenchGBufferRecorder`), `GBufferRecVerify` (same-frame compare), `[Model] GBufferRecorderCockpit`.
- Pose hook `[Suite] HoldYawDeg / HoldPitchDeg / HoldPose`; `dev\qvbench_cmd.txt` (`CMD=<n>`, `RESTART`, `FLY=`)
  read by `dev\hooks\QvBenchAutostart.lua` for `qvbench*` missions.

Added by this kit (all default off; `src/measure_kit.h` unless noted):

- `[Suite] PassTimelineSec=N` (a): per frame and per pass the render-thread exclusive time, mean / p50 / p95 per
  pass kind, render-thread pass total per frame, the render thread's own CPU (`QueryThreadCycleTime`), the culling
  timeline over the same window, and `passtl_<time>.csv` (frame, pass kind, ms). Cost when off: one relaxed load in
  `pass_timing.h`'s recording path only.
- Every A/B now also reports `render_thread_cpu_ms/frame` and `render_thread_busy_%` (two `QueryThreadCycleTime`
  calls per block, `bench.h`). Before, the A/B only had the collect thread's CPU, which is a pool thread, not the
  render thread.
- `[Bench] PassMs=1` (b): every A/B block also records render-thread `pass_ms/frame` and `gbuffer_pass_ms/frame`
  (pass recording on in both arms, so both arms carry the same overhead).
- Bench mode 35, `[Suite] BenchGBufferRecorderCockpit=1` (b): recorder on in both arms; OFF = cockpit items drawn by
  DCS, ON = recorded. Needs `[Model] GBufferRecorder=1` and `GBufferRecorderCockpit=1` at DCS start (the worker pool
  is shaped at install). Restores the configured state afterwards.
- Bench mode 34, `[Suite] BenchPrefetchTableBits=1` (f): OFF = 2^16 table, ON = `[Prefetch] TableBits` (20); needs
  `[Prefetch] Mode=skip`. Restores the configured table afterwards.
- `[Suite] ChainCensusSec=N` (d): reads back, without waiting, the per-object counts of every `calccounters`
  dispatch (one per IM2 culling chain) on 1 frame in 4, and for each pair of chains of one frame checks whether one
  chain's counts are <= the other's for every object. Runs inside a GPU pass timing session, so the same report has
  the chains' GPU ms. Staging buffers are created and released on the render thread.
- `[Suite] GpuBundleTiming=<mask>`, `GpuBundlePairs=N` (e): ABBA GPU pass timing (OFF ON ON OFF ...), 5 s blocks.
  ON switches the requested bundle items that are built. **None of the five is built yet** (R26 11.7a, R26 9.2b,
  R27 R4, R28, R36 4b), so today both arms are stock: the run measures the A/A noise floor that the bundle's later
  GO rule needs. A built item registers its switch in `mkit::kBundle`.
- `[Suite] Label=<text>`: logged as `suite label: <text>` under the suite header, so the parser can name each step.

Not covered by this kit (out of scope or needs code that does not exist): building the five GPU items, the full
set check of rank 3 (instance ids, not only counts), the Main/MP meter of R39.

## Preparation (DCS closed, 15 minutes)

1. Build: `pwsh scripts/build-dcsqvcull.ps1` (payload `artifacts/native/dcsqvcull/DcsQvCullPayload.dll`, tests pass).
2. Dev mode as in earlier live rounds: `[Dev] PayloadPath` points at that payload, `[Dev] IniPath` at the dev ini
   (`DcsQvCull.dev.ini` in the analysis folder's `DcsQvCull\dev`). Never copy anything into Saved Games or the DCS
   install. Back up the dev ini first; write it through a temp file and a move, never in place (an in-place write
   once read back empty).
3. Profile: the user's production profile, **frame generation off** (FG pins the frame rate and hides CPU gains).
   Note the headset refresh (90 Hz on the Pimax): a CPU-bound scene must stay under the cap.
4. Close other GPU programs. Turn the headset on only now and wait for the runtime (Pimax 2D first-launch note).
5. Dev ini base block (kept for the whole session; later blocks only change the keys they list):

```ini
[General]
Beeps=0
[Model]
GBufferRecorder=1
GBufferRecorderCockpit=1        ; needed at DCS start for mode 35 (pool shape); switched off live in step A1
[Prefetch]
Mode=skip
TableBits=20
[Bench]
Blocks=16
BlockSec=10
SettleSec=2
PassMs=0
[Suite]
Quick=1
BenchEngine=0                   ; the suite's default A/B (all engine optimizations) is not wanted here
HoldYawDeg=285
HoldPitchDeg=0
HoldPose=1
```

## Mission A: CPU-bound busy airfield (`dev/missions/qvbench_airfield.miz`, F2, about 30 minutes)

Start DCS with `qvbench_airfield.miz` (the bench hook skips the briefing and sets F2). The view is held at yaw 285
(the R124 benchmark view). Each step: write the keys, wait 3 s for the reload line, create `run_suite.flag` in the
dev folder, wait for "==== suite finished ====" in `DcsQvCull.log` (or the three high beeps if Beeps=1).

| Step | Sim | View | Keys (on top of the base) | Suite length |
|---|---|---|---|---|
| A1 | paused (`CMD=52` toggles pause; start paused as R124) | F2 yaw 285 | `[Model] GBufferRecorderCockpit=0`; `[Suite] Label=A1 airfield paused F2`, `Quick=0`, `PassTimelineSec=60`, `FrameStartGap=1`, `FrameStartGapSec=20`, `Profile=1`, `FxApplyCount=1` | about 4.5 min |
| A2 | paused | F2 yaw 285 | `[Suite] Label=A2 tablebits`, `Quick=1`, `PassTimelineSec=0`, `FrameStartGap=0`, `Profile=0`, `FxApplyCount=0`, `BenchPrefetchTableBits=1` | 16 x 10 s, about 3 min |
| A3 | **running** (`CMD=52`) | F2 yaw 285 | `[Suite] Label=A3 airfield unpaused F2`, `BenchPrefetchTableBits=0`, `PassTimelineSec=60`, `FrameStartGap=1`, `Profile=1` | about 2 min |
| A4 | paused (`CMD=52`) | F1 (`CMD=7`), `HoldYawDeg=0` | `[Suite] Label=A4 airfield cockpit F1`, `PassTimelineSec=60`, `FrameStartGap=0`, `Profile=0` | about 1.5 min |
| A5 | paused | F1, yaw 0 | `[Model] GBufferRecorderCockpit=1`; `[Suite] Label=A5 cockpit recorder`, `PassTimelineSec=0`, `GBufferRecVerify=1`, `BenchGBufferRecorderCockpit=1`; `[Bench] PassMs=1`, `BlockSec=40` | verify 6 s + 16 x 40 s, about 11.5 min |

After A5 set `[Bench] PassMs=0`, `BlockSec=10`, `[Suite] GBufferRecVerify=0`, `BenchGBufferRecorderCockpit=0`.
Quit DCS normally (not by killing it).

Notes:
- A1-A3 run with the cockpit part of the recorder off (the shipped state); the pool keeps its cockpit shape.
- A5 is judged only if the cockpit view is render-thread-bound: see the validity gate under (b). If it is not
  (render thread busy under 90 %, or GPU utilisation 90 % or more), repeat A5 once with the periphery resolution
  lowered in the profile (a profile change needs a DCS restart).
- A heavier cockpit ramp (`qvbench_f15e_ramp.miz` or `qvbench_a10c2_ramp.miz`, F1) is the fallback for A4/A5 if
  the airfield cockpit view is GPU-bound.

## Mission B: dense map (`dev/missions/qvbench_f4_germany.miz`, F2, about 12 minutes)

Start DCS with `qvbench_f4_germany.miz` (Germany, F2). Keep `HoldPose=1`, `HoldYawDeg=0`.

| Step | Keys | Suite length |
|---|---|---|
| B1 | `[Suite] Label=B1 germany counters`, `Quick=1`, `FxApplyCount=1`, `Terrain=1` | about 1 min |
| B2 | `[Suite] Label=B2 chain census`, `FxApplyCount=0`, `Terrain=0`, `ChainCensusSec=60` (`[D3D] ChainFast` as shipped, 7) | about 1.5 min |
| B3 | `[Suite] Label=B3 gpu bundle`, `ChainCensusSec=0`, `GpuBundleTiming=0x1f`, `GpuBundlePairs=4`, `GpuPassTiming=1`, `GpuPassStats=1` | about 1.5 min |
| B4 (optional, someone wears the headset) | `[Suite] Label=B4 chain census gaze`, `GpuBundleTiming=0`, `GpuPassTiming=0`, `GpuPassStats=0`, `HoldPose=0`, `ChainCensusSec=60`; the wearer looks around (eye tracking moves the focus views) | about 1.5 min |

Then reset every key of this plan to 0 (or restore the dev ini backup), quit DCS, collect the files, and turn the
headset software off.

## Files to collect

From the payload's output folder (next to the deployed DLL; with dev mode the log and reports land where earlier
rounds found them): `DcsQvCull.log`, every `report_*.txt`, `bench_*.csv`, `passtl_*.csv`, `profile_*.txt` written
during the session. Copy them into one folder and run:

```powershell
pwsh scripts/parse-live-measurement.ps1 -LogDir <that folder> -OutFile <that folder>\summary.md
```

The parser prints one table per question and the verdict of each rule below. It never decides on its own when a
validity gate fails: it prints INVALID and the reason.

## Where each answer comes from, and the decision rules

### (a) Fresh render-thread profile (A1, A3, A4)

Log lines (inside each step's `report_*.txt`):

- `pass timeline: N frames ...; render-thread pass execution mean X ms, p50, p95, max`
- `pass timeline: render thread CPU X ms/frame (busy Y% of wall time)`
- `    pass timeline <kind> <mean> <p50> <p95> <calls/f>` (top 40 kinds), plus `passtl_*.csv`
- culling timeline rows (`aggregated (render thread)`, `provider collectRenderables`, `collect objects`,
  `sort+batch`), the `-- frame start: ...` section, and `profile_*.txt`.

No GO/NO-GO; this is the ranking input. Rules that follow from it:

- **Main-thread bound check (A3 vs A1):** if unpaused the render thread is busy < 85 % while fps is below the cap
  and GPU utilisation < 90 %, the unpaused mission is limited elsewhere (Main thread): render-thread items lose value
  in flight; record it and weight CPU candidates by the unpaused numbers.
- **B5 (terrain recorder / terrain parse):** reopen only if the terrain passes (`TerrainModule` and the terrain
  G-buffer work) take >= 1.5 ms/frame exclusive in A3.
- **B7 (more culling threads):** retest `CollectThreadsMax` only if the culling rows on worker threads
  (`provider` / `collect objects`) are longer than the render thread's own `aggregated` wait in A1 or A3.
- **A1 CPU half:** the per-eye single pass is worth its CPU estimate only if the focus G-buffer passes cost
  >= 1.0 ms/frame of render thread in A1 (per-call rows in the GPU pass timing of B3 split the views).

### (b) Cockpit G-buffer recorder in render-thread ms (A5)

Log lines: `==== BENCH RESULT: g-buffer recorder cockpit part (R24) ...` then the field rows
`render_thread_cpu_ms/frame`, `render_thread_busy_%`, `gbuffer_pass_ms/frame`, `pass_ms/frame`, `p95_frame_ms`,
`gpu_util_%`; the verify line `verify: ...` of `GBufferRecVerify` before it; `bench_*.csv` has the blocks.

- **Validity gate:** verify reports no difference; OFF blocks' `render_thread_busy_%` >= 90; `gpu_util_%` < 90; no
  `NOTE: frame rate was locked` line. Otherwise INVALID (rerun as described under mission A).
- **GO** (make `GBufferRecorderCockpit=1` the default): `render_thread_cpu_ms/frame` ON - OFF <= -0.3 ms and
  significant, and `p95_frame_ms` not significantly worse.
- **NO-GO:** the change is within noise or smaller than 0.3 ms. Close B3 with the number.

### (c) Effect Applies on a dense map (B1, plus A1 for the airfield)

Log lines: `fx apply: same pass as the previous Apply X/frame (Y%)`, `fx apply: time inside Apply ...`,
`fx apply estimate: same-pass fast path saves about X ms/frame (upper bound Y ms/frame)`.

- **GO** (build A3, the same-pass Apply fast path, verified against the stock Apply like the split filter): estimate
  >= 0.3 ms/frame on Germany F2.
- **NO-GO:** estimate < 0.15 ms/frame. In between: build only together with another render-thread item.

### (d) Focus chain from the periphery chain (B2, B4)

Log lines: `chain census: N frames compared (K calccounters chains per frame; ...)`,
`chain census #i <pass> call #c: objects, instances/frame, non-empty objects/frame`, and
`chain census pair #a >= #b: counts contained in x/y frames (z%), objects over max m, ...`; the GPU pass timing in
the same report (`gpu passes by call order`, `TerrainModule` calls; R34 live: calls #1-#4 are the view chains,
order periphery L, periphery R, focus L, focus R).

- Identify the four view chains: the chains run by the same pass object (`(same pass)` pairs) with the largest
  object count. Periphery L / focus L and periphery R / focus R are the 1st/3rd and 2nd/4th of them.
- **GO** for the 2-3 week rank-3 build: both eye pairs (periphery >= focus) contained in 100 % of >= 100 compared
  frames in B2, and in B4 when it was run; and the focus chains' GPU time >= 0.25 ms/frame together.
- **NO-GO:** any frame with an object over in a view pair at a fixed gaze (LOD or selection is not the periphery's).
- Counts are a necessary condition, not the full set check (instance ids): a GO here opens the build, whose own
  same-frame `ChainDeriveVerify` (R38 plan) is the exactness proof.

### (e) Small exact GPU items as one bundle (B3)

Log lines: `gpu bundle item i: ... NOT BUILT ...`, `gpu bundle block k/n: ON|OFF`, and per block
`gpu passes: X ms/frame exclusive GPU time in passes` and `gpu frame: period X ms (p95 Y)`. B3's last block also
has `GpuPassStats` rows (the cascade, terrain and animator passes the items touch).

- Today: the parser reports the A/A spread of the per-block exclusive GPU ms (mean of ON - OFF per ABBA pair and its
  95 % interval). That interval is the floor for the later rule.
- **Later GO** (once items are built and registered in `mkit::kBundle`): the bundle's ON - OFF <= -0.3 ms and
  outside 2x the A/A interval measured here, with each item's own same-frame compare at 0 differences.

### (f) TableBits=20 p95 in a CPU-bound scene (A2)

Log lines: `==== BENCH RESULT: prefetch repeat table 2^16 vs TableBits (R41 p95) ...`, rows `p95_frame_ms`,
`frame_ms`, `render_thread_cpu_ms/frame`; `prefetch calls during the bench: N, skipped X%`.

- **Keep 20** (close the R41 question): `p95_frame_ms` change within noise or <= +1 %, and
  `render_thread_cpu_ms/frame` not significantly worse.
- **Go back to 16:** `p95_frame_ms` significantly worse by more than 1 % (about +0.3 ms at 30 fps).

## Time budget

| Part | Minutes |
|---|---|
| Preparation (DCS closed) | 15 |
| DCS start and mission A load | 4 |
| A1-A5 | 23 |
| DCS restart and mission B load | 5 |
| B1-B3 (B4 +2 optional) | 4-6 |
| Collect, parse, restore | 10 |
| **Total** | **about 60-70 (about 40 with DCS running)** |

## Rules for the session

- Never start or kill DCS or the app from a script that was not asked for; quit DCS normally between missions.
- Never copy anything into Saved Games\DCS or the DCS install; no Windows or driver settings.
- `[Suite]` keys are engine keys: changing them resets the in-flight kill switch (R142). Nothing here uses it.
- Before a step, check the log for `config: dev mode` and the reload line; after it, for `==== suite finished ====`.
