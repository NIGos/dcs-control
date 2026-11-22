# Flight rig (test-only)

Tools that fly DCS unattended with a capture layer in the OpenXR chain and judge what was captured. They are the
only proof of the foveated frame-generation patches (OFXR 0080..0091). Nothing here is shipped, installed or built
into a component.

The rig was moved here from git-ignored scratch in two steps: the runner, five scenarios and the analysis scripts,
then the capture layer's sources with its build script and the two cursor scripts. `RIG.md` is the record written
while the rig was built (dated, scratch paths); this file is the current one.

## What is here

| File | What it does |
|---|---|
| `run-scenario.ps1` | One unattended flight per scenario and arm: applies a profile with the installed DCS Control CLI, flies a copy of it with the capture layer appended, runs the timed schedule, collects logs, runs `verdict.py`, restores |
| `scenarios\hud.json`, `motion.json`, `gaze.json`, `simgaze.json`, `cursor.json`, `cursor-calib.json` | What a flight does and measures: mission, seconds, arms, schedule (`"simGaze"` + `"do": "simgaze"` actions: the simulated eye gaze) |
| `riglib.py` | Reads a capture (shared by the tools below) |
| `verdict.py` | Per capture: interpolation ratio, layer mismatch, seam, alpha, cursor; writes `verdict.txt` / `verdict.json` |
| `compose.py` | What the compositor shows for one captured submission (both layers composed) |
| `hudfind.py` | Finds HUD symbology, or the briefing's FLY button, in a picture; used by the runner |
| `test_autostart_hook.lua` | 19 checks of the engine hook's rig autostart (FLY pressed in DCS's GUI state, no click) against stand-ins for DCS: `<DCS>\bin\luae.exe tools\rig\test_autostart_hook.lua native\dcsqvcull\lua\DcsQvCull.lua` |
| `selftest.py` | 26 checks of `compose.py` and `verdict.py` on generated data: `py tools\rig\selftest.py` |
| `analyze.py` | Numbers of one flight in a time window, from its logs (the engine module's perf lines, the Pimax frame counters, the OFXR flight log) |
| `startup_measure.py` | The start-of-flight stall from an OFXR flight log |
| `late.py` | Per-submission reconstruction from an OFXR flight log, written as a pickle: `py late.py <log> <t0 s> <t1 s> <out.pickle>` |
| `build-capture-layer.ps1` | Builds the capture layer, its harness and the chain shim into `artifacts\rig\capture-layer\bin` and writes the layer's manifest there; `-Test` runs the two offline tests (the harness on WARP with a byte check; OFXR's call-chain test with the layer below OFXR) |
| `capture-layer\rigtap.cpp` | The capture layer: a pass-through OpenXR API layer that, when armed, records what reaches the runtime in `xrEndFrame` |
| `capture-layer\simgaze.h`, `simgaze_harness.cpp` | Simulated eye gaze in the capture layer (`RIGTAP_SIMGAZE=1`, a scenario's `"simGaze"`; programs: fixed, main-sequence saccades between targets, sweeps, blinks, lost, replay of a CSV trace; `gaze.txt` in the tap folder switches the program in flight). The layer answers `XR_EXT_eye_gaze_interaction` itself, so every layer above it (Quad Views, Cheeky, Pupil shift; OFXR through Quad Views' focus) reads it as the runtime's gaze, with or without a tracker. `simgaze_harness` is its offline test (`build-capture-layer.ps1 -Test`, test 0) |
| `capture-layer\rigtap_harness.cpp`, `check_harness.py` | A fake loader and runtime that drive the layer without a headset, and the byte check of what it captured |
| `capture-layer\rig_chain_shim.cpp`, `check_chain.py` | Puts the capture layer below OFXR in OFXR's own call-chain test, and checks the capture taken there |
| `cursor_calib.py` | From a `cursor-calib` flight: where DCS's cockpit cursor is for a place of the mouse in DCS's window; `--write` stores `artifacts\rig\cursor-calibration.json` |
| `cursor_verdict.py` | The smooth-cursor counters of a run's OFXR flight log, per second and per mouse stretch; writes `cursor-verdict.txt` / `.json` in the run folder |
| `bf.ps1`, `bfloop.ps1` | Build targets and run ctest tests of an OFXR build folder under the quiet lock; `bfloop.ps1` repeats one test until it fails and keeps the failing run's work folder |

## Nothing of one machine is in these files

Every location comes from a parameter, else an environment variable, else a default under the repository or a
standard Windows folder:

| Parameter (`run-scenario.ps1`) | Environment variable | Default |
|---|---|---|
| `-StateBackup` | `DCSC_RIG_STATE_BACKUP` | none: required. A copy of the app's state files taken before any test; the profile flown is that copy's draft |
| `-MissionDir` | `DCSC_RIG_MISSIONS` | none: required unless the scenario gives a full path. The folder with the `.miz` files the scenarios name |
| `-RunsDir` | `DCSC_RIG_RUNS` | `artifacts\rig\runs` |
| `-TapDir` | `DCSC_RIG_TAP_DIR` | `artifacts\rig\capture-layer\bin` |
| `-SavedGamesDcs` | `DCSC_SAVED_GAMES_DCS` | `%USERPROFILE%\Saved Games\DCS` |
| `-PimaxCtl` | `DCSC_PIMAX_CTL` | `tools\runtime-probe\bin\pimax_ctl.exe` |
| `-PimaxClient` | `DCSC_PIMAX_CLIENT` | `%ProgramFiles%\Pimax\PimaxClient\pimaxui\PimaxClient.exe` |
| `-PimaxReboot` | `DCSC_PIMAX_REBOOT` | none: `pimax_ctl reboot` is used |
| `-QuietLock` | `DCSC_QUIET_LOCK` | `artifacts\.lane\quiet.lock` of this checkout. A second checkout or worktree on the same machine names the first one's file, so that the machine has one lock (`bf.ps1`, `bfloop.ps1` and `build-capture-layer.ps1` read the variable too) |

