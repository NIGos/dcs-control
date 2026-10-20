# DCS VR on Pimax Super Micro OLED: feasibility and development plan

Research date: 2 October 2026. The objective is to combine VR frame generation, DLSS 5, and foveated rendering in DCS, with selectable original Pimax and Sboys routes, an installer, a GUI, and diagnostics.

The 0.2 preview implements an experimental three-way integration using a local Cheeky focus-pair adapter, software Quad Views composition, and stereo OFXR. The original Pimax and Sboys routes share that composition path. Configuration and source-level routing have been tested offline. A working three-way rendering session in DCS has not been established; stereo synchronization, real resources, moving focus views, neural appearance and synthetic frame timing still require headset evidence.

## Local read-only inventory

| Component | Observed result |
| --- | --- |
| GPU | NVIDIA GeForce RTX 5090 |
| CPU / memory | AMD Ryzen 7 9800X3D / 64 GiB |
| GPU driver | Windows version 32.0.16.1714 |
| DCS | Steam Edition, executable 2.9.30.28536 |
| DCS directory | `<SteamLibrary>\steamapps\common\DCSWorld` |
| Native DLSS DLL | 310.2.1.0 in `bin` and `bin-mt` |
| Active OpenXR runtime | Pimax `PiOpenXR_64.json` |
| Installed Pimax software | PimaxEVO 1.0.1.103 |
| Saved DCS options | DLSS, Quad Views, eye gaze; pixel density 1; FPS limit 89 |
| Sboys | Configuration and SteamVR driver folder exist; active use is unverified |

Saved options do not prove gaze or Quad Views are active in a session. Refresh rate, effective view resolutions, tracking mode, driver version, gaze validity, and mission performance still need runtime observation.

## Technologies and primary evidence

