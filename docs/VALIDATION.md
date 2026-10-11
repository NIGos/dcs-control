# Offline validation evidence

> **Historical record.** This page keeps the evidence of earlier releases (0.2-0.4), including screens and counts that have changed since. The current checks run in `scripts/test-release.ps1` and `scripts/verify-web-release.py`.

## Frame pacing and FPS limiter verification (0.4.1, 3 October 2026)

The Release build passes with zero warnings/errors. All 124 manager tests pass, including both runtime routes with preserved, headroom, refresh-matched and fractional custom caps; framegen-off matching; invalid/nonfinite values; missing-field restoration; owned-cap edit rejection; preservation of conflicting user edits; and changed/new autoexec launch guards. Quality tuning and guided stages retain customized pacing choices.

The actual packaged WebView2 passes 126 interaction/bridge checks. The installed application passes 136 with the user-supplied signed DLSS-NR 310.8 runtime. Both capture 48 layouts: seven pages and five wizard steps at four viewport/scaling combinations. The Frame pacing page distinguishes rendered caps from calculated output ceilings, exposes all cap choices, preserves unknown external states, and shows exact Lua values in Preview. Compact and high-DPI layouts retain header/footer actions. Previews use the same profile at each viewport and clear deliberately triggered test errors.

All 40 eligible native OFXR tests pass. A new delayed-application fixture pauses the producer below the simulated 100 Hz runtime's half-rate target and checks continued presenter progress, frame ordering and teardown. The same fixture passes against the exact bundled OFXR 0.2.1 V116 DLL. These simulated runtime checks do not prove physical headset presentation or real DCS limiter placement. The prior combined RTX 5090 GPU correctness evidence remains valid because native rendering binaries and component packages are unchanged.

The self-contained installed package passes application upgrade/repeat install, Windows PowerShell frontend verification, both Pimax/Sboys combined deployments with neural rendering off/on, matched 45 FPS caps at a selected 90 Hz, explicit desktop VSync values, tamper guards, launch checks, selective restoration and uninstall preserving unowned files. No DCS process was launched by these checks; all deployment paths are isolated fixtures.

Fresh read-only inventory reports DCS graphics.maxFPS = 90, graphics.sync = false, and a data-only autoexec.cfg containing HUD_MFD_after_DLSS = true with no max_fps assignment. That harmless setting is reported as a file fact, not a limiter conflict. No RTSS process is detected; NVIDIA/background caps and runtime interpolation remain manual checks. The options file differs from the previous 0.4.0 observation; no test writes to the actual installation. Final verification separately checks options.lua and autoexec.cfg hashes against the refreshed read-only snapshot.

Pimax baseline/combined preparation passes file checks; Sboys still needs the pinned driver instead of 1.3.0-beta.2, explicit tracking selection and a verified OpenXR gaze bridge for eye-tracked focus. Actual DCS hooks, both-eye quality, gaze, latency, dropped/fresh frames and limiter interactions remain for live validation. See [FRAME_PACING.md](FRAME_PACING.md) and artifacts/release-verification-0.4.1.json for scope and evidence.

## Setup and live-test preparation (0.4.0, 3 October 2026)

Release compilation succeeds with zero warnings/errors. The manager suite passes 103/103, adding route/stage quality preservation, invalid choices, Steam library discovery, driver registration/blocking/version detection, missing runtime and gaze rejection, archive import/download digest guards, and transactional removal/reintroduction/rollback of obsolete application files.

The packaged hidden WebView2 passes 104 interaction/bridge checks; the installed release passes 112 with the signed user-supplied neural DLL. Both capture 44 screenshots: six pages plus all five wizard steps at four viewport/scaling combinations. Cancellation preserves the draft, missing prerequisites block wizard progression, headset observations remain unverified, quality precision is retained, and review creates a preview rather than applying. Upgrading 0.3.0 now removes its formerly bundled Sboys ZIP with a recoverable journal. Both runtime routes apply/restore NR-off/on fixtures with exact settings; launch contracts, tamper guards and unowned-file retention pass.

The existing combined GPU correctness fixture was rerun successfully: real DLSS-NR 310.8 -> exact Quad Views shaders -> production OFXR NVIDIA stereo optical-flow synthesis, on RTX 5090. This is an offscreen stage test. Native render binaries and quality defaults are unchanged.

Read-only checks of actual installed files pass Pimax baseline/combined preparation. Sboys has 1.3.0-beta.2, requires a selected tracking route, and has no detected enabled OpenXR gaze bridge. The wizard reports those blockers. Physical gaze, DCS hook/resource mapping, both-eye presentation, latency and cockpit quality remain unverified. Actual options.lua retains SHA-256 946a357e0ae691ac52f34a135c728835cfba097b1a3ae769a7370fbe38e31730.

