# Installation and guided setup

Run `DcsControl-<version>-Setup.exe` (recommended): it lets you choose the install folder and the Start menu / desktop shortcuts, verifies the payload, installs the app for the current user with an entry in Settings → Apps, and offers to open it. **Uninstall DCS Control** (Settings → Apps) removes it again. Or extract the binary ZIP (keep its corresponding-source ZIP alongside it) and run **Install DCS Control.cmd**, or start `DcsControl.exe` directly; that installation gets a Start menu shortcut. It checks WebView2 and the Microsoft C++ x64 runtime (14.50 or newer). Missing prerequisites are downloaded from Microsoft, checked for a valid Microsoft signature and installed. Internet access and Windows elevation may be needed for the C++ runtime. Neither VR drivers nor game mods are activated by application installation.

On this development PC the prerequisites are already present. Their missing-prerequisite installation branches have not been exercised on a clean Windows machine. For an offline PC, obtain the official Evergreen WebView2 standalone installer and C++ x64 runtime in advance. Do not substitute the Edge browser for WebView2 Runtime. The self-contained package includes .NET.

Open **Guided setup** in the right-hand panel:

1. **Choose**: the headset route (Pimax Play or Sboys/SteamVR; the running route is preselected), the features (Engine Optimizations, CPU Boost, Quad Views, DLSS 5, frame generation; or Optimizations only for a monitor or another headset) and, for pipelines with Quad Views, eye-tracked or fixed focus. Your current quality values are kept. Changing routes clears a runtime override that belonged to the previous route.
2. **Check**: the app checks files, runtime architecture, package hashes, the neural DLL signature/version, Sboys registration, SteamVR add-on blocking and the gaze bridge. Problems are listed first; passed checks and file paths are folded away and open by themselves when a path is the problem. DLSS 5 needs your signed NVIDIA nvngx_dlssnr.dll (tested contract 310.8); the app does not download it. Nothing is written in this step.
3. **Fly**: start the headset software and press **Launch DCS** here. It is the same button as in the right panel: the checked profile is written over whatever is applied (originals are backed up the first time a file is written), and DCS starts (DCS skips its own launcher window while a profile is applied) so DCS receives its private OpenXR environment. Keep Steam running for the Steam edition, and run neither DCS nor the app as administrator. The headset checks listed there need your eyes; they are never reported as automatic passes. **Back to stock DCS** in Recovery puts the original files back at any time.

Closing the wizard before **Launch DCS** writes nothing and leaves the editable profile and its quality values intact; **Keep as draft without launching** takes the choice into the editor without writing anything.

## Pimax Play route

Install Pimax Play from its official distribution and verify the headset works there. The original runtime should be detected as PiOpenXR_64.json. Use Pimax Play OpenXR for Crystal Super; the older third-party PimaxXR runtime does not support this headset. The wizard's combined stage chooses our bundled software Quad Views compositor. Leave Quad Views **on** in Pimax Play (with eye tracking): that is the tested setup. The bundled Quad Views takes DCS's quad views over from Pimax's own, so DCS only ever sees the bundled one, and the app reads your Pimax Play Quad View values. Only the provider choice in the app matters: select Bundled, not Pimax native, for DLSS 5. (Pimax native as the provider cannot host the DLSS 5 focus adapter.) Provider-specific settings must be checked in Pimax before the headset session; the tool does not silently change them.

For eye-tracked focus, enable and calibrate eye tracking in Pimax and verify that the focus region actually follows both eyes. Fixed focus is an explicit alternative and retains the configured image-quality values.

## Checks added in 0.4.2

- **Standard user launch**: error when the app is elevated or DCS.exe has a Run-as-administrator compatibility flag; elevated processes ignore the private runtime and layers.
- **Write access**: error when the Saved Games Config folder, or the DCS bin folder for Cheeky profiles, is not writable as the current user (common under Program Files). Launch DCS asks Windows for the permission itself (one prompt), and Checks has a Grant access button; never elevate the app instead.
- **Other OpenXR layers**: warning listing enabled implicit layers; they load above the profile layers.
- **Steam edition launch**: manual reminder to keep Steam running and confirm the layers loaded (`Available Layers` in dcs.log).
- **Gaze bridge**: on the Sboys route the profile loads OpenXR-Eye-Trackers explicitly below Quad Views; without a bridge, eye-tracked focus falls back to a fixed region (warning).

