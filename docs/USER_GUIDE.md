# DCS Control — User Guide

**Version 0.5.5-preview (early beta).** This guide is for pilots. It explains what the app does, how to set it up and how to get help. Developer documentation lives in the [README](../README.md#for-developers) and the other files in `docs/`.

> **Early beta.** DCS Control has been flown mostly on one PC (Pimax Crystal Super, RTX 5090, DCS 2.9 Steam edition). Expect rough edges. Every file it changes is backed up and can be put back with one click (**Back to stock DCS**), so trying it is safe — but please read [Requirements](#requirements) and [Launch DCS](#launch-dcs) before your first flight.

![Overview with the last flight](screenshots/overview-last-flight.png)

## Contents

- [What the app does](#what-the-app-does)
- [Requirements](#requirements)
- [Install](#install)
- [First run](#first-run)
- [The features](#the-features)
- [Launch DCS](#launch-dcs)
- [In flight: hotkeys and the diagnostic panel](#in-flight-hotkeys-and-the-diagnostic-panel)
- [After the flight: the Last flight card](#after-the-flight-the-last-flight-card)
- [Checks](#checks)
- [Back to stock DCS](#back-to-stock-dcs)
- [Troubleshooting and FAQ](#troubleshooting-and-faq)
- [Reporting a problem](#reporting-a-problem)
- [Support](#support)

## What the app does

DCS Control makes DCS World faster and, on Pimax headsets, runs **DLSS 5 + frame generation + Quad Views together in VR**, set up and launched with one button, without you copying files by hand. It was called **DCS VR Control** before 0.5.0; it is no longer only for VR.

**For every pilot, on a monitor or with any headset**

| Feature | What you get |
| --- | --- |
| **Engine Optimizations** | A small module DCS loads by itself from Saved Games takes CPU work off DCS's render and model threads, with exactly the same image: about +50-60 % FPS at a busy airfield when DCS is CPU-bound. It also fixes a DCS terrain loop that wastes CPU. Once applied it stays on every time DCS starts, also from Steam or a desktop shortcut. |
| **CPU Boost** | Gives DCS priority and its best CPU cores while it runs, and keeps background apps off those cores. Plus Free VRAM before flight. |

Not on a Pimax? Choose **Optimizations only** in the right panel: DCS runs exactly as you set it up and the app adds only these two (see [Optimizations only](#optimizations-only-any-other-headset-or-no-vr)).

**For Pimax headsets (Pimax Play, or SteamVR with Sboys)**

| Feature | What you get |
| --- | --- |
| **Quad Views** | Foveated rendering: DCS draws a sharp *focus* area where you look (eye tracked or fixed in the centre) and a lower-resolution periphery. Uses the focus values you already set in Pimax Play. |
| **DLSS 5** | NVIDIA's DLSS-NR neural rendering, applied to the Quad Views focus area on DCS's own DLSS (you bring the NVIDIA runtime file). Without Quad Views, optional Foveated Super Resolution (Cheeky) saves GPU outside where you look. |
| **Frame generation** | OFXR optical-flow frame generation: DCS renders half (or a third) of the headset refresh rate and generated frames fill the rest. |

You tick the features you want in the right panel, adjust them on their pages if you like, and press **Launch DCS**. The app writes the needed files into DCS (backing up the originals the first time), starts DCS, and puts everything back when you press **Back to stock DCS**. What stays on without the app: Engine Optimizations, terrain prefetch fix included. What needs DCS started from the app: the VR features and CPU Boost's priority and cores.

What it does **not** do: it does not change Windows settings, your GPU driver settings, your global OpenXR runtime or your DCS graphics settings (apart from a few listed `options.lua` values such as the frame limit with the VR features, which it restores). It does not add NVIDIA DLSS Frame Generation to DCS; frame generation here is OFXR's optical flow.

## Requirements

- **Windows 10 or 11, 64-bit** (tested on Windows 11; Windows 10 is supported by everything the app uses but has not been flown yet). Run as a normal user — not as administrator.
- **For Engine Optimizations and CPU Boost:** any GPU and headset, or none. The items below are for the VR features.
- **NVIDIA RTX graphics card** for DLSS 5 and for NVIDIA optical-flow frame generation. (The Framegen page also offers FidelityFX, which works on any GPU, but the tested setup is NVIDIA.) Development and testing: RTX 5090.
- **Pimax headset** (tested: Crystal Super Micro OLED) with either
  - **Pimax Play** (the original Pimax OpenXR runtime), or
  - **SteamVR with the Sboys driver** (CustomHeadsetOpenVR 1.3.0; for SLAM tracking it needs Pimax EVO).
- **DCS World 2.9**, Steam edition or standalone.
- **Microsoft Edge WebView2 Runtime** (already on most Windows 10 and 11 PCs) and the **Microsoft Visual C++ x64 runtime 14.50 or newer**. `Install DCS Control.cmd` checks both and installs them from Microsoft if they are missing.
- For DLSS 5: your own signed NVIDIA **`nvngx_dlssnr.dll`, version 310.8** (see [DLSS 5](#dlss-5)). It is not included and the app does not download it.

## Install

Download `DcsControl-0.5.5-preview-win-x64.zip` from the [Releases page](https://github.com/NIGos/dcs-control/releases) and extract it to a folder of your choice. Then either:

- **Run it directly:** double-click `DcsControl.exe` in the extracted folder (.NET is included), or
- **Install it:** double-click `Install DCS Control.cmd`. It verifies the files, installs the app for your Windows user under `%LOCALAPPDATA%\Programs\DcsControl`, adds **DCS Control** to the Start menu, and installs WebView2 / the C++ runtime from Microsoft if they are missing (this step may need internet access and an administrator prompt for the C++ runtime).

The folder contains only three things: **`DcsControl.exe`** (the app), **`Install DCS Control.cmd`** and a **`files`** folder with everything else (keep it next to `DcsControl.exe`), plus a short `README.txt`. The folder can be anywhere: Desktop, Downloads, another drive. The app keeps its own data (backups, profiles) in `%LOCALAPPDATA%`, so moving the folder loses nothing.

**How the app finds DCS:** the standalone version from Windows' list of installed programs ("DCS World", also OpenBeta), the Steam version from your Steam libraries, and your settings from `Saved Games\DCS` (or `DCS.openbeta`). If it finds nothing, or you have more than one install, pick `DCS.exe` and `options.lua` on the **DCS & headset** page.

If you had **DCS VR Control** installed (the app's name before 0.5.0), `Install DCS Control.cmd` replaces it and its Start menu entry; your backups and profiles stay where they are and Back to stock DCS keeps working.

Installing the app does not touch DCS or your VR software. Nothing is written into DCS until you press **Launch DCS** (or **Apply without launching**).

The preview is not code-signed, so Windows SmartScreen may warn you the first time you open it.

> **Always open the app from the Start menu or from Explorer**, not from a script runner or another program's terminal. See [the DCS launcher option](#the-dcs-launcher-option) for why.

## First run

### Detect my setup

Click **Detect my setup** in the right panel. The app looks at what runs on your PC and proposes a profile: the headset route (Pimax Play, or SteamVR with Sboys), eye tracking, the refresh rate SteamVR uses, your Pimax Play Quad View values and a valid DLSS 5 runtime file (it looks in Downloads, Documents and Desktop). Each value shows where it came from; nothing changes until you accept.

### Guided setup

Or click **Guided setup** for three short steps:

1. **Choose** — how you fly (Pimax, Sboys, or Optimizations only for a monitor or another headset), the features you want and, with Quad Views, whether the focus follows your eyes or stays centred. Your quality settings are kept.
2. **Check** — the app checks files, the VR runtime, package hashes, the DLSS runtime file and (on Sboys) the driver and gaze bridge. Problems are listed first; nothing is written in this step.
3. **Fly** — start your headset software, then press **Launch DCS**.

![Guided setup, step 1](screenshots/guided-setup-choose.png)

Closing the wizard before **Launch DCS** writes nothing.

### The right panel

![The right panel](screenshots/right-panel-features.png)

- **How you fly** — **Pimax** (Pimax Play), **Sboys** (SteamVR), or **Optimizations only** (a monitor, or any other headset with its own software). The line below shows which VR software is running; the app warns you if your profile uses the other one.
- **Features** — one row per feature, the optimizations first: tick it on or off, read its current settings at a glance, click the row to open its page. With Optimizations only, the VR features are hidden.
- **Import / Export** — load or save a profile file.
- **Launch DCS** — the one button you need. Under it: **Review files** (what Launch will write, read-only), **Apply without launching** and **Back to stock DCS**.

Everything you change is kept as a draft across app restarts. **Undo changes** (shown when the draft differs) brings back what is currently installed.

## The features

### Quad Views

![Quad Views with Pimax Play's Quick values](screenshots/quad-views-pimax-play.png)

The app uses its **bundled Quad Views** (Quad-Views-Foveated). This is the one that works with DLSS 5 and frame generation. **Pimax native** Quad Views (Pimax route only) is also offered as the provider, but it cannot be combined with DLSS 5. Leave Quad Views **on** in Pimax Play (with eye tracking): that is the tested setup. The bundled Quad Views takes DCS's quad views over from Pimax's own, so DCS only ever sees the bundled one, and the app reads your Pimax Play Quad View values. Only the provider choice in the app matters: select Bundled, not Pimax native, for DLSS 5.

- **Focus movement** — *Eye tracked* (needs working eye tracking in Pimax Play, or a gaze bridge on Sboys) or *Fixed (centred)*.
- **Focus area source** — *From Pimax Play's settings* (default) or *This profile*.
  - With **Pimax Play's settings**, the app reads your Quad View values from Pimax Play every time you launch (on both routes; it never writes to Pimax). The page shows them **exactly as Pimax Play shows them**: same labels, same order, same rounding — for **Quick** mode: Center Resolution, Peripheral Resolution, Horizontal FOV, Vertical FOV, Vertical Offset, Transition Mode; for **Fine** mode: the "→ Center" sliders, Transition Range and Alpha.
  - The app follows whichever tab is active in Pimax Play. **Note:** in Pimax Play, simply clicking the *Fine* tab switches Pimax to Fine mode, so the app will read the Fine values from then on. Go back to *Quick* in Pimax Play if that is the one you want.
  - If you change the values in Pimax Play, you don't need to do anything else: the next **Launch DCS** updates the focus area ("Pimax Play changed: focus updated to …").
  - With **This profile**, you set the values in the app, in the same Quick units as Pimax Play.
- **Focus sharpening**, **edge blending**, **turbo mode** — fine-tuning. The focus area is round; edge blending is how wide its edge fades into the periphery (wider is softer, a little less fully sharp area). Turbo is off by default: in flight it caused a visible "old frames" effect when turning the head.
- **Focus ahead of fast eye movements** and **Steady focus while looking** (both on, eye tracked only). With frame generation the focus area follows your eyes only on the frames DCS renders and reaches the headset 55–88 ms later, so after a quick glance your eye can land outside the sharp area for a frame or two. The first option makes the focus area reach ahead along a fast eye movement while it lasts, by about that latency (25 ms without frame generation, 55 ms in 2×, 85 ms in Auto and 3×). The second keeps the focus still while your eyes rest (movements under half a degree), so eye-tracker noise does not make its edge shimmer. Neither costs GPU time: the focus keeps its pixel count and is only briefly less dense while the eyes move. The Quad Views log has a `DCSVR gaze` line every 30 s with the saccades seen and how often each option acted.
- **How bundled Quad Views reproduces this** (folded) shows the conversion and, once DCS has run, the focus size in pixels per eye.

**Cost/benefit, measured on the test PC (Crystal Super, RTX 5090):** with Pimax Play's own Quad View values, bundled Quad Views ran at about **78 FPS** with working eye tracking, against about 37 FPS with the much larger old defaults. At the same Pimax percentages, bundled Quad Views draws a smaller focus than Pimax's own runtime does, so it is roughly three times cheaper. The bundled provider is limited to 90% of the view per axis.

### DLSS 5

![DLSS 5 page with the saved runtime copy](screenshots/dlss5-runtime-file.png)

DLSS 5 runs NVIDIA's DLSS-NR neural model on the two Quad Views focus views. DCS's own DLSS keeps running as usual.

**The runtime file — bring your own.** DLSS 5 needs NVIDIA's `nvngx_dlssnr.dll`, **version 310.8**, x64, signed by NVIDIA Corporation. It is not included in this app. Select it once with **Browse** (or let **Detect my setup** find it): the app checks it and keeps a **verified copy** in `%LOCALAPPDATA%\DcsControl\runtimes`, which every profile then uses. The page shows "Saved copy · version 310.8.0.0 · from …" with **Change…** and **Forget**. A file that fails the checks is refused and nothing is saved.

Settings:

- **Working scale** — lower = less GPU work and less neural detail.
- **DLSS 5 area** — the whole focus area (default) or its central 80%, 70% or 50%, faded into the normal image at the edge. Measured GPU time for both eyes on an RTX 5090 at 1764×2480 per eye: **10.75 ms** whole area, **7.9 ms** at 80%, **6.35 ms** at 70%, **4.8 ms** at 50%.
- **Intensity** (default 1 = full model output) and **Model style** (Standard, Natural, Cinematic) change the look, not the cost.
- **Process at render resolution** — on: before DLSS upscaling (faster, subtler); off: at output resolution (more detail, more GPU).
- **In-flight toggle key** — turn DLSS 5 on/off in flight to compare (default **Ctrl+Shift+F12**).
- **Advanced image controls** (folded) — model corrections, all at their defaults; change one only to fix something specific you can see. **Reset to defaults** puts them back.
- **Foveated Super Resolution** (Cheeky) is a separate switch on the same page, for **stereo without Quad Views** only: a performance option that runs DLSS at full quality only where you look and lighter in the periphery. With Quad Views it is hidden and stays off: Quad Views already renders the periphery at lower resolution, and DLSS 5 builds on DCS's own DLSS of the focus views (with Foveated Super Resolution on, Cheeky would redo that upscale privately instead).

### Frame generation

![Framegen page](screenshots/framegen-auto-2x-3x.png)

OFXR generates frames between the frames DCS renders. The top of the page shows what DCS needs to render for your headset refresh rate (for example 45 FPS for 90 Hz).

- **Frame generation** — *OFXR · NVIDIA Optical Flow* (needs an RTX GPU) or *FidelityFX*.
- **Generated frames**
  - **2×** — DCS renders half the refresh rate (45 FPS at 90 Hz).
  - **3×** — DCS renders a third (30 FPS at 90 Hz): more headroom, but **more latency and more artifacts in sideways motion**.
  - **Auto 2×/3×** (default) — runs 2× while DCS can hold half the refresh rate with margin, drops to 3× when DCS falls behind, and tries 2× again later. The diagnostic panel shows "2x auto" / "3x auto".
- **Smoothness buffer** (on by default) — keeps one more frame in flight: smoother when DCS frame times vary, but adds about one refresh of latency (about 11 ms at 90 Hz). Off = lower latency, more stutter when frame times jump.
- **Smooth mouse cursor** (off by default) — DCS's mouse cursor is a small picture that frame generation cannot follow, so in generated frames it fades between where it was and where it is. On: generated frames find the cursor in the two real frames and draw it at its in-between position, so it moves at the headset's rate. Only the generated frames change; where you click doesn't change. When anything else could be the cursor (a look-alike symbol right next to it, or too much moving to search), that frame is left exactly as without the option. It works only while the mouse has moved in the last few seconds and costs about 0.05 ms of GPU time per real frame then (RTX 5090), nothing otherwise. See [Frame pacing](FRAME_PACING.md#smooth-mouse-cursor) for how to check it in a flight.
- **Latency trade-off, measured on an earlier build:** about 55 ms in 2× and about 88 ms in 3× from the start of a DCS frame to its display. If 3× feels laggy, see the [FAQ](#framegen-3-feels-laggy).
- **Optical flow resolution / quality** and **bidirectional flow** — higher costs more GPU.
- **DCS frame limit** — keep your current DCS limit (default), match the refresh rate, 300 FPS, or a custom value. Written to `options.lua` and restored with the originals.
- **Desktop VSync off** — stops VSync of the desktop DCS window from holding the frame rate back. Headset sync is unaffected.
- **Before you fly:** turn off other limiters and smoothing that fight frame generation — NVIDIA *Max Frame Rate* / *Background Application Max Frame Rate*, RTSS, Pimax Smart Smoothing, SteamVR motion smoothing. The app does not change these; Checks reminds you.

### CPU Boost

![CPU Boost and What Boost will do](screenshots/cpu-boost-plan.png)

CPU Boost is off by default. When on, a small helper starts with DCS and **undoes everything when DCS exits**:

- **DCS priority** — Above normal (default), Normal or High (High can starve audio, input and headset services).
- **Move VR runtime and headset services** off the cores DCS uses for its main and render threads (Pimax route; on SteamVR the compositor is left alone).
- **Move background apps** (your list, e.g. browsers, OneDrive) to the slowest cores at below-normal priority.
- The core ranking comes from DCS's own CPU classification in `dcs.log`, so **DCS needs to have run once** on this PC. Without a ranking, only DCS's priority changes, and the page says so.
- **What Boost will do** shows the exact plan for your PC before you launch: which processes, which cores, what is running now, and what needs administrator rights. **Administrator rights** (optional) lets it move services that run under other accounts; DCS still starts as your user.

The terrain prefetch fix that used to be a CPU Boost option is now part of [Engine Optimizations](#engine-optimizations-cpu).

#### Flight helpers (VRAM)

Four more options on the CPU Boost page, all off by default and usable without CPU Boost:

- **Free VRAM before flight** — closes the listed programs when DCS starts so their video memory goes to DCS (default list: NVIDIA Overlay, Edge, ChatGPT, Hue Sync, Razer Cortex/Synapse, Wallpaper Engine; Discord and OBS are deliberately not listed). Programs get a normal close request; they are ended after 5 s only if you allow it. **Reopen after the flight** starts them again when DCS exits. What Boost will do shows each program's VRAM now (approximate). Measured on the test PC: NVIDIA Overlay held about 1.4 GB.
- **Pause the desktop Tobii eye tracker** — for a Tobii Eye Tracker 5 or 4C next to a Pimax Crystal. Its software (Tobii Experience) also takes hold of the headset's Tobii eye tracker, and Pimax eye tracking (and so the moving focus area of Quad Views) stops working. With this on, the desktop tracker's services (Tobii Experience and the Eye Tracker 5/4C runtime) are stopped while DCS runs and started again when DCS exits; the headset's own eye-tracking services are never touched. It needs administrator rights, so Windows asks (UAC) when DCS starts. What Boost will do lists the services it would pause.
- **Small DCS window in VR** — sets the DCS desktop window to 1280×720 windowed (restored with the originals). The headset image is unchanged; the smaller desktop buffers save some VRAM and GPU time.
- **Lower the monitor while flying** — switches your main monitor to a smaller mode (1920×1080 at 60 Hz suggested) for the flight only; the previous mode comes back when DCS exits, even after a crash (at the next start). Windows never saves the flight mode. HDR is left alone.

### Engine Optimizations (CPU)

![Engine Optimizations page](screenshots/engine-optimizations.png)

**Engine Optimizations** is its own page, off by default, and it is for every pilot, on a monitor or with any headset. Once applied it stays on every time DCS starts, also from Steam or a desktop shortcut, until you turn it off or use Back to stock DCS. When on, Launch DCS installs a small module (DcsQvCull) that DCS loads itself through a Lua hook in `Saved Games\DCS\Scripts\Hooks`; nothing in the DCS install folder changes. It removes CPU work from DCS's render thread without changing what is drawn:

- **Streaming timer cache** (on) — DCS reads a high-resolution clock on every texture bind, tens of thousands of times a frame, only to timestamp texture-streaming use. The module serves it from a value refreshed every millisecond, far finer than streaming needs; nothing you see changes. Measured: **+5 to +6 % FPS**.
- **Culling partition boost** (on) — DCS splits the search for visible objects into 12 uneven tasks and waits for the slowest. The module splits the same work into 16; the same objects are found. Measured: **+2 % FPS**, p95 frame time −3 %.
- **Culling partition by real cost** (on) — the visible-object search is split into tasks by what each object actually costs to check, measured as DCS runs, so the tasks end together. Measured: **+2.75 % FPS**, p95 −4 %.
- **Frame memory per thread** (on) — short-lived per-frame memory comes from one shared heap the model threads queue for; each thread gets its own blocks. Measured: **+1.5 % FPS**, culling 9 % faster.
- **Task queue clock cache (camera motion)** (on, needs the streaming timer cache) — the frame-start drain of DCS's task queue reads the cached clock. It helps while the camera moves fast (render-thread drain 2.9 → 1.6 ms per frame in a fast flight) and changes nothing when static.
- **Shadow caster instancing (exact)** (on) — identical objects casting shadows are drawn with one instanced draw per shadow cascade. The module compiles instanced versions of DCS's own shadow shader in the background, about 2.5 minutes after DCS starts (nothing is written to the DCS install); until then shadows are drawn as before. Shadow draws also skip the textures those shaders never read, and the grouping is planned off the render thread. Verified texel-identical; it turns itself off on any fault. Measured: **+2.8 % FPS**, **+1.9 %** more from planning in the background, and 2–4.5 % shorter frames while the head moves (p95 30.0 → 25.7 ms in slow head motion).
- **Model data allocator (parallel)** (on) — the threads that prepare aircraft and object models share one allocator and wait for each other; the module gives each thread its own blocks. Measured: **+6 % FPS**, p95 −9 %.
- **Texture streaming dedupe** (on) — repeated streaming requests for the same texture and size within a millisecond are skipped. Measured: **+2 % FPS**.
- **Effect constant-buffer skip** and **Statistics counter without lock** (on) — a constant buffer set again although it is already set is skipped, and a statistics-only triangle counter no longer makes the model threads wait on each other. Measured together: **+3.6 % FPS**.
- **Terrain prefetch fix** (Off / Observe / Skip repeats; Skip by default) — DCS's terrain workers ask Windows to prefetch the same memory ranges about a million times a second. The fix skips the repeats; a prefetch is only a hint to Windows, so the image cannot change. **Observe** only counts the calls. Measured in an A/B: about **4 % less DCS CPU**, more on CPU-bound scenes. Statistics every 10 s in `Saved Games\DCS\Scripts\DcsQvCull\DcsQvCullPrefetch.log`. Until 0.5.4 this was a CPU Boost option that put `dxgi.dll` and `dxgi2.dll` in DCS's `bin` folder; the next Launch DCS (or Apply) removes them and puts back your own `dxgi.dll` if you had one.
- **Multi-threaded G-buffer** (on) — DCS draws the scene's models into its G-buffer on the render thread, one at a time. The module records those draws on worker threads in DCS's exact order and DCS replays them; parts whose textures changed are recorded again. Verified pixel-identical live. Measured: **+22 to +24 % FPS** on its own. It uses about 12 more worker threads and turns itself off, with a warning on the page, if DCS's code is not what it expects.
- **Multi-threaded shadows** (on) — DCS draws the shadow cascades on its render thread, one object at a time. The module records those draws on 8 worker threads and DCS replays them; the nearest cascade but one stays as DCS draws it. Verified texel-identical live. Measured: **+3 to +5 % FPS**, p95 −4 to −7 %; total CPU use rises 4 to 7 % because the work moves to other cores, and it uses about 30 MB more memory. It turns itself off, with a warning on the page, if DCS's code is not what it expects.
- **Redundant state filter** (on) — DCS's renderer sets graphics state (shaders, buffers, textures, samplers) that is already set, about 228,000 times a frame on an airfield; the repeat is skipped where DCS makes the call. Verified identical live (362 frames, no mismatch). Measured: about **+0.5 to +1 % FPS** and **1.2 % less CPU**. It turns itself off after a DCS update until it is checked against the new build.
- All together about **+50-60 % FPS** depending on the scene (+46.0 % ±1.1 and +49.9 % ±3.7 in two runs, 42 → 63 FPS), p95 −34 %, with the image verified identical pixel by pixel; with everything on, the GPU becomes the limit (96-97 % busy), measured with the module's automatic A/B test (the in-flight switch off and on, 24 alternating blocks) at a busy VR airfield (Ryzen 7 9800X3D, RTX 5090, Pimax Crystal Super, Quad Views, no frame generation); the rendered objects were identical off and on. How much you gain depends on how CPU-bound you are; when the GPU is the limit, little or nothing. Each optimization checks the DCS build first; after a DCS update it looks for the code it patches by signature and checks the bytes it expects, and stays off, with a warning on the page, if it does not find them.
- **In-flight switch** (Alt+Shift+F11 by default; click the field and press any other combination, Backspace for Off) turns all the optimizations off and back on during a flight so you can compare: one beep off, two beeps on (**Beeps** turns the sounds off). Applying a profile starts with them on again.
- **Developer mode** (off; keep it off for normal flying and for testing a release) is for working on the module: when on, the page says so in Status in DCS, and DCS hot-reloads a payload DLL built elsewhere and takes every engine setting from a separate settings file (a `run_suite.flag` next to it starts the test suite). Logs and reports still go to `Saved Games\DCS\Scripts\DcsQvCull`.

**Status in DCS** reads the module's own log: whether DCS loaded it and whether each optimization found what it needs in this DCS version. After a DCS update the module checks itself; a part that does not recognise the new build turns itself off and the page shows a warning.

**Test suite:** in flight, in VR, with the headset on and awake, press **Run test suite** (or Ctrl+Alt+F11 in DCS). It measures each optimization off and on in alternating 5-second blocks for about 7 minutes and writes a report (**Open last report**). Keep your head still in a busy scene; blocks while the headset sleeps are thrown away. The module's developer keys are off; Ctrl+Alt+F8 to F12 and Ctrl+Alt+Page Up/Down stay reserved for it.

Tested on DCS 2.9.30 in single player. Back to stock DCS (or turning the option off and launching) removes the hook, the module and everything it wrote.

### Pupil shift (experimental)

When your eyes turn, each pupil moves a few millimetres, but DCS renders both eyes from fixed points, so near objects shift slightly against far ones as you look around. With eye-tracked Quad Views, Pupil shift renders each eye from where its pupil is, then gives the headset its own poses back so the image is placed exactly as before. No GPU cost; off by default.

- **Eye rotation radius** (about 10-11 mm) and **Lens image distance** (1-2 m) — the defaults suit most people.
- **In-flight switch** (default Ctrl+Shift+F10) — turns it off and on to compare: a low beep off, a high beep on.
- **DCS's custom IPD**: if you set one in DCS, Pupil shift applies the same value itself and DCS's own option is off while the profile is applied, because DCS would otherwise undo the pupil's sideways move. Back to stock DCS turns it on again.
- The page shows the status from the layer's log (gaze range during the flight, largest shift, the headset views' cant and the eye distances) and what it corrects and what not. Details: [PUPIL_SHIFT.md](PUPIL_SHIFT.md).

### Diagnostics

- **DLSS 5 on the focus area**: after a session, says whether DLSS 5 reached the Quad Views focus area and, when the panel said "DLSS 5: ON but not applied", why, in plain words, with what to check.
- **Create diagnostic package…**: one ZIP to attach to a report (see [Reporting a problem](#reporting-a-problem)).
- What the in-headset panel's messages mean.

### Optimizations only (any other headset, or no VR)

Another headset (Quest, Varjo, Reverb, Bigscreen…) or no headset at all? Pick **Optimizations only** under Pimax and Sboys in the right panel (also in Guided setup). DCS then runs exactly as you set it up, in VR or on the monitor, with your own VR settings, OpenXR runtime and layers (for example your own Quad Views or OpenXR Toolkit). The app adds only what is not VR:

- **Engine Optimizations** (its own page, with the terrain prefetch fix) and **CPU Boost**;
- **Free VRAM before flight**.

Quad Views, DLSS 5 and frame generation are hidden, and so are the helpers made for the Pimax setup (small DCS window, lower monitor mode, desktop Tobii pause). The app does not touch DCS's VR, frame limit or vertical sync options and sets no OpenXR variable. Choosing Pimax or Sboys again brings your VR features back as they were. The engine optimizations were measured with a Pimax Crystal Super; they change DCS's own engine, not the headset path, so they apply the same way with another headset or a monitor, but the gain there has not been measured yet.

## Launch DCS

Press **Launch DCS** (right panel, Overview or the last step of Guided setup). With DCS closed, it:

1. **Checks** the profile (validation, file hashes, your Pimax Play values) — any error stops here, before anything is written.
2. **Applies what changed**: if your draft differs from what is installed, it writes the profile. The first time a file is written, the original is **backed up** (or noted as absent). If nothing changed, nothing is written. If only Pimax Play's values changed, only the focus area is updated.
3. **Starts DCS** directly (skipping the DCS launcher window) with its private VR settings, so only this DCS session uses the profile's layers. The CPU Boost helper starts with it if enabled.

The status line says **Ready to fly** (installed files match the draft), **Changes will be applied when you launch**, or **DCS is running**.

Good to know:

- **Always start DCS with Launch DCS.** A Steam or desktop shortcut does not pass the profile's VR settings to DCS.
- **Steam edition: keep Steam running.** The app passes Steam's launch identifiers so DCS does not restart itself through Steam (which would lose the profile).
- **Don't run DCS or the app as administrator.** The VR loader ignores private settings in elevated processes; the app refuses to launch in that case.
- Only DLSS 5 and Foveated SR put files in DCS's `bin` folder. If another mod already has a `dxgi.dll` there (ReShade, an overlay), it keeps working: Launch DCS says so, asks once, backs it up and loads it after the DLSS 5 loader (as `dxgi2.dll`). If you prefer it off while flying with DLSS 5, choose **Set it aside** (or turn off *Keep another program's dxgi.dll working* on the DLSS 5 page). **Back to stock DCS** puts the original back either way.

### The DCS launcher option

**Show the DCS launcher** (DCS & headset page, off by default) keeps DCS's own launcher window; press Play there. When you press Play, the DCS launcher restarts DCS and asks Windows to take it out of the "job" its parent runs in. If DCS Control itself was started by a program that puts its children in a restricted job (a script runner, an automation tool, some sandboxes), Windows refuses: DCS never opens and `dcs.log` ends with "… failed with error code 5". The app detects this, disables the option and explains why. **Opened from the Start menu or Explorer, the app is not in such a job and the launcher works.**

## In flight: hotkeys and the diagnostic panel

![Recording a hotkey](screenshots/framegen-hotkey-capture.png)

| Key (default) | What it does | Where to change it |
| --- | --- | --- |
| **Ctrl+Shift+F12** | DLSS 5 on/off for this session (compare with and without) | DLSS 5 → In-flight toggle key |
| **Alt+Shift+F12** | Show/hide the in-headset diagnostic panel | Framegen → In-headset diagnostics → Diagnostic panel key |
| **Alt+Shift+F11** | DCS engine optimizations off/on (one beep off, two on) | Engine Optimizations → In-flight switch |
| **Ctrl+Shift+F10** | Pupil shift off/on (low beep off, high beep on) | Pupil shift → In-flight switch |
| **Ctrl+Alt+F11** | Start or stop the DCS engine optimizations' test suite | Fixed |

To change a key, click the field and press the combination you want. Esc cancels, Backspace clears it. Keys need Ctrl, Alt or Shift unless they are F-keys, Pause, Insert, Home, End and similar. Windows' own shortcuts, the Windows key, another in-flight key's combination and the engine module's reserved keys (Ctrl+Alt+F8 to F12, Ctrl+Alt+Page Up/Down) are refused; a combination DCS binds by default is accepted with a note (and listed under Check yourself).

The **diagnostic panel** (frame-generation profiles) is drawn in the headset and shows FPS, DLSS 5 state (on with GPU ms, off, or why it is not applied), the frame-generation backend and mode (2×, 3×, auto), and DCS (app) vs headset (output) FPS. With the engine optimizations on, an **Engine** line says whether all of them are working ("Engine: all 17 on", green), only some ("Engine: 12 of 17 on", amber; for a few seconds after a mission loads this is normal while the multi-threaded recorders learn the scene), none ("Engine: none on", red) or that you switched them off ("Engine: OFF (Alt+Shift+F11)"). With Pupil shift installed, a **Pupil shift** line shows "ON" with the largest pupil shift, "no gaze" or "OFF". Options under **In-headset diagnostics** (Framegen page):

- **Panel visible at start**
- **VRAM counter** — adds the video memory in use to the panel (off by default). Use it when you suspect VRAM is full.
- **OFXR FPS counter** and **OFXR log** — for troubleshooting.

## After the flight: the Last flight card

After DCS exits, the **Overview** shows a summary read from the logs of that session (see the screenshot at the top):

- **Headset** — average FPS delivered, share of time at the refresh rate, missed and discarded frames (Pimax runtime log).
- **Framegen** — share of time at 2× and 3×, number of Auto switches.
- **DCS frame time** — median and 90th percentile of DCS's CPU/GPU time per frame (with Auto 2×/3×).
- **DLSS 5** — whether it ran, runtime version, hotkey toggles.
- **CPU Boost** — apps moved and restored, anything that needed administrator rights.
- **Terrain prefetch fix** (Engine Optimizations) — share of prefetch calls skipped.

Parts without a log are left out. VRAM is not logged; use the in-headset VRAM counter for that.

## Checks

![Checks](screenshots/checks.png)

The **Checks** page lists every finding once:

- **Must fix** — blocks Launch DCS (each with a link to the fix).
- **Check yourself** — things only you can verify (refresh rate in Pimax Play/SteamVR, other FPS limiters, motion smoothing, eye calibration, image quality).
- **OK** — folded.

**Check this PC** adds the runtime, C++ runtime, drivers, write access and headset checks. **Export report** saves a JSON report; the **Diagnostics** page's package holds it and the logs (see [Reporting a problem](#reporting-a-problem)). Below come the files Launch DCS will write and what was detected on this PC.

## Back to stock DCS

![Stock DCS page with the original files](screenshots/recovery-original-files.png)

The **Stock DCS** page (last in the menu) has one card, *Original files*, with one button: **Back to stock DCS**. It puts every file DCS Control ever changed back to what was there before:

- backups are written back,
- files that did not exist before are removed,
- in `options.lua`, only the settings DCS Control changed go back — your other edits stay.

Only a running DCS stops it. The folded list shows what will happen to each file. Use it **before repairing or updating DCS**, before uninstalling the app, or whenever you want plain DCS back. Backups live in `%LOCALAPPDATA%\DcsControl\originals` — keep that folder until you have restored.

## Troubleshooting and FAQ

### Black screen in the headset / DCS hangs at the start of VR

1. Make sure you started DCS with **Launch DCS** from the app, not from Steam or a shortcut.
2. Open **Checks → Check this PC** and fix anything under *Must fix*. Look for **Other OpenXR layers** under *Check yourself*: other OpenXR tools (overlays, older Quad Views or eye-tracking layers) load above the profile's layers and can break it. Disable them for the test.
3. Narrow it down: untick **Frame generation**, launch; then add features back one at a time (Quad Views → Frame generation → DLSS 5).
4. If it still fails, **Back to stock DCS** and [report it](#reporting-a-problem) with the logs.

### "Access to the path … is denied" (DCS under Program Files)

Windows protects `Program Files`: a normal program cannot write there, whatever the antivirus exclusions or the read-only flag say. With DLSS 5 or Foveated SR, DCS Control writes a few files into DCS's `bin` folder (the DLSS 5 loader and Cheeky's files), so when DCS is installed there, **Launch DCS asks Windows once** (its own prompt) to give your account Modify permission on that folder, then carries on. You can also do it beforehand from **Checks → Write access to game folders → Grant access**, or by hand: folder Properties → Security → Edit → your account → Modify. Don't run DCS Control as administrator instead: DCS would start elevated and ignore the profile's layers. If Windows Security's **Controlled folder access** is on, the check says so: allow DCS Control there (Ransomware protection → Allow an app through Controlled folder access).

### ReShade (or another dxgi.dll mod) with DLSS 5

DLSS 5 and Foveated SR need the file name `dxgi.dll` in DCS's `bin` folder. When another program already has one there (ReShade, an overlay), Checks says so and Launch DCS asks once: **Keep it working** loads it after the DLSS 5 loader (as `dxgi2.dll`), **Set it aside** keeps it off while the profile is applied. It is backed up either way, and **Back to stock DCS** puts it back. If DCS misbehaves with both, turn off *Keep another program's dxgi.dll working* on the DLSS 5 page.

### DCS doesn't start

- **Steam edition:** Steam must be running.
- **Administrator:** don't run DCS or the app as administrator, and make sure `DCS.exe` has no "Run as administrator" compatibility flag. Checks flags both.
- **With "Show the DCS launcher" on:** if pressing Play does nothing and `dcs.log` ends with "error code 5", the app was started from inside another program's job. Close it and start it from the Start menu or Explorer, or turn the option off.
- **Write access:** if DCS is under `Program Files` and the profile uses DLSS 5 or Foveated SR, Launch DCS asks Windows once for permission to write to its `bin` folder (see [Access to the path … is denied](#access-to-the-path--is-denied-dcs-under-program-files)). Don't run the app as administrator instead.
- Read the status line and Checks: any error during Launch stops before DCS starts and says why.

### Framegen 3× feels laggy

3× renders only a third of the refresh rate (30 FPS at 90 Hz), so the latency is higher by design (about 88 ms vs about 55 ms in 2× measured on an earlier build) and fast sideways motion shows more artifacts. Options:

- Use **Auto 2×/3×** (default) so 3× is used only while DCS can't hold half the refresh rate.
- Lower the load so DCS stays in 2×: smaller **DLSS 5 area**, lower **working scale**, smaller Quad Views focus or DCS graphics settings.
- In 2×, turning the **Smoothness buffer** off saves about one refresh (about 11 ms at 90 Hz), at the cost of more stutter when frame times jump.
- The Last flight card tells you how much time you spent in 2× and 3×.

### VRAM is full (stutters, textures loading late)

- Turn on the **VRAM counter** (Framegen → In-headset diagnostics) to see it in the headset.
- On the CPU Boost page: **Free VRAM before flight**, **Small DCS window in VR**, **Lower the monitor while flying**. What Boost will do shows how much the listed programs hold now.
- Frame generation reserves a fixed amount of memory, sized by the headset image it receives; it does not grow during a flight. DCS's own texture and terrain settings are the other big lever.

### Multiplayer and Integrity Check

**Not tested yet.** The DCS engine optimizations (terrain prefetch fix included) have only been tested in single-player. With DLSS 5 or Foveated SR the app adds files to DCS's `bin` folder (`dxgi.dll`, Cheeky's files) and, with Engine Optimizations on, a hook and a module in `Saved Games\DCS\Scripts`. Whether a server with Integrity Check accepts them has not been verified. If a server refuses you, **Back to stock DCS** before joining it, and please [report](#reporting-a-problem) what happened.

### How do I uninstall?

1. Open the app and press **Back to stock DCS** (Stock DCS page) with DCS closed.
2. Run `files\Uninstall.cmd` from the installed (or extracted) folder. It removes the application files after checking them; your profiles and backups stay.
3. If you want everything gone, delete `%LOCALAPPDATA%\DcsControl` (or `%LOCALAPPDATA%\DcsVrControl` if you used the app before 0.5.0) afterwards (only after Back to stock DCS — it holds the backups).

The Sboys driver, if you installed it, is managed by its own tool.

### Does it change my DCS graphics settings?

Only the `options.lua` values a feature needs — the frame limit, desktop VSync, the DCS launcher setting, the small window if you enable it — and only when they differ. **Review files** lists every change before you launch, and **Back to stock DCS** puts them back. Everything else (DLSS quality, resolution, textures, shadows …) stays yours to set in DCS.

## Reporting a problem

Please open an issue on [GitHub](https://github.com/NIGos/dcs-control/issues) with:

1. **What you did and what happened** (route, features, 2×/3×/Auto, single-player or multiplayer).
2. **The diagnostic package:** open the **Diagnostics** page and press **Create diagnostic package…** right after the flight that went wrong, before starting DCS again (DCS starts its log anew). It saves one ZIP with everything needed to understand the problem: the app's report (profile, what was detected, the checks, the last flight), `dcs.log`, `options.lua`, the logs and settings of DLSS 5 (Cheeky), frame generation (OFXR), Quad Views, Pupil shift, the engine optimizations and CPU Boost, the DLSS and DirectX file versions in DCS's `bin` folder, Pimax Play's or SteamVR's logs and settings, the OpenXR runtime and layers registered on the PC, and the Windows, CPU and GPU driver versions. Attach the ZIP to the issue.

> **Privacy:** the package holds only text files. Your Windows user name, PC name and e-mail addresses are replaced by `<user>`, `<pc>` and `<e-mail>`, and the account tokens, account ids and headset serial number in Pimax's logs are removed; nothing is sent anywhere. You can open the ZIP and look it over before posting.

**"DLSS 5: ON but not applied" in the headset?** The **Diagnostics** page reads the last DCS session and says whether DLSS 5 reached the Quad Views focus area, and if not, why (for example DCS running DLSS on images that are not the focus views, or another Quad Views layer in the way), with what to check.

## Support

DCS Control is free. If it improved your flying and you'd like to support the work: [ko-fi.com/nigos](https://ko-fi.com/nigos).
