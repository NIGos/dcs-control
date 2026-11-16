# OFXR layer: known limits (0.6.6 / 0.6.7)

What the frame-generation layer (the patched OFXR Bridge fork, `patches/ofxr-djules75`) is known not to do well, as
measured in the bug-fix round of 2026-11-15/16. Each limit has its numbers and the file they come from.

The evidence files are flight notes and logs in the developer's `artifacts\` folder. That folder is not in the
repository; the names are given so a number can be traced to its flight. Patch numbers are files in
`patches/ofxr-djules75`.

Terms: *two layers* = foveated frame generation (a periphery and a focus layer per frame, 0.6.7, off by default);
*composed* or *twin* = the same flight with that option off; *buffer* = the Smoothness buffer (`deep_pipeline`).

## 1. Smooth mouse cursor (0087, 0088)

**1.1 The cursor is redrawn less often over bright cockpit areas, and less with two layers than composed.**
In the stretch flown low in the cockpit (`panel-48`), the cursor was redrawn in 237 of 450 synthetic pairs with two
layers (0.53) against 696 of 900 composed (0.77). The refusals are `score too low` (145 with two layers, 159
composed): the cursor is drawn soft and fat there and matches its template weakly. A refused frame shows the
interpolated cursor (a fainter, smeared cross), never a cross in a wrong place (0 found in either mode).
The captured part of that stretch was over the lower HUD glass. **A cursor over a panel or over the DED text has not
been captured in any flight.**
Evidence: `artifacts\foveated\notes-r4\cursor-flight-check.md`, second flight
(`cursor-speeds2_2x_1115-2347_on` / `_off`), section 3.

**1.2 A cross arm lying on a bright green line is drawn dark or washed out for one synthetic frame.**
Seen in 3 of 54 synthetic frames with two layers and 2 of 54 composed, per eye; 37 to 53 % of the arm's yellow is
gone in that frame. Both modes, so it is not a two-layer defect. The cursor is separated from the scene by how close
a pixel's colour is to the cursor's (0087); yellow on bright green fails that test. 0090 states it, and 1.1, in a
comment beside that test (`cursor_likeness`, `src/d3d12/frame_synthesis.hlsl`).
Evidence: the same file, second flight, section 2 and "Verdicts, second flight", point 2.

**1.3 Fast mouse motion was not flown.** The fastest stretch reached 10 to 11 focus pixels per frame (single steps
up to 19). 20 to 30 pixels per frame, a moving gaze and 3X were never captured.
Evidence: the same file, "What the captures cannot show" and "Verdicts, second flight", point 1.

Where the cursor crosses the focus border there is no visible step: the focus layer's alpha is 0 there, the cursor
shown is the periphery layer's, and the two halves differ by at most 5.7 composed pixels (under one periphery pixel).

## 2. Pimax native quad views at a fixed 2X, adaptive control off, buffer off: 0078 does not arm

0078 moves the release earlier when the first synthetic frame is handed to the runtime before it is written. At a
fixed 2X with adaptive control off and the buffer off a pair has no readiness fence, so nothing is observed, no record
410 is written and the release is not moved. 0083 states this in comments (`latency_control.hpp`, `read_leader_lead`).
Under Auto the case is answered by cadence 3; with the bundled Quad Views it does not arise.

A candidate fix (a fence of its own for this case; it is not patch 0090 of the series) was flown, one flight per arm, F-16C, 150 s:

| | DCS fps (mean) | first synthetic's xrEndFrame p50 / p90 | Pimax "Missed" per second | records 410 / 411 |
|---|---|---|---|---|
| native quad views, 0.6.7 as packaged | 37.6 | 8.36 / 14.98 ms | 15.70 | 0 / 0 |
| native quad views, with the candidate | 39.7 | 5.51 / 11.85 ms | 11.12 | 1 / 227 |
| bundled Quad Views, with the candidate | 45.0 | 0.31 / 0.40 ms | 0.04 | 0 / 0 |

A gain, not a cure: the lead goes at once to its ceiling (one display period, 11.1 ms) and the output is still
written 6 to 10 ms after its hand-over, so about a third of the frames are lost instead of half. A real remedy needs
more than one period of lead. The candidate is in neither version, and the whole test suite was never run with it.
Evidence: `artifacts\foveated\port\final\candidate-native-fixed-2x\README.md`;
`artifacts\foveated\notes-r4\four-points-progress.md`, section A2; flights `verify067\a2_native_old`,
`a2_native_new`, `a2_bundled_new`.

## 3. The 2X release offset guard counts the wait for the presenter (0081, 0084)

With the buffer on, the guard of 0081 measures the application's own time including its wait for the presenter's
capacity (7 to 9.5 ms a frame at 90 Hz). It therefore refuses every step that would raise the release offset while
the buffer paces the application (record 805, about once a second), and the offset stays at 1.7 to 3.6 ms. No harm
was seen from this. 0084 as shipped is comments only: it states the limit at `ReleaseOffsetGuard` and
`read_release_offset_guard`.

