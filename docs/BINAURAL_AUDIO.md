# Binaural (HRTF) audio for DCS

DCS mixes every sound straight to the headphones with a left/right amplitude panner. Front and back sound the same,
there is no height, and the direction only updates at DCS's frame rate. This feature replaces that panning, for the
mono 3D sounds of the world and the cockpit, with real HRTF rendering:
- direction cues per ear (time, level, pinna colouring);
- the head followed at audio rate between DCS frames;
- close cockpit sources rendered per ear.

Everything else stays DCS's own: distance, doppler, propagation delay, filters, canopy damping, radio and intercom.

This page is the contract between the four build tasks (section 9). The exact hook points are in
[BINAURAL_AUDIO_HOOKS.md](BINAURAL_AUDIO_HOOKS.md). The research behind it is
`artifacts/research/hrtf-audio-design-2026-11-24.md`; this page overrides it where they differ (no Steam Audio stage,
no WorldGeneral hooks, scope of v1).

## 1. Scope of v1

**In:**
- Object-based binaural rendering inside ed_sound's own render callback, with 0 added buffering.
- A generic default HRTF that works without any personal data: SADIE II KU100 (section 4).
- Head size from the head circumference: scales the ITD. Presets small 54 cm, medium 57 cm, large 60 cm.
- Personal SOFA import, converted by the app.
- Near-field rendering for cockpit sources (per-ear parallax and ILD).
- Late head rotation and translation at audio rate from `Local\DcsVrHeadPose_v1`.
- Live settings, kill switch, Back to stock, logging and Checks.

