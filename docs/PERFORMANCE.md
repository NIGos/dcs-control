# Performance and quality controls

The performance audit keeps original quality defaults. No actual DCS settings, clocks, power limits or VR drivers were changed during the audit. There are no quality presets (the optional starting points of earlier versions were removed in 0.4.2): every quality setting is edited directly on its feature's page and saved in the profile. Apply and launch are separate actions.

## Individual controls

- **Quad Views / DLSS 5 / Frame rate:** focus width/height, peripheral resolution, neural intensity, neural working scale, before/after DLSS SR placement and optical flow resolution (50/75/100%).
- **Quad Views:** focus resolution (0.5–2×), sharpening (0–100%), edge blending (0–50%), Fast/Medium/Slow flow and bidirectional flow. Zero sharpening skips the provider's additional sharpening pass. A lower neural intensity changes visual strength; it does not reduce neural inference work.
- **DLSS 5 (Advanced image controls):** tone, structure, skin response, mask, UI correction, colour/HDR reconstruction and depth/motion inputs. See [NEURAL_TUNING.md](NEURAL_TUNING.md).
- **Frame rate (In-headset diagnostics):** diagnostic panel key and start state, OFXR FPS counter and OFXR log.

DCS's DLSS Super Resolution quality, render resolution, pixel density, textures, shadows and other game settings remain editable in DCS. The manager preserves their current values. Changing a profile control invalidates an existing file preview, requiring a fresh validation before apply. Settings take effect after applying the profile and restarting DCS.

The coverage preview on the Quad Views page approximates the bundled provider's advertised pixel count:

`peripheralScale² + min(foveaWidth, 0.9) × min(foveaHeight, 0.9) × focusScale²`

The original combined profile is approximately 81% of native stereo pixels before game DLSS SR. This includes overlapping peripheral/focus images. Runtime sizing, distortion, rounding and workload can differ; this figure is neither frame time nor predicted FPS. The original provider caps focus coverage at 90% per axis. It is not shown for Pimax native or alternative providers.

## Measured GPU stage costs

Local RTX 5090, driver 617.14, real NVIDIA-signed DLSS-NR 310.8.0.0 and installed signed NGX core. Sequential process runs, six warmup frames and 24 measured frames per run; each row below is the mean of two run medians, with run order reversed on the second pass. Dimensions are square texture dimensions **per eye**, not headset recommendations. This is a synthetic offscreen workload, not a DCS benchmark.

| Focus / composed stereo dimensions | Neural scale | Flow resolution / preset | Bidirectional | GPU stage sum |
| --- | --- | --- | --- | --- |
| 1024 / 2048 | 75% | 50% / Medium | Off | 6.13 ms |
| 1024 / 2048 | 65% | 50% / Medium | Off | 6.02 ms |
| 1024 / 2048 | 75% | 50% / Fast | Off | 5.98 ms |
| 1024 / 2048 | 65% | 50% / Fast | Off | 5.75 ms |
| 1024 / 2048 | 75% | 50% / Medium | On | 6.86 ms |
| 1024 / 2048 | 75% | 100% / Slow | On | 15.49 ms |
| 2048 / 4096 | 75% | 50% / Medium | Off | 10.89 ms |
| 2048 / 4096 | 75% | 50% / Fast | Off | 10.36 ms |
| 2048 / 4096 | 65% | 50% / Fast | Off | 9.64 ms |
| 2048 / 4096 | 50% | 50% / Fast | Off | 8.68 ms |
| 2048 / 4096 | 100% | 100% / Slow | Off | 31.46 ms |
| 3072 / 6144 | 75% | 50% / Medium | Off | 20.68 ms |
| 3072 / 6144 | 75% | 50% / Fast | Off | 19.30 ms |
| 3072 / 6144 | 65% | 50% / Fast | Off | 17.36 ms |
| 3072 / 6144 | 50% | 50% / Fast | Off | 14.90 ms |

