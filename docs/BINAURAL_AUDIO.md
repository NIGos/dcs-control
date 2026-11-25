# Binaural (HRTF) audio for DCS

DCS mixes every sound straight to the headphones with a left/right amplitude panner. Front and back sound the same,
there is no height, and the direction only updates at DCS's frame rate. This feature replaces that panning, for the
3D sounds of the world and the cockpit (mono and stereo waves), with real HRTF rendering:
- direction cues per ear (time, level, pinna colouring);
- the head followed at audio rate between DCS frames;
- close cockpit sources rendered per ear.

Everything else stays DCS's own: distance, doppler, propagation delay, filters, canopy damping, radio and intercom.

This page is the contract between the four parts (section 9). The exact hook points are in
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
     ownership (3D, mono/stereo, not 2D ctx, top MaxObjects)     │  late correction per context
     mono gain m (DCS's frontal gain), tag (m,0)(0,m) + mark    │  original mix:
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

| Component | Location | Part |
|---|---|---|
| Interface headers | `native/binaural/include/` (`binaural_core.h`, `hrtf_format.h`, `head_pose_shm.h`, `binaural_config.h`) | contract |
| BinauralCore (static lib, no DCS dependency) | `native/binaural/src/` | core |
| Offline harness | `native/binaural/test/` | core |
| Generic HRTF data (generated `.dcshrtf`) | `native/binaural/data/` | core |
| SOFA → `.dcshrtf` converter (C#) | `src/DcsVr.Core/Hrtf/` + a CLI command | core |
| Audio module (hooks, ownership, mixing back) | `native/dcsqvcull/src/audio_*.h`, wired into the payload | audio module |
| Head pose publisher | `native/pupil_shift/` | head pose |
| App: settings, page, ini, Live, Checks, diagnostics, docs | `src/DcsVr.Core`, `src/DcsVr.App`, `docs/` | app |

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
| **Crossfade on every parameter change** | Filters: old and new FIR both run and crossfade over 64 frames when the direction moves more than 0.5°. Gains and shelves: linear ramps. HRTF swap: 10 ms crossfade per source. Every crossfade and delay glide keeps its length across calls, so short calls never shorten it. Ownership: one-block crossfade with DCS's own output, exact at both ends (hook spec §5.2). |
| **Near field: per-ear parallax (< 2 m) + first-order ILD shelf (< 1 m)** | Cockpit sources are 0.3 to 1 m away; far-field HRTFs measured at 1.2 m put them in the wrong place per ear. The Brungart-style shelf adds the close-range level difference (up to 12 dB more ILD at 0.25 m, 90°), level-neutral: its two ears' gains are scaled to a mean power of 1 (+2.75 / -9.25 dB there), so the loudness stays DCS's own. |
| **Diffuse-field equalised, front normalised to 0 dB** | The KU100 colouring stays neutral on average, and a sound from the front keeps DCS's level exactly (the mono gain rule, hook spec §5.1). |
| **No distance, doppler or filter of our own** | DCS already applied them once; applying them twice would change DCS's mix. The only distance term is the late-translation correction `r_dcs / r_now` for sources near the head (under 1.4 m, kept until past 1.6 m), clamped to ±3 dB; it scales a wide sound's kept side as well as its mid. |
| **DCS's own level for the direction (PanLevel)** | The HRTF is normalised to the front, and at low frequencies (where engine and cockpit sounds carry most of their power) it is about as loud from every direction, while DCS's level-only pan law plays a source behind or above 3-6 dB quieter than ahead. So the HRTF part's gain carries `PanLevel = sqrt((gL² + gR²) / 2) / m` of that voice's own DCS gains (m: DCS's gain for a source ahead on a ±60° pair), between -12 dB and +2.5 dB (4/3: the most any speaker pair can give against m): the kill switch keeps the level, the HRTF adds the direction's cues (hook spec §10.3). |
| **Stereo waves: point or wide** | The HRTF input is the mid `(L' + R') / 2` (DCS's own stereo-to-mono rule) at the stereo's loudness. A voice near the head (under 1.4 m, until past 1.6 m) or a diffuse bed (by its own mid / side powers) keeps DCS's side around the binaural mid, delayed per ear by that ear's HRTF delay so the two arrive together (no comb). auto_width is not a reason: ED's flag only narrows a sound with distance (hook spec §10.3). |

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
  - SADIE II D2 (KEMAR) is in the same database and licence. The core may add it as a second bundled set when the harness
    and the listening test justify it.
- **Pinning:** `scripts/fetch-hrtf.ps1` downloads the ZIP only from the Zenodo record above (or reuses the copy in
  `.cache/hrtf`) and checks its size, MD5 and SHA-256 and the 48 kHz SOFA's SHA-256 (all in `docs/THIRD_PARTY.md`); the
  licence text and attribution stay next to the generated `.dcshrtf`. The generated file is committed
  (`native/binaural/data/sadie2_ku100.dcshrtf`, 9,329,824 bytes: 8,703 directions x 2 ears x 128 taps; SHA-256
  `5a23db950066065e5428d08a16342c7cbbd2f3682c38110d1e236302497d38e1`) and embedded in the payload as the RCDATA
  resource `HRTF_DEFAULT`. The original SOFA is not committed; the fetch script rebuilds the set bit-identically (checked
  twice, and by the C# test below whenever `.cache/hrtf` holds the archive).
- **What the conversion found** (`hrtf-convert` on the 48 kHz file): 8,802 measurements, 99 of them repeats of a
  direction already measured (merged, their responses averaged), so 8,703 directions; elevation -90 to 90 degrees, 100 %
  of the sphere within 15 degrees of a measurement; fitted head radius 0.0943 m (the same within 0.5 mm from the 44.1
  and 96 kHz files). That fit is Woodworth's ITD model on the converter's onset delays (largest ITD about 0.70 ms).
  The onsets are the slope of each ear's excess phase over 200-1500 Hz, where hearing uses the phase between the ears.
  Rendered (stored minimum-phase IR plus stored onset), the phase delay between the ears over 200-1500 Hz is 8.9 us RMS
  from the measured one (31 us at worst, straight ahead -1.4 us against -12.7 us measured), and adjacent directions on
  the 1-degree horizontal ring differ by at most 19 us in ITD (the measurement's own steps: 16 us) and 46 us in one
  ear's onset. The earlier onsets, from a full-band cross-correlation with the minimum-phase version, locked onto the
  high-frequency lobes of a shadowed ear: 119 us RMS (272 us at worst), the front 154 us to the left (about 15 degrees),
  jumps up to 209 us (ITD) and 215 us (one ear) between 1-degree neighbours, a fitted radius of 0.119 m that moved with
  the sample rate. Each ear keeps its own delay: holding the two ears' mean constant per direction (and splitting the
  ITD around it) was tried and dropped, because with the near field on the renderer looks each ear up from its own
  direction and needs that ear's own delay curve (A14's near-field crossfade went from 4.8e-4 to 1.3e-3, over the
  A10 limit). The mean of the two ears varies by 0.14 ms around the horizontal ring (0.29 ms before). The head size is relative to the fit (57 cm
  plays the set's own timing; it scales the stored onsets, the phase the minimum-phase IRs themselves carry between
  the ears, about 15 % of the total at 90 degrees, stays). The renderer's ITD radius range is 0.04-0.15 m
  (`kMinItdRadiusM`, `kMaxItdRadiusM`), so 48-66 cm scale it unclamped.
- **Front level:** the front normalisation measures noise from straight ahead over 200 Hz-8 kHz (the mean of both ears
  at 0 dB). Measured on white noise over the whole band, the 12-24 kHz half of the spectrum decided the level; the set
  is now 1.0 dB louder in the band where DCS's sounds are.

### 4.2 Personal SOFA files
- **Accepted input:** AES69 SOFA `SimpleFreeFieldHRIR`, any sample rate from 44.1 to 96 kHz, any grid with at least 100
  directions, `Data.Delay` either 0 or per-measurement.
- **Conversion:** the app converts with the same C# converter as the default set (`src/DcsVr.Core/Hrtf`). It reads
  HDF5 with **PureHDF 2.2.0** (MIT, managed code only, pinned `[2.2.0]`; `docs/THIRD_PARTY.md`). `SofaReader` takes:
  - Data.IR `[M R N]` (or `[M R N 1]` of FIR-E files), float or double, two receivers;
  - Data.SamplingRate `[I]` or `[M]` (all equal; hertz, kHz understood);
  - Data.Delay `[I R]`, `[M R]` or `[R]` in samples, or none;
  - SourcePosition `[I C]`/`[M C]`, spherical (degree or radian, distance in metre, cm or mm) or cartesian, relative to
    ListenerPosition / ListenerView / ListenerUp when the file has them;
  - ReceiverPosition to tell the left ear (larger y), receiver 0 = left without it.
  GeneralFIR and FreeFieldHRIR files of the same shape are read with a warning. Missing optional attributes are assumed
  with a warning (no convention, no coordinate type or units, no distance: 1.2 m). Anything else is refused with the
  reason in plain words: not HDF5, netCDF-3, transfer functions or SOS instead of impulse responses, another
  convention, not two ears, no or mixed sample rates, negative delays, unknown units, positions that do not match the
  measurements. PureHDF's default chunk cache is replaced by one that decodes each chunk once (SADIE II's file: a minute
  -> 0.1 s); a whole conversion takes about 2 s. The result is written to
  `Saved Games\DCS\Scripts\DcsQvCull\hrtf\<name>.dcshrtf`, through the deployment planner so that Back to stock removes it.
- **Measurement distance:** a set measured closer than 1 m already holds some near-field effect; the converter notes
  it in the report ("Measured at 0.5 m: ..."). The renderer starts its near-field model from the file's `measurementDistanceM` D when
  D is under 1 m (clamped to 0.15-1 m, `kNearFieldFloorM`: just above the largest ear radius, 0.12 m; the file format
  allows 0.1 m, and the converter's report says that such a set is treated as measured at 0.15 m): the ILD shelf adds only the difference between the source's distance and D
  (nothing at and beyond D), and each ear looks up the point at D on its own ray through the source, so a source at D
  plays the set's own filters (no parallax or ILD added twice). Sets measured at 1 m or farther, SADIE II's 1.2 m
  among them, get the model as before, bit for bit (A7b).
- **Repeated directions** (closer than 0.05 degrees) are merged with their impulse responses and `Data.Delay` averaged.
- **Before it is offered:** the app shows the conversion report (directions, rate, grid coverage, the largest time
  difference between the ears that the fitted radius stands for, warnings). A file the converter rejects is never
  deployed.
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

### 8.2 BinauralCore harness (the core, `native/binaural/test`, ctest)

| # | Test | Pass criterion |
|---|---|---|
| A1 | ITD: white noise at azimuth −180…180° step 5°, elevation 0; L/R cross-correlation lag | within ±20 µs of the set's own delays scaled by `headRadius / datasetHeadRadius`; for head radius 0.08 vs 0.10 m the lag ratio is 0.8 ± 0.02 |
| A2 | ILD per octave band (250 Hz-16 kHz) at measured directions | within ±1 dB of the `.dcshrtf` IRs' own magnitude |
| A3 | Interpolation at unmeasured directions (midpoints of triangles) | the magnitude lies between the three neighbours' per band (±0.5 dB tolerance); no notch deeper than the deepest neighbour + 3 dB |
| A4 | 360°/s horizontal sweep of a 1 kHz sine and of noise | no sample-to-sample step larger than the input's own maximum step × 1.05 plus 1e-4; band level monotonic in the lateral angle (±0.5 dB) |
| A5 | Energy, front | white noise from the front: output per ear 0 dB ± 0.5 dB (front-normalised set; a measured set over 200 Hz-8 kHz, the converter's band) |
| A6 | Front/back | KU100 front vs back, 2-8 kHz band level difference ≥ 3 dB, as in the source set ±1 dB |
| A7 | Near field | ILD at 0.25 m, 90° minus ILD at 2 m, 90°: matches the shelf model ±1 dB; the level with near field on vs off within 1 dB (the shelf is level-neutral); parallax: a source 0.3 m at 45° right gets each ear's own direction (the near, right ear sees it at < 45°, the left ear at > 45°) |
| A7b | Near field from the set's distance | sets measured at 1.0, 1.2, 1.5 and 5 m render bit-identically (near field on, 0.1-1.8 m); a set measured at 0.5 m: a source at 0.5 m equals near field off (< 1e-4 RMS), ILD 0.25 m minus 0.5 m matches the shelf model from 0.5 m ±1 dB, 0.8 m minus 0.5 m 0 ± 0.5 dB, parallax from the 0.5 m sphere ± 0.5°; sets at 0.10 and 0.15 m render bit-identically (the floor), finite with the largest ears, 0.2 m differs |
| A8 | Latency | impulse from the front appears at sample `round(onset delay)` ± 1 with no block delay, for block sizes 1, 7, 480, 1056, 4096 |
| A9 | Block-size invariance | the same input rendered with blocks of 480 and with random sizes 1-2048: identical within 1e-5 |
| A10 | Crossfades | HRTF swap, head-radius change, ownership fade (w 0→1 and 1→0), kReset: no step above −60 dBFS on a 0 dBFS sine |
| A11 | Robustness | NaN/Inf input: source reset, output finite, `nonFinite` counted; denormal input: no slowdown (block time within 2× of normal input); frames > 8192 rejected |
| A12 | Real-time | 64 sources × 480 frames at 48 kHz: p99 block time < 0.5 ms, p50 < 0.3 ms (Release, AVX2; SSE fallback reported, budget 2×); zero heap allocations after Init (counting allocator hook) |
| A13 | Rates | 44.1, 48, 96 kHz: A1, A5, A8 pass at each |
| A14 | Default data | the embedded set validates; 8,703 directions (8,802 measured, 99 repeats merged); fitted head radius 0.085-0.105 m (it is 0.0943 m, section 4.1), scaled by 48-66 cm unclamped; A5 on measured ears: their mean 0 ± 0.5 dB, each ear ± 1 dB, the rendered difference between the ears the set's own front IRs' ± 0.2 dB (the KU100's right ear is 1 dB louder at the front); A6 ≥ 2 dB (the KU100's own 2-8 kHz front/back difference is 2.4 dB) and the set's ± 1 dB; A8 |

**C# converter tests** (`tests/DcsVr.Tests`):
- **SOFA conversion:** the default set converts bit-identically twice. Against the source SOFA's HRIRs, the magnitude
  error per 1/3 octave (200 Hz-16 kHz) is ≤ 1 dB at every direction (minimum phase keeps the magnitude) and the onset
  ITD error is ≤ 10 µs on the synthetic set, whose true delays are known, also with a strong late 12 kHz lobe in every
  IR (what pulled the earlier cross-correlation onsets). On SADIE II (whenever `.cache/hrtf` holds the archive): worst
  magnitude error ≤ 1 dB over all 8,703 directions; the rendered phase delay between the ears over 200-1500 Hz within
  15 µs RMS and 50 µs at worst of the measured, straight ahead within 30 µs, adjacent directions of the horizontal ring
  less than 30 µs apart in ITD and 60 µs in one ear's onset; the 44.1 and 96 kHz files fit the same radius within 1 mm; the front level 0 dB over
  200 Hz-8 kHz.
- **Repeats and distance:** a repeated direction is the average of its measurements; a set measured under 1 m gets a
  warning.
- **SOFA reader and `hrtf-convert`** (`SofaTests.cs`, SOFA files written in the test with PureHDF): spherical
  degrees/radians, cartesian cm, float and double, chunked + deflated and contiguous, Data.Delay `[I R]` and `[M R]`,
  swapped receivers, a turned and a moved listener, FIR-E, missing optional attributes; every refusal of section 4.2;
  the command in process (exit codes, `warning:`/`coverage:` lines, deterministic, `--generic`) and as DcsVr.Cli.exe
  through the app's `HrtfLibrary.Import`; SADIE II from the cached archive gives the committed file byte for byte
  (skipped without `.cache/hrtf`).
- **Triangulation:** closed, every triangle counter-clockwise, `t = 2n − 4`.
- **Bad inputs:** wrong convention, NaN data, fewer than 100 directions or an unsupported rate are rejected with a clear
  reason.
- **Personal grid:** a synthetic irregular grid converts and validates.

**The core as built** (`native/binaural`, `binaural_core_test`; numbers from the run in the commit message):
- The harness runs on a synthetic spherical-head set (an icosphere of 2,562 directions, Brown-Duda head shadow per ear,
  a back shelf, Woodworth delays), so A1-A13 need no downloaded data. A14 runs as its own ctest (`--default-data`) on
  the committed `native/binaural/data/sadie2_ku100.dcshrtf` (it would print `SKIP A14` without the file).
- Measured against the criteria above, with these readings of them:
  - A2/A3: octave bands up to the 16 kHz band, whose top edge is cut at 20 kHz (no fractional delay is flat to
    Nyquist; 20 kHz is the limit of hearing).
  - A4: "the input's own maximum step" is the largest step any static direction on the path produces (the HRTF's
    gain is not 1, so the dry input's step is not a click threshold).
  - A8: the onset is the first sample at half the peak (a fractional delay's sinc side lobes stay below it), and
    nothing is output before the interpolator's look-ahead window. The latency is the set's delay plus 12 samples
    (the 24-tap fractional-delay interpolator); no block delay at any block size.
  - A10: "no step above -60 dBFS" = the largest second difference during the change exceeds the steady signal's
    (before and after) by at most 1e-3. Also checked: near field off -> on.
  - A12: thread CPU time per block (what preemption by other programs does not inflate), each statistic the best
    of 5 runs of 1,000 blocks. Three scenes: 64 static sources; 64 sources with the head turning 90°/s (the limits
    apply to both); 64 sources all sweeping ~190°/s (stress, p99 < 1 ms). SSE2 budget 2x. `binaural_core_test
    --real-time` runs A12 alone. The measuring thread runs at above-normal priority, and a scene over its limits
    is measured again (up to 3 times). Thread CPU time still grows when other programs share the core's SMT
    sibling and cache (0.341 ms p50 once during a release build), so the test also records how busy the rest of
    the PC was: a time limit missed while other programs kept at least 25 % of the logical processors busy even in
    the quietest measurement is printed as SKIPPED with that load instead of failing; on a quieter PC it fails.
    `BINAURAL_RT_STRICT=1` fails it regardless. The zero-allocation check never skips. A delay change glides over at least 64 frames (`kMinGlideFrames`), so very short
    calls stay smooth; a floor of a whole sub-block (256) carried 1/8 of the 224-frame tail of every 480-frame call
    into the next one, sent 48 % more sub-blocks to the per-sample interpolator and cost 5 % at 64 moving sources
    (p50 0.289 against 0.274 ms, measured interleaved with the earlier renderer); with 64 frames the 7-frame-call
    sweep of A10 is as smooth as before.
- Design changes from section 3, all measured: the FIR runs before the per-ear delay (both ears and both filters of a
  crossfade share one pass over the mono input); the fractional delay is a 24-tap Kaiser-windowed sinc (flat to
  0.42 x the rate; a 4-tap Lagrange loses 3 dB at 16 kHz); a direction crossfade lasts the first 64 frames of the
  sub-block; a triangle change alone does not crossfade (the weights are continuous across edges).
- The converter's onsets are the excess-phase slope over 200-1500 Hz (section 4.1), not a -20 dB threshold nor a
  full-band cross-correlation (both miss the low-frequency timing of a shadowed ear).
- **Data and reader (done 2026-11-24, with the user's go for both downloads):**
  - `D1_HRIR_SOFA.zip` from Zenodo record 12092466, pinned by size, MD5 and SHA-256 (`scripts/fetch-hrtf.ps1`,
    `docs/THIRD_PARTY.md`); `native/binaural/data/sadie2_ku100.dcshrtf` generated from it and committed with
    `SADIE_II_LICENSE.txt` and `NOTICE` (the archive has no licence file of its own); A14 runs on it.
  - PureHDF 2.2.0 (NuGet, MIT) for `SofaReader` (SOFA/HDF5 -> `HrirSet`, section 4.2) and the
    `DcsVr.Cli hrtf-convert <in.sofa> <out.dcshrtf> [--name] [--source] [--generic]` command (`SofaConversion`).
  - The real data changed two expectations of the harness, both written in A14 above: the repeats merged (8,703
    directions, not 8,802) and the fitted radius (0.119 m then, not 0.08-0.095 m; 0.0943 m since the excess-phase
    onsets, section 4.1); and one limit of the renderer, the ITD
    radius range (0.04-0.15 m, section 4.1). The set's base delay (`delayBase`) covers a 0.12 m head as before and at
    least the head-size maximum 66 / 57 (`kMaxHeadSizeScale`), so the synthetic harness is unchanged.
  - Two radii: the ITD radius (`Config.headRadiusM`, `SetHeadRadius`: the set's fit x C / 57 cm; kept as a scale of the
    set's fit, so a new set keeps the head size and is never paired with another set's radius) only scales the
    delays; the near-field ear positions use the head's real radius (`Config.earRadiusM`, `SetEarRadius`:
    `HeadRadiusFromCircumferenceCm`, 0.06-0.12 m), which the module sets from the same circumference. On the KU100 the
    ears sat at 0.119 m before (the fit then), now at 0.0907 m for 57 cm.
  - Near field switched on or off fades over 20 ms (`kNearFieldFadeS`): on the KU100 at 0.3 m, switching inside one
    480-frame call let the per-ear delays jump by several samples (A10 step 1.05e-3 at 57 cm, 1.20e-3 at 66 cm, limit
    1e-3); faded, 3.2e-4 to 4.0e-4 (4.1e-4 to 4.8e-4 on the set with the excess-phase onsets).
  - A14 adds, on the real set at 48, 57 and 66 cm: the near-field off -> on crossfade (A10 limit), a live head-size
    change from 57 cm (A10 limit) and the parallax angles from the real ear positions (A7, +-0.5 degrees).

### 8.3 Audio module (`native/dcsqvcull/test`)
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

### 8.4 Head pose (`native/pupil_shift`)
1. **Writer:**
   - with the fake runtime, each new display time publishes exactly one entry: VIEW-in-base pose, velocities and flags;
   - `[HeadPose] Publish=0` publishes nothing and creates no mapping;
   - a failed VIEW locate falls back to the eye-view average with `from_views`.
2. **Harness:**
   - a scripted 90°/s head yaw through the fake runtime;
   - a reader thread at 100 Hz predicting to "now + 20 ms" stays within 0.5° of the scripted truth;
   - torn reads are never accepted in a 10 s writer/reader stress (orientation norm checked on every read).
3. **Cost:** added time per xrLocateViews < 20 µs (p99) in the harness.

### 8.5 App
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

**The pilot's first flight.** About 15 minutes, headphones on.

**Set in the app before Launch DCS:**
- Audio page:
  - 3D headphone audio on; Ears (HRTF) Generic;
  - Head size: your own measurement, or Medium (57 cm);
  - Follow the head between frames on;
  - Advanced at the defaults: Close sounds per ear on, Most 3D sounds 48, 3D sound level 0 dB, CPU budget 25 %;
  - Diagnostics: Sound log on, for this flight only.
- DCS's output: the headphones. Launch DCS sets the speaker layout to Stereo.
- Keys & headset panel: the 3D audio key is Alt+Shift+F6 (the default).
- Live (top bar) on.

**Where to fly:** a mission with the engine running on the ground or parked in the air, with AI traffic or a
wingman passing around, then a short flight with ATC/wingman radio.

**Where to look:** every `audio:` line is in `DcsQvCull.log` (Saved Games, `Scripts\DcsQvCull`). The sound log
`audio_voices.csv` is in the same folder: one row per 3D voice per second, columns
`time,slot,gen,ctx,owned,x,y,z,dist,m,gL,gR,flags,comp,diffuse,wide,panLevel,power`, listener frame (x forward, y up,
z right, metres). `flags` are the voice's meta flags, added: 1 owned, 2 owned from its creation (no fade in), 4 the head
pose DCS used is known (late head tracking can apply), 8 near (under 1.4 m, kept until past 1.6 m: kept wide, the
late-translation gain), 16 that pose had a tracked position (late translation can apply), 32 a stereo wave, 64 the sdef
sets auto_width (informational); so 63 is an owned, near stereo wave with a full pose, owned from its creation. The last
five columns are the render side's view of the voice in its last block (empty when it was not rendered by the module):
`comp` the loudness factor `sqrt((P_M + P_S) / P_M)` (1-2), `diffuse` 0/1, `wide` 0/1 (DCS's side kept: near or
diffuse), `panLevel` DCS's level for the direction (§3), `power` the smoothed per-frame power of `m L'`, `m R'` (sum it
by `wide` for the point / wide energy share).

| # | What to do | What to listen for (pass) | Log proof (pass) |
|---|---|---|---|
| 1 | Start the mission | DCS sounds as usual, nothing missing or doubled | `audio: hooks in (ed_sound.dll ...)`; `audio: output <p> bound (...)`; `audio: on (48000 Hz stereo, HRTF default: 8703 directions, head radius 9.4 cm, near field on, at most 48 sounds)` (head radius 9.43 cm x size / 57); one `audio: ctx <i> 2d=<0/1> cockpitGain=<0/1> output=<n> crosstalk=...` line per context; no `audio: off (` line |
| 2 | Front/back: an aircraft or vehicle passes in front, then behind (or turn your head 180 degrees from a running engine) | Clearly in front, then clearly behind; no flip between the two | CSV: that voice `owned=1`, `x > 0` while in front, `x < 0` behind |
| 3 | Sides: a sound passes from left to right | Fully on each side at 90 degrees, a smooth move between, no jump at the centre | CSV: `z < 0` left, `z > 0` right, `owned=1` throughout |
| 4 | Elevation: an aircraft or helicopter overhead | Above you, not just in the middle of the head | CSV: `y > 0` for that voice |
| 5 | Head turns: turn the head left and right (about 90 degrees per second) for 10 s, a few times | Outside sounds stay where they are in the world; no lag, wobble or zipper noise | `audio: running, ...; head pose on`; `audio: listener-vs-pose: ...` (once a minute; check the line after the turns): best match at 0 or ±1 frame(s), correlation 0.8 or higher while the head turned |
| 6 | Radio: ATC and wingman calls, intercom | In the middle of the head, unchanged when turning the head | The radio context's `ctx` line has `2d=1`; CSV: no `owned=1` row with that context id |
| 7 | Cockpit switches: flip switches on the left console, then the right, then overhead | Each from its own side, close to that ear; overhead from above. With Close sounds per ear off (Live) they sound farther and less to one side | CSV: those voices `dist < 1`, `owned=1`, their context's `ctx` line `cockpitGain=1` |
| 8 | Whole flight | No clicks or pops: sounds starting and stopping, head turns, settings changes | Every `audio: running` line: `refused 0`, `overruns 0`, p99 under the budget (under 2.5 ms at 25 %); no `audio: CPU budget: at most` line; no `audio: off (` line; no `render time exceeded` in `dcs.log` |
| 9 | Kill switch: Alt+Shift+F6, listen 10 s, Alt+Shift+F6 again | Back to DCS's own left/right sound within a moment, then 3D again; no click either way | `audio: kill switch ([Hotkeys] AudioToggle): binaural OFF (DCS's own panning)`, then `... binaural ON` |
| 10 | Live on: Head size 57 -> 60 -> 57 cm; Close sounds per ear off -> on; 3D sound level -6 -> 0 dB; 3D headphone audio off -> on | Each change heard within about a second, no click; off = DCS's own sound | One `audio: config: Binaural=... HeadCircumferenceCm=60.0 ...` line per change; off: `audio: off (binaural is off in the settings)`; on again: `audio: on (re-enabled in the settings, HRTF default: 8703 directions)` |
| 11 | Live off: change Head size | Nothing changes until the next launch | No new `audio: config:` line |
| 12 | Close DCS | - | `audio: hooks out (ed_sound.dll's call sites restored...)` |

Afterwards turn the Sound log off (the file stops at 8 MB). A failed row: keep `DcsQvCull.log`, `audio_voices.csv`
and the time of the moment.

**Rig flight** (with the user's go, parked with the engine running):
- loopback recording of a yaw sweep: ILD/ITD monotonic, front/back asymmetry, lag ≤ 1 block;
- zero `render time exceeded` lines;
- toggling mid-flight gives no click above −40 dBFS;
- the log's per-context lines and `listener-vs-pose` (the listener of the first 3D context, named as `ctx <n>` in the line; context 0 is DCS's 2D one) settle the open [A] items of the hook spec (context ids, listener
  lag).

Then the user's blind A/B: stock vs Windows Sonic vs binaural, with six cues (wingman behind, rotor overhead, switch
clicks left/right/overhead, gun, missile launch).

## 9. Parts

Each part was built on a branch from `feat/hrtf-contract` (`git switch -c feat/hrtf-<part> feat/hrtf-contract`) in its own
worktree. Files are split so the four could merge without conflicts. Where two parts touch one file, the owner is named;
others only append.

### BinauralCore, data, converter, harness: `feat/hrtf-core`
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
- **Hand-off:** the core library target name and its include path for the audio module.

### DCS audio module: `feat/hrtf-module`
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

### Head pose publisher: `feat/hrtf-pose`
- **`native/pupil_shift/pupil_shift_layer.cpp`:**
  - create a VIEW reference space per session;
  - at each xrLocateViews with a new display time, `xrLocateSpace(view, info->space, displayTime)` with
    `XrSpaceVelocity`;
  - publish per `head_pose_shm.h`;
  - `qpcDisplay` through `XR_KHR_win32_convert_performance_counter_time`: the application's when it enabled it, else
    added by the layer at instance creation when `Publish=1` then and the runtime lists it (only then; a refusal retries
    without it, the rest unchanged); otherwise 0 (`writer_flags::qpc_display` says which);
  - the fallback with `from_views`; a velocity the runtime does not give is derived from the previous entry (same base
    space, at most 100 ms before);
  - gate with PupilShift.ini `[HeadPose] Publish` (default 0; Live): off = no mapping, no VIEW space, no runtime call;
  - one log line per session (`head pose: publishing` / `off`), one per Live change.
- **Built** (`feat/hrtf-pose`): `native/pupil_shift/head_pose_publisher.hpp` (writer, pure),
  `native/binaural/include/head_pose_reader.h` (the module's reader: bounded tear-free read, staleness, extrapolation
  with a capped horizon and speed limits, `LateCorrectionNow`). No higher-rate source: the layer's only other
  xrLocateSpace (eye gaze) runs at the same display time, so the feed is one entry per DCS frame and readers predict.
- **Fakes and tests:**
  - the fake runtime: VIEW space and velocity support, the time conversion extension;
  - `head_pose_tests.cpp` (writer, reader, seqlock stress) and the harness's `head_pose_*` scenarios (8.4);
  - a reader test using only the header (the module's reader path).
- **Constraint:** no change to how the layer moves DCS's views (pose publishing is read-only).

### App: `feat/hrtf-app`
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
  - status from the module log: "Running · 32 sounds · 0.2 ms" / "Off: <reason>"; "Off in this profile" when the
    module is installed for other features and the deployed DcsQvCull.ini has no `[Audio] Binaural=1`; "Not available"
    when the applied profile has it on but there is no HRTF set (nothing of it was deployed); "Switched off" for the key,
    the flag or `audio: off (binaural is off in the settings)` (turned off live: not a failure, no Checks warning).
- **UI text:** short and plain, for example "Sounds come from where they are: front, back, above. For headphones."
- **Model and writers:**
  - `VrProfile` fields → `ConfigurationWriters` → `[Audio]` in DcsQvCull.ini, and `[HeadPose] Publish` in
    PupilShift.ini;
  - `written-keys.txt`, `contracts.json`, `module-only-keys.txt` as the scripts require;
  - `LiveSettings` category "Audio" (every key).
- **Kill switch:** a key in Keys & headset panel (`AudioToggle`) and the `toggle_audio.flag` action. Launch DCS removes
  a flag the module never took (DCS closed right after the button), so the next session does not start switched off.
- **Back to stock:** section 7.
- **Checks lines:**
  - DCS output not stereo (with the fix);
  - binaural on but the pupil shift layer not active (no late tracking);
  - an HRTF file missing;
  - the module reported `audio: off (<reason>)`.
- **Other places:**
  - Last flight: "3D audio: on/off/fallback (reason), p99 cost";
  - diagnostics package entries;
  - SOFA import UI calling the converter, with the conversion report;
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

Done on `feat/hrtf-audio`: the payload compiles and links the real core (`DcsQvBinauralCore` in
`native/dcsqvcull/CMakeLists.txt`); the stand-in stays for the module's own scenarios, and the end-to-end test
`DcsQvCullAudioE2ETests` drives the module with the real core (docs/BINAURAL_AUDIO_HOOKS.md, "Tests"). The release
build also runs `scripts/build-binaural.ps1`. The app's status reader parses the module's own lines (section 10), the
module's LogVoices file is `audio_voices.csv`, and the invariant test reads back every `[Audio]` key the app writes.
The generic set (`native/binaural/data/sadie2_ku100.dcshrtf`, SADIE II through `scripts/fetch-hrtf.ps1`) is committed
and embedded as `HRTF_DEFAULT`, and `DcsVr.Cli hrtf-convert` converts SOFA files (PureHDF), so a release has 3D audio
with the generic ears and Import SOFA. A build without the set (only a `-LocalOnly` package can be one) keeps the
behaviour below: with 3D audio on, the module stays off (`audio: off (no HRTF set: this build has no default HRTF set)`).
`Binaural.PayloadHasAudio` (Live) finds the resource name in the module's code either way, so it tells "this payload
has the audio module", not "this payload has the set".

Review fixes (`fix/hrtf-audio-review`, on `feat/hrtf-audio`):
- **No set, nothing deployed.** `Binaural.PayloadHasDefaultHrtf` reads the payload's PE resource table (`PeResource`):
  the RCDATA `HRTF_DEFAULT` with data. Without it and without a personal set (`Binaural.Usable`), Launch DCS deploys no
  part of 3D audio (no `[Audio]`, no head pose, no Stereo layout; profile.json keeps the choice), and since 0.7.1 (a release always has the
  generic set) no Checks line or greyed switch is left for it; the module patches ed_sound only when a
  set loads (`HrtfReady`). `scripts/build-release.ps1` refuses a release without the set, its Apache-2.0 text and its
  NOTICE (a `-LocalOnly` package warns), and copies both into `licenses/`.
- **SOFA import:** every build ships the converter, so since 0.7.1 the app no longer runs `DcsVr.Cli hrtf-convert --check`;
  `CliHrtfConverter.IsAvailable` stays for the tests (exit 0 = the converter is there; part of the CLI contract below).
- **Head size** is relative to the set's own head: the renderer gets `PilotItdRadiusM(set's fitted radius,
  circumference)` = fit x C / 57 cm (binaural_config.h), so 57 cm plays the set's own timing, whatever head it was
  measured on, and a set picked live gets its own fit again. (C / 2 pi against a Woodworth fit scaled the reference head's
  ITDs by about 4 %.)
- **Inside the head radius** (the module allows 0.05 m) the per-ear parallax offset shrinks to 0 between two radii and
  one, so the near ear's filter never turns to the far side (harness A7, sweep 0.3 to 0.05 m).
- **Two DCS outputs**, the overrun stop counted only against our own share and re-armed by the kill switch or Live, a
  busy settings slot keeping the last good copy, and a control thread that does not stop within 3 s leaving every
  binding in place: docs/BINAURAL_AUDIO_HOOKS.md section 9.

## 10. The app side: what the app writes, reads and calls

Implemented on `feat/hrtf-app` (`src/DcsVr.Core/BinauralAudio.cs`, `HrtfLibrary.cs`; the Audio page in `src/DcsVr.App/ui`).
The core and the audio module match these; the tests check them against the code.

**Profile** (`VrProfile`): `binauralAudio`, `binauralHrtf`, `headCircumferenceCm`, `binauralLateHeadTracking`,
`binauralNearField`, `binauralMaxObjects`, `binauralLevelTrimDb`, `binauralCpuBudgetPercent`, `binauralExcludeContexts`
(profile file only), `binauralLogVoices`, `audioToggleKey` (default `117:6`, Alt+Shift+F6). Derived, never saved:
`UsesBinauralAudio` (also with Optimizations only), `UsesHeadPosePublish` (VR, LateHeadTracking on).

**Files Launch DCS writes** while 3D audio is on:
- `DcsQvCull.ini`: `[Audio]` with every key of `binaural_config.h` (ExcludeContexts only when set), each clamped to the
  table's range, and `[Hotkeys] AudioToggle` in the one `[Hotkeys]` section. Off: neither is written. The engine module is
  installed for 3D audio alone (Perception-only mode when Engine Optimizations are off).
- `Scripts\DcsQvCull\hrtf\<name>.dcshrtf`: every valid set of the app's library (state folder `hrtf\`), so another set
  can be picked live. A profile naming a set that is gone gets `Hrtf=default` (Checks: `audio-hrtf-missing`).
- `PupilShift.ini [HeadPose] Publish=1` in VR with LateHeadTracking; the pupil shift layer is deployed for it, with
  `OpenXrEyeGaze=0` when nothing else needs the gaze. Without the layer component, 3D audio installs without it.
- `options.lua sound.main_layout = "stereo"` (owned, restored by Back to stock and when 3D audio is turned off). The
  `spatial` driver is never written; when `sound.driver_name` is `spatial`, Checks warns (`audio-spatial`).
- Runtime files removed by Back to stock: `toggle_audio.flag`, `audio_*.csv` (the module writes LogVoices to `audio_voices.csv`).

**Live:** every field above but `binauralExcludeContexts`, file `DcsQvCull.ini`, only while the installed payload has the
audio part: the app looks for the UTF-16 resource name `HRTF_DEFAULT` in the installed `DcsQvCullPayload.dll` (the audio module
embeds the generic set under that name). `Audio.Hrtf` naming a set not installed at launch waits. `HeadPose.Publish` is
not a live key (read when DCS starts VR), so turning 3D audio on in VR waits: `Audio.Binaural` 0 -> 1 is held back
(the whole `DcsQvCull.ini` write waits) when DCS started with it off and either the head pose is needed (VR with
LateHeadTracking) or options.lua's speaker layout is not Stereo yet (`LiveSettings.IsLiveKey`, `audioOnLive`). Back on
after a live off is live.

**Log lines the app reads** (`Binaural.ParseStatus`; Audio page status, Last flight, Checks), all after the last
`loader started` of `DcsQvCull.log`, each containing ` audio: `:

`<set>` is `default` (the generic set) or `personal` (one of the pilot's own): the module never logs a set's name (the
pilot names them, often after a person, and the log reaches diagnostic packages). The app takes the name of a
`personal` set from the deployed `DcsQvCull.ini` `Hrtf=`. Older logs that still carry names are read the same way (and
redacted in the diagnostic package).

| Line | Meaning for the app |
|---|---|
| `audio: on (<rate> Hz stereo, HRTF <set>: <n> directions, ...)` | running, with that set |
| `audio: HRTF <set> (crossfading)` | another set picked live |
| `audio: off (<reason>)` | off for the session; the reason is shown as is (Checks `audio-status`) |
| `audio: off (<id> not found (<why>))` | a hook point missing: shown as "this DCS version's sound engine is not the one it was made for (<id>)" |
| `audio: kill switch (<by>): binaural OFF (...)` / `... binaural ON` | the kill switch (key or flag) |
| `audio: running, <n> sounds, p99 <x> ms per block, ..., limit <n>; mono <n>, stereo <n>; wide <n> (near or diffuse); ...; head pose on\|off\|unused` | every 10 s: sounds rendered in 3D (of them: mono waves, stereo waves; and all voices kept wide in the last block, near the head or diffuse, hook spec §10), the p99 cost per block, and whether the headset's pose reached the audio (`unused` with LateHeadTracking=0) |
| `audio: idle, blocks <n> (armed <n>), pan calls <n>; refused: off <n>, output <n>, not 3D <n>, offline <n>, channels <n>, context <n>, distance <n>, output not stereo <n>, gains <n>, pan law <n>, not at start <n>, limit <n>, no wave yet <n>` | every 10 s instead of `running,` while 3D audio is on and nothing was rendered in 3D: over those 10 s, the output blocks the module saw and how many it armed, DCS's panner calls, and why each call's sound was not given to the 3D renderer (the first reason that applied: a 2D sound is `not 3D` on any output, `output` is a 3D sound on another output, `channels` a wave of more than two channels). No blocks: the module is not on DCS's output; blocks but none armed: no output bound; pan calls all refused: the reason with the count. Diagnostic only (the app does not read it) |
| `audio: CPU budget: at most <n> sounds now (... over budget)` | a CPU back-off, down to n sounds (Checks `audio-backoff`); `(10 s under half the budget)` raises it again |
| `audio: on (re-enabled in the settings, HRTF <set>: <n> directions)` / `audio: on (re-armed by the kill switch ..., ...)` | running again on the same binding (after Binaural=0 live, or after the overrun stop); a later `running,` line also means running |
| `audio: HRTF <set> not usable (<why>); the default set is used` | the pilot's set was refused by the module: the Audio page and Checks (`audio-hrtf-refused`) say so with the reason |

These are the module's own lines (`native/dcsqvcull/src/main/audio.inc`); the module's `Phrases` test keeps them in the
source and the app's status test parses them verbatim.

**SOFA converter** (`src/DcsVr.Core/Hrtf/SofaConversion.cs`), called through the CLI contract
`DcsVr.Cli hrtf-convert <in.sofa> <out.dcshrtf> --name <name>` (`CliHrtfConverter`, hidden, below-normal priority,
two-minute limit; a conversion takes about 2 s): exit 0 with the file written is a conversion (written beside the
target, then moved over it; the target needs no `.dcshrtf` extension, the app passes a `.tmp`); stdout lines
`warning: <text>` are shown in the report and `coverage: <percent>%` as the grid coverage (the share of 4,000 evenly
spread directions with a measurement within 15 degrees). Any other exit is a refusal whose reason is the last stderr
line: 2 for a file the reader or converter refuses, 1 for wrong arguments or a file that cannot be read or written.
`--source` defaults to the database, subject and organisation the file names (never its path), `--name` to the file's
name; without `--generic` the set is marked personal. `DcsVr.Cli hrtf-convert --check` exits 0 when the converter is there
(for the tests; the app does not probe it, since every build ships the converter). The app validates the result again with a port of
`hrtf_format.h Validate()` (`HrtfFile.Validate`) and keeps nothing it refuses. `IHrtfConverter` is the seam: a later version may
replace `CliHrtfConverter` with an in-process call. The SOFA file and its folder are never kept; the library keeps the
`.dcshrtf`, and a sidecar with the SOFA file's name, the import time, the warnings and the coverage.

**Checks codes:** `audio-hrtf`, `audio-head-size`, `audio-max-sounds`, `audio-level`, `audio-cpu` (ranges, errors),
`audio-contexts`, `audio-hotkey`, `audio-layout` (info), `audio-spatial`, `audio-hrtf-missing`, `audio-head-pose`,
`audio-status`, `audio-backoff` (readiness, from the log). The diagnostic package adds `dcs-control/audio.txt` (the library's
sets and the audio log lines; never a SOFA file, its path, or a `.dcshrtf`). The pilot's set names (often a person's) are
"personal set N" everywhere in the package: audio.txt, the module's log, DcsQvCull.ini, the profiles in report.json.
