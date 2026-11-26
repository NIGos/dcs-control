# Frame pacing and FPS limits

Open **Framegen** before a live test. The tool preserves existing FPS limits and image-quality settings by default. The refresh rate is read from the headset software (Pimax Play's runtime as it runs, or SteamVR's setting on the Sboys route); the number field is only for setting it by hand.

OFXR (the djules75 fork 0.2.9.1 with DCS Control's patches) produces one (2×) or two (3×) generated frames per rendered frame, or switches between them (Auto). Its OpenXR presenter handles display pacing. The manager uses DCS's real `graphics.maxFPS` option for a rendered-frame cap; it does not write a fictitious OFXR limiter key. OFXR is optical-flow frame generation, separate from NVIDIA DLSS Frame Generation.

| FPS limit mode | DCS change | Intended use |
| --- | --- | --- |
| Preserve existing DCS limit | No change to `graphics.maxFPS` | Keep an existing setup; the detected cap appears in the frame-rate summary. |
| Runtime pacing | Set cap to 300 FPS | Leave DCS cap headroom while investigating OpenXR pacing. This is not an unlimited mode. |
| Match refresh | Refresh / 2 with OFXR; full refresh with framegen off | Explicit comparison of a matched rendered cap. A 90 Hz selection writes 45 FPS with OFXR and 90 FPS without it. |
| Custom rendered-frame cap | User value, 30–300 FPS | Fine-tune real game FPS independently of display refresh. Fractional values are preserved. |

The supported range matches the installed DCS `MissionEditor/modules/Options/optionsDb.lua`: `graphics('maxFPS')`, slider 30–300, default 180. This version does not assume that zero means unlimited. The original value, including a missing field, is backed up and restored. Match refresh recalculates when framegen is enabled or disabled; the wizard shows that derived cap in Review.

**A cap is a ceiling, not a performance guarantee.** For a 90 Hz headset, 45 rendered FPS can supply up to 90 fresh frames/s with one intermediate frame per rendered frame. A 30 FPS cap can supply at most 60 before other bottlenecks. GPU/CPU load, failed synthesis, gaze/composition work and runtime presentation can reduce fresh output. A 300 FPS DCS cap does not force a 300 FPS render loop through OpenXR.

## Other limiters and synchronization

- **Disable DCS desktop VSync** explicitly writes `graphics.sync = false`. Leaving it off preserves the original value. This does not disable OpenXR synchronization or rotational timewarp.
- **NVIDIA / RTSS / other limiters** are a check you do yourself, listed by preflight: inspect NVIDIA global and DCS program **Max Frame Rate** and **Background Application Max Frame Rate**, plus the DCS RTSS profile. The tool does not read or modify these driver settings, so it no longer asks you to report them (earlier versions did; those saved answers are ignored). An external cap's position relative to OFXR must be established live.
- **Runtime motion smoothing** (Pimax Smart Smoothing, SteamVR motion smoothing) is likewise a preflight check you do yourself when frame generation is on: establish OFXR cadence with this second interpolation stage disabled first. The tool does not change the runtime.
- **Legacy autoexec.cfg** is detected beside the chosen options.lua. Simple `max_fps` assignments are reported without executing Lua. Unsupported commands remain unknown. The file is preserved; creating or changing it after Apply invalidates the saved launch check so pacing cannot silently change.
- Detected **RTSS processes** are reported as process facts. Their presence does not prove a cap is active, and their absence does not prove driver caps are off.

Use **Run preflight** for fresh detected settings and warnings. **Review files** lists the exact previous and next DCS scalar values, including caps and VSync. **Launch DCS** (or **Apply without launching**) writes those values with the launch configuration. Launch checks treat an edit to an owned cap as a change: the profile is restored and applied again, and a conflicting edit stops at Recovery. Recovery preserves conflicting user edits and reports them.

## First live comparison

1. Confirm the actual headset mode and enter its refresh rate. Keep all image-quality values fixed.
2. Check external/background caps and runtime interpolation. Begin with one cap strategy at a time.
3. Compare Preserve or Runtime pacing against Match refresh in the same cockpit scene. Match refresh is an optional test, not an automatic optimization. If either a runtime or another layer already reduces application cadence, do not add a second half-rate mechanism blindly.
4. Measure real application frame time, fresh synthetic frames, dropped frames, HUD clarity, head-motion latency and comfort. OFXR's output overlay counts accepted OpenXR submissions, including possible presenter repeats; a 90 FPS overlay alone does not prove 90 fresh images reached the panel.
5. Exit DCS, restore, then repeat for framegen, foveation and the combined neural pipeline. Export a fresh diagnostic report with the cap and refresh recorded.

## Smooth mouse cursor

**Framegen → Smooth mouse cursor** (off by default; `smooth_cursor=1` and `cursor_follow_mouse=1` in the profile's `ofxr_bridge.ini`, OFXR fork patches 0010, 0011, 0087 and 0088). In the cockpit DCS draws its VR mouse cursor as a spot about 2° across along the mouse ray (`enlight.dll`'s RayCursor, built by `CockpitBase.dll` with a 1° half-angle; shader `Bazar\shaders\enlight\rayCursor.fx`): the current cursor image (the yellow cross or the green sight), lit by the scene's exposure, brighter at its centre, mixed into the frame before tone mapping and DLSS with 30 % of the panel still showing under it. On a Pimax Crystal Super's 5424-px eye image that is about 65 px, twice the 32-px image. The menus use a plain white dot (149). Optical flow cannot carry either: each generated pixel takes the motion found at its own position, and a cursor that moves further than its own size between two real frames has no motion vector that lands on it, so a generated frame shows it faded at both places.

With the option on, and only while the Windows cursor moved within the last 5 s, OFXR finds the cursor in both real frames of each pair and in both eyes (a few compute passes on the GPU, no read-back), then redraws a small area around both positions in every generated frame: the cursor is taken out of both real frames there and drawn once more at its position for that frame's moment (half way in 2×, one and two thirds in 3×), copied pixel for pixel from the newer real frame. The rest of the generated frame is untouched, and so are the real frames. The cursor images are DCS's own, read from `bin\Visualizer.dll` in the running game (or from DCS.exe's folder written as `cursor_templates`): the cockpit's yellow cross (147) and green sight (145), and the menus' white dot (149). The control markers DCS shows over a switch are left out (0011): they last only while the mouse rests on a switch, when there is nothing to smooth, and every image searched costs a pass and is one more possible look-alike. Nothing of DCS is shipped; a plain arrow stands in if no image is found. The match allows the cockpit spot's colour shift (yellow arriving orange counts as yellow; grey or another primary does not) and sizes from 0.4 to 3.2 times the image; from twice the image up, the final placement compares each image texel's whole footprint, so the redraw lands on the exact pixel.

A pair is redrawn only when every check passes: a confident match in both frames and both eyes, no look-alike within 48 px scoring nearly as well, the same cursor at the same size and a consistent vertical offset in both eyes, both eyes moving by the same amount, and no movement while the mouse was still. Otherwise the generated frames are exactly what they would be without the option.

Measured on an RTX 5090 (NVIDIA optical flow, 50 % input, 2 × 2560×2560 and 2 × 3200×3200): finding the cursor costs about 0.05 ms per pair while it is tracked and 0.17–0.22 ms on the pair that first finds it; it runs while the optical-flow engine works, so a whole pair took only 0.02–0.03 ms longer. The redraw adds nothing measurable to the composition (0.16 / 0.26 ms with and without).

To check it in a flight, turn on **OFXR log** and fly once with the option on and once off, moving the mouse over the cockpit and over a switch (the cursor changes to a green marker there). The log has one `op=smooth_cursor result=1` line a second while it is on: `a` holds four 16-bit counts, pairs judged (bits 0–15), redrawn (16–31), searched from scratch (32–47) and tracked (48–63); `b` the pairs left alone because the cursor was not found (0–15), matched too weakly (16–31), had a look-alike near it (32–47) or the eyes disagreed (48–63); `c` an impossible jump (0–15), too much moving to search (16–31), pairs with the mouse idle (32–47) and the detection's mean GPU time in µs (48–63). `op=smooth_cursor result=2` once per swapchain says how many cursor images were loaded and from where (`b`: 1 the game, 2 `cursor_templates`, 3 the built-in arrow). `op=smooth_cursor result=3` (0011), in a second with matches too weak, gives their mean score ×1000 (`a`, 720 is needed), their mean size step ×100 (`b`; steps 0–9 are 0.4, 0.5, 0.63, 0.79, 1, 1.26, 1.59, 2, 2.52 and 3.17 times the image) and the latest rejected match packed in `c` (image bits 0–7, size step 8–15, score ×255 16–23, best look-alike ×255 24–31). Redrawn should be close to judged while you move the mouse over the cockpit; a run of "not found" or "matched too weakly" means DCS draws the cursor in a way the matching does not recognise — please send that log.

In the first flight with 0010 (2026-10-06) only 87 of 8286 pairs were redrawn: 6576 matched too weakly. The matching expected a sprite drawn as the image is, and the colour check alone cut every orange-lit yellow cross to a score of 0; the cockpit spot was also at the very top of the size range. 0011's offline tests draw the spot as DCS does (radial brightness, 30 % of the panel under it, orange) at 2× in 2X and 2.5× in 3X: both are redrawn exactly, with nothing left at either real position.

**Following the mouse** (0087, and 0088 for the two layers of foveated frame generation; `cursor_follow_mouse=1`, written whenever Smooth mouse cursor is on). In flights the cockpit cursor was still rarely recognised, for two reasons. With a HUD in view, dozens of small bright structures match the cursor's dot nearly as well as the cursor does (0.70 to 0.76), and one of them, taken once, was tracked for a whole flight. And DCS draws the cockpit cursor fatter and softer than its image, so the outline right beside the image's bars matched weakly (0.52 to 0.64 on real frames). Now, of everything that could be the cursor in a frame, the candidate that moves with the mouse is taken instead of the one with the best score: a look-alike that stands still while the mouse moves is passed over, and the place where the last match is expected counts for a candidate. The outline is compared three texels out (0.87 to 0.92 on the same frames). The log gets `op=smooth_cursor result=7` once a second while this changes a choice: `a` is the frame-views where the candidate taken was not the best-scoring one, `b` the pairs judged (bits 0–15) and drawn (16–31) in that second. What is still weak (bright panels, a cross arm on a bright line, fast motion) is in [OFXR limits](OFXR_LIMITS.md), section 1.

**Diagnostic captures** (0011): with the OFXR log on, the smooth cursor also saves up to 8 captures a session, at least 5 s apart, while the mouse moves, next to the log: `cursor-capture-<pid>-<n>-previous.bmp` and `-current.bmp` (384×384 px of the left eye around each real frame's best match), `-overview.bmp` (the left eye at an eighth of its size) and `-info.txt` (each frame's match: status, position, image, size step, score, look-alike score). The log gets `op=smooth_cursor result=4` per capture. They show what the matching actually sees, for the cases the scores alone cannot explain.

**Video memory in the log** (0011): with **VRAM in the headset panel** on, the OFXR log also gets `op=vram_status result=1` once a second, the panel's figures in MiB: `a` this process's resident use (bits 0–31) and the budget DXGI gives it (32–63), `b` every process's use of the GPU's dedicated memory (0–31) and its size (32–63), `c` 1 while DCS's window is in the foreground (Windows gives a background process a smaller budget).

**Optical flow settings while DCS runs** (0066): OFXR checks `ofxr_bridge.ini` every 250 ms on a thread of its own (only when the file's time or size changed). A change of `nvidia_preset`, `nvidia_input_scale`, `nvidia_bidirectional` or `nvidia_occlusion_aware` is taken in at once: the next frame rebuilds the synthesis with it (expect one short hitch, as for any rebuild). The log gets `op=live_settings result=1`: `a` the new values and `b` the old, packed as the preset (0 slow, 1 medium, 2 fast; bits 0–7), the input scale in percent (8–15), bidirectional (16) and occlusion handling (24–31); `c` which keys changed (1 preset, 2 scale, 4 bidirectional, 8 occlusion; bits 0–7) and the control revision (from bit 32). The `op=embedded_configuration` record with that revision in `a` follows, its result the rebuild's. Every other key waits for the next launch; when one differs from what the session started with, the log gets `op=live_settings result=2` with `c` naming them (1 enabled, 2 backend, 4 motion_vectors, 8 d3d11_bridge, 16 deep_pipeline, 32 pipelined_deep_hold, 64 async_arm, 128 adaptive_cadence, 256 hud_symbology, 512 cockpit_mask, 1024 cockpit_depth, 2048 dcs_motion, 4096 smooth_cursor, 8192 cursor_templates, 16384 diag_vram, 32768 visibility_mask, 65536 parallel_eye_synthesis; `c=0` when they are back). `triple_frame_gen`, `adaptive_frame_gen` and the `[overlay]` position were live already. DCS Control writes only the scale, bidirectional and occlusion keys live: a preset change re-initialises the flow (R148, DCS 2.9.31: `synthesis_initialize` 5.5 s, a 6.8 s DCS frame gap), so the app leaves it for the next launch; scale and bidirectional cost one frame of about 0.5 s.

## Generated-frame quality options

OFXR patches 0104 (the options) and 0105 (their tests): four `[ofxr]` keys, one per option, NVIDIA Optical Flow only, all
0 by default (the Frame generation page, Image quality, **Fine-tune**; hidden with FidelityFX). With every key at 0 each
generated frame is the same, byte for byte, as without the patches (the NVIDIA occlusion fixtures' 112 output hashes
against the build without them; the DCS motion fixtures check that an option that cannot act on a scene leaves it
unchanged). OFXR follows a change of them live without rebuilding its synthesis (they are shader bits read per pair; the
log's `op=live_settings result=1` has value 16 in `c` and the keys' bits in bits 32-39 of `a` and `b`; each synthesizer
then logs `op=live_settings result=5`: `a` the quality keys it holds in bits 0-7, why (1 made, 2 set live, 3 rebuilt)
in bits 16-23, `b` the swapchain); the app writes them live with NVIDIA frame generation when the installed
OFXR has their live contract (the marker `DCSC-OFXR-LIVE-FG/2` in the DLL, patch 0123), otherwise for the next launch, and into a profile only when on (0.7.0 refuses keys it does not know). With every key off the compositions are 0.7.0's (0123): the keys' code is compiled only into an `_extras` twin of each NVIDIA composition, bound for a pair that has a key on; its pipeline is made at the start when a key is on; with every key off none is made, and the first pair after a key is turned on (in flight) starts their build in the background (0125): the key takes effect a pair or two later without holding the frame thread (0123 made them on the frame thread at first use: a 0.37-0.82 s stall). A session that never turns one on holds no extra pipeline or driver memory. Measured offline (`XRFG_TEST_FG_TRIPLE_BENCH`, 5424 × 5356 × 2): keys off 3X 2075 us and 2X 1229 us per pair, as 0.7.0; all quality keys 2746 / 1349 us (+0.67 / +0.12 ms). A change of a key in flight no longer arms swapchains no projection layer uses (0123: before it, each live change made four synthesizers for DCS's early swapchains and stalled the frame thread 1.62 s, then about 0.63 s). The 0.7.1 development builds also had an occlusion fill around moving aircraft (`dcs_motion_occlusion_fill`, patches 0121/0122); it was removed before release: in flight it never fired in the cockpit view, and in the external view it made the halo around an aircraft worse by day (1.15× [1.05, 1.26]), the same at dusk, and failed the 30-fps cost gate.

| Key (app setting) | What it does |
|---|---|
| `dcs_motion_sky_occlusion` (Clean edges against the sky) | On DCS's motion vectors, a sky pixel whose two samples differ by more than a quarter of the brighter one (linear light), where the two frames do not agree at the pixel itself and a G-buffer surface is found around it: the surface's own composition where its vector puts it here, else the frame whose end DCS's class map calls sky. The sky's own colour check compares encoded values, as on Quad Views' focus layer. Content DCS draws after its G-buffer (HUD, labels, smoke, contrails) is never forced. |
| `dcs_motion_hud_guard` (Keep the HUD steady over the sky) | On an sRGB swapchain (read in linear light), DCS's motion vectors judge a sky pixel's two samples, and 0057's guard for content drawn after the G-buffer, on encoded values, as on Quad Views' focus layer. In linear light a HUD stroke or a dark aircraft over the night sky passed as sky and was smeared with its vector. Surfaces keep the linear-light check. |
| `nvidia_edge_choice` (Sharper moving edges) | Where the four flow vectors around a pixel spread over 1.5 px and the two frames differ at the pixel, the vector whose two ends agree best of the four and their mix. |
| `dcs_motion_focus` (profile `FrameGenFocusMotion`, no page control; patch 0128) | Start-only, off. With an engine module that exports the focus views' own MVs and depth (motion contract revision 4), the focus area is resampled from them at 4 px instead of from the periphery's MVs. Without that export, or with the key off, nothing changes. Fixtures and cost are in docs/FOCUS_MOTION.md. |
| `dcs_motion_cover` (profile `FrameGenCover`, no page control; patch 0130) | Off until flown. On DCS's motion vectors a distant aircraft is smaller than DCS's motion texel and most of it reads as the sky: its vector and class are spread one texel over its edge (a sky cell next to a model texel takes the model's), and a sky pixel with an aircraft within about 16 px is composed along the aircraft's own vector before the sky's is accepted (else, where the two frames differ, the frame whose end is sky alone). With the depth plane of the focus view a farther surface gives way too. A pair with only this key on binds a `_cover` twin (this code alone, the plain composition's register count). Live in OFXR; the app writes it at the start. Fixtures, numbers and the live A/B: docs/FOCUS_MOTION.md "Aircraft cover". |
| `dcs_motion_mask_band` (Cleaner world beside the cockpit) | DCS's motion vectors also inside the cockpit mask's blocks where DCS's class map says world at the pixel and at both of the vector's ends, with the strict colour check on encoded values. |

Flown (validation round 08, `runs\20261125-1851`: DCS's external camera F2 circling an F-16 at 20 degrees a second, live
toggles in ABBA blocks, day, dusk and night; error in the 2-16 px band outside the aircraft's silhouette against a
motion-compensated reference, ratio on / off with a 95 % bootstrap interval): the four quality keys together took the
halo down at dusk to 0.63 [0.51, 0.80] (trailing edge 0.68, leading edge 0.52) and by day on the leading edge to 0.74
[0.63, 0.88] (the whole band 0.79 [0.65, 0.955]); at night every error was below one luma code. Composition and pack per
pair +3 to +6 us. In the cockpit view by day they can carry a crossing aircraft's faint trailing copy ahead of it. The
app's Fine-tune fold says it in one line: cleaner edges around your aircraft in the external view, little effect in the
cockpit. All four stay EXPERIMENTAL and off by default.

Offline evidence (error against the true middle frame, mean absolute error in 8-bit sRGB codes; the DCS motion fixtures
at 640x480 on the RTX card, `XRFG_TEST_DCS_MOTION=1`, `XRFG_TEST_FG_QUALITY=<bits>` for one option):

- Night occlusion (a grey aircraft at 0.02-0.06 linear passing 30 px over a 0.002 sky; MVs at 10 or 3 pixels a texel;
  UNORM and sRGB views), with `dcs_motion_sky_occlusion`: the trailing copy 6.3-11.1 -> 0.02-0.08 (the test asserts at
  most a quarter), the aircraft 0.42-0.50 -> 0.41-0.44, the strip ahead of it at 10 px a texel 1.7-2.4 -> 3.0 (the coarse
  class map's edge).
- HUD over a dark rolling sky: unchanged on a UNORM view (5.38), 14.8 -> 11.2 on an sRGB view. A contrail at an aircraft
  over that sky: the contrail 3.7-3.8 -> 3.6-3.9, the aircraft 0.20 -> 0.01.
- `dcs_motion_hud_guard`: the HUD over a dark rolling sky on an sRGB view 14.8 -> 0.76 (the flow alone gives 0.97; the
  test asserts it no worse than the flow), over a bright sky 24.50 -> 24.57; the night trailing copy on an sRGB view
  8.3-11.1 -> 7.3-9.6, a contrail 3.67 -> 3.37; on a UNORM view nothing changes (the same values); thin wires, smoke and
  every surface unchanged. With `dcs_motion_sky_occlusion` too: the HUD 0.76, the trail 0.02-0.06.
- `nvidia_edge_choice`: the NVIDIA occlusion fixtures' moving edges (Medium, one-way) 10.5 -> 9.6 (object), 9.2 -> 7.9
  (both moving), 11.0 -> 10.1 (thin bar); the DCS fixtures' flow at the occlusion's edges 2.41 -> 2.12; the HUD unchanged.
- `dcs_motion_mask_band`: the world inside the mask's blocks 6.7 -> 5.2, the world beyond 0.50 -> 0.33, the cockpit
  unchanged (0).
- All four on: no bright fixture worse (thin wires, smoke, HUD, sky, terrain unchanged); the MVs' fallback to the flow
  0-5.6 % of the pixels.

Measured and removed (no gain, or worse elsewhere): the colour checks on encoded values everywhere (thin wires on an sRGB
view 19.7 -> 22.1), the current frame as the low-confidence fallback (moving edges 9.2 -> 11.9), sRGB-encoded flow input
(the night aircraft better, but a HUD over a dark rolling sky 0.97 -> 10.1 on the flow: the sky's texture then drags the
HUD), the periphery's vectors in the focus fade (0.906 -> 0.911), the flow engine's temporal hints (62 of 64 jobs with
hints over 33 frames of continuous motion: 0.5039 -> 0.5039, flow time unchanged within noise).

GPU (5424x5356 per eye, two eyes, 240 pairs, sRGB view, `XRFG_TEST_DCS_MOTION_BENCH=1 XRFG_TEST_FG_QUALITY_BENCH=<bits>`):
the composition 1.10-1.28 ms (GPU clocks), +1 us with dcs_motion_hud_guard, +5-7 us with all four (identical pipelines:
+-2 us); the pack unchanged. No new resource: no video memory. That was before 0123 split the compositions: the cost
with the keys on is the `_extras` twin's, measured by `XRFG_TEST_FG_TRIPLE_BENCH` above (all quality keys +0.12 ms at 2X and
+0.67 ms at 3X per pair, the fill on Always +0.27 / +1.13 ms).
## Offline evidence and limits

The release's manager and actual WebView2 tests cover both runtime routes, custom/derived caps, precise numeric values, preview invalidation, applied launch checks, restoration and the combined pipeline with the supplied signed DLSS-NR runtime. Native tests exercise OFXR's SteamVR continuous presenter while the application deliberately pauses below the half-refresh target. The exact bundled V116 DLL also passes this delayed-application fixture: presentation continues and frame order is retained. Repeated presentation during a pause is not fresh frame generation.

These fixtures do not establish DCS's actual limiter placement, the live Pimax/Sboys compositor, physical scanout or latency. No real game files or driver settings were changed during this verification.

Primary references: [OFXR source and documentation](https://github.com/tig3rmast3r/OFXR-Bridge), [OFXR overlay semantics](https://github.com/tig3rmast3r/OFXR-Bridge/blob/0.2.1/docs/FPS_OVERLAY.md), [NVIDIA Max Frame Rate and background-limit reference](https://www.nvidia.com/content/Control-Panel-Help/vLatest/en-us/mergedProjects/3D%20Settings/Manage_3D_Settings_%28reference%29.htm). DCS's cap and desktop VSync fields were verified directly in this PC's installed options database.