DLSS Super Resolution reconstructs a higher-resolution image. Foveated rendering reduces graphics work away from gaze. Frame generation interpolates images. DLSS 5 adds neural lighting and material processing and can impose additional GPU cost; it is initially aimed at RTX 50 hardware. The local RTX 5090 is in that hardware class. [NVIDIA DLSS 5 introduction](https://www.nvidia.com/en-us/geforce/news/dlss-5-3d-guided-neural-rendering/).

Foveated upscaling changes reconstruction work and does not establish the same savings as reducing pixels rendered through Quad Views. Published percentage gains from different games and technologies cannot simply be added together.

The DCS 2.9 FAQ describes DirectX 11 and no native DLSS frame generation in that implementation. It is a 2023 source and does not certify every feature of the current executable. No official confirmation of native DCS VR DLSS 5 plus framegen was found in this investigation. [Eagle Dynamics FAQ](https://forum.dcs.world/topic/335368-29-graphics-update-faq/).

| Project | Integration role | Verified constraint |
| --- | --- | --- |
| [OFXR Bridge 0.2.1](https://github.com/tig3rmast3r/OFXR-Bridge/releases/tag/0.2.1), 15 September | OpenXR frame generation, NVIDIA/FidelityFX optical flow | Experimental; DCS and Pimax headset behavior unverified |
| [CheekyFoveatedDLSS 0.5.4](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/releases/tag/v0.5.4), 29 September | Foveated DLSS and DLSS 5 | Upstream excludes Quad Views; local preview adds a focus-only adapter |
| [Sboys CustomHeadsetOpenVR 1.3.0](https://github.com/sboys3/CustomHeadsetOpenVR/releases/tag/1.3.0), 7 September | Alternative Pimax driver through SteamVR | Tracking-dependent Pimax software requirements |
| [Quad-Views-Foveated 1.1.3](https://github.com/mbucchia/Quad-Views-Foveated/tree/1.1.3) | Software quad-to-stereo composition | DX11 route; gaze and composition require verification |
| [VectorXR 0.18.0](https://github.com/DienerTech/vectorxr/releases/tag/v0.18.0), 30 September | Alternative software Quad Views and diagnostics | Use one software Quad Views provider at a time |

OFXR uses color-only optical flow in the current standalone route, without game depth or motion vectors. Fast head turns, disocclusion, and cockpit edges need direct inspection. [OFXR README](https://github.com/tig3rmast3r/OFXR-Bridge). Its 0.2.1 code supports DX11/DX12 interoperability but limits a mapped swapchain to two slices and two distinct views. Four views on separate resources are not automatically rejected; a four-slice array encounters an explicit limit. That does not establish correct interpolation of moving focus views. [Examined layer source](https://github.com/tig3rmast3r/OFXR-Bridge/blob/0.2.1/src/layer/openxr_layer.cpp).

Cheeky's standalone route supports native DLSS games on DX11, DX12, and Vulkan, while explicitly excluding Quad Views. [Release README](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/blob/v0.5.4/README.md). DLSS 5 on DX11 requires DX12 Transport and a compatible separately supplied NVIDIA runtime. Processing before upscaling remains an option to compare in stereo. [Settings and restrictions](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/blob/v0.5.4/USAGE.md).

Pimax documents native Quad Views with DCS and Pimax OpenXR, providing the baseline original route. [Pimax DCS guide](https://eu.pimax.com/pages/pimax-crystal-super-the-ultimate-dcs-setup-guide).

Sboys 1.3.0 supports Crystal Super Micro OLED. SLAM/P2 uses EVO; Lighthouse can use EVO or Play. Its September release is newer than earlier Pimax guidance. [Release notes](https://github.com/sboys3/CustomHeadsetOpenVR/releases/tag/1.3.0). The driver publishes eye data through SteamVR components and the OpenXR gaze interaction property, but validity and update rate in DCS remain to be measured. [Eye tracking implementation](https://github.com/sboys3/CustomHeadsetOpenVR/blob/1.3.0/CustomHeadsetOpenVR/src/Helpers/EyeTrackingOutput.cpp).

VectorXR is an alternative when control over software composition is needed. Its synthesized Pimax route requires native Pimax Quad Views to be disabled and one software provider selected. It is researched but not bundled in this preview. [VectorXR documentation](https://github.com/DienerTech/vectorxr).

PureDark's AFW and DLSS 5 bridges for Luke Ross and UEVR are engine-specific references. Their existence does not make them directly portable to DCS. [AFW](https://github.com/PureDark/REFramework/releases), [R.E.A.L. VR bridge](https://github.com/eregnier/dlss5-vr), [UEVR bridge](https://github.com/eregnier/uevr-dlss5).

## Architecture and feasibility

These are design judgments, not benchmark results.

| Objective | Assessment | Evidence still needed |
| --- | --- | --- |
| Installer, GUI, profiles, recovery | High feasibility; implemented in preview | First real application installation review |
| Native DLSS SR with Pimax Quad Views | Documented baseline | Local runtime confirmation |
| Dynamic foveation through Sboys | Plausible | Valid gaze and compatible software provider |
| OFXR in DCS stereo | Plausible, experimental | Visible synthetic frames and improved pacing |
| OFXR with Quad Views | Uncertain; headless loading succeeds | Resource layout, focus continuity, presentation |
| DLSS 5 in DCS stereo | Plausible, experimental | ABI, transport, stereo readability, GPU cost |
| All three together | Experimental focus adapter implemented; headless chain loads | Real DCS mapping, NR runtime, seams, presentation and benefit on both routes |

The manager selects one runtime for the launched DCS process:

```text
Original: DCS OpenXR → selected layers → Pimax OpenXR → headset
Sboys:    DCS OpenXR → selected layers → SteamVR OpenXR → Sboys → headset
```

OpenXR runtime and explicit layer selection use child-process environment variables rather than global registry changes. Pinned packages supply independent modules. C++ handles graphics layers; .NET/WPF handles the GUI and transactions. The Sboys route retains its documented Pimax dependencies.

For the full combination, the chosen implementation preserves peripheral SR and adapts only the focus pair before software composition into stereo, followed by OFXR. Exact resources or observed copies establish focus routing; single-mip destination arrays are supported with proven subresource copies. The headless loader test verifies the full adapted layer chain loads and exposes four application-facing views. Real-layer WARP tests verify focus selection and invalid-layout rejection. These tests do not verify composition or generated pixels. Native Pimax composition still requires a different adaptation.

The adapter keeps Cheeky's two-view ABI for the two focus slots and preserves original NGX feature ownership. Peripheral, missing or ambiguous outputs retain DCS native processing. Mapping loss releases transport and resets temporal state on transition. Quad Views owns gaze and coverage, while adapted Cheeky processes the full focus region. This avoids nested moving crops but may expose neural/non-neural appearance differences at the focus boundary. [Implementation contracts](docs/QUAD_FOCUS_ADAPTER.md).

Four-view NR, post-composition NR, alternative compositors, generic injectors, native DLSS FG and engine-specific VR bridges were compared. The selected path minimizes new data capture and compositor work; it is not a measured performance winner. [Approach comparison and reasons](docs/APPROACHES.md).

## Development gates

1. **Reproduce the baseline.** Record runtime, gaze validity, view sizes, headset refresh, CPU/GPU times, and cockpit readability in a repeatable mission. Identify existing loaders and layers.
2. **Demonstrate framegen.** Begin with stereo and native DLSS SR. Compare NVIDIA and FidelityFX. Then test native and software Quad Views. Require visible synthetic frames, improved pacing, and acceptable artifacts; a higher submission counter alone is insufficient.
3. **Demonstrate DLSS 5 in stereo.** Test fixed foveation, then valid gaze, DX12 Transport, and a compatible NVIDIA runtime. Measure transport and neural processing costs separately. Check both eyes, HUD, MFDs, text, and distant contacts before adding OFXR.
4. **Validate the adapted full combination.** Confirm concrete DCS focus/resource mapping first with neural processing off, then add NR. Exercise fast gaze moves, blinks, invalid gaze, resolution changes, and recentering. Require continuity between focus/periphery in real and synthetic frames and a benefit over the best previous profile. If focus-only NR shows unacceptable seams, reassess four-view or post-composition NR.
5. **Validate release behavior.** Confirm installation, upgrades, restoration, uninstall, and a version-specific compatibility matrix. Keep unproven profiles explicitly experimental.

Offline development has advanced the manager and packaging before hardware gates. This does not waive those gates or turn the preview into a certified DCS VR release.

## Measurements and acceptance

Use the same aircraft, weather, mission, settings, and logged resolution for each comparison. Warm caches, repeat at least three times, and report median and 95th-percentile frame times. Separate CPU-heavy and GPU-heavy scenes.

Record real application frames, generated frames, runtime submissions, and headset presentation separately. OFXR's recorder covers negotiation, mapping, resources, and generation stages. [Recorder documentation](https://github.com/tig3rmast3r/OFXR-Bridge/blob/0.2.1/docs/DIAGNOSTICS.md). Its overlay counts accepted submissions rather than physical panel refresh; the synthetic marker assists visual verification. [Overlay limitations](https://github.com/tig3rmast3r/OFXR-Bridge/blob/0.2.1/docs/FPS_OVERLAY.md).

At 90 Hz a display interval is about 11.1 ms; 45 real frames per second leaves about 22.2 ms between application frames. Interpolation, copying, and composition must still fit the budget. Framegen does not double simulation or input frequency.

Headset acceptance includes instrument reading, rapid head turns, low-altitude scenery, formation, propellers, clouds, small targets, and moving gaze. Test mission loading, recentering, runtime restart, recovery, and 30-minute sessions. Check actual multiplayer server Integrity Check behavior rather than assuming compatibility from a layer's general properties.

The next decision is whether OFXR improves DCS while retaining useful foveation. The following decision is whether DLSS 5 provides worthwhile quality at acceptable cost. The preview supplies separate baseline, stereo, framegen and combined profiles so those costs and benefits can be measured independently.
