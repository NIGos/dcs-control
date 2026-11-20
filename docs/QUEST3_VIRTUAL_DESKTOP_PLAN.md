# Quest 3 + Virtual Desktop port — plan

Goal: run the full DCS Control VR pipeline (fixed-focus Quad Views + DLSS 5 + OFXR frame
generation) on a Meta Quest 3 streamed through Virtual Desktop (VDXR runtime), RTX 40/50 GPU.
Upstream is NIGos/dcs-control; offered there as a pull request (2026-11-20, see 5.2).

## Starting point (found 2026-10-29)

- The pipeline is a stack of OpenXR layers, not Pimax code:
  `DCS → Cheeky (DLSS 5 on focus views) → Quad-Views-Foveated (4 views → 2) → OFXR → runtime`
  (`docs/APPROACHES.md`). The layers are runtime-agnostic; only the bottom "runtime" slot and the
  setup/checks around it are Pimax- or Sboys-specific.
- Headset route = `RuntimeKind { Pimax, SboysSteamVr }` in `src/DcsVr.Core/Models.cs:6`;
  27 `RuntimeKind.` branches across 9 files.
- Fixed focus already exists (`GazeMode.Fixed`) — Quest 3 has no eye tracking, so this is the mode.
- Quad View sizes are normally read from Pimax Play; a Quest route needs its own values.
- This PC: active OpenXR runtime is VDXR
  (`C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr.json`);
  implicit layer OpenKneeboard is installed (loads above the profile layers).