## Sboys / SteamVR route

1. Install SteamVR and establish a working headset connection. Sboys 1.3.0 requires **Pimax EVO for SLAM/P2 tracking**; Lighthouse can use EVO or Pimax Play.
2. Choose **Download / verify Sboys** or **Import official ZIP**. The tool pins 1.3.0 and checks the archive SHA-256 before extracting. It downloads directly from the official release; the binary archive is not bundled with this release because it includes private functionality without an established redistribution grant.
3. Choose **Open Sboys tool**, open its **About** page and install/reinstall its driver. Configure the Crystal Super Micro OLED profile, tracking, IPD and distortion there. This external driver installation has its own lifecycle and is not part of the DCS mod recovery journal.
4. Confirm CustomHeadsetOpenVR is enabled in SteamVR Manage Add-ons and that tracking works. Refresh preflight; the tool checks registration plus the driver DLL, not merely an extracted directory. Prepared package files are checked again before opening its GUI.
5. Dynamic OpenXR gaze requires a valid gaze provider. Quad Views documents OpenXR-Eye-Trackers for the original Crystal on SteamVR. That archived guide does **not** establish Crystal Super compatibility. Verify its test application and your hardware/provider combination. If gaze is unavailable, explicitly choose Fixed focus for the initial tests. An enabled DLL alone cannot establish valid eye data.

The manager chooses the OpenXR runtime for its DCS process; it does not activate Sboys merely because the route was selected, or change the global runtime. An existing unrelated OpenXR layer can still affect the process. Review detected layers and competing overlays/injectors if the live result differs from the preview.

## Readiness and recovery

Checks → **Run preflight** checks the current draft. **Export report** refreshes inventory and includes readiness plus the original files DCS Control changed. Recovery → **Back to stock DCS** puts every one of them back in one step (`DcsVr.Cli.exe status` and `DcsVr.Cli.exe restore` do the same from a console). A fresh report is also available without opening the GUI:

```powershell
./DcsVr.Cli.exe readiness --profile profile.json --out readiness.json
```

Exit status 2 means automatic preparation has blockers. Before launch, saved file/settings and external runtime/provider hashes are checked again. This does not certify the headset, gaze or private vendor services. Export a new report when changing drivers or runtime versions and Restore any active transaction before preparing another stage.

Use [FIRST_FLIGHT.md](FIRST_FLIGHT.md) for the detailed live acceptance procedure, including focus-hook trace evidence and both-eye presentation. Passthrough rendering, an accepted neural call, a rising FPS counter or a successful installer is not proof that the complete combined pipeline works.

## Official references

- [Microsoft WebView2 deployment](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution)
- [Microsoft C++ runtime](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)
- [Sboys 1.3.0 release and tracking requirements](https://github.com/sboys3/CustomHeadsetOpenVR/releases/tag/1.3.0)
- [Sboys source and private-component notice](https://github.com/sboys3/CustomHeadsetOpenVR/tree/1.3.0)
- [Quad Views Pimax setup](https://github.com/mbucchia/Quad-Views-Foveated/wiki/Pimax-Crystal)
- [Archived OpenXR gaze bridge Crystal setup](https://github.com/mbucchia/_ARCHIVE_OpenXR-Eye-Trackers/wiki/Pimax-Crystal)


## FPS limits before a live test

Open **Framegen** and confirm the actual headset refresh rate. Preserve is the default. Compare Runtime pacing (DCS cap 300) with Match refresh or a custom rendered cap while keeping image-quality values fixed. For 90 Hz, Match refresh uses 45 rendered FPS with OFXR and 90 FPS with framegen off. Check external/background caps and runtime interpolation manually. Preflight reports detected limits and unknown states honestly; **Review files** lists the exact cap and VSync changes before you launch. See [FRAME_PACING.md](FRAME_PACING.md) for the full comparison and recovery procedure.
