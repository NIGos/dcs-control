# Release flight matrix

One command flies a fixed set of DCS flights unattended, runs the engine's test suite in each one and compares the
result with the previous release. It exists because a regression got through: the terrain prefetch fix in skip mode
added about +1.3 ms of render-thread time per frame in stock DCS, Germany low level. It shipped from 0.5.5 to 0.7
because only "everything on vs everything off" was measured, and only in a dense ATMOSX scene. The matrix measures
every engine part on its own, in four scenes. TEST-ONLY. A live run needs the headset, DCS and about 55 minutes.

## What is measured

`matrix.json` (version 1) holds 4 scenes. Each one is a single DCS start:

| Scene | DCS | Mission | Suite starts |
|---|---|---|---|
| `atmosx-dense` | ATMOSX as installed | Caucasus, `qvbench_i23_f16_ap_map.miz` (F-16 on ALT HOLD autopilot) | FLY+95 s (the mission opens the F10 map at model time 60-87 s) |
| `atmosx-sparse` | ATMOSX as installed | Germany low level, `qvbench_ff_land.miz` | FLY+30 s |
| `stock-dense` | ATMOSX off | Caucasus, as above | FLY+95 s |
| `stock-sparse` | ATMOSX off | Germany low level, as above | FLY+30 s |

Each flight runs the module's suite through the app's own `StartSuite`. The suite keys come from the engine's
developer mode: a developer ini and payload in the run folder, flown only in the test copy of the profile. The
developer ini is the ini the package installs for the user's profile, plus the keys from `matrix.json`
(`[Suite] Quick=1, WaterSkipTiming=8, BenchSplitFilter=1, BenchPrefetch=1, BenchShadowRecorder=1,
BenchGBufferRecorder=1, BenchEngine=1`, `[Bench] Blocks=16, BlockSec=5, PassMs=1`). The module code is not changed;
these are its existing bench modes:

| Part | Bench mode | OFF arm |
|---|---|---|
| W1 skip hidden water | `WaterSkipTiming` pairs | SkipHiddenWater=0 |
| Split-path D3D11 state filter | 27 | stock |
| Terrain prefetch fix | 33 | observe (hook in place, no skip) |
| Shadow recorder | 28 | stock |
| G-buffer recorder | 29 | stock |
| Engine on vs off (all parts) | 11 | stock |

