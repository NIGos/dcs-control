# Runtime probe

Checks, with nobody in the headset, how an OpenXR runtime composites **two projection layers**: a low-resolution
periphery layer and a focus layer with an alpha fade whose FOV is a moving sub-rectangle of the first. It exists to
decide whether Quad Views can stop composing one 5424x5356 image per eye (see
`artifacts/research/foveated-framegen-deep-dive-2026-11.md` and `runtime-probe-design-2026-11.md`).

It is a development tool: not part of the app, not shipped, touches neither DCS nor the forks.

## What it does

- `probe_run.exe <outdir>` talks to the runtime DLL directly (no OpenXR loader, no API layers), D3D11. It submits test
  images, reads the Pimax compositor's own mirrors and writes one PNG plus one `key=value` file per case.
- `probe_analyze.exe <outdir>` turns the cases into `results.csv` and `report.md` (Italian).
- `tools/runtime-probe/run-runtime-probe.ps1` does a whole unattended run: starts Pimax, wakes the headset, runs both programs,
  puts back what it changed and closes Pimax.

## Foveated replay

`fovreplay.exe` replays OFXR's foveated two-layer submission (four swapchain sets, real and generated frames) and
measures the compositor mirrors for a flickering focus disc. See `REPLAY.md`.

## Chain mode

`probe_run --quad --layer ...` tests the whole layer chain without DCS: the probe loads the Quad Views layer (and the
frame-generation layer) itself and submits one projection layer with four views. See `CHAIN.md` and
`tools/runtime-probe/run-chain-probe.ps1`. Everything below describes the direct mode, which is unchanged.

## Requirements

- Pimax headset on its stand **in a lit room**. While the headset is not tracked the Pimax compositor shows no
  application at all and both mirrors are a flat dark grey; `probe_run` then stops with exit code 3.
- DCS closed.
- MSVC build tools. `build.bat` builds everything into `bin\`. The OpenXR headers come from the OFXR fork's copy.

## How the mirrors are read

- The compositor's mirror switches (`enable_undistorted_mirror`, `enable_screen_mirror`) are not in the Pimax Play
  config API. They are set through the PVR client interface (`LibPVRClient64.dll`, `getPvrInterface`), with the HMD
  handle the OpenXR runtime itself uses; the probe gets that handle by replacing two entries of the PVR function table
  **in its own process**.
- Undistorted mirror: after layer composition, before reprojection. Used for position, fade, alpha, sharpness.
- Screen mirror: distorted, after per-layer reprojection. Used for reprojection coherence (layers submitted with a
  pose a few degrees off the real one) and sharpness.
- Both are 2880x1440, 8-bit, both eyes side by side, about 14 pixels per degree. One focus pixel is about a quarter of a
  mirror pixel, so positions come from dot centroids, and sharpness is measured on a scale model (all FOVs enlarged
  2.6 times, so the mirror samples the layers at the ratio the panel would).

## Cases

| Kind | What | Metric |
|---|---|---|
| `lattice` | green dots in the periphery layer, red dots in the focus layer on the same tangent lattice, along a path of focus positions | position error of the focus layer, in focus pixels |
| `gray` | both layers at the same grey, focus with the Quad Views fade | luminance deviation in the fade band (should be none) |
| `two` | two different greys | whether the fade follows a linear-light or an encoded-value blend |
| `sentinel` | patches with known colour and alpha | which blend rule the runtime really applies |
| `edge` | slanted edges on the scale model, against the composed reference | 10-90 % rise width |
| `lattice` on the screen mirror | layers submitted with a rotated pose | reprojection coherence between the layers |
| `timing` | 20 s of each configuration | missed and discarded frames from the Pimax server log |

Variants: swapchain format (sRGB, UNORM), separate or array swapchains, and three alpha conventions for the focus
layer (unpremultiplied, premultiplied in linear light, premultiplied on the encoded values).

## Exit codes of `probe_run`

0 done, 2 session failure, 3 headset not tracked, 4 no mirror image, 5 the runtime refused the session (right after a
headset restart; start it again, the runner does).

## Limits

- D3D11 only. The production chain reaches the runtime through OFXR's D3D12 device.
- Nothing after the mirror is seen: lenses and panel.
- The fade thresholds are proposals until calibrated with the user's eyes.
- VRAM is not measured (the swapchains belong to the compositor's process).