The release includes component license/notice files and a delivery-policy manifest. Sboys private binaries and NVIDIA neural DLLs are excluded; Sboys is an official pinned download/import. The corresponding-source archive excludes general-purpose vendor SDK build executables/binary libraries and documents obtaining those tools from official pinned distributions. Archive verification checks CRCs, all manifest hashes, source correspondence, nested packages, policy exclusions and unchanged native components.

Native desktop file dialogs, visible-window interaction, clean-machine missing-prerequisite installer branches, external Sboys activation and a live DCS/headset session remain untested. No game or VR driver files were changed during this audit.

## WebView2 migration (0.3.0, 3 October 2026)

The settings UI now renders through real WebView2 runtime 154.0.4258.48 with SDK 1.0.4258.31. The previous WPF settings screens are removed. Native render components and quality defaults are unchanged. Release compilation passes with zero warnings/errors, and the manager suite remains 89/89.

The hidden WebView2 fixture passes 93 interaction/bridge checks, including navigation/search, exact numeric input, inactive controls, every numeric range, reduced motion, quality review/cancel/undo, missing-runtime errors, opening the planned-file list, native-owned preview invalidation and exact path binding. Both Pimax and Sboys combined routes apply and restore through the web command channel. The installed test additionally verifies the supplied real neural DLL through that same interface on both routes, for 101 checks. Native launch contracts are checked without executing DCS.

Twenty-four actual WebView2 screenshots cover all six pages, wide/compact sizes and 100%/150%/200% rasterization. Each checks horizontal overflow and header/footer visibility. Native file dialogs, visible-window interaction, screen-reader usage, minimize/suspend behavior and in-game UI overhead have not been interactively tested. The application installer also checks an upgrade from 0.2.3, exact custom quality settings, Pimax/Sboys NR-off/on deployments, hashes, restoration, tamper rejection and unowned-file retention. All writes target isolated fixtures. Actual DCS and VR drivers remain unchanged. See [INTERFACE.md](INTERFACE.md).

Validation on 2 October 2026, Windows x64, RTX 5090. DCS was not launched and the desktop was not controlled.

| Area | Result | Scope |
| --- | --- | --- |
| .NET solution | Release build, zero warnings/errors | Core, CLI, WPF, fixture tests |
| Manager tests | 79/79 passed in the 0.2.1 audit | Parsing, archives, compatibility, deployment, backups, rollback, signatures, process environment, application lifecycle, selective Lua restoration, both combined runtime routes, early validation, locked active-profile checks, installed launch verification and changed-runtime rejection |
| OFXR full source build | 39/39 eligible tests passed | Core and actual DX11/DX12 paths with fake OpenXR runtime |
| Cheeky source build | 158 eligible regression tests passed; final gaze and mixed calibration rerun passed | WARP graphics, stereo calibration, transport and resource handling; tests without a real neural runtime |
| Focus adapter native policy | Passed | Routing, freshness, ambiguity, copied array slices, invalid bounds/mips, unchanged projection and one-time history transition reset |
| Real adapted layer DLL | Focus test plus hook/input regressions passed | WARP packed and four-slice resources, focus 2/3 selection, invalid layout rejection and disabled adapter |
| Source adaptation reproduction | Passed from pristine pinned source; second run unchanged | All 11 modified/added upstream source files match the built checkout |
| Actual Khronos loader | Four layer-chain cases passed | OFXR; Cheeky + OFXR; Quad Views + OFXR; adapted Cheeky + Quad Views + OFXR |
| Quad Views | Source build succeeded | Private per-process configuration patch |
| WPF presentation | Nine offscreen views and 24 real-control checks | Full and compact layouts, custom identity/precision, all seven presets, selected neural-runtime retention, active/disabled settings, accessible slider updates, preview invalidation and consistent diagnostics; no window shown |
| Packaged application | Install, repeat install/update, and uninstall passed | Isolated destination; self-contained CLI/GUI, both combined apply/restore fixtures with NR off, tampered adapter rejected, user file retained |

Cheeky's mixed calibration initially exceeded its 60-second timeout under concurrent tests. The final isolated rerun passed in 109.22 seconds with a 300-second limit; the final gaze suite passed in 542.87 seconds. The patch/build scripts preserve the increased mixed-test limit and run it separately. This was a timeout recovery, not a claimed graphics fix. Subsequent array-layout changes were checked by the dedicated real-layer focus test and hook/input regressions rather than repeating unrelated long graphics tests.

Tests that show overlay windows or exercise desktop input are excluded. OFXR tray lifecycle tests are also excluded. Other graphics tests use hidden or offscreen fixtures. The loader fixture overrides its runtime and app-data locations and has no Pimax/SteamVR/headset connection. It validates instance creation and two/four-view enumeration, not rendered stereo pixels or frame generation quality.

The manager verifies the original local DCS Lua file by parsing and patching an in-memory copy, then checking the original hash. Deployment tests write to isolated fixtures under `artifacts/tests`. Fault injection verifies rollback after partial writes; conflicting user edits and altered backups are retained rather than overwritten.