The sum includes two real neural histories, the exact Quad Views projection/composition HLSL shaders and production OFXR stereo optical-flow synthesis. It excludes DCS scene rendering, game DLSS SR, DX11 transport, the provider's separate sharpening pass, full OpenXR composition/session work, headset presentation, frame pacing, initialization, uploads/readbacks and fixture fence waits. The fixture feeds procedural textures through repeated neural evaluations; it does not evaluate moving aircraft, cockpit legibility, depth correctness or perceived quality. Never convert these sums into DCS FPS or headset latency.

The comparable set consists of 48 runs / 2,880 successful neural evaluations, with zero neural failures and zero steady-state neural codec recreations. Real-frame output remains bit-identical to the composed source; finite neural output and stereo flow timing are checked. An additional 14-run quality pass also passed correctness checks, but had substantial timing variation and was excluded from the comparison table. Background load and dynamic GPU clocks can affect results; no external programs were stopped or global GPU settings changed. Smaller reductions at low resolutions overlap measurement variation, so lower scale should not be assumed to deliver a useful quality/performance tradeoff in every case.

The recorded per-run timings and GPU state snapshots are in [performance-results.json](performance-results.json).

## CPU Boost

CPU Boost is an optional, off-by-default measure that only changes the priority and CPU affinity of *running* processes while DCS is alive, and reverts them when DCS exits. It never edits game settings, system settings, clocks or power limits, and it adds no Microsoft Defender exclusions.

What it does, per the profile:

- **DCS** keeps every CPU and is raised to the chosen priority (Normal, Above normal or High; default Above normal).
- **VR runtime and headset services** (Pimax route: `pi_server`, `pi_overlay`, `PiPlayService`, `pi_vst`, `PimaxClient`, `platform_runtime_*_service`, `Tobii.Service`) are confined to the common (non-render) cores. Affinity only; their priority is left alone so the compositor keeps up. On the Sboys/SteamVR route the OpenXR compositor and `vrserver` are left untouched — confining them has not been shown to be safe.
- **Background apps** listed in the profile are confined to the slowest cores at below-normal priority.
- The old "apps to close" list is now **Free VRAM before flight** (below); profiles that used it keep closing the same apps.

The core ranking comes from DCS's own CPU classification in `dcs.log` (the CPPC performance classes, and the `render`/`common` core sets). "Render cores" are left entirely to DCS; the "common" cores take the VR runtime; the lowest performance classes (at least two logical CPUs, never all of them) take background apps. On a homogeneous CPU, or any machine where `dcs.log` carries no usable ranking, CPU Boost falls back to the Windows scheduler's CPU sets; if there is still no ranking it changes **DCS priority only** and nothing is moved between cores, and the plan says so.

The helper runs as its own process (`DcsVr.Cli boost`), started automatically when a boosted profile launches DCS, so it outlives the app. It waits for the real DCS (Steam first starts a short-lived `DCS.exe`; the helper takes one that has been alive at least 20 seconds), rescans every 15 seconds for processes that start later, and restores exactly what it changed — matched by PID, name and start time, so a recycled PID is never touched. It never confines a process that launches DCS or any of DCS's ancestors, because a child inherits its parent's affinity and that would confine DCS itself. It writes `%LOCALAPPDATA%\DcsControl\boost\status.json` and `boost.log` so the app can show, after the fact, what was changed, restored or failed.

Measurements from the script prototype this feature is based on (Ryzen 7 9800X3D, RTX 5090, VR with quad views; `render cores: {4, 5, 6, 7, 8, 9, 2, 3}`, `common cores: {10, 11, 0, 1, 12, 13, 14, 15}`): the priority and core changes were never measured on their own. The prototype once confined Steam by mistake, so DCS inherited four CPUs and a heavy mission fell from about 67 to 52 FPS; this build never confines launchers or ancestors, and DCS always keeps every CPU.

### Flight helpers: VRAM and the desktop

Three more options sit on the CPU Boost page, each off by default and each usable without CPU Boost. The helper is started when any of them (or CPU Boost) is on.

