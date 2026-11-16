# Flight rig (test-only)

Tools that fly DCS unattended with a capture layer in the OpenXR chain and judge what was captured. They are the
only proof of the foveated frame-generation patches (OFXR 0080..0089). Nothing here is shipped, installed or built
into a component.

This is the first step of moving the rig out of git-ignored scratch: the runner, five scenarios and the analysis
scripts. `RIG.md` is the record written while the rig was built (dated, scratch paths); this file is the current one.

## What is here

| File | What it does |
|---|---|
| `run-scenario.ps1` | One unattended flight per scenario and arm: applies a profile with the installed DCS Control CLI, flies a copy of it with the capture layer appended, runs the timed schedule, collects logs, runs `verdict.py`, restores |
| `scenarios\hud.json`, `motion.json`, `gaze.json`, `cursor.json`, `cursor-calib.json` | What a flight does and measures: mission, seconds, arms, schedule |
| `riglib.py` | Reads a capture (shared by the tools below) |
| `verdict.py` | Per capture: interpolation ratio, layer mismatch, seam, alpha, cursor; writes `verdict.txt` / `verdict.json` |
| `compose.py` | What the compositor shows for one captured submission (both layers composed) |
| `hudfind.py` | Finds HUD symbology, or the briefing's FLY button, in a picture; used by the runner |
| `selftest.py` | 26 checks of `compose.py` and `verdict.py` on generated data: `py tools\rig\selftest.py` |
| `analyze.py` | Numbers of one flight in a time window, from its logs (the engine module's perf lines, the Pimax frame counters, the OFXR flight log) |
| `startup_measure.py` | The start-of-flight stall from an OFXR flight log |
| `late.py` | Per-submission reconstruction from an OFXR flight log, written as a pickle: `py late.py <log> <t0 s> <t1 s> <out.pickle>` |
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
- `pimax_ctl.exe` (from `tools\runtime-probe`, not tracked yet).
- **The capture layer is not promoted yet.** Its sources (`rigtap.cpp`, harness, chain shim, build script) are still
  in scratch; build it there and pass its `bin` folder with `-TapDir`. Until it is here the runner cannot fly from a
  fresh clone: `-DryRun` works and says what is missing.
- The scenarios' missions (`qvbench_i23_f16_ap.miz`, `qvbench_i23_f16_ap_map.miz`) are not in this repository.
- The `cursor` scenario needs `artifacts\rig\cursor-calibration.json`, made from a `cursor-calib` flight by a script
  that is not promoted yet (`cursor_calib.py`); `cursor_verdict.py` (the cursor counters of the flight log) is not
  promoted either: a scenario with `verdict.cursorLog` skips that step and says so in `steps.log`.

## Checks that need no flight

```powershell
py tools\rig\selftest.py                                             # 26 checks, 0 failed
pwsh -File tools\rig\run-scenario.ps1 -Scenario hud -Both -DryRun    # the plan and what is missing; starts nothing
```