## Phase 0 — Baseline ✅
- 0.1 ✅ Install .NET SDK 10.0.401 (`global.json`) and the MSVC build tools the native parts need;
  build the app from source unchanged (`scripts/build-release.ps1` / `bootstrap-sdk.ps1`).
  Done 2026-10-29: VS 2026 Build Tools 18.11.2 (winget, C++ workload; MSVC 19.51 / v145, CMake 4.3.1,
  Ninja 1.13.2). Full README chain green; release `DcsControl-0.6.2-preview` (unsigned dev build) in
  `artifacts/release/`, `test-release.ps1` PASS. Needed 4 local fixes (see *Upstream issues found*) and a
  local, git-excluded `nuget.config` (this PC's user NuGet.Config has no package sources).
- 0.2 ✅ Run the app in **Optimizations only** with Quest 3 + VD; note FPS at a busy airfield
  with and without Engine Optimizations. This is the reference for every later phase.
  Done 2026-10-31 (first estimate; map/airfield not recorded). Core Ultra 7 265KF, RTX 5070 Ti,
  DCS 2.9.31.29434, VDXR at 90 Hz, SSW off, all 18 engine optimizations active (module loads fine on VDXR).
  Readings from the module's `perf:` log lines (10 s windows), Alt+Shift+F11 to switch:
  - Heavy view: **off 75–81 FPS (~13 ms) / on 88–89 FPS (~11 ms)**, about +10–17 %; "on" was pinned at
    the 90 Hz ceiling, so the real gain is likely higher. Off windows were short (~10 s, see issue 5).
  - Lighter view: 90 FPS both off and on (refresh cap, no difference measurable).
  - With SSW on, VD locks DCS at 45 FPS (90/2) in both states, so SSW must be off for any A/B.
  - Retest tip: set VD to 120 Hz so "on" is not capped; AltGr counts as Ctrl+Alt (use left Alt).

## Phase 1 — Virtual Desktop route ✅
- 1.1 ✅ Add `RuntimeKind.VirtualDesktop`; detect VDXR from the OpenXR `AvailableRuntimes`
  registry key (same way `WindowsInventory.cs` finds Pimax/SteamVR manifests).
  Done 2026-10-31: `InventorySnapshot.VirtualDesktopRuntime`, `WindowsInventory.VirtualDesktopRuntimeManifest` /
  `IsVirtualDesktopRuntime` / `ActiveOpenXrRuntime`; `DetectActiveRoute` returns VD when `VirtualDesktop.Streamer`
  runs and VDXR is the active runtime (`DetectionSources.ActiveOpenXrRuntime` for tests); `SetupDetection.RouteName`.
  Solution builds clean. Remaining ternaries still route VD into SteamVR logic until 1.2.
- 1.2 ✅ Walk the 27 `RuntimeKind.` branches: give the VD route its own checks (VD Streamer
  installed, VDXR selected in the VD app, no Sboys/Pimax checks), and force `GazeMode.Fixed`.
  Done 2026-10-31: new `HeadsetRoute.cs` (manifest / name / guide per route) replaces the Pimax-else-SteamVR
  ternaries (DeploymentPlanner, Setup, WebBridge). VD checks: `virtual-desktop` warning (not streaming via VDXR),
  `vd-not-found` error, `vd-gaze` error; VD headset reminder text; guide link vrdesktop.net. PimaxNative Quad Views
  becomes bundled on any non-Pimax route; `ResolveFeatures` + Detect force Fixed focus on VD; refresh not read on VD
  (profile value used); CPU Boost note leaves VD Streamer alone. Not done: VD logs in Diagnostics (log path unknown).
  Build clean, 298/298 existing tests pass.
- 1.3 ✅ Replace Pimax Play Quad View values with app-owned focus size/resolution settings for VD.
  Done 2026-10-31: `HeadsetRoute.WithQuest3Focus` = fixed focus 0.55 × 0.45, periphery 0.40, focus 1.0×, edge blend 0.2
  (starting points, tune in Phase 2). `PimaxFovea.UsesPimaxPlay` is false on VD; `ResolveFeatures` forces
  FoveaSource = Profile on VD (values kept); Detect my setup sets the Quest 3 values and skips Pimax Play on VD;
  new preset `vd-quadviews` "Virtual Desktop · Quad Views (fixed focus)". Choosing VD in the UI loads the values in 1.4.
  Build clean, 298/298 tests pass (preset test now expects VD presets to use their own values).
- 1.4 ✅ UI: third route button + guided-setup text; hide Pimax-only helpers.
  Done 2026-10-31: "Virtual Desktop" route button (full width, live dot when VD runs) and guided-setup option
  "Virtual Desktop (Quest 3, VDXR) · not tested yet". (Renamed "Quest 3 VD" in 4.3; since 0.6.6 the route switch
  is a bar at the top of the window, where the button now sits.) `ROUTES` table in app.js names routes/ids/software;
  `switchRoute` loads the Quest 3 focus values (`QUEST3_FOCUS`, same as C#) on entering VD and goes back to Pimax Play +
  eye tracked on leaving it. Hidden on VD: focus movement, focus source, read refresh from headset, desktop Tobii pause
  (C# `UsesTobiiPause` is also false on VD); refresh help says to match the VD app's rate. Quad Views / DLSS 5 / framegen
  show "Not tested on Virtual Desktop yet". Smoke: 4 route buttons + 3 VD checks. C# 298/298; web smoke (Debug build)
  passes 190 checks incl. the VD ones, then stops at a Pimax check that needs the native components of a release build
  (full run in 1.5).
- 1.5 ✅ Unit tests for VD detection and the VD route's checks; full build + `test-release.ps1`.
  2026-10-31: 3 new tests (route detection + manifest; fixed focus / own values / no Pimax-only parts / preset /
  vd-not-found; readiness warning + route match + Detect my setup ignoring Pimax Play) — 301/301 pass.
  Full `build-release.ps1` + `test-release.ps1` PASS (all native suites, web smoke 483/483 incl. the 3 VD checks).
  First run failed R94 SymbologyTrace (far depth HDR sequence: 16 GPU copies still pending) because DCS was running;
  passed with DCS closed. Close DCS before any release build.

Decisions (2026-10-31):
- Git: commit the 4 local build fixes + this plan on `main` ("local fixes"), then all VD work on
  branch `quest3-vd`. Local only: nothing is pushed to GitHub (done: `8657aaa` on main). Superseded 2026-11-20,
  see 5.2.
- VD route shows all VR features (Quad Views fixed focus, DLSS 5, framegen), each labelled
  "not tested on Virtual Desktop yet"; Phases 2–4 verify them one by one.
- "Optimizations only" is `VrProfile.Desktop`, not a `RuntimeKind` — keep it that way.
- Watch the ternaries `Runtime == Pimax ? … : SteamVR…` (Setup.cs:51/70/80, DeploymentPlanner.cs:51,
  WebBridge.cs:847, SetupDetection.cs:88): VD would silently fall into the SteamVR branch.
- Route detection: `SetupDetection.DetectActiveRoute` (process + log based) and
  `WindowsInventory` (`ActiveRuntime` registry value, line ~81). UI: `src/DcsVr.App/ui/app.js`, `setup.js`, `index.html`.

## Phase 2 — Fixed-focus Quad Views on VDXR ✅
- 2.1 ✅ Launch with only Quad-Views-Foveated (no Cheeky, no OFXR) on VDXR, fixed focus.
  Done 2026-10-31: VD route + Quad Views + Engine Optimizations; `dcs.log` Available Layers lists
  `XR_APILAYER_MBUCCHIA_quad_views_foveated` (with OpenKneeboard and VD's oculus_compatibility layer).
  Quad Views log: "Eye tracking is not supported" (fixed focus used); focus 1162x1752 (1.0x),
  periphery 844x922 (0.4x), composited 2112x2304 per eye.
- 2.2 ✅ Check in headset: sharp centre, soft edges, both eyes aligned; `dcs.log` "Available Layers".
  Done 2026-10-31: with 0.55 x 0.45 the lower centre (instruments) was too fuzzy, sides fine. Vertical FOV raised
  to 0.76 in the app: good, no double image. Default Quest 3 focus height is now **0.75** (C# `WithQuest3Focus`,
  UI `QUEST3_FOCUS`, smoke + test). FPS not recorded. Alternative kept in reserve: Quad Views'
  `vertical_fixed_offset` (-0.5..0.5) could move the focus down instead of making it taller (not exposed in the app).
- 2.3 ✅ Check VD's own settings don't fight it (VD foveated encoding, VD frame-rate
  "Synchronous Spacewarp" — note which to turn off).
  Done 2026-10-31: no interference seen in the headset. Keep VD's SSW off (it locks DCS at half refresh, see 0.2).

## Phase 3 — DLSS 5 on the focus area ⬜ (skipped by choice: no signed runtime)
Decision 2026-10-31: no unsigned or patched `nvngx_dlssnr.dll` is used (it runs inside DCS for every DLSS 5
flight). DLSS 5 is optional; Quad Views + frame generation + optimizations work without it. Revisit only if a
validly NVIDIA-signed copy turns up.
On hold 2026-10-31 until a validly NVIDIA-signed `nvngx_dlssnr.dll` 310.8/310.9 is available
(e.g. from a game that ships DLSS 5, such as NBA 2K27). Checked and not used:
- Not on this PC (drives C/D/F/G/J, NVIDIA driver 2026-10-20, NGX cache, DCS install, game folders).
- Not in NVIDIA's public DLSS SDK (github.com/NVIDIA/DLSS 310.9.16: only dlss, dlssd, dlssg).
- RHI (RankFTW/rhi-repo `dlssnr-310.9.16`): 310.9.16 x64, **no signature at all** ("DVS PRODUCTION").
- DLSS 5 Swapper 2.2.9 portable: 310.8.0.0, NVIDIA signature **broken (HashMismatch)**, the
  community compatibility patch; SHA256 `8270B350CD82DE5CE89806872CDD6B6A9249B80836B91BBEB3573470744CC206`.
  Its other NVIDIA files (dlss/dlssd/dlssg 310.9.1, Streamline 2.13) are validly signed.
- The RTX 5070 Ti does not need the patch; DCS Control's Browse would accept either copy, so the
  signature check above is the safety gate.

- 3.1 ⬜ Add Cheeky (needs a validly signed `nvngx_dlssnr.dll`, contract 310.8/310.9).
- 3.2 ⬜ Confirm via the headset panel / "DLSS 5 on the focus area" report that it reaches the
  focus views on VDXR.

## Phase 4 — Frame generation (OFXR) ⬜
- 4.1 ✅ Add OFXR below Quad Views on VDXR; headset test with Quad Views + Frame generation (NVIDIA), defaults.
  Done 2026-10-31 at **120 Hz** (VD app + DCS Control refresh 120, Match refresh, Only when needed), release
  0.6.2-preview: `dcs.log` lists `XR_APILAYER_XRFrameBridge_diagnostic` (OFXR Bridge) with Quad Views on
  VirtualDesktopXR; OFXR frame period 8333 us. Flight 1 (22 min): 1x 5 %, 2x 95 %, 3x 0 %, DCS frame time
  median 6.7 ms. Flight 2 (15 min, heavy): 1x 11 %, 2x 89 %, 3x 0 %, median 20.6 ms / p90 27.7 ms.
  In the headset: smooth, no ghosting or wobbly edges, better than Phase 2. Note: the refresh slider was
  at 144 Hz (not a Quest 3 rate) and had to be set to 120 by hand; the greyed Custom limit (45) is unused.
  The default "Only when needed" (adaptive cadence) keeps the DCS frame limit at the full VD refresh rate
  (90 at 90 Hz) and generates frames only while DCS falls behind; "half the refresh rate" applies only with
  adaptive cadence off. Code done 2026-10-31: `FramePacing.Checks` on VD says "in the Virtual Desktop app"
  (refresh-rate) and "turn off Synchronous Spacewarp (SSW)" (runtime-reprojection) instead of Pimax Play /
  SteamVR; test added, build clean, 301/301 pass. (Text change only; the headset test above stands.)
- 4.2 ⬜ Compare against VD's Synchronous Spacewarp (SSW); keep whichever is smoother, document it.
- 4.3 ✅ Tailor the route to Quest 3 (2026-10-31, from the 4.1 test):
  - Renamed the route "Quest 3 VD" (button, profile names, preset, Detect, checks); "Virtual Desktop" stays where
    the app itself is meant (VD app, Streamer, SSW). Internal id `VirtualDesktop` / `vd` unchanged.
  - Refresh: VD slider 72-120 Hz (`QUEST3_REFRESH`), switching to VD snaps to the nearest Quest 3 rate (144 → 120);
    C# `vd-refresh` error outside `ProfileValidation.VdMin/MaxRefreshHz` (72-120).
  - DLSS 5 off on VD (Phase 3 decision): C# + JS `ResolveFeatures` clear `NeuralRendering`, `vd-dlss5` error,
    "Use DLSS 5" greyed out; the DLSS row on VD turns on Foveated Super Resolution instead (stereo only, untested)
    and is disabled with Quad Views.
  - Pupil shift / Depth boost greyed out on VD (they need eye tracking; `UsesPupilShift` was already false).
  - "Not tested on Virtual Desktop yet" notes removed for Quad Views and frame generation (tested).
  - C# tests added, build clean, 301/301 pass; smoke checks added (run in the release build).

## Phase 5 — Wrap-up ⬜
- 5.1 ⬜ User guide section "Quest 3 / Virtual Desktop".
- 5.2 ✅ Offered upstream: NIGos asked for a pull request (#13). Opened 2026-11-20 from a fork, as one commit on
  top of 0.6.6 (merged and re-checked: build clean, C# tests pass). NIGos reviews the code and runs the tests;
  in-headset testing stays with Mav87th, and the route is marked community-tested in the README.

## Contact with NIGos
- 2026-11-01: offered the Quest 3 VD route (fork + pull request, only if he wants it):
  [NIGos/dcs-control#13](https://github.com/NIGos/dcs-control/issues/13).
- NIGos replied yes: pull request for the route, build/test issues as separate issues.
- 2026-11-20: pull request opened; the build/test issues below are reported separately.

## Upstream issues found (fixed locally, report to NIGos)
1. `scripts/build-ofxr-djules75.ps1`: the Ninja toolchain patch appends `/machine:x64` to an empty
   `CMAKE_EXE_LINKER_FLAGS` (gives `" /machine:x64"`) and again on a rerun (`"/machine:x64 /machine:x64"`);
   FidelityFX's exact-match check then fails with `Unsupported target platform ""` (CMake 4.3.1).
   Fix: add it only when missing, `string(STRIP ...)`.
2. (Fixed upstream in 0.6.6.) `native/dcsqvcull/test/test_main.cpp:670`: "QVFR smoothing parsed" reads the real
   `%LOCALAPPDATA%\Quad-Views-Foveated\Quad-Views-Foveated.log`; fails on a PC where Quad Views never ran.
   Fix: skip (`QvSkip`) when no log.
3. `src/DcsVr.Core/Diagnostics.cs` redactor: the user name is replaced before the PC name, so a PC named after
   its user (e.g. `ALEX-PC` for user `alex`) becomes `<user>-PC`, not `<pc>` (no leak, but the test fails). Fix: PC name first.
4. (Fixed upstream in 0.6.6.) `scripts/test-release.ps1:105`: still expects a 45 FPS cap; since adaptive cadence became the default
   (c661ed9) the cap is the full refresh (90). Fix: expect 90 when `usesAdaptiveCadence`.
5. Engine Optimizations **test suite** (Run test suite / Ctrl+Alt+F11) only counts Quad Views frames;
   on a plain stereo runtime (VDXR, 2 views) it aborts after 2 blocks ("0 quad-view frames … nothing to
   compare"). Not fixed locally. Suggest: fall back to whole-frame timing when no focus/peripheral pairs.
   Related: the suite is bound to exactly Ctrl+Alt+F11, so AltGr+Shift+F11 (meant as the Alt+Shift+F11
   kill switch) starts the suite when Shift is released first (`main/engine.inc` ReadKeys).
6. DLSS 5 runtime has no source: `docs/USER_GUIDE.md` / `SETUP.md` ask for "your own signed NVIDIA
   `nvngx_dlssnr.dll` 310.8" but say nowhere where to get it. It is not in the NVIDIA driver, not in the
   public DLSS SDK (github.com/NVIDIA/DLSS 310.9.16 ships only dlss, dlssd, dlssg) and not in DCS. The copies
   the community tools hand out are not validly signed: DLSS 5 Swapper 2.2.9 = 310.8.0.0 with a broken NVIDIA
   signature (compatibility patch), RHI `dlssnr-310.9.16` = no signature at all. A signed copy seems to come
   only from a game that ships DLSS 5 (e.g. NBA 2K27). Not fixed locally. Suggest: name a legitimate source
   in the guide, and on RTX 50 cards warn when Browse picks a patched/unsigned file (the patch is only for
   RTX 20-40, so there it just means an unverifiable file).
   NIGos's own repos (dcs-control, dlss5-bridge, dlss5-d3d12-fix, ngxGym; files + all release assets, checked
   2026-10-31) do not host it either: their READMEs say it is "shipped with that add-on", i.e. the community
   RenoDX `renodx-dlss5.addon64`, the same chain as RHI / Swapper. dlss5-bridge also notes that drivers
   32.0.16.1664/1686 with 310.8.0.0 fault (`ngx_loader` option); this PC runs 32.0.16.2101.

7. `native/dcsqvcull` test "R94 SymbologyTrace (far depth HDR sequence)" is flaky: it failed (2 checks: "the tone
   map's source learnt through the draw arm (pre_same 1)" and "no draw hook and no trace arm left") in 2 of 3
   `build-release.ps1` runs on 2026-10-31, passed on a rerun, with no native code changed. Not fixed locally.

Also seen: OFXR pacing test `adaptive_cadence_jitter` failed 3/3 once with background load (Edge etc.),
passed on the next run. Keep the PC idle when running `build-ofxr-djules75.ps1`.

## Risks
- Only Mav87th can test in the headset (NIGos has no Quest 3); every phase ends with a VR check, not an offline one.
- VDXR may not like some layer combinations; a crash at instance creation means a layer-order problem.
- Single player only (upstream hasn't tested MP).