- **Free VRAM before flight** closes listed programs when DCS starts, so the video memory they hold goes to DCS. Default list (process names, editable, `*`/`?` wildcards): `NVIDIA Overlay`, `msedge`, `ChatGPT`, `HueSync`, `RazerCortex`, `RazerAppEngine` (Razer Synapse 4), `wallpaper64`, `wallpaper32` (Wallpaper Engine). Voice chat (Discord) and recording (OBS) are deliberately not listed; `nvsphelper64` (NVIDIA's capture helper) is not either, so Instant Replay keeps working. Measured on the development PC (RTX 5090, 3840×1600 desktop) with GPU Process Memory\Dedicated Usage: NVIDIA Overlay about 1.4 GB, Hue Sync 250 MB, Razer AppEngine 90 MB, Edge 80–130 MB. Each program gets a normal close request (its main window, or WM_CLOSE to its windows); helper processes of a listed program close with it. After 5 s a program still running is ended only with **End programs that do not close**; otherwise it keeps running. The same protections as CPU Boost apply: never DCS, its ancestors, launchers (Steam, Explorer, Epic, the DCS launcher and updater, this app), `dwm`, `audiodg` or other system processes, whatever the list says. No program setting (NVIDIA App, Instant Replay) and no Windows setting is touched. Each closed process's path and command line are recorded first; with **Reopen after the flight** (on by default when the option is used) each program is started once more when DCS exits, as the signed-in user and never elevated (through the shell, or with Explorer's token when the helper runs elevated), unless it is already running again (NVIDIA App restarts its overlay itself). Chromium/CEF helpers (`--type=…`) are never started on their own. What Boost will do shows each program, its running processes and their VRAM now, and the total. The per-process counters can count memory shared between programs twice, so the figures are approximate.
- **Small DCS window in VR** sets `graphics.width = 1280`, `graphics.height = 720`, `graphics.fullScreen = false` (and `graphics.aspect` to 16:9 when DCS stores one) in `options.lua`, as owned settings like the frame limit: turning it off or Back to stock DCS puts your values back. The headset image is rendered at the headset's resolution either way; DCS and Windows only keep smaller desktop buffers, which saves some VRAM and GPU time on the mirror window.
- **Lower the monitor while flying** switches the primary monitor to the chosen mode (only modes the monitor reports are offered; 1920×1080 at 60 Hz is suggested) when the helper starts with DCS, using `ChangeDisplaySettingsEx` without `CDS_UPDATEREGISTRY`, so Windows never saves it and a restart also brings the saved mode back. The exact previous mode (resolution, refresh, colour depth, position) is set back when DCS exits, on Ctrl+C, console close, logoff or shutdown, on an unhandled error, and at the next app or helper start when `boost\display-mode.json` shows a change that was not undone. If the monitor no longer shows the flight mode (you changed it), it is left as it is. HDR is not changed: turning it off through the DisplayConfig API is a setting Windows keeps, so it could not be made as safe as the mode change.

### Terrain prefetch fix (Engine Optimizations)

DCS's terrain workers (edterrain4 through edCore) call `PrefetchVirtualMemory` in a tight loop over the same ranges: about 840,000 calls per second, 1.71 CPU cores of kernel time in the prototype's trace. Prefetching is only a hint to the memory manager, so skipping a repeat changes no results; the pages are faulted in on first access instead. With the fix in Skip mode the prototype measured 99.5 % of calls skipped and 0.06 cores spent, about +1 to 1.5 FPS (3–5 %) in a CPU-bound scene and no change when GPU-bound. DCS was stable; only single-player was tested.

Since 0.5.5 the fix is part of the DCS engine optimizations module (`native/dcsqvcull/src/prefetch_fix.h`), which DCS loads itself from `Saved Games\DCS\Scripts`; nothing goes to DCS's `bin` folder any more (until 0.5.4 it was a separate DLL deployed as `bin\dxgi2.dll` behind the Cheeky `dxgi.dll` loader). Nothing is injected from outside and no code is patched: edCore looks the function up with `GetProcAddress`, so the fix replaces stored copies of that function pointer in DCS's own modules and their `GetProcAddress` import, and puts them back when it is switched off. Its mode is `[Prefetch] Mode` in `DcsQvCull.ini`, written from the Engine Optimizations page and re-read every second: Off, Observe (count only) or Skip (drop a range already prefetched within `WindowMs`, the module's default of 5000 ms; a thread that keeps hitting skips back to back yields one scheduler tick every 256 of them). It works however DCS starts (the app, Steam, a shortcut), and the in-flight engine switch turns it off with the rest. Statistics go to `DcsQvCullPrefetch.log` next to `DcsQvCull.log` every 10 s: calls per second, the share skipped, and how many pointers and imports were redirected. If DCS keeps the function somewhere the fix cannot see, those counts stay at zero and the fix has no effect. In a measured A/B DCS used about 4 % less CPU with Skip, more on CPU-bound scenes.

## Audit decisions

- Retain separate temporal histories and cached resources for the two mapped focus views. Preserve peripheral views; avoid a second neural pass over the entire composed stereo image.
- Keep one foveation owner in the combined route. The focus adapter disables nested Cheeky crops, peripheral DLAA and automatic alignment in that route.
- Keep asynchronous GPU fence synchronization and bounded transport slots. Removing completion checks risks incorrect allocator/resource reuse and stereo history corruption. Their actual DX11/DCS cost needs an in-game measurement.
- Keep Quad Views turbo disabled for the combined pacing route. It has not been validated with the complete headset chain.
- Keep OFXR file flushing buffered and neural logs sampled. Recorder/overlay controls are optional; no unmeasured speedup is claimed.
- Expose the additional focus sharpening pass instead of silently removing it. Preserve original image quality defaults and allow custom motion analysis.

## Optimization round, October 2026

An audit of our own code (OFXR fork, Cheeky, Quad Views edits, the app), every change measured or proved equivalent offline on the RTX 5090. Not yet flown.

| Change | Where | Measured | Image |
| --- | --- | --- | --- |
| The composition's bilinear samples of both frames through the texture unit (one filtered fetch instead of four loads) | OFXR 0012 | Composition −15 % at 5424×5356 (1587 → 1354 µs per pair), −16 % at 2712×2678 | NVIDIA quality fixtures' error against the true intermediate the same or lower (moving scene 25.7 → 22.9 and 32.2 → 27.5 mean absolute error) |
| 3X composes both synthetics in one pass (two render targets) | OFXR 0012 | Composition −3 to −5 % per 3X pair (75 µs at 5424×5356) | Bit-identical to one pass per synthetic on the RTX 5090 (0 differing bytes) |
| One eye per synthesizer: no separate timing-marker list and its queue wait; composition and current copy in one submission | OFXR 0012 | 2 submissions and 1 queue wait fewer per eye per pair on the xrEndFrame path | None |
| Per-image swapchain calls logged only when slow or failed | OFXR 0011 | About two thirds fewer log lines (each a synchronous write on DCS's render thread) | None |
| The two focus views share the transport textures (colour, depth, motion vectors, output) | Cheeky | About 57 MB | None (the views run strictly one after the other) |
| The two focus views share DLSS-NR's intermediate textures | Cheeky | About 34 MB | None (nothing in them outlives the frame) |
| Idle views of earlier missions give back their private resources | Cheeky | About 1.0 GB after a light mission, 0.5 GB after a heavy one (flown) | None |
| Transport timing read without flushing DCS's context | Cheeky | No extra command-buffer flush | None |
| DLSS-NR encode/decode take one texel where every bilinear sample falls on a texel centre (working scale 1.0 only; at the default 0.75 the bilinear path runs) | Cheeky | Decode 3 loads per pixel instead of 9, encode's proxy 1 instead of 4 | Identical for every finite value |
| Quad Views composition: pixels outside the focus area return the stereo image without the focus calculations | Quad Views | 0.01–0.03 ms per frame at 5400×5400 with a 2076 focus area on the RTX 5090 (5.960 → 5.926 ms without sharpening) | Identical (0 differing pixels, 8 cases: round and rectangular, offsets, sharpening on and off) |
| Quad Views composition and sharpening views created once per swapchain image instead of every frame (10 D3D11 views per frame) | Quad Views | CPU time on DCS's render thread, not measured on its own | None |
| Focus sharpening skips the tiles wholly outside the round focus area | Quad Views | About a fifth of the sharpening dispatch; inside the GPU time above | Identical (same harness) |
| Cheeky's focus composite reuses its shader and UAV views (6 created per frame before) | Cheeky | CPU time on DCS's render thread; views dropped with idle views between missions | None |
| Cheeky captures and restores only the DLSS creation parameters DCS actually set | Cheeky | Fewer NGX parameter calls per feature push | None (unset keys stay unset) |

Looked at and not done, with the reason:

- **Optical-flow inputs shared between work slots** (about 58 MB): the next pair's flow would overwrite buffers the previous composition may still read on another GPU queue; an intermittent corrupted flow is not worth 0.2 % of the VRAM.
- **Both eyes' packs and flows ahead of both compositions** (about 0.85 ms less latency per pair): DCS's synthesis shares the bridge queue that also orders the D3D11 hand-over and the runtime's images; reordering it touches the image ownership transfers that once lost generated frames on their way to the headset. Not without in-headset testing.
- **One D3D11 flush per frame instead of one per swapchain** (tens of µs): the private queue would wait on a signal still in an unflushed D3D11 command buffer, a stall or deadlock if anything waits for the GPU meanwhile.
- **Preparing the private D3D12 device and NGX during the mission load** (the about 1 s hitch on the first focus frame): NGX initialization would run on another thread while DCS creates its own DLSS features on the render thread; NGX makes no thread-safety promise.
- **Cheeky's final composite as a plain copy in whole-view mode** and **batched resource barriers**: the composite has too many conditions (shape, feather, debug borders, resampling) to prove the copy identical; batching saves microseconds.
- **DLSS-NR decode reading the colour in place instead of its copy**: after the two views share the copy it saves 11 MB in all, not worth changing the decode's input.
- **Upscaling the focus views with DCS's own DX11 DLSS instead of a private D3D12 feature** (possibly about 1 GB): the `VRAM_STAGE` lines Cheeky now logs (device + NGX init, SR feature, DLSS-NR runtime and feature) will say from the next flight how much of the first focus view's 1.4–1.7 GB is the private SR at all.
- **Skipping Cheeky's save and restore of its private DLSS parameters**: Reset and the exposure texture would stick across calls.
- **Caching Cheeky's environment switches**: the tests and the hotkey change them while running; a few microseconds.
- **Trimming Cheeky's D3D11 binding save and restore**: the render target has to be unbound around its copies; the rest is microseconds.
- **Reordering Cheeky's feature adoption ahead of the cheaper checks**: it changes which feature a view adopts in edge cases.
- **OFXR writing the real frame during synthesis instead of a separate copy** (about 0.07 ms per eye): touches many paths and the sRGB formats; after a flight with the current build.
- **Sharpening merged into the composition, 8-bit sharpening, a visibility mask in the optical flow**: each changes the image.
- **Focus sharpening on top of DLSS** (two passes and two 34 MB copies): an image choice; the setting is on the Quad Views page.

## Reproducing the hardware test

Build the source dependencies described in [VALIDATION.md](VALIDATION.md), then run:

```powershell
scripts/benchmark-gpu.ps1 -NeuralRuntime 'C:\path\nvngx_dlssnr.dll'
```

Use `-QualityCasesOnly` for the bidirectional/high-resolution cases and `-Repeats 1..5` to change repetition count. This verifies trusted runtime binaries, builds the test harness and runs the combined GPU correctness fixture before timing. It writes individual median/p95/minimum timings, memory counters, GPU state snapshots and logs under `artifacts/performance/<id>`. It does not launch DCS or an OpenXR headset session. Proprietary neural binaries are not redistributed.

The final choice needs a repeatable DCS mission at the actual headset resolution: compare HUD/MFD text, distant contacts, both-eye consistency, focus seams, head movement, motion artifacts, frame-time percentiles and input latency. Keep original quality settings until those comparisons justify a change.
