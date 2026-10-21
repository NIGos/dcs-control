# Components and provenance

| Component | Pinned version / revision | Use |
| --- | --- | --- |
| OFXR Bridge | 0.2.1 / `dad56acafc6e1dde219940427738b926cf2ea555` | Original release ZIP; LGPL-3.0-or-later |
| OFXR Bridge (djules75 fork) | 0.2.9.1 / `8cdee48ec97afc41cff0a48c68a44566643c9943` (https://github.com/djules75/OFXR-Bridge) | Source build by `scripts/build-ofxr-djules75.ps1`, shipped in `components/ofxr` and used for every framegen profile (DCS's second-thread xrWaitFrame deadlocks upstream 0.2.1); LGPL-3.0-or-later, source in the corresponding-source archive under `upstream/ofxr-djules75` |
| CheekyFoveatedDLSS | 0.5.4 / `d6c18b23d31f7339735114c621bc3f9066e1a32d` | Original standalone ZIP/loaders; modified source-built host/runtime and matching OpenXR layer for focus profiles; GPL-3.0 |
| Quad-Views-Foveated | 1.1.3 / `957ff0327185ea29acc41fc86c4cdc3caf428fb9` | Locally compiled provider; MIT |
| Sboys CustomHeadsetOpenVR | 1.3.0 / `21185da63a9e4307ae876b3702a43a43044d451c` | Official download/import only; GPL-2.0 source plus separate private binary functionality |
| FidelityFX SDK | 1.1.4 / `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55` | OFXR source build dependency; MIT |
| VectorXR (DienerTech) | not used | Not used or distributed. DCS Control only sets `DISABLE_XR_APILAYER_DIENERTECH_VECTORXR=1` for the DCS processes it starts; some frame-generation ideas were compared with it, no code was copied; MPL-2.0 |
| .NET | SDK 10.0.401 | Self-contained application runtime; Microsoft notices included in publish output |
| Microsoft WebView2 SDK | 1.0.4258.31 | Managed browser host and native loader; SDK LICENSE and NOTICE included |

The WebView2 Evergreen Runtime is installed separately and governed by Microsoft's runtime terms. The development machine has runtime 154.0.4258.48. All web assets, navigation icons and aircraft artwork are original project code. No DCS game images, fonts or launcher files are redistributed.

Package SHA-256 values and source URLs are embedded in `PackageCatalog` and the diagnostic report. Original ZIPs retain their upstream notices and third-party license files. The manager's source code is MIT-licensed; third-party modules retain their own licenses.

The NVIDIA DLSS 5 runtime is user-supplied and is not redistributed here. The source-built Quad Views provider has a small documented change: `DCSVR_QUAD_SETTINGS` selects a private settings file for the launched process, preserving default global behavior when absent. Its build script applies that patch reproducibly.

The local Cheeky modifications add opt-in focus-pair view selection, resource/copy routing, array/bounds checks, history recovery, adjusted downstream extension probing and corresponding tests. `scripts/patch-cheeky.py` applies them to the pinned base. The binary distribution includes hashes for the modified components. Their complete corresponding source and dependencies are in the matching 0.4.0 source archive, with base revisions and modification notices. Standard stereo profiles keep original upstream host/runtime binaries and use the matching locally built layer with the adapter disabled. [Adapter documentation](QUAD_FOCUS_ADAPTER.md).

FidelityFX build scripts contain compatibility fixes for Ninja and shader output dependency names. These do not change shaders or rendering algorithms. Corresponding source snapshots, dependencies, and build scripts are supplied separately with this local preview. For the public release the binary ZIP ships only the components listed above under their licences, with the complete corresponding source in the matching sources ZIP; proprietary NVIDIA, Pimax, SteamVR and Sboys binaries are not redistributed.

## 0.4.0 distribution audit

`distribution-policy.json` records each delivery method. Sboys binaries, NVIDIA neural DLLs, Pimax and SteamVR installers are absent from the binary ZIP. Sboys source documents private DRM-related functionality without an established binary redistribution grant; users obtain the original release directly. The archived gaze bridge is not bundled and Crystal Super compatibility remains a live requirement.

OFXR retains its original LGPL/GPL and third-party notices inside its ZIP. Its NVIDIA optical-flow headers have their notice there; nvofapi64.dll is provided by the installed NVIDIA driver and is not bundled. Cheeky dependency notices, Quad Views THIRD_PARTY, OpenXR MixedReality NOTICE and Windows Implementation Library notices accompany the modified binaries. The matching source ZIP must accompany binary redistribution. General-purpose SDK executables, DLLs, binary libraries and debug symbols are omitted from the source ZIP: obtain MSVC / Windows SDK and FidelityFX 1.1.4 tools from their official distributions, then overlay these modified source snapshots. All renderer source and shader changes remain included. `UPSTREAM_REVISIONS.json` pins the base revisions.

Microsoft WebView2 / C++ installers are downloaded only when required by the setup (`DcsControl-<version>-Setup.exe`) or Install DCS Control.cmd and checked for a valid Microsoft signature before execution. They are not bundled. Their official installer terms apply. Full redistribution policy and hashes are supplied for review; the private preview is unsigned.

Cheeky is distributed as the three upstream binaries the profiles load (`dxgi.dll`, `CheekyFoveatedDLSSHost.dll`, `CheekyFoveatedDLSSRuntime.dll`), extracted at build time from the pinned 0.5.4 archive and hash-verified. The archive and its `version.dll` fallback loader are not distributed: Microsoft Defender flags that loader (`Trojan:Win32/Posilod.CA!cl`, a cloud heuristic), and `dxgi.dll` alone is sufficient. When another `dxgi.dll` is already in the DCS bin folder, the app asks the user to remove it instead of falling back.
