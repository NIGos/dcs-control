# Experimental Quad Views focus adapter

The adapter is a documented local modification of CheekyFoveatedDLSS 0.5.4. Upstream still excludes Quad Views. The 0.2 preview adds a narrower DX11 route: process the focus pair, preserve peripheral processing, compose four views in the bundled provider, and send stereo to OFXR.

## Contracts

The manager deploys source-built Cheeky host/runtime binaries and their matching OpenXR layer. Original standalone loader binaries remain unchanged. Each local component has a SHA-256 sidecar checked before deployment. The combined preset requires the bundled Quad-Views-Foveated provider and rejects an alternative provider path.

`DCSVR_QUAD_FOCUS=1` is set only for a combined profile's child process. When absent or zero, upstream stereo selection and guards remain active. `XR_ENABLE_API_LAYERS` orders the adapted Cheeky layer, Quad Views compositor, and OFXR. Pimax and SteamVR runtime manifests are selected privately through `XR_RUNTIME_JSON`.

The application-facing four-view projection is forwarded unchanged. For `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO`, the layer publishes views 2 and 3 as its two focus slots. It also selects that pair for projection observations and calibration. ABI version 4 and its structure sizes remain unchanged; local status bit 12 identifies focus-pair semantics. Local view flag bit 9 identifies a proven single-mip destination layout. These extensions require the matching local binaries.

## Routing and fallback

Before each intercepted DX11 SR evaluation, the adapter requires a focused session, a ready focus snapshot, a predicted display time and a QPC publication no older than 50 ms. It requires exactly one match against the two submitted focus regions.

An exact match uses canonical COM resource identity, a single-slice output and the full submitted rectangle. Otherwise the upstream observed-copy graph must connect the NGX output region to the submitted focus resource and subresource. A one-mip destination makes `imageArrayIndex` the exact D3D subresource index, allowing proven copies into slices 2/3 without enlarging Cheeky's two-view processing ABI. Destination rectangles, array bounds and mip count are validated. Multiple-mip layouts are rejected. The copy graph has bounded, expiring history and invalidates known destroyed resources.

No view is classified from its dimensions or NGX call order. Each original NGX handle retains its own feature and temporal state. This avoids treating a peripheral feature as a focus feature simply because the game evaluates it second or reuses a resolution.

Without a unique proven match, the callback runs original DCS processing. It releases that handle's DX12 transport and skips its neural history. A transition from adapted processing back to original processing temporarily requests `Reset=1` for one original evaluation, then restores the parameter. Repeated unmapped evaluations do not repeatedly reset the original history. Cold start therefore remains original processing until observations establish a mapping.

DCS's own DX11 DLSS features for the focus views are deferred: the core create hook hands DCS an opaque handle and creates the real feature only when nothing else can produce the view, because NGX keeps a created DX11 feature's VRAM until shutdown (about 255 MiB per 2076×2048 focus view once evaluated, measured with `tests/native/dlss11_vram_probe.cpp`). Before the focus proof is ready (the first frames after a mission load, when the snapshot or layout is stale), a handle whose output is the published focus extent runs on the private D3D12 SR without NR: the same feature and transport textures NR then uses, and DCS's own upscale as output. Every private SR of a deferred view (with or without NR) is evaluated with DCS's creation-time DLSS parameters (PerfQualityValue, preset hints, create flags, render and output size) replayed on DCS's shared parameter bag and restored afterwards, exactly what DCS's own feature would use. With DLSS 5 toggled off in flight the private D3D12 SR also keeps running without NR, and features created while it is off are still deferred. With no Quad Views layout at all, the view is held with a plain bilinear upscale for at most 3 s. DCS's own feature is created only for a peripheral extent, a layout that never appears, or a transport that cannot run (`DCS deferred DX11 DLSS materialized … reason=`).

The combined configuration turns off Cheeky's nested SR/NR foveation and automatic crop alignment. Quad Views alone owns moving focus placement. NR, when enabled and supported by the supplied NVIDIA runtime, processes the entire mapped focus region through Cheeky's DX11/DX12 Transport. Peripheral views retain DCS native SR. This can create visible neural/non-neural appearance differences at the focus boundary; the headset acceptance procedure specifically checks them.

Cheeky's optional pre-instance extension probing is suppressed in adapter mode because the downstream Quad Views layer initializes its extension dispatch during instance creation. Explicit DCS extension requests are retained. This change fixes the reproduced full-chain instance-creation crash; it does not establish gaze validity in a headset.

## Tests and reproduction

- Native policy checks exercise snapshot freshness, focus membership, duplicate/ambiguous resources, observed copies, array subresources, invalid rectangles/mips, unmodified four-view selection and one-time history transition resets.
- `DcsQuadFocusLayerTests` loads the real source-built layer DLL with WARP resources and a fake runtime. It checks packed focus rectangles, slices 2/3, invalid array/rectangle rejection, four-view preservation and the disabled-adapter guard.
- The actual Khronos loader creates the adapted Cheeky → Quad Views → OFXR chain and enumerates four application-facing views against an isolated fake runtime.
- `CheekyRuntimeStandalone-TransportDcsDeferred*` (generic DX11 host, fixture NGX core and a fixture layer publishing the snapshot and layout): the deferred lifecycle with the bounded bilinear hold, the unproven startup frames on the private SR (no NGX create or evaluate of DCS's feature, NR later on the same textures), and DLSS 5 off in flight.
- Manager fixtures verify component hashes, ordered manifests, per-process configuration, and apply/restore through both runtime selections.

```powershell
py -3 scripts/patch-cheeky.py
py -3 scripts/test-adapter-patch.py
./scripts/msvc.cmd cmake --build artifacts/native/cheeky --parallel 4
ctest --test-dir artifacts/native/cheeky --output-on-failure -R DcsQuadFocusLayerTests
./scripts/test-loader.ps1
```

The patch script uses exact source anchors and fails on an unexpected checkout. A separate reproduction test starts from pristine files at the pinned commit, verifies all 25 resulting files against the built checkout, and confirms that applying the patch again changes nothing. The corresponding source archive includes modified upstream files, the adapter headers/tests and the reproducible patch script.

These tests do not call a real NVIDIA DLSS 5 neural runtime or run DCS. They establish routing, layer negotiation and deployment behavior. Real DCS resource mapping, feature ABI, focus history under gaze motion, neural appearance, synthetic pixels and headset timing remain separate acceptance gates.