For each part and scene, `results.json` holds the OFF and ON means, the change in % (each ON block against the mean
of its OFF neighbours, the module's own method), the standard error, the module's 95 % interval, n pairs, n blocks
and the fps cap. It does this for the render-thread CPU per frame, the p95 frame time, process CPU, pass ms and fps
(for W1: render-thread CPU, top pass ms and GPU frame period; W1 has no p95). Everything is computed again from the
module's block CSVs, and the module's printed lines are kept for checking. On the Part 3 data of 2026-11-23 the
recomputed change % and interval match the report to the last digit.

**SE vs the module's "+/-":** the module's report prints a 95 % interval (Student's t times the SE), not an SE. With
8 pairs that interval is 2.36 SE. Numbers quoted as "+- SE" in earlier notes (SHORT-CHECK.md) are these intervals.
The gate uses the true SE.

**Missing bench mode (not added):** the prefetch fix has no A/B of stock vs observe, so the cost of the observe
hook itself is not measured. The engine on/off A/B (mode 11) covers it only together with the other parts. A new bench
mode in the module would close this gap.

FPS is capped (45 = 90/2) in these flights, so render-thread CPU is the measure, not fps. GPU-side costs are not judged.

## How to run

From a normal PowerShell 7 window, not the Claude sandbox, at normal priority (DCS inherits the priority). DCS
Control must be closed.

```powershell
# checks only: matrix, missions (sha256), CLI, capture layer, Pimax tools, ATMOSX backup, the rig runner's own dry run
pwsh -File tools\release-matrix\run-matrix.ps1 -DryRun -Cli <package>\files\DcsVr.Cli.exe
# the live run, gated against the previous release
pwsh -File tools\release-matrix\run-matrix.ps1 -Cli <package>\files\DcsVr.Cli.exe -Label 0.7.0 -Baseline <previous>\results.json
# a subset, for example after a failed scene
pwsh -File tools\release-matrix\run-matrix.ps1 -Cli ... -Scenes stock-sparse,stock-dense
# the gate on its own
pwsh -File tools\release-matrix\compare-matrix.ps1 -Baseline <old>\results.json -Current <new>\results.json
```

Where things are: a parameter, else an environment variable, else a default resolved at run time.

| Parameter | Variable | Default |
|---|---|---|
| `-Cli` | `DCSC_CLI` | the installed app's `DcsVr.Cli.exe` (its `DcsVr.Core.dll` starts the suite) |
| `-OutDir` | `DCSC_MATRIX_OUT` | `<main checkout>\artifacts\release-matrix\runs\<yyyyMMdd-HHmm>` |
| `-MissionDir` | `DCSC_MATRIX_MISSIONS` | `<main checkout>\artifacts\release-matrix\missions` |
| `-TapDir` | `DCSC_RIG_TAP_DIR` | `<main checkout>\artifacts\rig\capture-layer\bin` (`tools\rig\build-capture-layer.ps1`) |
| `-SavedGamesDcs` | `DCSC_SAVED_GAMES_DCS` | `%USERPROFILE%\Saved Games\DCS` |
| `-AtmosxBackup` | `DCSC_ATMOSX_BACKUP` | `<Saved Games DCS>\ATMOSX Backup` (the installer's own backup) |
| `-PimaxCtl` | `DCSC_PIMAX_CTL` | `tools\runtime-probe\bin\pimax_ctl.exe` of this checkout, else of the main one |
| `-PimaxClient` | `DCSC_PIMAX_CLIENT` | `%ProgramFiles%\Pimax\PimaxClient\pimaxui\PimaxClient.exe` |
| `-QuietLock` | `DCSC_QUIET_LOCK` | `<main checkout>\artifacts\.lane\quiet.lock` |
| `-Payload` | `DCSC_MATRIX_PAYLOAD` | the payload the package installs (pass a build to test one) |

The missions are not in the repository: 4.4 + 6.8 MB of binaries, and one comes from the peer repository.
`matrix.json` names each one by file name and sha256. Copy them into the mission folder. The dry run refuses a
mission whose hash differs.

Exit codes: 0 done and gate passed, 4 done and gate failed, 1 a scene failed or something must be put back by hand
(REPORT.md says what), 2 not started (checks failed), 3 no quiet machine within `-QuietWaitMin` (default 60; nothing
changed).

## What a live run does

1. It waits until no build or test runs (cl, link, ninja, cmake, ctest, MSBuild, xrfg*) and no `quiet.lock` exists.
   Then it creates the lock (create-new, never another session's) and removes it at the end only if the text is its own.
2. It records the state: a copy of `%LOCALAPPDATA%\DcsControl` and the user state files, plus hashes of the installed
   app, the state, the ATMOSX-managed files, options.lua, `Saved Games\DCS\Config` and the Scripts tree (logs aside).
   It also records the ATMOSX hashes and a copy of options.lua.
3. It switches Pimax on (service, elevated; PimaxClient minimised), sets the screen-saver key to 0 and waits up to
   300 s for tracking (`slam warning flags: 0`). Without tracking there are no flights. The headset is never rebooted.
4. It applies the user's profile once with the CLI under test and reads the engine ini and payload that this
   installs (read only), as the developer base.
5. It flies the ATMOSX scenes first, then turns ATMOSX off once, from the mod's own backup. This is hash-checked and
   sets visibRange from ATMOSXUltra to Extreme, as in the live rounds. Then it flies the stock scenes. Each scene is
   one call of `tools\rig\run-scenario.ps1` with `-NoLock -KeepPimax -StopOnFailure`: a test copy of the applied
   profile, the capture layer with simulated gaze fixed at 0,0, rig autostart (no click, key or focus), FLY confirmed
   by dcs.log, the suite, DCS closed, the stock profile restored. **A failed or crashed flight stops all flying:** that
   covers a new `dcs.*.crash`, DCS gone, a suite that does not end within 1200 s, FLY not confirmed, or DCS killed at
   close.
6. In `finally`, each step runs on its own: ATMOSX back and checked against the record, the screen-saver key 180
   (read back), the user's profile re-applied (if it was applied before; otherwise restore), the user files compared
   with the start, state hashes compared (expected differences such as `originals\`, `boost\`, shader cache and DCS's
   own InstantActionGeneratorParams.lua / steam_authdata.bin are marked; anything else is UNEXPECTED), Pimax off
   (service procedure, StartType stays Automatic), the lock removed.
7. It writes `results.json`, `REPORT.md` (a table per scene, plus the restore list) and `GATE.md`
   (compare-matrix against `-Baseline`, or the cost rule alone).

It never writes by hand under `Saved Games\DCS\Scripts` (the app installs the module, and the developer files are in
the run folder). It leaves Export.lua as found and changes no Windows, driver, NVIDIA or Pimax Play settings apart
from the screen-saver key. It changes no DCS graphics options apart from ATMOSX's own visibRange value, sends no input
to DCS and never steals focus.

## The gate (`compare-matrix.ps1`)

| Rule | Flags when |
|---|---|
| regression | a part's effect on render-thread ms/frame or p95 frame time (change % ON vs its OFF arm, same flight) grew by more than max(2 x sqrt(SE_baseline² + SE_current²), 3 %) percentage points |
| cost | in the current run, a part's render-thread change vs its OFF arm is above +2 % **and** above 2 SE |
| missing | a scene or part of the baseline, or one listed in the current run, was not measured (failed scene, skipped bench) |

The rule compares the change % (the effect measured inside one flight), not absolute milliseconds between two runs.
Between flights the scene, DCS version and streaming move the absolute numbers by more than the parts' own effects
(Part 3: the same scene's OFF arms ran from 9.9 to 12.3 ms). The absolute ON values are printed next to each finding.
Warnings that do not flag: a different matrix version, flown profile (sha256 without the developer keys) or fps cap.
The limits are in `matrix.json` under `gate` and are copied into each `results.json`.

## Baselines

`baselines/0.7.0-preview-cdf337d-partial.json` is **partial**. It comes from two live flights of 2026-11-23 with the
cdf337d package, not from a matrix run:

- scene `stock-sparse` only;
- split filter, prefetch, shadow recorder, G-buffer recorder and W1: SHORT-CHECK Part 3 (`artifacts/live-07/short2`,
  16 blocks);
- engine on/off: Part 2 C (`artifacts/live-07/short`, a different DCS start, 24 blocks, prewarm held).

Against itself, it flags two **cost** findings, as it should: prefetch skip +13.4 % +- 0.7 SE and engine on vs off
+18.7 % +- 0.8 SE in sparse stock. Those are the shipped regression. The first full matrix run gives a complete
baseline: keep that run's `results.json` next to the release.

## Time

Suite: 5 benches x (16 x 5 s + ~6 s) + W1 8 pairs ≈ 8 min. Per scene, add about 2 min from apply to FLY, the wait
until the suite starts (30 or 95 s) and about 1 min for the close and the restore. Sparse scenes take about 11.6 min
and dense ones about 12.7 min. With 5 min for Pimax tracking, backups and the final restore, the **estimate is about
54 min**, which `-DryRun` prints. The target of about 40 min would need `[Bench] Blocks=12` (about 47 min; SE about
15 % larger) and dropping the dense suite delay (using the mission without the F10 map). Either change is a new
matrix version, so the earlier baselines no longer compare.

## Files

- `matrix.json`: the matrix (scenes, missions with sha256, suite keys, parts, metrics, gate).
- `run-matrix.ps1`: the entry point (checks, dry run, live run, restore, results, report, gate).
- `compare-matrix.ps1`: the gate.
- `matrix-lib.ps1`: the shared code (statistics, report parsing, scene results, gate rules).
- `tests/test-matrix.ps1`: the offline tests (`pwsh -File tools\release-matrix\tests\test-matrix.ps1`; synthetic
  inputs, no DCS).
- `baselines/`: stored results to compare with.
- `tools/rig/run-scenario.ps1` (shared with the rig): `-Cli`, `-NoLock`, `-StopOnFailure`, a mission per arm, the
  actions `suite`, `waitsuite` and `devini`, the module's report and CSV files copied into the flight folder, and
  crash files detected.