The other form of 0084 (the guard's own time without that wait) was flown and dropped:

| Flight (composed profile, buffer on, mission with a map visit) | Release offset |
|---|---|
| 0.6.6 with the fix form (`flight20_066_user`) | 16 steps; 5.08 ms after 10 s; 10.6 ms during the map (1X); 4.37 ms back at 2X; 5.66 ms at the end and still rising |
| A/B, fix form (`flight17_ab_c1`) | 9 steps, 3.92 ms |
| A/B, 0081 as flown (`flight16_ab_0081`) | 3 steps, 2.27 ms |
| 0.6.6 final, comment form (`flight25_066def_user`) | 3 steps up to 3.20 ms, 805 x155 |

Both 0.6.6 flights held 45 fps with 0.2 % of frames late. The fix form let the offset climb back to the values 0081
was made to end, with no gain in any flight, so the guard stays as flown.
Evidence: `artifacts\overnight\2026-11-15\_registro.md`, entries 03:42 and 05:35;
`patches/ofxr-djules75/0084-*.patch`; commit 97bf2c6.

## 4. Review findings kept as designed

From the review of 0077..0081 (`artifacts\overnight\2026-11-15\ofxr-review-0077-0081.md`); the decisions are in
`artifacts\foveated\notes-r4\bugfix-round-progress.md` (C4, C5).

**Finding 4: the hold after missed slots at cadence 2 never lapses by itself.** It ends on the application's work
changing, a scene change, a start or a stop. That is 0079's stated and flown rule, and the unit tests assert it. A
lapse would be a change of policy on the path flown for issue #23, which only a flight can judge. The sequence that
would justify it has not been produced: `adaptive_switch result=16` followed by `presenter_transition result=802` and
missed slots at 2.

**Findings 6 and 8, what 0090 changed.** At a presenter start the run of late first synthetics (`leader_late_streak`)
and the hand-over still to be judged (`leader_block_pending / written / call`) start over, beside the state that
already did; the table of private swapchain images kept for the log (`g_private_image_log`, 128 entries for the
process) is emptied when a session is destroyed. None of the three is shown by an offline test (state inside the
layer; the fake runtime hands out the same handles again): the whole suite passes with them. Left as it was from
finding 6: the application's completion samples keep filling while 0078's lead is in force.
**Finding 5: `release_rephase_anchor` stays set across a surplus hold.** After a re-phase the estimate of the time
told to the application can be one period off until the count starts over at a presenter start. By the review's own
account this restarts nothing by itself; it was not reproduced, and no rig observes that estimate.

## 5. Two layers cost more than composed when the focus area is very large

With a focus swapchain of 4556x2700 per eye (heavy profile, buffer on, Auto, running at cadence 3):

| | Two layers (`flight14_load_fov`) | Composed (`flight15_load_off`) |
|---|---|---|
| synthesis of a pair on the GPU | 11.2 to 11.5 ms (p95 12.2 to 13.3) | 7.8 ms (p95 8.3 to 8.5) |
| median margin before the first synthetic's slot | 4.8 ms | 8.2 ms |
| record 406 (a pair finished after its slot) | 225 | 22 |

The focus layer alone takes 9.6 to 10.6 ms at that size. With a focus of 3580x3536 at a fixed 2X two layers were
still the cheaper (5.48 / 6.07 ms against 7.07 / 10.41, p50 / p95); the size at which the two are equal was not
measured.
Evidence: `artifacts\foveated\notes-r4\startup-warm-and-overload.md`, point 6 and its section;
`artifacts\overnight\2026-11-15\foveated.md`.

## 6. `[ofxr]` keys that are never written, or always written the same

The app writes `ofxr_bridge.ini` (`ConfigurationWriters.cs`); the layer reads it (`implicit_layer.cpp`). Every key
the app writes is read, and no reader is left for a removed key.

**Read by the layer, never written by the app.** The default always applies. They are kept as kill switches (one
line each, and the off arm is what the `*_off` tests run) unless marked otherwise.

| Key | Patch | Default | What it is |
|---|---|---|---|
| `adaptive_probe` | 0007 | 1 | kill switch |
| `own_binding_queue` | 0007 | 1 | kill switch |
| `display_time_correction` | 0003 | 1 | kill switch |
| `lazy_runtime_swapchains` | 0008 | 1 | kill switch |
| `history_in_place` | 0008 | 1 | kill switch |
| `lazy_synthetic_slot` | 0034 | 1 | kill switch |
| `leader_lead` | 0078 | 1 | kill switch |
| `cadence_accountable` | 0079 | 1 | kill switch |
| `release_offset_guard` | 0081 | 1 | kill switch |
| `cockpit_parallax_limit` | 0034 | built in | tuning value, never exposed |
| `late_frame_skip` | 0006 (off since 0009) | 0 | unshipped experiment: the late-skip path is permanently off, kept alive by its tests |
| `foveated_uncovered_alpha` | 0080 | 0 | unshipped experiment |
| `foveated_uncovered_exact` | 0080 | 0 | unshipped experiment |
| `[diagnostics] hud_capture` | 0053 | 0 | developer capture |
| `vulkan_bridge`, `control_event`, `frame_generation` | upstream | 1 / empty / - | upstream's own |

**Written by the app with one constant value.**

| Key | Patch | Written | What it is |
|---|---|---|---|
| `motion_vectors` | upstream | `off` | constant |
| `cockpit_mask` | 0031 | `1` | constant (costs nothing without the engine module) |
| `cockpit_depth` | 0035 | `1` | constant |
| `[diagnostics] flush_each_event` | upstream | `0` | constant |
| `parallel_eye_synthesis` | 0038 | `0` | unshipped experiment ("off until flown" since 0038) |
| `foveated_periphery_flow_scale` | 0080 | `100` | the reader accepts 50, 75 and 100; only 100 is written |

**Written from a profile field that has no control in the app:** `pipelined_deep_hold`, `diag_vram`.

**Not independent:** `foveated_layers`, `foveated_uncovered_fill`, `warm_private_images` and
`lazy_runtime_reclaim_ms` all follow the one foveated switch; `cursor_follow_mouse` always equals `smooth_cursor`.

Related, and not reachable in DCS: with `warm_private_images=1` (written whenever foveated frame generation is on)
the warm-up counts an image per view before the layer's other views are known usable. DCS uses array swapchains, so
the second view finds its swapchain already handled.