**Out (later, each its own decision):**
- Stereo 3D sources: they keep DCS's panning in v1.
- Cockpit room send, airframe shadow, helmet damping, headphone EQ (the design's S6).
- Speaker layouts other than stereo.
- Windows Sonic: it stays the user's own alternative, not something the app writes.
- Radio as a positioned speaker.

## 2. Architecture

```
 DCS simulation thread (SND_process, ed mutex held)          DCS render thread (WASAPI, MMCSS "Games")
 ───────────────────────────────────────────────────         ─────────────────────────────────────────────
  panner call P3a/P3b ──► PanWrap                              mix call P5 ──► MixWrap: BeginBlock
     original panner (DCS state stays exact)                     │  read head pose (newest, predicted to output time)
     ownership (3D, mono, not 2D ctx, top MaxObjects)            │  late correction per context
     mono gain m (DCS's frontal gain), tag ch0=(m,0) ch1=(m,0)    │  original mix:
     VoiceMeta[slot] ← pos, dist, m, ctx, gen  (seqlock)          │    voice call P4 ──► RenderWrap
     once per frame: q_used tag (pose DCS used)                   │      candidate: original → scratch (2 ch)
                                                                  │      tag state → pass / fade in / HRTF / fade out
                                                                  │      BinauralCore::Render(slot, pos', mono) += out
                                                                  │    limiter, master gain (DCS)
                                                                  └─ EndBlock: stats, budget
 pupil shift layer (xrLocateViews) ──► Local\DcsVrHeadPose_v1 (ring of 16, seqlock per entry) ──► readers above
 app ──► DcsQvCull.ini [Audio] (Live) ──► AudioConfig (atomic swap); control thread loads/resamples HRTF sets
```

Components and where they live:

| Component | Location | Task |
|---|---|---|
| Interface headers | `native/binaural/include/` (`binaural_core.h`, `hrtf_format.h`, `head_pose_shm.h`, `binaural_config.h`) | contract |
| BinauralCore (static lib, no DCS dependency) | `native/binaural/src/` | A |
| Offline harness | `native/binaural/test/` | A |
| Generic HRTF data (generated `.dcshrtf`) | `native/binaural/data/` | A |
| SOFA → `.dcshrtf` converter (C#) | `src/DcsVr.Core/Hrtf/` + a CLI command | A |
| Audio module (hooks, ownership, mixing back) | `native/dcsqvcull/src/audio_*.h`, wired into the payload | B |
| Head pose publisher | `native/pupil_shift/` | C |
| App: settings, page, ini, Live, Checks, diagnostics, docs | `src/DcsVr.Core`, `src/DcsVr.App`, `docs/` | D |

**Loading:** the audio module is part of the existing engine payload (`DcsQvCullPayload.dll`). It is loaded the
existing way:
- `Saved Games\DCS\Scripts\Hooks\DcsQvCull.lua` loads the stable loader `DcsQvCull.dll`;
- the loader loads and hot-reloads the payload.

No new loader, DLL or Lua file is added. The payload starts after ed_sound has started, so the hooks are installed
live, under the rules of the hook spec (§1 there). The audio module must also run when the engine optimizations are
off or in perception-only mode, since audio is not an engine optimization.

## 3. DSP approach and why

| Choice | Why |
|---|---|
| **Own core, no Steam Audio** | Head size (ITD) and near field are requirements; Steam Audio exposes neither. An own core gives full control of crossfades, latency and the test metrics. One renderer, no second engine to ship. |
| **Minimum-phase FIR + separate per-ear onset delay** | Minimum-phase IRs interpolate linearly without comb filtering, can be truncated short (128 taps at 48 kHz, about 2.7 ms), and the ITD lives in a delay line where head size can scale it. Standard practice (e.g. libmysofa/SADIE pipelines, Kulkarni et al. on minimum-phase + ITD). |
| **Barycentric interpolation on the measurement triangulation** | Works for any measurement grid (SADIE's lat-long grid, personal SOFA files with irregular grids); 3 FIRs per lookup; no resampling of the data onto another grid. The convex hull is computed once by the converter and stored in the file. |
| **Fractional delay (4-tap Lagrange/Thiran), changed per sample** | Smooth ITD changes with head motion; no zipper noise. |
| **Direct-form FIR, AVX2/FMA** | Any block length (WASAPI asks for a varying number of frames), no FFT framing latency. At 128 taps it is cheaper than partitioned convolution: about 3 µs per source per 10 ms block. |
| **Crossfade on every parameter change** | Filters: old and new FIR both run and crossfade over the sub-block when the direction moves more than 0.5°. Gains and shelves: linear ramps. HRTF swap: full-call crossfade. Ownership: one-block crossfade with DCS's own output, exact at both ends (hook spec §5.2). |
| **Near field: per-ear parallax (< 2 m) + first-order ILD shelf (< 1 m)** | Cockpit sources are 0.3 to 1 m away; far-field HRTFs measured at 1.2 m put them in the wrong place per ear. The Brungart-style shelf adds the close-range level difference (up to +12 dB at 0.25 m, 90°). |
| **Diffuse-field equalised, front normalised to 0 dB** | The KU100 colouring stays neutral on average, and a sound from the front keeps DCS's level exactly (the mono gain rule, hook spec §5.1). |
| **No distance, doppler or filter of our own** | DCS already applied them once; applying them twice would change DCS's mix. The only distance term is the late-translation correction `r_dcs / r_now` for sources under 1.5 m, clamped to ±3 dB. |

## 4. HRTF data

### 4.1 Generic default: SADIE II, subject D1 (Neumann KU100 dummy head)
- **Source:** the SADIE II database, University of York. Project page
  <https://www.york.ac.uk/sadie-project/database.html>; data on Zenodo, record 12092466, version **v2-2**
  (<https://doi.org/10.5281/zenodo.12092466>). File used: `D1_HRIR_SOFA.zip`, the 48 kHz SOFA of subject D1 (8,802
  directions).
- **Licence:** Apache License 2.0, copyright 2018 University of York. It allows use, modification and redistribution,
  including in a GPL-3.0 project (Apache-2.0 is GPLv3-compatible). Its terms:
  - keep the licence text and the copyright notice with the redistributed data;
  - state that the data was modified (ours is resampled, minimum-phase, truncated and equalised);
  - reference the dataset: the database page asks for it whenever it is used, original or modified, and gives the paper
    Armstrong, Thresh, Murphy, Kearney, "A Perceptual Evaluation of Individual and Non-Individual HRTFs: A Case Study of
    the SADIE II Database", Applied Sciences 8(11):2029, 2018, doi:10.3390/app8112029.
- **Why the KU100:**
  - a dummy head designed as an average listener, so it suits pilots without a personal file;
  - measured on a dense 8,802-point grid that covers the whole sphere;
  - widely used as the generic set for binaural rendering;
  - SADIE's own perceptual study evaluates it as a non-individual HRTF.
- **Alternatives considered:**
  - MIT KEMAR: only "free use, credit requested", no formal licence, sparse grid;
  - CIPIC: licence unclear for redistribution;
  - HUTUBS and FABIAN: CC BY 4.0, fine, but human or simulated heads with sparser grids. They stay possible as
    additional bundled profiles.
  - SADIE II D2 (KEMAR) is in the same database and licence. Task A may add it as a second bundled set when the harness
    and the listening test justify it.
- **Pinning:** task A downloads the ZIP only from the Zenodo record above. It records the ZIP's SHA-256 and the SOFA's
  SHA-256 in `docs/THIRD_PARTY.md` and in the fetch script, and keeps the licence text and attribution next to the
  generated `.dcshrtf`. The generated file is committed (`native/binaural/data/`, about 2 MB) and embedded in the
  payload as the RCDATA resource `HRTF_DEFAULT`. The original SOFA is not committed; the fetch script rebuilds it
  bit-identically.

### 4.2 Personal SOFA files
- **Accepted input:** AES69 SOFA `SimpleFreeFieldHRIR`, any sample rate from 44.1 to 96 kHz, any grid with at least 100
  directions, `Data.Delay` either 0 or per-measurement.
- **Conversion:** the app converts with the same C# converter as the default set (`src/DcsVr.Core/Hrtf`). It reads
  HDF5 with **PureHDF** (MIT; task A pins the version and records it in `docs/THIRD_PARTY.md`). The result is written
  to `Saved Games\DCS\Scripts\DcsQvCull\hrtf\<name>.dcshrtf`, through the deployment planner so that Back to stock
  removes it.
- **Before it is offered:** the app shows the conversion report (directions, rate, fitted head radius, grid coverage,
  warnings). A file the converter rejects is never deployed.
- The DCS process never sees SOFA.

## 5. Threading and real-time rules

| Thread | Work | Rules |
|---|---|---|
| DCS simulation (SND_process) | PanWrap: original panner, ownership, mono gain, tag, VoiceMeta publish | Under ED's mutex: no allocation, no locks of ours, no logging; bounded per voice |
| DCS render (MMCSS) | MixWrap/RenderWrap: pose read, scratch render, BinauralCore | No allocation, locks, logging, file I/O or waits; FTZ/DAZ around our work and restored; NaN guards; bounded by frames × MaxObjects |
| Module control thread (existing worker) | Config reload, HRTF load + resample, renderer binding/retirement, log lines, stats summary, kill-switch flag | May allocate; publishes with atomics; frees only what the audio side reported as retired |
| Pupil shift layer (DCS's xrLocateViews) | Head pose publish | Microseconds per new display time |

Handoffs:
- simulation → render: a VoiceMeta table, one seqlock per slot, single writer;
- layer → render: the pose ring;
- control → render: atomic pointer swaps, applied at BeginBlock.

There is no audio ring buffer anywhere.

## 6. Latency budget

| Path | Stock DCS | With binaural |
|---|---|---|
| Added buffering | 0 | **0** (rendered inside DCS's callback, into its buffer) |
| Head rotation to sound | pose → DCS frame (22-33 ms under frame generation) → next callback (0-10 ms) → device buffer (10-20 ms); steps at the DCS frame rate | pose read at the callback and predicted to its output time (≤ 50 ms extrapolation) → device buffer; per-block steps ≤ 10 ms, swept inside the block |
| Impulse to output (harness) | n/a | minimum-phase onset + onset delay only; no block delay |
| Without the pose feed (layer off) | as stock | HRTF from DCS's listener direction, no late correction; logged |

## 7. Configuration, Live, kill switch, Back to stock

**[Audio] keys.** Their names, defaults, ranges, what each does, and the Live and kill-switch rules are in
`native/binaural/include/binaural_config.h`:

| Key | Default | Range |
|---|---|---|
| `Binaural` | 0 | 0/1 |
| `Hrtf` | `default` | file name |
| `HeadCircumferenceCm` | 57 | 48-66 |
| `LateHeadTracking` | 1 | 0/1 |
| `NearField` | 1 | 0/1 |
| `MaxObjects` | 48 | 8-64 |
| `LevelTrimDb` | 0 | −12..+6 |
| `CpuBudgetPercent` | 25 | 5-50 |
| `ExcludeContexts` | (empty) | context ids |
| `LogVoices` | 0 | 0/1 |

- **Live:** every key is Live. `[Audio]` is not an engine section.
- **Kill switch:** `toggle_audio.flag`, and the optional `[Hotkeys] AudioToggle`. It fades to DCS's panning within one
  block; the hooks stay, in pass-through.
- **PupilShift.ini `[HeadPose] Publish=1`:** written by the app only when Binaural and LateHeadTracking are on.
- **DCS output layout:** the module supports stereo devices only. The app reads `options.lua` `sound` settings:
  - if the main layout is not stereo or headphones, a Checks line says so and offers to set it through the existing
    originals-backed `LuaOptions` writer (restored by Back to stock);
  - the `spatial` driver is never written.
- **Back to stock** removes:
  - the `[Audio]` section;
  - `Scripts\DcsQvCull\hrtf\`;
  - `[HeadPose]`;
  - any `options.lua` change (originals).
- **Nothing is written in the DCS install folder.**

## 8. Test plan

All of these run offline without DCS, through ctest or `dotnet test`. The live checks at the end need the user's go.

### 8.1 Contract (this commit)
`native/binaural` ctest `binaural_contract`. It covers:
- the four headers compile together with `windows.h`;
- the struct layouts;
- the OpenXR ↔ DCS axis mapping (1,000 random rotations);
- the late correction: yaw left 30° puts a frontal source 30° right, roll, translation, identity;
- the pose ring: lapping, torn-entry refusal, prediction and its 50 ms cap;
- `.dcshrtf` validation, including both checksums;
- the config clamps and parsers.

Pass: `0 failure(s)`.

### 8.2 BinauralCore harness (task A, `native/binaural/test`, ctest)

| # | Test | Pass criterion |
|---|---|---|
| A1 | ITD: white noise at azimuth −180…180° step 5°, elevation 0; L/R cross-correlation lag | within ±20 µs of the set's own delays scaled by `headRadius / datasetHeadRadius`; for head radius 0.08 vs 0.10 m the lag ratio is 0.8 ± 0.02 |
| A2 | ILD per octave band (250 Hz-16 kHz) at measured directions | within ±1 dB of the `.dcshrtf` IRs' own magnitude |
| A3 | Interpolation at unmeasured directions (midpoints of triangles) | the magnitude lies between the three neighbours' per band (±0.5 dB tolerance); no notch deeper than the deepest neighbour + 3 dB |
| A4 | 360°/s horizontal sweep of a 1 kHz sine and of noise | no sample-to-sample step larger than the input's own maximum step × 1.05 plus 1e-4; band level monotonic in the lateral angle (±0.5 dB) |
| A5 | Energy, front | white noise from the front: output per ear 0 dB ± 0.5 dB (front-normalised set) |
| A6 | Front/back | KU100 front vs back, 2-8 kHz band level difference ≥ 3 dB, as in the source set ±1 dB |
| A7 | Near field | ILD at 0.25 m, 90° minus ILD at 2 m, 90°: matches the shelf model ±1 dB; parallax: a source 0.3 m at 45° right gets each ear's own direction (left ear sees it at < 45°) |
| A8 | Latency | impulse from the front appears at sample `round(onset delay)` ± 1 with no block delay, for block sizes 1, 7, 480, 1056, 4096 |
| A9 | Block-size invariance | the same input rendered with blocks of 480 and with random sizes 1-2048: identical within 1e-5 |
| A10 | Crossfades | HRTF swap, head-radius change, ownership fade (w 0→1 and 1→0), kReset: no step above −60 dBFS on a 0 dBFS sine |
| A11 | Robustness | NaN/Inf input: source reset, output finite, `nonFinite` counted; denormal input: no slowdown (block time within 2× of normal input); frames > 8192 rejected |
| A12 | Real-time | 64 sources × 480 frames at 48 kHz: p99 block time < 0.5 ms, p50 < 0.3 ms (Release, AVX2; SSE fallback reported, budget 2×); zero heap allocations after Init (counting allocator hook) |
| A13 | Rates | 44.1, 48, 96 kHz: A1, A5, A8 pass at each |
| A14 | Default data | the embedded set validates; 8,802 directions; fitted head radius 0.08-0.095 m |

**C# converter tests** (`tests/DcsVr.Tests`):
- **SOFA conversion:** the default set converts bit-identically twice. Against the source SOFA's HRIRs, the magnitude
  error per 1/3 octave (200 Hz-16 kHz) is ≤ 1 dB at every direction (minimum phase keeps the magnitude) and the onset
  ITD error is ≤ 10 µs.
- **Triangulation:** closed, every triangle counter-clockwise, `t = 2n − 4`.
- **Bad inputs:** wrong convention, NaN data, fewer than 100 directions or an unsupported rate are rejected with a clear
  reason.
- **Personal grid:** a synthetic irregular grid converts and validates.

### 8.3 Audio module (task B, `native/dcsqvcull/test`)
1. **Fake ed_sound:**
   - a test DLL whose code section carries the four call-site byte sequences of the hook spec and small stand-in
     panner, voice render (an adding kernel) and mix functions using the same struct offsets;
   - the patcher finds, validates, installs and restores every site;
   - a changed byte or offset → feature off, nothing patched, log line;
   - after uninstall the bytes are identical.
2. **Pass-through exactness:** with Binaural=0, or for a non-candidate voice, the mix output is bit-identical to the
   unhooked fake for 10,000 random blocks.
3. **Mono gain solve:** for random directions, crosstalk x ∈ [0,1] and gains, the solved `m` equals the pan law's
   frontal value within 1e-5. The median-plane fallback stays within 1e-3 after one prior frame. A corrupted pan law
   is detected (mismatch count).
4. **Ownership:**
   - rank and hysteresis: kept for 250 ms, swapped only on a 3 dB margin;
   - 2D, stereo, excluded-context and under-5 cm voices are never owned;
   - the transition table of hook spec §5.2: fade in/out have no step above −60 dBFS, and the steady state equals
     BinauralCore's output.
5. **Threads:** a stress test with a simulation thread and a render thread on the fake for 60 s: no torn VoiceMeta used
   (generation check), no crash, and the payload Stop/hot reload restores the sites while both run.
6. **Budget and overruns:**
   - an injected slow core lowers MaxObjects by 8 steps and recovers;
   - 3 injected ED overruns turn binaural off for the session;
   - the kill-switch flag fades out in one block.
7. **Config:** every `[Audio]` key, its clamps and Live reload. `ini_keys_test.h` and `log_phrases_test.h` are updated.
8. **Offline/spatial paths:** P4 calls outside an armed P5 block pass through.

### 8.4 Head pose (task C, `native/pupil_shift`)
1. **Writer:**
   - with the fake runtime, each new display time publishes exactly one entry: VIEW-in-base pose, velocities and flags;
   - `[HeadPose] Publish=0` publishes nothing and creates no mapping;
   - a failed VIEW locate falls back to the eye-view average with `from_views`.
2. **Harness:**
   - a scripted 90°/s head yaw through the fake runtime;
   - a reader thread at 100 Hz predicting to "now + 20 ms" stays within 0.5° of the scripted truth;
   - torn reads are never accepted in a 10 s writer/reader stress (orientation norm checked on every read).
3. **Cost:** added time per xrLocateViews < 20 µs (p99) in the harness.

### 8.5 App (task D)
1. **Settings model:**
   - every key round-trips through the profile → ini writer → reader;
   - `written-keys.txt`, `contracts.json`, `ini_keys_test.h` (engine side) list exactly the written keys;
   - Live categories are correct;
   - Back to stock leaves no `[Audio]`, no `hrtf` folder, no `[HeadPose]`, and `options.lua` restored.
2. **SOFA import:** the conversion report is shown; a rejected file deploys nothing; a duplicate name asks for a new
   one.
3. **UI tests:** the Audio page at the four viewport/scaling combinations; the EXPERIMENTAL tag; disabled states
   (no stereo layout, layer off → "Follow the head" says it needs the pupil shift layer); Checks lines; text
   contrast.
4. **Diagnostics package:** includes the `audio:` log lines (already in DcsQvCull.log), `[Audio]` and the converted
   file names, never the user's SOFA file or path.

### 8.6 Live (only with the user's go; never run by an agent unasked)
Rig flight, parked with the engine running:
- loopback recording of a yaw sweep: ILD/ITD monotonic, front/back asymmetry, lag ≤ 1 block;
- zero `render time exceeded` lines;
- toggling mid-flight gives no click above −40 dBFS;
- the log's per-context lines and `listener-vs-pose` settle the open [A] items of the hook spec (context ids, listener
  lag).

Then the user's blind A/B: stock vs Windows Sonic vs binaural, with six cues (wingman behind, rotor overhead, switch
clicks left/right/overhead, gun, missile launch).

## 9. Tasks

Each task branches from `feat/hrtf-contract` (`git switch -c feat/hrtf-<part> feat/hrtf-contract`) in its own
worktree. Files are split so the four can merge without conflicts. Where two tasks touch one file, the owner is named;
others only append.

### (A) BinauralCore, data, converter, harness: `feat/hrtf-core`
- **Core:**
  - `native/binaural/src/`: implement `binaural_core.h` exactly (no API change without updating this page and the
    header in the same commit);
  - AVX2/FMA FIR with an SSE2 fallback chosen at Init (`IsProcessorFeaturePresent`/cpuid);
  - add the `binaural_core` static library target to `native/binaural/CMakeLists.txt`.
- **Harness:** `native/binaural/test/`, tests A1-A14 of section 8.2, with numeric output and a short WAV/CSV dump
  folder for listening (not committed).
- **Converter (C#):**
  - `src/DcsVr.Core/Hrtf/` (`SofaReader` on PureHDF, `HrtfConverter`, `DcsHrtfWriter`, convex hull) and a
    `DcsVr.Cli` command `hrtf-convert <in.sofa> <out.dcshrtf> [--name]`;
  - tests in `tests/DcsVr.Tests`;
  - the exact algorithm in `hrtf_format.h`'s header comment.
- **Data:**
  - `scripts/fetch-hrtf.ps1`: download from Zenodo, verify the pinned SHA-256, extract `D1` 48 kHz;
  - generate `native/binaural/data/sadie2_ku100.dcshrtf` with the CLI and commit it;
  - keep the licence and attribution in `native/binaural/data/SADIE_II_LICENSE.txt` and `NOTICE`;
  - add rows to `docs/THIRD_PARTY.md` (SADIE II, PureHDF);
  - `scripts/package-sources.py` must include the data and notice.
- **Build:** `scripts/build-binaural.ps1`, in the style of `build-pupil-shift.ps1`: msvc.cmd, build-jobs, test cache,
  BelowNormal.
- **Hand-off:** the core library target name and its include path for task B.

### (B) DCS audio module: `feat/hrtf-module`
- **Module:**
  - `native/dcsqvcull/src/audio_hooks.h` (sites, patcher, validation), `audio_voice.h` (PanWrap, ownership, mono gain,
    VoiceMeta), `audio_render.h` (MixWrap/RenderWrap, binding, budget), `audio_config.h` (reads `[Audio]` with
    `binaural_config.h`);
  - wiring in the payload (`main.cpp`, `src/main/*.inc`): start/stop, hot reload restore, `qvexit::g_exiting`
    pass-through, worker polling of `toggle_audio.flag`, hotkey;
  - status bit in the status word, `audio:` log lines.
- **Hooks:** exactly per [BINAURAL_AUDIO_HOOKS.md](BINAURAL_AUDIO_HOOKS.md). Add ed_sound to `tools/hooks_curated.py`
  and the manifest, regenerate `src/hook_sigs.h`, and extend `dcs_patch_check.py`.
- **Default HRTF:** embed `native/binaural/data/sadie2_ku100.dcshrtf` as RCDATA `HRTF_DEFAULT` in the payload (an
  `.rc` file in the payload target). Personal sets come from `Scripts\DcsQvCull\hrtf\<Hrtf>.dcshrtf`.
- **Link:** BinauralCore from `native/binaural` (CMake `add_subdirectory` or the sources). Until A is merged, the
  module's tests link a test-only stand-in `binaural::Renderer` (`test/audio_fake_core.cpp`); the integrator swaps in
  the real one.
- **ini rules:** `[Audio]` is listed as not an engine section in `ini_diff.h`'s comment and classification test.
- **Reuse:** the existing loader, `exit_guard`, `reloc`/`sigscan`, `DetourPage`, async log, Live reload, worker. Do
  not add a loader.
- **Tests:** 8.3, in `native/dcsqvcull/test/audio_*_test.h`, included by `test_main.cpp`.
- **Pose reading:** with `head_pose_shm.h`. The q_used tag comes from the engine's own `DcsVrMotion_v1` pose display
  time.

### (C) Head pose publisher: `feat/hrtf-pose`
- **`native/pupil_shift/pupil_shift_layer.cpp`:**
  - create a VIEW reference space per session;
  - at each xrLocateViews with a new display time, `xrLocateSpace(view, info->space, displayTime)` with
    `XrSpaceVelocity`;
  - publish per `head_pose_shm.h`;
  - `qpcDisplay` through `XR_KHR_win32_convert_performance_counter_time` only if the app enabled it, else 0;
  - the fallback with `from_views`;
  - gate with PupilShift.ini `[HeadPose] Publish` (default 0);
  - one log line per session (`head pose: publishing` / `off`).
- **Fakes and tests:**
  - the fake runtime: VIEW space and velocity support;
  - tests 8.4 in `pupil_shift_tests.cpp` and the harness;
  - a reader test using only the header (the module's reader path).
- **Constraint:** no change to how the layer moves DCS's views (pose publishing is read-only).

### (D) App: `feat/hrtf-app`
- **Page:** a new **Audio** page (rail label "Audio", id `page-audio`), in the VR group but **not** hidden with
  Optimizations only, because binaural audio also helps on a monitor with head tracking. Why not the Perception page:
  that page is about eye distance and depth (vision); page names must match their content, and the audio settings
  would be lost there.
- **Card:** "3D headphone audio", EXPERIMENTAL tag, no consent gate:
  - switch;
  - Head size slider in cm with Small/Medium/Large presets;
  - HRTF dropdown: "Generic (KU100)" plus imported files, with "Import SOFA…";
  - "Follow the head between frames" switch;
  - Advanced fold: Near field, Max sounds, Level trim, CPU budget;
  - Diagnostics fold: Log voices;
  - status from the module log: "Running · 32 sounds · 0.2 ms" / "Off: <reason>".
- **UI text:** short and plain, for example "Sounds come from where they are: front, back, above. For headphones."
- **Model and writers:**
  - `VrProfile` fields → `ConfigurationWriters` → `[Audio]` in DcsQvCull.ini, and `[HeadPose] Publish` in
    PupilShift.ini;
  - `written-keys.txt`, `contracts.json`, `module-only-keys.txt` as the scripts require;
  - `LiveSettings` category "Audio" (every key).
- **Kill switch:** a key in Keys & headset panel (`AudioToggle`) and the `toggle_audio.flag` action.
- **Back to stock:** section 7.
- **Checks lines:**
  - DCS output not stereo (with the fix);
  - binaural on but the pupil shift layer not active (no late tracking);
  - an HRTF file missing;
  - the module reported `audio: off (<reason>)`.
- **Other places:**
  - Last flight: "3D audio: on/off/fallback (reason), p99 cost";
  - diagnostics package entries;
  - SOFA import UI calling task A's converter, with the conversion report;
  - Feature guide entry;
  - `docs/USER_GUIDE.md` section;
  - `docs/INTERFACE.md` Pages list;
  - release notes line.
- **Tests:** 8.5.

### Integration (`feat/hrtf-audio`)
1. Merge in the order A, C, B, D.
2. Replace B's stand-in core with A's library.
3. Run every native and .NET test.
4. Run the personal-data check.
5. Live tests (8.6) only with the user's go, DCS state checked first.