The 0.2.1 audit reran 38 OFXR tests in the sandbox plus its isolated registry-lifetime test with permission to use its temporary `HKCU\Software\OFXRBridgeTest` key. All 39 eligible tests passed; the tray lifecycle test remains excluded. The initial registry test reported access denied in the sandbox and passed when rerun with the required access. OpenXR and headset configuration were not changed. OFXR's texture-overlay tests are offscreen; the excluded window/input tests belong to Cheeky's desktop overlay suite.

New launch profiles record hashes of the selected external runtime manifest/library and any alternative provider manifest/library. The packaged `launch-check` path performs the same validation used before an actual launch, without executing DCS. Existing 0.2.0 launch records remain readable but lack these new dependency hashes; restore and reapply to record them. The native rendering components are unchanged in 0.2.1.

## Combined GPU stages and neural controls (0.2.2)

The user's NVIDIA-signed `nvngx_dlssnr.dll` 310.8.0.0 (SHA-256 `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e`) and installed NVIDIA NGX core were executed on the local RTX 5090. The offscreen fixture completed 12 real feature-18 evaluations across two view histories with zero neural failures. Both eyes produced finite changed pixels. It then executed the exact Quad Views projection shaders with lower-resolution peripheral images and generated two stereo pairs through OFXR's production NVIDIA optical-flow synthesizer. Pixel checks reject missing focus content, altered peripheral corners, changed real frames and synthetic passthrough. Both NR processing-order settings and non-default model parameters are exercised. Evidence is in `artifacts/native/combined-gpu/result.json` and its log; [NEURAL_TUNING.md](NEURAL_TUNING.md) explains the scope.

The 0.2.2 manager adds validated RenoDX-style tuning, atomic INI import, advanced-control reset and explicit rejection of neural runtime versions outside the supported 310.8 contract. No neural binaries are included in the release. No actual DCS installation or runtime settings were modified.

The final 0.2.2 checks pass: 83 manager tests, 29 actual WPF control checks, and eleven offscreen interface views. Packaged install/update/uninstall checks include both Pimax and Sboys combined routes with NR off and with the real user-supplied signed NR runtime, verifying its deployed hash, tuning values, launch contract and complete restoration. Those deployment fixtures do not execute the game. A first-flight profile was previewed against the actual DCS executable and the explicitly selected real Saved Games options; its original options hash remained unchanged.

## Performance and quality controls (0.2.3)

The performance audit adds optional tuning presets with exact change previews and undo, plus individual focus resolution, sharpening, edge blending, flow preset and bidirectional controls. Original quality defaults are preserved; the quality-preserving preset changes only OFXR recorder/overlay settings. Native game quality and resolution are not overridden. Manager checks pass 89/89; actual WPF controls pass 40 checks, with fourteen rendered views including full/compact quality panels.

The hardware matrix measures real GPU stages across 48 comparable runs, with 2,880 successful neural evaluations and no steady-state neural codec recreations or neural failures. Texture dimensions reach 6144 pixels per eye for composed stereo. An additional 14-run pass passed correctness but had variable timings and was excluded from cost comparisons. [PERFORMANCE.md](PERFORMANCE.md) records median costs, quality tradeoffs, exclusions and reproducible commands. The separate sharpening pass and complete DCS/OpenXR pipeline are outside those timings. Native render components shipped in 0.2.3 are unchanged from 0.2.2. Packaged install/update/uninstall tests pass, including upgrade from 0.2.2 and custom focus/sharpening/blending/preset/bidirectional settings on both Pimax and Sboys routes with NR off and on. These fixtures verify exact configuration values and restoration without executing DCS.

## What remains unverified

- Real DCS DLL interception and DX11/DX12 transport with the actual DCS input guides. The 310.8 NR ABI is now verified on this GPU/installed driver through the offscreen evaluator.
- Native Pimax Quad Views resource layout and synthetic frame presentation.
- Gaze delivery through the installed Sboys version and selected tracking mode.
- GPU/CPU frame-time improvements, motion latency, stereo comfort, HUD/MFD readability, and focus boundary artifacts.
- Long sessions, real mission loading, runtime restarts, recentering, and multiplayer Integrity Check.
- A full rendered DCS DLSS 5 + framegen + Quad Views session through the new focus adapter. Its routing, layer negotiation and configuration are tested. Real neural and composed/generated pixels are now verified through the offscreen GPU-stage fixture; game mapping, the full XR session and headset presentation remain unverified.
- Neural/native appearance continuity at the focus boundary, and recovery after real gaze or runtime changes.

Build/test logs are under `artifacts/native`, manager results under `artifacts/tests`, and offscreen images under `artifacts/gui`. Final adapter evidence includes `cheeky-final-adapter-tests.log`, `cheeky-array-tests.log` and `loader-final-smoke.log`. Packaging and installer evidence is in `artifacts/release-0.2-build.log`, `artifacts/release-0.2-test.log` and `artifacts/release-tests`. No FPS improvement is claimed from these checks.
