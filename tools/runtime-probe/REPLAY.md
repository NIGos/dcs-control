# Foveated replay

`fovreplay.exe` plays, without DCS, OFXR or Quad Views, the submission pattern OFXR's foveated frame generation hands
to the Pimax runtime, and reads the compositor's mirrors while it does. It exists to find the white feathered disc that
flickers over the focus in foveated frame generation (`artifacts\perf-07\flicker.md`) with as few headset sessions as
possible.

## What it submits

Per frame at the display rate, as the rig tap saw OFXR do: periphery projection (flags 0, sRGB, 1076x1060 per eye, the
eye's FOV), focus projection (SOURCE_ALPHA | UNPREMULTIPLIED, UNORM, 1952x1928 per eye, a centred box of 36 % of the
eye's tangent size, same pose, Quad Views' rounded fade), and a small head-locked quad (the diagnostic panel). No depth.

Both layers show the same uniform sky colour, so a correct composite is uniform whatever blend rule the runtime uses;
a disc means the runtime composited the focus wrongly (for example unpremultiplied colour taken as premultiplied gives
a bright fade ring). The focus is black where its alpha is 0, so an ignored alpha shows as dark corners.

Each periphery image carries a tag above the focus (4 blocks: generated, set bit 0, set bit 1, frame parity) and the
variant number in big digits below it: each mirror grab says which submission it shows, and a person in the headset can
name the variant.

## Variants

| # | Name | Difference from `prod_2x` |
|---|---|---|
| 0 | `control_periphery_only` | no focus layer: the noise of the measurement |
| 1 | `prod_2x` | what OFXR sends: alpha 0, arrays, 4 sets (real 0/1, generated 2/3), R G R G, quad |
| 2 | `prod_3x` | R G G |
| 3 | `real_only_1x` | no generated frames |
| 4 | `one_set_2x` | one swapchain set for everything |
| 5 | `per_eye_2x` | one swapchain per eye instead of arrays (`dcsvr_two_layers=1`) |
| 6 | `alpha1_premult_2x` | premultiplied, no UNPREMULTIPLIED bit (`dcsvr_two_layers_alpha=1`) |
| 7 | `alpha2_srgb_2x` | premultiplied in an sRGB focus swapchain (`dcsvr_two_layers_alpha=2`) |
| 8 | `no_quad_2x` | no quad layer |
| 9 | `opaque_focus_2x` | focus opaque, no blend flags (design B: feather on the periphery side) |
| 10 | `stall_2x` | a 30 ms stall every 45 frames: the runtime's frame extrapolator |
| 11 | `prod_2x_again` | drift check |

Live control without restart: write `<outdir>\replay_cmd.txt` (`variant=6`, or `alpha=1 sets=1 cadence=3 ...` one key
per line, `cycle=1` to resume the cycle, `quit=1`).

## Measurement

A grab thread with its own D3D11 device reads `undistorted_mirror_handle_0/_1` (after composition, before reprojection)
and `screen_mirror_handle` (after reprojection) as fast as it can. Calibration: a base image (periphery only), each tag
block lit alone, and a dark focus to map the focus inside, the fade band and a ring around it in each mirror. Per grab
and eye: band, inside and ring luminance minus the base, the worst 12x12 block of the band, the decoded tag.
`summary.md`: per variant, mirror and eye, the disc (band minus ring, mean), the flicker (its standard deviation over
the grabs), the worst block, real minus generated frames; thresholds from the control variant. `grabs.csv` and
`submissions.csv` hold every grab and every submission; `worst_vNN_*.png` the worst grab of each variant.

Limits: the mirror is written at about half the display rate, so the tag says which frames were seen; nothing after
the mirror (the runtime's display path: upscale, sharpening, lens distortion on the panel) is measured. That is what the
user session is for: the variant number is in the headset.

## Running

- Unattended (headset on its stand, lit room): `tools\runtime-probe\run-fovreplay.ps1`. It starts Pimax, sets the
  headset screen saver to 0 and restarts the headset, runs the replay, and always puts back the screen saver, closes
  Pimax and stops the PiServiceLauncher service (start type stays Automatic). Output in
  `artifacts\runtime-probe\fovreplay\<time>`.
- With the headset worn: `run-fovreplay.ps1 -UserSession` (beeps at start and end).
