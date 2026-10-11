# First headset validation

This procedure is a checklist for a first test on a new PC or headset.

Run **Guided setup** first. [SETUP.md](SETUP.md) covers prerequisites, the Pimax/Sboys installation route and the automatic preflight. On the audited PC, Pimax baseline and combined preparation pass their automatic file checks. Sboys currently has driver 1.3.0-beta.2, no detected OpenXR gaze bridge and no selected tracking route in the generated test profile. Update to the pinned 1.3.0 through its About page, select the actual tracking hardware, and validate gaze or explicitly choose Fixed focus before that route's test. These are detected preparation requirements, not headset measurements.

Start with the original Pimax runtime and your existing working headset setup. Confirm refresh rate, gaze calibration, and tracking. Choose a small repeatable mission with readable cockpit displays. Record the runtime, headset resolution, DCS settings, and CPU/GPU frame times.

1. Capture the baseline with framegen and neural rendering off.
2. Switching profiles needs no restore: Launch DCS writes the new profile over the applied one. Use Back to stock DCS only to return DCS to its original files.
3. Test OFXR in stereo first, with native DLSS SR. Keep other interpolation mechanisms off for this initial comparison.
4. Verify that synthetic frames reach both eyes. OFXR's submission FPS counter is insufficient evidence by itself. The DCS main menu submits no projection layers, so framegen and DLSS only engage in a 3D mission; check `generation_prepare` results in the OFXR flight log and `Available Layers: (3)` in dcs.log.
5. Inspect fast head turns, HUD and MFD text, small distant contacts, clouds, and low-altitude scenery. Compare pacing and latency with the baseline.
6. After a successful stereo test, try native Pimax Quad Views, then the bundled software provider. Inspect the moving focus boundary.
7. Test the Sboys route only after its official GUI confirms the driver and tracking setup. Select SteamVR OpenXR in the manager and verify gaze in the headset.
8. Test Cheeky without Quad Views, initially with fixed foveation. Enable DLSS 5 only with a compatible signed NVIDIA runtime, then compare neural processing cost and readability separately.
9. Test the **DLSS 5 + framegen + Quad Views** pipeline with the bundled software Quad Views provider, the focus adapter on, Focus movement set to Fixed and DLSS 5 on (Foveated Super Resolution is stereo-only and stays off with Quad Views). Confirm stable focus mapping first. Check the Cheeky trace for `DCS quad focus: mapped focus accepted`; persistent `preserving unmapped/peripheral view` messages alone are not proof of adapted processing. Confirm both focus features are routed, not just one. If mapping remains unavailable, the adapter preserves original rendering: record the logs and resource layout rather than treating passthrough as success.
10. Add the supplied NVIDIA neural runtime and enable DLSS 5. Compare before/after SR processing and working scale. Inspect lighting/material differences at the focus boundary, stereo consistency, moving HUD/MFD edges and rapid gaze changes. Compare with neural rendering off; full composition and presentation remain necessary even if the runtime accepts NR calls.
11. Enable dynamic gaze after fixed-focus quality is acceptable. Exercise blinks, invalid gaze, mission loading, recentering and resolution changes. Confirm mapping recovery without persistent history artifacts. Repeat through the other runtime route after its baseline and gaze are proven.
12. Pupil shift (experimental, eye-tracked Quad Views only): turn it on, look at a cockpit switch and move only your eyes left and right. With the correction on, the cockpit should stay put against the outside; Ctrl+Shift+F10 compares. On the Pupil shift page, the gaze range should follow how far you looked and "Runtime poses given back" should say Yes. If the gaze range stays near zero, the focus area is not following your eyes.

The full combined profile uses software Quad Views on both runtime routes. Disable competing Quad Views layers (for example a separately installed Quad-Views-Foveated); the app reports detected layer conflicts during preview. Keep Quad Views on in Pimax Play, as in the tested setup: the bundled one takes over from it for DCS. Do not select Pimax native as the provider in the app for this Cheeky adapter. The Pimax driver/runtime itself remains the original one on that route.

Use **Launch DCS** for every test. Keep the application and backup state directory available. Close DCS and use Back to stock DCS before using a normal shortcut or repairing/updating game files.

Record three runs per configuration and compare median/p95 frame times. A higher FPS counter is useful only if both eyes show stable images with acceptable latency and readability. Run the best candidate for at least 30 minutes and test mission reload and recentering before considering it a usable preset.