`build-capture-layer.ps1`: `-OutDir` (`DCSC_RIG_TAP_DIR`, default `artifacts\rig\capture-layer\bin`, the runner's default
`-TapDir`), `-OpenXrInclude` (`DCSC_OPENXR_INCLUDE`, default the OFXR fork's `external\OpenXR-SDK\include`), `-OfxrBuild`
(`DCSC_OFXR_BUILD`, default `artifacts\native\ofxr-djules75`, for `-Test`), `-TestOut` (`DCSC_RIG_TEST_OUT`). The two
command files and the manifest it writes hold absolute paths: they are build output, under `artifacts\`.

`bf.ps1` and `bfloop.ps1`: `-WorkRoot` (`DCSC_RIG_BUILDS`, default `artifacts\rig\builds`) and `-DcsBin` (DCS's `bin`
folder for the tests that read it; without it `XRFG_TEST_DCS_BIN` is left as the caller has it).

Everything a run writes goes under `artifacts\` (git-ignored): runs, captures, logs, the self-test's output, the
cursor calibration. No capture, log or run folder belongs in `tools\rig`.

## What the runner does to the machine, and how it puts it back

- The app's own files are never edited: a profile is applied with the installed CLI and a copy of it is flown.
- At the end, always: DCS closed, stock restored with the CLI's `restore`, the quiet lock removed.
- The headset's screen-off timeout is changed only with `-Prep`. Its value is read first and that value is put back at
  the end. A value that cannot be read is not changed.
- Pimax Client is closed at the end only when it was not open before the run (and not with `-KeepPimax`).
- A failed flight or run ends with exit code 1, after the cleanup; `steps.log` in the runs folder has the lines.
- If the script is killed nothing restores: run the installed `DcsVr.Cli.exe restore`, set the screen-off timeout
  back to the value `steps.log` shows as "screen-off key before", delete `artifacts\.lane\quiet.lock`.

## What a flight needs, and what is not here yet

- Python through the `py` launcher with numpy, scipy and Pillow (the analysis scripts; `late.py`, `analyze.py` and
  `startup_measure.py` use the standard library only).
- An installed DCS Control (its CLI applies and restores the profile), DCS, the Pimax runtime and client, a headset
  that is tracked, in a lit room.
- `pimax_ctl.exe` (from `tools\runtime-probe`; build it with its `build.bat`).
- The capture layer, built once on the machine that flies: `pwsh -File tools\rig\build-capture-layer.ps1 -Test`
  (MSVC through `scripts\msvc.cmd`; the OpenXR headers of the OFXR fork's checkout; `-Test` also wants a built OFXR
  fork). The runner finds it in `artifacts\rig\capture-layer\bin`; `-DryRun` says so when it is missing.
- The scenarios' missions (`qvbench_i23_f16_ap.miz`, `qvbench_i23_f16_ap_map.miz`) are not in this repository.
- The `cursor` scenario needs `artifacts\rig\cursor-calibration.json`: fly `-Scenario cursor-calib`, then
  `py tools\rig\cursor_calib.py <its run folder> --write`. The calibration belongs to one headset and window size.
  On the one calibration flight flown so far (2026-11-15) the script found fewer than three points for its own fit
  (exit 1, nothing written): the calibration then in use was worked out by hand from that flight's captures.
## Known failing checks

- `crop.hud.position` (`verdict.py`, scenario `hud`) fails in both arms, two layers and twin: the HUD mark of a
  generated frame is found at a real frame's place instead of half way. It failed with the installed build on
  2026-11-15 as well. Not explained yet, so a FAIL of this check alone says nothing about a build.
- `analyze.py` windows the Pimax server log by time of day only. That log runs over several days, so the same
  minutes of an earlier day are counted in (a "Stale" rate far too high is the sign). Until the script takes the
  date as well, count on the lines of the flight's own day.

## Checks that need no flight

```powershell
py tools\rig\selftest.py                                             # 26 checks, 0 failed
<DCS>\bin\luae.exe tools\rig\test_autostart_hook.lua native\dcsqvcull\lua\DcsQvCull.lua   # 19 checks, 0 failed
pwsh -File tools\rig\build-capture-layer.ps1 -Test                   # builds the capture layer; harness and call-chain checks
pwsh -File tools\rig\run-scenario.ps1 -Scenario hud -Both -DryRun    # the plan and what is missing; starts nothing
```

## Starting the mission (FLY)

The runner starts DCS with `DCSC_RIG_AUTOSTART` naming `<run>\autostart.flag`; the engine hook (`DcsQvCull.lua`, deployed by app) then presses the briefing's FLY in DCS's own GUI state (`BriefingDialog.Fly_onChange`), so window focus and on top of DCS do not matter. Off in every other DCS start: no variable, or the file gone (deleted when the stops). The flight counts as started only on dcs.log's `WWT (Main): <aircraft>` line; without it in 45 s the clicks FLY as before (`-ClickFly` goes straight to the click). The hook logs `DcsQvCull (Main): rig autostart: ...`.
