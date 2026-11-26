# Cockpit space: out-of-head 3D headphone audio for every DCS aircraft

Design for the next step of 3D headphone audio ([BINAURAL_AUDIO.md](BINAURAL_AUDIO.md),
[BINAURAL_AUDIO_HOOKS.md](BINAURAL_AUDIO_HOOKS.md)). Today every owned 3D voice is rendered as a dry HRTF source
(direct sound only). Dry binaural sound is heard close to, or inside, the head, most of all for sources near the median
plane, which is where the cockpit is: in the hrtf live runs of validation-08 (2026-11-25) about 94 % of the owned energy
was cockpit sound within about 1 m of the head, above and in front of it.

"Cockpit space" adds what a real cockpit adds to every sound: a few early reflections from the canopy, the panels and
the seat, and a short diffuse tail, fixed to the aircraft so they stay put when the head turns. It does this from a
small parametric model per aircraft family, with no per-aircraft simulation. The user gets one control: **Cockpit
space: Off / Subtle / Normal / Strong** (default Subtle), Live.

Status: CORE, MODULE and APP built and merged on `feat/cspace` (from `dev/next`), tested offline end to end: the
module on the fake ed_sound image with the real Room (section 13.2). Not heard in DCS yet: the live checks and the
pilot's listening test are in section 10.1. Where the implementation differs from the first draft of this document, the
text below says what was built and why (marked "Built:").
Labels as in the hook spec: **[V]** read in the disassembly or the files at the address or line given, **[A]**
inferred, with what settles it.

Contents:
1. What makes headphone sound leave the head
2. What is known about real cockpit acoustics
3. What DCS gives us (static reading of 2.9.31)
4. DSP design
5. Aircraft families and presets
6. Which voices get it, level neutrality, interplay with point/wide and PanLevel
7. Configuration, Live, UI
8. Interfaces (core API, module data, Lua bridge, config keys, log lines, CSV columns)
9. CPU budget and real-time rules
10. Validation: objective metrics
11. Listening test (pilot, in the headset, about 10 minutes)
12. Task split and file ownership (CORE, MODULE, APP)
13. Test lists
14. Limits and open questions

---

## 1. What makes headphone sound leave the head

From the literature (summarised from knowledge; not re-checked online for this document):

- **Room information is the strongest single cue.** Adding reverberation (early reflections plus a tail) to an
  anechoic binaural source makes it externalised far more often than individual HRTFs or head tracking alone do
  (Begault, Wenzel and Anderson, JAES 2001: reverberation gave the largest externalisation gain; head tracking mostly
  reduced front/back reversals). Hartmann and Wittenberg (JASA 1996) and Durlach et al. (1992) showed that the
  interaural cues must be consistent across frequency; Best, Baumgartner, Lavandier, Majdak and Kopčo (Trends in
  Hearing 2020) review the field.
- **Temporal fluctuations of the interaural cues caused by reverberation.** Catić, Santurette and Dau (JASA 2015)
  found that the fluctuations of the interaural level difference that reverberation causes, mainly at the ear far from
  the source, drive externalisation; Leclère, Lavandier and Perrin (2019) confirm the role of the contralateral ear's
  reverberation. A tail that is **decorrelated between the ears** gives these fluctuations; a mono (coherent) tail does
  not, and is heard inside the head.
- **Interaural coherence of the late part.** In a diffuse field the coherence between the ears is high below about
  200-300 Hz (the head is small against the wavelength) and near zero above about 1 kHz. A tail with that
  frequency-dependent coherence sounds like a space around the head; a fully decorrelated low end sounds phasey and a
  fully coherent one sounds inside the head.
- **Early reflections from consistent directions that rotate with the head.** Reflections that are fixed in the
  world (here: in the aircraft) and move against the head when it turns give dynamic cues that resolve front/back and
  move the image out (Brimijoin, Boyd and Akeroyd 2013: head movement and externalisation; head-locked reflections
  reduce externalisation). A room that turns with the head is no better than a reverb plug-in.
- **Direct-to-reverberant ratio (DRR).** DRR is the main absolute distance cue (Zahorik 2002). Too much reverb pushes
  sources away and smears the cockpit; too little leaves them in the head. For sources at arm's length in a tiny space
  the direct sound dominates: we aim for an early-reflection sum about 9 to 16 dB under the direct sound and a tail 13
  to 22 dB under it (section 5), i.e. inside the -15..-25 dB range per reflection found effective in small rooms.
- **Room divergence.** A simulated room that is implausible for the scene, or very different from what the listener
  expects, reduces externalisation (Werner et al. 2016, "room divergence"). A pilot expects a small, dry, glazed
  enclosure: short decay (RT60 about 0.08-0.25 s), bright reflections from glass, dull ones from padding and the body.
- **Spectral colouration.** Very early reflections (1-3 ms in a cockpit) comb-filter the direct sound. Real cockpits
  do this; it is part of their sound, but in excess it is heard as colouration ("tin can"). Keep each reflection low,
  low-pass the panel and seat reflections, high-pass the glass reflections (glass passes low frequencies out instead of
  reflecting them), and let the HRTF of each reflection's own direction decorrelate them between the ears.
- **The direct sound's spectral detail matters more than the tail's** (Hassager, Gran and Dau 2016): the tail can be
  cheap (smoothed spectrum, generic diffuse-field EQ) as long as the direct path keeps the full HRTF, which it does.

What follows for the design: (a) keep DCS's direct sound and our HRTF exactly as now; (b) add a few early reflections
per voice from aircraft-fixed directions, rendered binaurally and rotated with the head; (c) add a short shared tail
with frequency-dependent interaural coherence; (d) keep levels low and the overall loudness unchanged.

## 2. What is known about real cockpit acoustics

- **Size.** A single-seat fighter cockpit has about 1-2 m³ of air around the pilot; the head is 0.2-0.3 m from the
  canopy above, 0.3-0.45 m from the canopy sides, 0.6-0.9 m from the instrument panel and windscreen, 0.25-0.35 m from
  the headrest. Two-seat tandem cockpits are the same per seat. Helicopter cabins (UH-1H, Mi-8, CH-47, SA342 to a
  lesser extent) are several to tens of cubic metres, often open to a cargo cabin and with doors open. Transport flight
  decks (C-130J) are about 6-10 m³, lined and carpeted.
- **Surfaces.** Acrylic/polycarbonate canopies and windscreens are hard and smooth: they reflect mid and high
  frequencies almost fully but, being thin panels, let low frequencies through (panel resonances in the 100-300 Hz
  range), so their reflection is "bright". Consoles and panels are hard but broken up by switches and knobs (diffuse,
  moderate high-frequency loss). The seat, harness, survival kit, G-suit and the pilot's body and legs absorb (dull,
  weak reflections). Warbirds have bare metal fuselage skins behind the seat (reflective, resonant). Transport flight
  decks are lined (more absorption, longer path lengths).
- **Decay.** Measured RT60 in small vehicle cabins (cars, light aircraft) is about 0.05-0.15 s at mid frequencies;
  cockpits with large glazed areas sit at the upper end, helicopters with cargo cabins at 0.2-0.4 s. Below the
  Schroeder frequency (about 2000 √(T/V): ~500-600 Hz for 0.12 s and 1.5 m³) a cockpit is modal, not diffuse: a
  geometric model is not accurate there, and a booming low end would be wrong anyway.
- **What the pilot hears in reality** is dominated by boundary-layer and air-conditioning noise, engine and gearbox
  noise through the structure, all through a helmet with ear cups or ANR. DCS already models the levels and the canopy
  damping of these sounds; our task is not to re-simulate them but to give each of them a place outside the head.
  We model neither the helmet nor structure-borne paths.

## 3. What DCS gives us (static reading of 2.9.31, `bin\ed_sound.dll`, build of [BINAURAL_AUDIO_HOOKS.md](BINAURAL_AUDIO_HOOKS.md) §0)

Read with Python `pefile` + `capstone` (DCS not started, nothing written). New offsets for the hook spec (MODULE adds
them as section 11 there):

### 3.1 DCS's canopy damping ("cockpit gain") [V]

The sound update `sub_6f70` (rbx = sound, rdi = its context) runs once per sound per `SND_process`:

| Address | Code | Meaning |
|---|---|---|
| `0x6ff2..0x7005` | `gain = ctx+0x58 * sound+0xb8` → `sound+0x120` | listener gain x the source's gain parameter |
| `0x7010` | `cmp byte [ctx+0xe1], 0` | the context uses cockpit gain (`SND_context_use_cockpit_gain`, `0x40f6`) |
| `0x7019` | `cmp byte [sound+0x1a8], 0` | the sound's host allows cockpit gain (`SND_host_allow_cockpit_gain` `0x6330` writes host`+0x104` and every host sound's `+0x1a8`; set at creation in `sub_81c0`, `0x82d6..0x82e0`) |
| `0x7022..0x7035` | `sound+0x1b0 = proto(sound+0x50)->(+0x18)->(+0xa0)` | the sdef's `cockpit_gain` (string at `0x2df3f`) |
| `0x703e..0x7046` | `sound+0x120 *= sound+0x1b0` | **the canopy damping: a constant gain per sdef, applied only when both flags are set** |
| `0x705c..0x706d` | `sound+0x124 = ctx+0x5c * sound+0xbc` | pitch (listener x source) |
| `0x707b..0x7093` | `sound+0x128 = min(sound+0xc0, ctx+0x60)` | low-pass: the source's, capped by the listener's |
| `0x70a1..0x70b0` | `sound+0x12c = sound+0xc4` (8 bytes) | high-pass frequency and shelf gain (`hp_freq`, `hp_shelf_gain` per the format string at `activate_sound`) [A] |

Live evidence (validation-08 run 20261125-1351, F-16C_50): `audio: ctx 3 2d=0 cockpitGain=1` appears at the flight
start; context 3 carries the world's 3D sounds (owned voices at 3.5 m to 2.4 km), context 2 the cockpit (0.44 m
median, `cockpitGain=0`). So **context use-cockpit-gain = 1 means "the listener is in the cockpit"**, and DCS damps
outside sounds whose host allows it by the sdef's constant `cockpit_gain`.

What this means for us:
- DCS's direct path already carries the canopy damping; we never add damping to the direct sound.
- `ctx+0xe1` of the world context is the **inside/outside signal**, readable on the sim thread every frame for free.
- `sound+0x1a8 && ctx+0xe1` marks a voice as **heard through the canopy** (outside, damped); `sound+0x1b0` is the
  damping DCS applied (logged per voice).
- `cockpit_gain` is a constant per sdef: it does **not** follow the canopy position [V for ed_sound]. Whether an
  aircraft changes the world listener's gain `ctx+0x58` or low-pass cap `ctx+0x60` when the canopy opens cannot be
  read statically: `SND_set_listener` is called from WorldGeneral.dll, which is protected ([BINAURAL_AUDIO_HOOKS.md](BINAURAL_AUDIO_HOOKS.md)
  §0) [A]. MODULE adds a probe log line (section 8.5) that settles it in the first live flight with a canopy cycle.
- Third-party modules may play cockpit sounds from `"2D"` hosts (e.g. MB-339's `Sound_System.lua` creates
  `COCKPIT_MECHANICAL`, `COCKPIT_SYSTEMS`, `COCKPIT_ENGINE` hosts as 2D). 2D voices are never owned; they keep DCS's
  own panning and get no cockpit space.

### 3.2 The aircraft's orientation against the head [V layout, A meaning for cockpit hosts]

`sound+0xe8` (double4, x y z w) is the source's orientation **in the listener frame** ([BINAURAL_AUDIO_HOOKS.md](BINAURAL_AUDIO_HOOKS.md)
§4.2, `0x736c`), `sound+0x80` its world orientation, `ctx+0x20` the listener's orientation (listener → world). Cockpit
sound hosts are attached to the aircraft, so their orientation is the aircraft's [A: settled live by the consistency
check of section 4.4: all cockpit-context voices give the same rotation within 1°, and it turns against the head]. So
the rotation **aircraft frame → head frame** is `C = sound+0xe8` of any cockpit-context 3D sound, available on the sim
thread every frame, in single player and multiplayer alike, with no Lua. Between DCS frames the render thread applies
the existing late head correction to it (the same `ComputeLateCorrection` the voices use), so the room turns against
the head at audio rate.

### 3.3 Aircraft type and canopy position (Lua, the hook state the loader already uses) [V API, A availability]

- **Type:** `DCS.getPlayerUnitType()` in the GameGUI hook state (where `DcsQvCull.lua` runs) returns the player's unit
  type name (e.g. `F-16C_50`; dcs.log shows the same name as `MissionSpawn:spawnLocalPlayer 441,F-16C_50` and
  `WWT (Main): F-16C_50`). It does not depend on export permissions. Fallback: `Export.LoGetSelfData().Name`.
- **Canopy:** `Export.LoGetMechInfo().canopy.value` (0 closed .. 1 open; `Scripts\Export.lua` line ~795 documents
  `canopy = {status, value}`). Fallback: `Export.LoGetAircraftDrawArgumentValue(38)` (draw argument 38 is the canopy
  on most DCS models [A]). In multiplayer, ownship exports can be forbidden by the server: then the canopy is unknown
  and treated as closed.
- **Inside (fallback):** the loader's existing camera check (camera within 15 m of the ownship, `DcsQvCull.lua`
  `inCockpit`), used only when no context ever reported use-cockpit-gain in this session.

## 4. DSP design

### 4.1 Overview

```
 owned voice i (mono mid M_i, gain g_i, head-frame position p_i)
   |--> [existing] HRTF direct (core Renderer) -----------------------------x d_i (level comp) --> out
   |
   +--> send s_i = g_i x distance comp x class --> HP 200 Hz --+--> per-voice tap delay line (<= 12 ms)
                                                               |      8 taps: image sources of a box around the head
                                                               |      each tap: delay, gain, surface filter (1-pole)
                                                               |      panned (3 gains) onto 14 aircraft-fixed buses
                                                               |
                                                               +--> late send (pre-delay) ----+
 buses (14 mono) --> core Renderer, 14 far-field slots, positions = bus directions rotated     |
                     aircraft -> head (C x late correction) at block rate --------------------+--> out
 late: FDN 8 x 8 (two-band decay) --> 2 orthogonal outputs --> coherence mixer --> diffuse EQ -+
```

- The **direct path is unchanged** (only multiplied by a small level compensation `d_i`, section 6.2).
- **Early reflections**: per voice, image sources of a parametric box (6 walls) plus 2 second-order canopy taps, as
  delays and gains on that voice's own delay line, panned onto a **shared set of 14 buses fixed to the aircraft**
  (6 axes + 8 cube corners: 55-70° apart). Each bus is one far-field HRTF source in a second core Renderer. The HRTF
  cost is 14 sources whatever the number of voices; per voice the cost is 8 taps x 3 bus adds.
- **Late tail**: one 8-line feedback delay network shared by all voices, two-band decay, two orthogonal outputs mixed
  to the ears with frequency-dependent coherence, then a diffuse-field EQ taken from the HRTF set.
- **Rotation**: the buses are fixed in the aircraft frame; their head-frame directions are `C ⋅ bus` per block, with
  the late head correction. The core's own filter crossfade (0.5° threshold, 64 frames) makes the turn smooth. The
  tail is direction-free and needs no rotation.

Built (CORE), where the implementation differs from the overview above:
- **Wall filters per (tap, bus), not per voice.** The wall filters are linear and the same for every voice, so each
  voice's 8 taps are mixed (ramped VBAP gains) into 8 x 14 accumulators, and Render runs each live accumulator through
  its wall's one-pole high-pass and low-pass once for all voices (four accumulators per SSE vector). Per voice the cost
  is one pass over its delay line per tap; per voice filters cost 10 µs per voice (measured, scalar, latency-bound).
- **Send high-pass of fourth order** (two Butterworth sections), on each bus and on the late input, not per voice.
  Early reflections arrive within a few ms, so below 200 Hz they add to the direct sound in phase; with a second-order
  high-pass the DCS-like mix of R4 came out 1.3 dB louder, with fourth order -0.1 dB.
- **Largest single reflection 3 dB under the preset's early energy** (`room_model.h TapCap`: -15 dB at Normal). A
  voice 5 cm from the canopy otherwise gets one reflection a few dB under the direct sound 0.2 ms later; R9 measured
  5 dB of ripple at Normal before the cap, 2.7 dB after.
- **A fixed extra delay per tap, 0 to 0.29 ms** (`kTapJitterS`): the box is mirror-symmetric, so a median source got
  left/right (and up-left/up-right) reflections at exactly the same delay, adding in phase at both ears (+3 dB over the
  model) and identical at both ears.
- **Energies relative to the set's diffuse-field average**, not to its front direction: the room's energy does not
  depend on the voice's direction, the direct sound's does (on the synthetic set ±1.7 dB between the cockpit
  sources, 6 dB quieter behind), so neutrality holds on average over directions.
- **Calibrations at `SetHrtf`** (control thread, a few ms): κ (late chain vs the closed form) at RT60 0.05, 0.1, 0.2,
  0.4 and 0.8 s, interpolated per block (the closed form was 2 dB off at 0.25 s); and an early (bus) gain that makes
  the rendered energy of the generic preset's taps for six reference sources equal to the model's (-1.7 dB on the
  synthetic set, -1.5 dB on the KU100: the three bus filters of a tap are correlated below a few kHz).
- **AVX2/FMA tap mix** (`kernels_avx2.cpp RoomMixAvx2`, chosen at `Init` like the core's kernels; SSE2 otherwise).

### 4.2 The box and the image sources

The box is described by the distances from the head (at the origin, aircraft frame: x forward, y up, z right) to six
walls: left, right, front, back, up, down, each with an amplitude reflection coefficient `R` (broadband) and a
first-order filter: low-pass corner `fLP` (panels, seat, body) and/or high-pass corner `fHP` (glass).

For a source at `p` (aircraft frame, metres, head at origin; a source beyond the box along its direction is first
moved onto the box surface along that direction: the **excitation point**, see "through the canopy" below):

- first order, wall `w` at signed distance `D_w` on axis `a`: image `p'` = `p` with `p'_a = 2 D_w - p_a`;
- second order (canopy scatter): images through `up` then `left`, `up` then `right` (bubble and framed canopies: the
  curved canopy sends a reflection down to each ear);
- per tap: direction `u = p'/|p'|`, delay `τ = (|p'| - |p|) / c` (c = 343 m/s; relative to the direct sound, which
  DCS has already delayed by its own propagation), gain `G = (|p| / |p'|) ⋅ R_w (⋅ R_w2)`, filter of the wall(s);
- the tap gains are normalised so the early energy (sum of `G²`) at the reference distance (0.6 m, straight ahead)
  equals the preset's `earlyDb` (section 5); the geometry gives the ratios between taps.

Taps are recomputed per block at the block's end position and ramped: gains linearly across the block, a changed
delay as a crossfade between the old and new read positions across the block (as `SideDelayed` does), so a moving
source or a preset change never clicks. Whole-sample delays (at 48 kHz half a sample is 10 µs: inaudible for taps
15-25 dB down).

**Through the canopy.** An outside voice (its host allows cockpit gain and the world context uses it, section 3.1)
reaches the cabin through the canopy: its excitation point is where its direction crosses the box, so its early
reflections come mostly from the opposite side of the cockpit, as in a real canopy, and it feeds the tail. The send is
the voice's own (already damped by DCS) signal, so a sound DCS damps hard excites the cabin weakly too. A preset value
`throughDb` (default 0 dB) trims it.

**Distance law of the sends.** Inside the box the reverberant level of a real room does not depend on the source
distance, while DCS's voice gain does (it already includes its distance attenuation). The send therefore undoes part
of it: `send = g ⋅ clamp(r / 0.6 m, 0.5, 2)` for sources inside the box (r = distance), `send = g` for outside
voices. Close sources keep a high DRR (as in reality), farther cockpit sources get relatively more room.

**Send high-pass.** Every send goes through a second-order high-pass at 200 Hz (preset `sendHpHz`): the cockpit is
modal below the Schroeder frequency, DCS's engine and cockpit sounds carry most of their power at low frequencies, and
a reverberated low end only adds boom and loudness. Externalisation cues live above about 500 Hz.

### 4.3 The late tail

- FDN with 8 delay lines, lengths (samples at 48 kHz, scaled with the rate and rounded to primes) 149, 211, 263, 293,
  337, 389, 431, 479 (3.1-10 ms), orthogonal Hadamard feedback matrix (scaled 1/√8, 24 additions per sample).
- Two-band decay per line (Jot): a first-order shelving absorption filter per line from `rt60LowS`, `rt60HighS` and
  `crossoverHz` (1 kHz): `g_line = 10^(-3 d / (RT60 fs))` per band. Every gain below 1 (stable).
- Input: the sum of all late sends after a pre-delay (`preDelayMs`, 2-6 ms by family) and the 200 Hz high-pass,
  distributed to the lines with a fixed ± sign vector.
- Output: two orthogonal output vectors give `x1`, `x2` (uncorrelated). Ears: `L = cos θ x1 + sin θ x2`,
  `R = cos θ x1 - sin θ x2`, so interaural coherence = cos 2θ. θ is frequency-dependent through a 1st-order crossover at
  400 Hz: low band θ = 0.23 rad (IC ≈ 0.9), high band θ = π/4 (IC ≈ 0): the diffuse-field coherence of a head, in two
  bands.
  Built: a first-order crossover leaves the coherence at only 0.57 at 300 Hz (C7 asks ≥ 0.8 below 300 Hz). The mixer is
  `L = (x1 + H x2) / √2`, `R = (x1 - H x2) / √2` with `H = 0.234 + 0.766 HP2(650 Hz)` (second-order Butterworth
  high-pass), coherence `(1 - |H|²) / (1 + |H|²)`: 0.90 at DC (tan 0.23 = 0.234, as θ = 0.23), 0.96 at 300 Hz, 0.16 at
  1 kHz, 0.06 at 1.4 kHz, → 0 above (`src/room_fdn.h` Mixer).
- Built, absorption: each line's filter is `g_hi + (g_lo - g_hi) LP1(crossoverHz)`, a convex mix of the two band gains
  (every line gain < 1 at every frequency: stable, C6), `g = 10^(-3 d / (RT60 fs))` per band.
- Built, level: the late gain is `√(lateLin ⋅ E_front / (κ ⋅ E_model(RT60)))`, `E_model` the closed form
  `Σ_bands w_band / (1 - mean g²)` of `room_model.h FdnEnergy`, κ the measured ratio of the real chain (FDN, mixer,
  diffuse EQ) to it at a reference decay, computed in `SetHrtf` (control thread). `E_front` is the HRTF set's own
  front energy (both ears), so the tail is `lateDb` under the direct sound of a source straight ahead, for any set.
- Diffuse-field EQ: at `SetHrtf` (control thread) the room computes each ear's average power spectrum over all
  directions of the set and fits a low shelf + high shelf + one peak (3 biquads per ear) to it, normalised to 0 dB at
  1 kHz; the tail runs through it, so it has the same timbre as the HRTF-rendered voices.
- Level: the late gain is set so the tail's energy for a unit send equals `lateDb` (section 5) relative to the direct
  sound at the reference distance; the energy of an exponential decay is known in closed form, so this is exact per
  band, computed on the control thread.
- Denormals and silence: the audio thread runs under FTZ/DAZ (core's `MxcsrScope`); when no send has been non-zero
  for 3 x RT60 and the tail energy is under -140 dB, the FDN is zeroed and skipped (cost 0).

### 4.4 Head rotation: the aircraft frame

- **From DCS (always):** for each 3D sound of the cockpit context (the context whose owned voices are nearer than
  1.5 m most of the time over the last 2 s, found by vote; logged), the sim thread reads `sound+0xe8`. The first such
  sound of the frame gives `C_dcs` (aircraft → listener frame); every further one is compared with it (angle). The
  frame's `C_dcs` is published with the head pose DCS used (`qUsed`, `poseBase`, as for voices).
- **Consistency:** if the angle spread of a frame exceeds 1°, or the rotation moves more than 30° between frames while
  the head pose does not, `C_dcs` is "unreliable" for that frame and the last good one is kept (counted).
- **At audio rate:** the render thread computes `C_now = R_late ⋅ C_dcs`, with `R_late` the late head correction from
  `qUsed` to the current pose (the same `ComputeLateCorrection` and plausibility limits as `CorrectedPosition`). Without
  head pose (LateHeadTracking off, or a monitor) `C_now = C_dcs`, at the sim rate.
- **No frame** (no cockpit-context 3D sound for 1 s and no pose, or never reliable): the early reflections fade out
  (late only, which is rotation-free). A head-locked room would make things worse (section 1).
- **Head translation** is not followed by the walls: the box is measured from the head's seated position. Voices'
  positions do follow translation (they are DCS's, plus the late correction), so their image sources move correctly
  relative to the walls only for rotations. Leaning 20 cm changes reflection delays by about 0.5 ms, which is not
  heard as such. (v2: shift the box by the tracked head offset from its 10 s mean.)

### 4.5 Canopy open, inside/outside, crossfades

- **Canopy** `c` (0 closed .. 1 open), when `CockpitSpaceCanopy=1` and known: up-wall reflection ⋅ (1 - c); the two
  canopy scatter taps ⋅ (1 - c); late level ⋅ (1 - 0.7 c); RT60 ⋅ (1 - 0.4 c). Smoothed with a 0.3 s time constant (the
  canopy moves over seconds). Helicopters: the family has no "up" glass to open; `c` lowers only the late level
  (doors). Unknown: closed.
- **Inside/outside:** wet target 1 when inside, 0 when outside (F2, F10, external views), ramp 0.3 s.
- **Every change is crossfaded:** strength and family changes publish a new `RoomParams`; the room keeps the old set
  and crossfades old → new over 100 ms (both tap sets run during it, the FDN's gains ramp per block, its delay
  lengths never change). Wet, canopy, early on/off: linear ramps across the block, ending at the target. The kill switch
  and ownership fades reach the room through the voice weight (section 6.1), so they are as smooth as the direct path.
- **Off:** with `CockpitSpace=0` the wet target is 0; once the fade and the tails are done the room is idle and no
  room code runs: the output is bit-identical to a build without cockpit space (test M1).
- Built (CORE), how the Room smooths what it is given, so a caller cannot make it click:
  - `SetParams`: every parameter is interpolated from its current value to the new one over 100 ms (distances,
    reflections, levels and RT60 linearly, filter corners on a log scale; the family and strength switch at half way),
    and each block's taps, filters and FDN gains are derived from the interpolated values; changed tap delays crossfade
    between the two read positions across the block, FDN gains ramp per sample. One tap set runs, not two (half the
    CPU of the draft's two-set crossfade during a change); C10 shows it click-free for strength and family changes.
  - `RoomFrame::wet` slews at most 0 → 1 in 50 ms (`kRoomWetSlewS`), on top of the module's own 0.3 s ramp; the wet
    weight scales the sends (the input of the delay lines and the tail), so the tail of a voice decays naturally after
    going outside instead of being cut.
  - Early reflections (`frameValid && early`) fade in and out over 50 ms (`kRoomEarlyFadeMs`); while they are off and
    the buses' tails are done (about 50 ms), the bus Renderer is not run at all (CPU back-off step 1 frees its cost).
  - The canopy follows `RoomFrame::canopyOpen` with the 0.3 s time constant inside the Room (`kRoomCanopyTauS`): the
    module passes the raw value (0 when unknown or `CockpitSpaceCanopy=0`).
  - A new HRTF set: the buses crossfade in the core (10 ms per bus), the diffuse EQ crossfades over one block, the gains
    that depend on the set ramp over one block.

## 5. Aircraft families and presets

### 5.1 Families

| Id | Name in UI | For | Notes |
|---|---|---|---|
| `bubble` | Bubble canopy | F-16C, F/A-18C, F-15C/E, AV-8B, MiG-29, Su-27/J-11, Su-33, JF-17, M-2000C, A-10A/C, F-86F, Christen Eagle II | glass close above and to the sides, bright |
| `framed` | Framed canopy | F-14A/B, F-4E, F-5E, MiG-21bis, MiG-19P, MiG-15bis, Mirage F1, AJS37, Su-25/25T, F-100D, L-39, C-101, MB-339, Hawk, Yak-52, A-4/A-6 class, AH-64D, Ka-50, Mi-24P (tandem armoured cockpits) | more metal frame, smaller glass |
| `helicopter` | Helicopter cabin | UH-1H, Mi-8MTV2, CH-47F, SA342, OH-58D | bigger cabin, longer decay, doors |
| `warbird` | Warbird | P-51D, TF-51D, P-47D, Bf-109K-4, Fw 190 A-8/D-9, Spitfire LF Mk IX, Mosquito FB Mk VI, F4U-1D, I-16 | small, bare metal behind the seat |
| `transport` | Transport flight deck | C-130J | large, lined |
| `generic` | Generic (default) | anything not in the table, unknown type | between bubble and framed |

The type name → family table is one header (`native/binaural/include/cockpit_space.h`, CORE): exact names first,
then case-insensitive prefixes (e.g. `F-16`, `FA-18`, `F-15`, `MiG-29`, `Su-2`, `Mi-8`, `SA342`, `P-51`, `FW-190`,
`Spitfire`), else `generic`. Names to start from (unit type names as DCS reports them, [A] for the ones not seen in a
log yet; CORE checks each against the type names in `<DCS>\Mods\aircraft\*\entry.lua` and the aircraft Lua it can
read statically, and lists the unverified ones in the header comment): `F-16C_50`, `FA-18C_hornet`, `F-15C`,
`F-15ESE`, `AV8BNA`, `MiG-29A`, `MiG-29G`, `MiG-29S`, `Su-27`, `J-11A`, `Su-33`, `JF-17`, `M-2000C`, `A-10A`, `A-10C`,
`A-10C_2`, `F-86F Sabre`, `Christen Eagle II`; `F-14A-135-GR`, `F-14B`, `F-4E-45MC`, `F-5E-3`, `MiG-21Bis`, `MiG-19P`,
`MiG-15bis`, `Mirage-F1CE`/`EE`/`BE`/`M`..., `AJS37`, `Su-25`, `Su-25T`, `F-100D`, `L-39C`, `L-39ZA`, `C-101EB`,
`C-101CC`, `MB-339A`, `MB-339APAN`, `Yak-52`, `AH-64D_BLK_II`, `Ka-50`, `Ka-50_3`, `Mi-24P`; `UH-1H`, `Mi-8MT`,
`CH-47Fbl1`, `SA342L`, `SA342M`, `SA342Mistral`, `SA342Minigun`, `OH58D`; `P-51D`, `P-51D-30-NA`, `TF-51D`,
`P-47D-30`, `P-47D-30bl1`, `P-47D-40`, `Bf-109K-4`, `FW-190A8`, `FW-190D9`, `SpitfireLFMkIX`, `SpitfireLFMkIXCW`,
`MosquitoFBMkVI`, `F4U-1D`, `F4U-1D_CW`, `I-16`; `C-130J-30`.

Built: checked on 2026-11-26 against `make_flyable(...)` in every `Mods\aircraft\*\entry.lua` of the 2.9.31 install
(read-only): every name above is there except `Mirage-F1M-EE` (kept, [A]); added the variants found there:
`F-14A-95-GR`, `F-14A-135-GR-Early`, `F-14BU`, `F-5E-3_FC`, `F-86F_FC`, `MiG-15bis_FC`, and `MiG-29 Fulcrum` ([A]: the
full-fidelity module's `flyable_ID`; the prefix `MiG-29` covers it either way). `Hawk` is not installed (prefix only).

The family the user picks in Advanced (`CockpitSpaceFamily`) overrides the table.

### 5.2 Presets (Normal strength)

Distances in metres from the head; `R` amplitude reflection; filters: LP = low-pass corner of the reflection, HP =
high-pass corner (glass). Early and late levels are energies relative to the direct sound of a voice at 0.6 m straight
ahead, after the 200 Hz send high-pass (so the broadband addition is smaller still).

| Family | left/right | front | back | up | down | up (canopy) R / filter | sides R / filter | front R / filter | back R / filter | down R / filter | early dB | late dB | RT60 low/high s | pre-delay ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bubble | 0.38 / 0.38 | 0.75 | 0.30 | 0.25 | 0.95 | 0.85 / HP 300 Hz | 0.75 / HP 250, LP 8 k | 0.70 / LP 5 k | 0.45 / LP 2 k | 0.50 / LP 3 k | -12 | -17 | 0.10 / 0.08 | 2 |
| framed | 0.35 / 0.35 | 0.70 | 0.30 | 0.28 | 0.95 | 0.75 / HP 250, LP 7 k | 0.70 / LP 6 k | 0.70 / LP 5 k | 0.45 / LP 2 k | 0.50 / LP 3 k | -12 | -17 | 0.12 / 0.09 | 2 |
| helicopter | 0.70 / 0.90 | 1.00 | 1.20 | 0.60 | 1.00 | 0.75 / HP 200, LP 7 k | 0.70 / LP 6 k | 0.75 / HP 200 | 0.60 / LP 4 k | 0.60 / LP 4 k | -11 | -14 | 0.25 / 0.18 | 5 |
| warbird | 0.35 / 0.35 | 0.80 | 0.50 | 0.25 | 0.90 | 0.80 / HP 300, LP 8 k | 0.75 / LP 6 k | 0.70 / LP 5 k | 0.80 / LP 4 k (bare metal) | 0.50 / LP 3 k | -12 | -16 | 0.14 / 0.10 | 3 |
| transport | 0.80 / 0.80 | 1.00 | 1.50 | 0.70 | 1.10 | 0.60 / HP 200, LP 6 k | 0.55 / LP 4 k | 0.70 / HP 200 | 0.50 / LP 3 k | 0.45 / LP 2.5 k | -13 | -15 | 0.25 / 0.20 | 6 |
| generic | 0.40 / 0.40 | 0.80 | 0.35 | 0.28 | 0.95 | 0.80 / HP 250 | 0.70 / LP 6 k | 0.70 / LP 5 k | 0.45 / LP 2 k | 0.50 / LP 3 k | -12 | -17 | 0.12 / 0.09 | 2 |

These are starting values from the acoustics of section 2, to be tuned only by the metrics (section 10) and the
listening test (section 11), never per aircraft.

### 5.3 Strength

| Level | early | late | RT60 | Purpose |
|---|---|---|---|---|
| Off (0) | - | - | - | today's dry binaural, bit-identical |
| Subtle (1, default) | -4 dB from Normal (-16) | -5 dB (-22) | x 0.9 | out of the head, barely heard as "room" |
| Normal (2) | table | table | x 1 | clearly a cockpit |
| Strong (3) | +3 dB (-9) | +4 dB (-13) | x 1.2 | for listeners who still hear sounds inside the head |

## 6. Which voices get it, level neutrality, interplay

### 6.1 Which voices

- **Owned voices only** (the binaural ones): cockpit and world 3D voices, mono and stereo waves. Their input to the
  room is exactly the core's input for that voice (the mid `M`, `MonoInput`), at the voice's HRTF gain (level trim,
  late-translation gain, PanLevel, loudness factor), weighted by the voice's HRTF weight `w ⋅ h` of that block (the
  ownership fade and the kill switch), so the room follows every fade of the direct path.
- **Not owned:** radio, intercom, GUI, every 2D sound and 2D context (never owned), voices over MaxObjects or refused
  (DCS's own panning, no room: they are the quietest), excluded contexts.
- **Point voices** (`q = 0`): early reflections and tail.
- **Wide voices near the head** (`kNear`, the cockpit beds kept wide): early reflections and tail from their mid; the
  side keeps DCS's own stereo around the binaural mid as now.
- **Diffuse beds** (the render side's diffuse detector: wind, rain, cabin noise): tail only (`kLateOnly`), at -3 dB:
  they have no direction for image sources and are already wide.
- **Through-canopy voices** (section 3.1): excitation point on the box, `throughDb`.

### 6.2 Level neutrality

The room adds energy; the user should hear more space, not more volume. Per voice the direct HRTF gain (and the kept
side of a wide voice) is multiplied by `d = 1 / √(1 + E_early + E_late)`, where `E_*` are that voice's own wet
energies relative to its direct sound, computed analytically by the pure function `RoomDirectGain` from the preset,
the wet weight and the voice's aircraft-frame position and class. Because the sends are high-passed at 200 Hz and DCS's
sounds are low-frequency heavy, `d` slightly over-compensates the broadband level; the metric of section 10 decides
whether `RoomDirectGain` uses the full energy or a fixed fraction of it (`kNeutralShare`, start 0.6). Typical values:
Subtle `d` ≈ -0.1 dB, Normal ≈ -0.3 dB, Strong ≈ -0.6 dB. `d` ramps with the core's gain ramp.
Built: `RoomDirectGain` evaluates the same model the Room runs (`src/room_model.h VoiceWetEnergy`: the voice's 8 taps
with the early normalisation, each tap's wall filters as their white-noise power gain, the send high-pass, the distance
law, `throughDb`, `kLateOnly`, the canopy's effect on the up taps, the late level and the decay), taking the filters'
power gains at 48 kHz (it has no sample rate; at 44.1 or 96 kHz the wet energy differs by under 0.1 dB). Cost: one
`ComputeTaps` and a few `exp`/`pow` per call, under 1 µs.

### 6.3 PanLevel, loudness factor, wide sounds

Sends are taken **after** PanLevel and the loudness factor (they are in `u.gain`), so a voice behind the pilot that
DCS plays at -6 dB excites the room 6 dB less, as it should: the direct-to-room balance per voice does not depend on
the direction. The kept side of a wide voice gets `d` too, so a wide voice's mid/side balance is unchanged.

## 7. Configuration, Live, UI

- `[Audio] CockpitSpace` 0..3 (Off, Subtle, Normal, Strong), default 1, **main**, Live. Effective only with
  `Binaural=1`.
- `[Audio] CockpitSpaceFamily` `auto` | `bubble` | `framed` | `helicopter` | `warbird` | `transport` | `generic`,
  default `auto`, advanced, Live.
- `[Audio] CockpitSpaceCanopy` 0/1, default 1, advanced, Live: the space opens with the canopy.
- `[Audio] LogVoices` (existing, diagnostics): also writes the room columns and `audio_space.csv` (section 8.6).
- `[AudioTest]` (listening test only, written by `DcsVr.Cli audio-test` as a live write of the applied file; removed
  when the test stops, and by Launch DCS, any Live write of the file and Back to stock): `Active` 0/1, `Seed` (uint32,
  informational), `Trials` (count, informational), `Level` 0..3 (optional). With `Active=1` and `Level` present the
  module plays that level instead of `CockpitSpace`, crossfaded like a live change, and logs nothing that names it beyond
  its usual `audio: cockpit space:` line; without `Level`, or with `Active` absent, the pilot's `CockpitSpace` plays. No
  test hotkeys: Part B's trials and answers go through the test mission's F10 radio menu (section 11), so the test adds
  no key of ours.

UI (Audio page, under "3D headphone audio", disabled while it is off):

- **Cockpit space** — segmented Off / Subtle / Normal / Strong, tag EXPERIMENTAL.
  Description: "Puts cockpit sounds around you instead of inside your head."
- Advanced: **Cockpit type** — Automatic / Bubble canopy / Framed canopy / Helicopter cabin / Warbird / Transport
  flight deck / Generic. Description: "Automatic picks it from the aircraft you fly."
- Advanced: **Open canopy opens the space** — toggle.
- Status line when the module reports it (from the log line of section 8.5): "In the cockpit: F-16C_50, bubble
  canopy." (no line when unknown).

No consent gate, no warning.

## 8. Interfaces

### 8.1 Core: `native/binaural/include/binaural_room.h` (new)

```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include "binaural_core.h"
#include "cockpit_space.h"   // Surface, RoomParams, Family, presets (pure)

namespace binaural {

using cockpit_space::RoomParams;

constexpr std::uint32_t kRoomBuses = 14;            // 6 axes + 8 cube corners, aircraft frame
constexpr std::uint32_t kRoomTaps = 8;              // 6 first-order walls + 2 canopy scatter (up-left, up-right)
constexpr std::uint32_t kRoomFdnLines = 8;
constexpr float kRoomMaxTapDelayMs = 12.0f;          // per-voice delay line: 12 ms at 96 kHz (next power of two)
constexpr float kRoomBusDistanceM = 2.0f;            // buses are far-field sources (kNoNearField)
constexpr float kRoomParamsCrossfadeMs = 100.0f;     // SetParams: old set -> new set
constexpr float kRoomRefDistanceM = 0.6f;

namespace room_flags {
constexpr std::uint32_t kReset = 1u;          // a new voice in this slot (tap lines and filter states cleared)
constexpr std::uint32_t kThroughCanopy = 2u;  // outside voice heard through the canopy: excitation on the box, throughDb
constexpr std::uint32_t kLateOnly = 4u;       // diffuse bed: no early taps, late send -3 dB
}  // namespace room_flags

// Per block, set by the audio module at BeginBlock (audio thread). Every value is the target at the block's END;
// the room ramps from the previous block's values across this block.
struct RoomFrame {
    float aircraftToHead[4];   // unit quaternion (x, y, z, w): aircraft frame -> head frame (x fwd, y up, z right)
    bool frameValid;           // false: early reflections fade out (late only), the rotation is ignored
    float wet;                 // 0..1: inside x (module on, kill switch off); strength is in RoomParams
    float canopyOpen;          // 0..1 (0 when unknown or CockpitSpaceCanopy=0)
    bool early;                // false: early reflections fade out (CPU back-off step 1)
};

// Per owned voice per block (audio thread), the target at the block's end.
struct RoomSource {
    float posAircraft[3];      // aircraft frame, head at origin, metres (module: conj(C) applied to the corrected pos)
    float send;                // linear: the voice's HRTF gain (u.gain without RoomDirectGain) x the distance law
    std::uint32_t flags;       // room_flags
};

struct RoomStats {
    std::uint64_t blocks, sourcesAdded, earlyOffBlocks, frameInvalidBlocks, nonFinite, rejected, idleBlocks;
    std::uint64_t lastBlockNs, worstBlockNs;
    float lastWet, lastCanopy;
};

class Room {
public:
    Room();
    ~Room();
    Room(const Room&) = delete;
    Room& operator=(const Room&) = delete;

    // ---- control thread ---------------------------------------------------------------------------------------
    // Allocates everything: the bus Renderer (kRoomBuses slots, near field off), 14 bus buffers, per-slot tap lines
    // (maxSources x next pow2 of 12 ms), the FDN, two parameter sets. sampleRate as binaural::Config.
    bool Init(std::uint32_t sampleRate, std::uint32_t maxSources, char* error, std::size_t errorCapacity);
    void SetHrtf(const HrtfSet* set);           // the voices' set; also recomputes the diffuse-field EQ (may allocate)
    const HrtfSet* TakeRetiredHrtf();           // as Renderer::TakeRetiredHrtf
    void SetHeadRadius(float metres);           // ITD radius for the buses, as Renderer::SetHeadRadius
    void SetParams(const RoomParams& p);        // published atomically; crossfaded over kRoomParamsCrossfadeMs

    // ---- audio thread (same rules as Renderer: no allocation, locks, logging, I/O; bounded) ---------------------
    void BeginBlock(std::uint32_t frames, const RoomFrame& f);
    // Adds one voice's sends. `in` is the same MonoInput the voice's Render call got; w0..w1 its HRTF weight
    // (ownership fade x kill switch), linear across the call. False (nothing added, counted) when rejected.
    bool AddSource(std::uint32_t slot, const RoomSource& s, const MonoInput& in, std::uint32_t frames, float w0, float w1);
    void ResetSource(std::uint32_t slot);
    // After every AddSource of the block: renders the buses (the internal Renderer) and the tail, ADDS to outLR.
    void Render(float* outLR, std::uint32_t frames);
    void EndBlock();
    bool Idle() const;                           // wet 0, every tap line and the tail silent: the module may skip the room

    // ---- any thread -------------------------------------------------------------------------------------------
    RoomStats GetStats() const;
    void ResetStats();
};

// Pure (any thread): the direct gain d of section 6.2 for a voice, from the parameters, the wet weight and the
// voice's aircraft-frame position and flags. 1 when wet = 0.
float RoomDirectGain(const RoomParams& p, float wet, float canopyOpen, bool early, const float posAircraft[3],
                     std::uint32_t flags);
// Pure: the send's distance law of section 4.2 (clamp(r / kRoomRefDistanceM, 0.5, 2) inside the box, 1 outside).
float RoomSendDistanceGain(const RoomParams& p, const float posAircraft[3], std::uint32_t flags);

}  // namespace binaural
```

`native/binaural/include/cockpit_space.h` (new, pure, header-only, C++17; module, harness and contract test):

```cpp
namespace cockpit_space {
enum class Family : std::uint8_t { Generic = 0, Bubble, Framed, Helicopter, Warbird, Transport, Count };
enum Surface : std::uint8_t { kLeft, kRight, kFront, kBack, kUp, kDown, kSurfaces };
struct SurfaceParams { float distanceM, reflection, lowpassHz, highpassHz; };  // 0 Hz: no filter
struct RoomParams {
    Family family;
    int strength;                       // 1..3 (0 is "no room": never published)
    SurfaceParams surface[kSurfaces];
    float earlyDb, lateDb;              // energies re direct at kRoomRefDistanceM, after the send high-pass
    float rt60LowS, rt60HighS, crossoverHz, preDelayMs;
    float sendHpHz;                     // 200
    float throughDb;                    // 0
    float neutralShare;                 // kNeutralShare of section 6.2 (0..1)
};
const wchar_t* FamilyKey(Family f);                       // L"bubble", ... (the ini values)
bool ParseFamily(const wchar_t* s, Family* f, bool* isAuto);
Family FamilyForType(const char* unitType);               // the table of section 5.1 (exact, then prefixes)
bool Preset(Family f, int strength, RoomParams* out);     // section 5.2 x 5.3
}
```

`binaural_config.h` additions (CORE; one table, the module and the app tests read it):

```cpp
{L"CockpitSpace", KeyType::Int, 1, 0, 3, "main"},                 // "Cockpit space": Off, Subtle, Normal, Strong
{L"CockpitSpaceFamily", KeyType::Text, 0, 0, 0, "advanced"},      // "auto" or a family key
{L"CockpitSpaceCanopy", KeyType::Bool, 1, 0, 1, "advanced"},      // the space opens with the canopy
// AudioConfig: int cockpitSpace = 1; wchar_t cockpitSpaceFamily[16] = L"auto"; bool cockpitSpaceCanopy = true;
constexpr const wchar_t* kTestSection = L"AudioTest";             // Active, Seed, Trials (listening test, section 11)
```

Built (CORE), additions to the API above, nothing removed or renamed:
- `binaural_room.h`: `kRoomWetSlewS` (0.05 s), `kRoomEarlyFadeMs` (50), `kRoomCanopyTauS` (0.3 s),
  `kRoomSpeedOfSound` (343 m/s); `Room` has a private `Impl` (and a `RoomAccess` friend for the harness's probes).
- `Room::ResetSource` lets the slot's last reflections play out: the next blocks run its taps on silence for
  `kRoomMaxTapDelayMs` (plus 64 frames), then clear it; a voice added again without `kReset` continues, `kReset`
  clears at once. A slot not added in a block (without `ResetSource`) is treated the same way; one missing for more
  than one block (the module skipped the room) is cleared.
- `cockpit_space.h`: `kNeutralShare`, `kStrengthOff/Subtle/Normal/Strong`, `FamilyName` (the UI name, for log lines),
  `Sanitize` (the clamps `SetParams` applies: distances 0.1-3 m, reflection 0-0.95, levels -40-0 dB, RT60 0.03-1 s,
  pre-delay 0-10 ms, send high-pass 20-1000 Hz, through -40-+6 dB, share 0-1), the tables `kTypes` and `kPrefixes`.
- `binaural_config.h`: `kTestKeys` (`Active`, `Seed`, `Trials`), `kTestHotkeys` (the eight test hotkeys),
  `IsValidCockpitSpaceFamily`; it now includes `cockpit_space.h`.
- The `[Audio]` table has 13 keys: the module's test that every key is read (`audio_test.h`) fails until MODULE reads
  the three new ones, as planned (merge order CORE, MODULE, APP); the payload compiles BinauralCore from its sources,
  so MODULE adds `src/room.cpp` to `DcsQvBinauralCore` in `native/dcsqvcull/CMakeLists.txt`.

### 8.2 Module data per block

Sim thread (PanWrap, ed mutex held), new reads per 3D sound (offsets of section 3):

| Read | Where | Used for |
|---|---|---|
| `ctx+0xe1` (u8) of every non-2D context on the bound output | `BeginFrame` | inside = any is 1 |
| `sound+0x1a8` (u8) | `PanVoice` | through-canopy class (with the context's flag) |
| `sound+0x1b0` (float) | `PanVoice` | DCS's canopy damping applied (log/CSV only) |
| `sound+0xe8` (double4) | `PanVoice`, cockpit-context sounds | `C_dcs` (section 4.4) |
| `ctx+0x58`, `ctx+0x5c`, `ctx+0x60` (float) | `BeginFrame` | the listener-parameter probe (section 8.5) |

`MetaData` (audio_voice.h) gets no aircraft-frame position: the render side computes it from the head-frame position
and `C_now`, so the late correction is applied once, to both. New `meta_flags`: `kThroughCanopy = 128`,
`kCockpitCtx = 256`; new field `float cockpitGain` (sound+0x1b0 when applied, else 1).

New seqlock block `SpaceState` (new `audio_space.h`, sim thread writes once per frame, render thread reads per block).
As built, the canopy and the family are not in it (the control thread publishes them with the preset, `audr::SpaceCtl`);
it carries what the sim reads:

```cpp
struct SpaceData {
    uint32_t frame;          // SND_process frame
    uint32_t flags;          // kInside, kFrameValid, kFrameFromSound, kInsideKnown, kPoseValid, kReads
    float c[4];              // C_dcs: aircraft -> listener frame (x, y, z, w)
    float qUsed[4];          // head pose DCS used for it (as MetaData)
    uint64_t poseBase;
    int64_t frameQpc;        // when c was taken
    uint8_t cockpitCtx;      // context id voted as cockpit (8 unknown)
    uint8_t worldCtx;        // the context the listener probe read (8 none)
    float spreadDeg;         // the last frame's spread of C between the cockpit sounds
    uint32_t unreliable;     // frames whose C was refused
    float lgGain, lgPitch, lgLowpass;  // the listener probe
};
```

Render thread per armed block: read `SpaceData`; `C_now = R_late ⋅ C_dcs` (or `C_dcs`); `RoomFrame{C_now, valid,
wet = inside ⋅ (CockpitSpace > 0) ⋅ blk.on, canopy, early = !backoffEarly}`; `Room::BeginBlock`. In `ProcessVoice`,
after the voice's `CoreRender`: `posA = rotate(conj(C_now), u.position)`; `RoomSource{posA, send = u.gain ⋅
RoomSendDistanceGain(...), flags}`; `Room::AddSource(slot, src, in, frames, cw0, cw1)`; `u.gain *= RoomDirectGain(...)`
before the `CoreRender` call (and `gS` of the side). After the mix: `Room::Render(out, frames)` inside the same
`MxcsrScope` and `dspTicks` measurement, then `Room::EndBlock`. `Room::ResetSource` wherever `CoreResetSource` is
called. Skip all room calls while `Room::Idle()` and the wet target is 0.

### 8.3 Lua bridge (loader exports, `DcsQvCull.lua`)

| Export | Lua call | When |
|---|---|---|
| `DcsQvCull_AudioWanted() -> bool` | gate | false while `Binaural=0` or `CockpitSpace=0`: nothing below runs, nothing allocated |
| `DcsQvCull_AudioAircraft(typeName: string)` | `DCS.getPlayerUnitType()`, else `Export.LoGetSelfData().Name` | `onSimulationStart`, `onPlayerChangeSlot` (own), and every 2 s of model time if it changed (cheap string compare in Lua) |
| `DcsQvCull_AudioState(canopy: number or -1, source: 1 mech / 2 arg38 / 0 none, inside: number -1/0/1)` | `Export.LoGetMechInfo().canopy.value`, else `LoGetAircraftDrawArgumentValue(38)`, else -1; inside from the existing `inCockpit` check (-1 when its exports are missing) | every 0.2 s (wall clock, like `inCockpit`) while wanted |

Every call in `pcall`; nil exports (MP restrictions) give -1. The type is sent when it changed (checked every 2 s) and
again every 10 s, so a hot-reloaded payload hears it too. The payload copies the values into atomics read by the
control thread, which resolves the family and publishes the preset, the canopy and the camera check for the render side
(`audr::SpaceCtl`); the sim thread publishes what it reads from ed_sound in `SpaceState` (`audio_space.h`).

As built (MODULE, `feat/cspace-module`): the payload exports behind them are `DcsQvPayload_AudioWanted()`,
`DcsQvPayload_AudioAircraft(const char*)` and `DcsQvPayload_AudioState(double canopy, int source, int inside)`
(`loader_api.h`). The payload and `DcsQvCullAudioE2ETests` link the core's Room (`native/binaural/src/room.cpp`,
`DCSQV_HAVE_ROOM=1`); `DcsQvCullTests` links a recording fake Room (`test/audio_fake_core.cpp`). The module's
stand-in headers were removed at integration. The module skips every room call while the block-end wet weight and the previous one are 0 and `Room::Idle()` is
true; CORE's `Idle()` must therefore become true once the wet weight is 0 and the tail has decayed, whatever the
sends (voices keep playing in external views).

### 8.4 Control thread

- On config load / Live change / family change / aircraft change: `cockpit_space::Preset(family, strength)` →
  `Room::SetParams`; `CockpitSpace=0` → wet 0 (no SetParams).
- On HRTF change: `Room::SetHrtf` with the voices' new set; retire as for voices (both renderers must release a set
  before it is freed).
- Budget ladder (section 9).
- `[AudioTest] Active`/`Level` (section 7): the effective level is `Level` while the test is on, else `CockpitSpace`.

### 8.5 Log lines (`DcsQvCull.log`; the app reads the first two)

```
audio: cockpit space: <Off|Subtle|Normal|Strong> (<family> <auto|setting>, aircraft <type|unknown>)
audio: cockpit space: inside <0|1>, canopy <0.00..1.00|unknown> (<mech|arg38|none>)          (on change; canopy steps >= 0.1)
audio: cockpit space: frame from <sound|none>, cockpit ctx <i>, spread <deg> deg, unreliable frames <n>   (on change, and once per minute while unreliable)
audio: cockpit space: early reflections off (CPU budget) | early reflections on again
audio: ctx <i> listener gain=<f> pitch=<f> lowpass=<f>                                       (probe: at enable and on any change > 1 %)
audio: running, ... ; space <level> <family> inside <0|1> wet voices <n> early <on|off> room p99 <ms> ms, direct <dB> dB; head pose <on|off|unused>
audio: cockpit space: ed_sound's cockpit fields not found (<why>); <what is off>               (at install, only when a signature row is missing)
```

As built, the space's part of the running line sits before `; head pose ...` (the app's parser expects the line to end
with it), and reads `space Off` while the space is off.

The probe line answers [A] of section 3.1: open and close the canopy once in the first live flight and see whether the
world context's listener gain or low-pass moves with it.

### 8.6 CSV

`audio_voices.csv` (LogVoices), columns appended after `flags`: `room,send_db,through,cgain,ax,ay,az,dcomp_db`
(`room` 1 when sent; `send_db` the send gain; `through` 1 through-canopy; `cgain` DCS's `sound+0x1b0` when applied,
empty otherwise; `ax..az` aircraft-frame position; `dcomp_db` 20 log10 d).

`audio_space.csv` (new, LogVoices, one row per second):
`time,level,family,type,inside,canopy,canopy_src,frame_src,cockpit_ctx,yaw_deg,pitch_deg,roll_deg,spread_deg,wet,early,wet_voices,room_p50_us,room_p99_us,dcomp_mean_db,lg_gain,lg_lowpass`
(`yaw..roll`: the head against the aircraft from `C_now`; `lg_*`: the world context's listener gain and low-pass).

## 9. CPU budget and real-time rules

- Expected cost (48 kHz, 480 frames, AVX2), to be measured by C9: taps 48 voices x 8 taps x (1 read + filter + 3
  adds) x 480 ≈ 0.6 M flops; 14 bus HRTF renders ≈ 14/48 of today's 0.21 ms ≈ 0.06 ms; FDN ≈ 0.02 ms. **Target: room
  ≤ 0.15 ms p99 per 10 ms block** next to today's 0.26-0.40 ms p99 (budget 2.5 ms at 25 %).
- Measured (CORE, C9: thread CPU time per 480-frame block at 48 kHz, best of 5 x 1000 blocks, half the voices in the
  cockpit and half through the canopy, the room alone):

  | Kernels | Voices | Static p50 / p99 | Head turning 90°/s p50 / p99 | AddSource / Render (medians) |
  |---|---|---|---|---|
  | AVX2/FMA | 48 | 0.145 / 0.187 ms | 0.157 / 0.210 ms | 0.072 / 0.073-0.085 ms |
  | AVX2/FMA | 64 | 0.168 / 0.201 ms | 0.181 / 0.239 ms | 0.095 / 0.073-0.085 ms |
  | SSE2 | 48 | 0.202 / 0.242 ms | 0.229 / 0.310 ms | 0.090 / 0.111-0.138 ms |
  | SSE2 | 64 | 0.231 / 0.279 ms | 0.258 / 0.324 ms | 0.119 / 0.111-0.138 ms |
  | AVX2/FMA, early off (back-off step 1) | 48 | 0.043 / 0.058 ms | | |

  The 0.15 ms p99 target is missed by about 0.04-0.06 ms with AVX2 (p50 meets it at 48 voices static); SSE2 meets its
  0.30 ms only static. Render's fixed part is about 0.04 ms for the 14 bus HRTF sources (AVX2; 0.08 SSE2), 0.015 ms
  for the pair filters and the bus high-pass, 0.015 ms for the tail; AddSource is 1.5 µs per voice. The harness gates
  at p99 0.30 ms (AVX2) / 0.50 ms (SSE2) so a regression fails, and prints the target as met or MISSED. With the
  module's 2.5 ms budget (25 % of the block) and today's 0.26-0.40 ms for the voices the total stays under 0.7 ms.
  `RoomDirectGain` + `RoomSendDistanceGain`: 0.17 µs per voice (0.35 µs with the canopy partly open).
- Room time is inside `dspTicks` (the module's budget measure).
- Back-off ladder: over budget (> 10 % of the blocks of the last second) → **step 1: early reflections off** (late
  only, logged) → then the existing MaxObjects steps. Recovery in reverse: MaxObjects back to the setting, then after
  10 s under half the budget the early reflections come back.
- ED overruns (3 in 10 s) turn binaural off as today; the room goes with it.
- Audio thread rules (both builders): no allocation, locks, logging, file I/O or waits; bounded work (frames x
  (voices x taps + buses + FDN)); FTZ/DAZ inside our DSP only; non-finite input or state resets that voice's tap line
  (counted, nothing added); the FDN is reset on a non-finite state; every parameter change ramps (section 4.5).

## 10. Validation: objective metrics

A CORE tool `binaural_room_render.exe` (in `native/binaural/test`, built with the harness, no DCS) renders the direct
path (Renderer) and the room (Room) for a unit impulse and for test signals, writing WAVs and a JSON of metrics. All
metrics per family x strength x source set (cockpit: 0.45 m above-front, 0.5 m left console, 0.5 m right console,
0.8 m front panel, 0.35 m behind; outside: 20 m at 0°, 90°, 180°) x head yaw 0°, ±45°, ±90°.

| # | Metric | How | Pass |
|---|---|---|---|
| R1 | IACC early (direct + 0-5 ms) and late (5 ms to the end; also the standard 0-80 / 80- windows), 500 Hz-4 kHz octave bands | max of the normalised interaural cross-correlation over ±1 ms | late IACC ≤ 0.3 above 500 Hz; early IACC of frontal/median sources lower than Off by ≥ 0.05 at Normal |
| R2 | DRR | direct = the HRTF window (first 2.5 ms after onset of the Off render, same samples), rest = room | Subtle +13..+19 dB, Normal +9..+15, Strong +6..+11 (1-4 kHz) for the 0.6 m front source |
| R3 | EDT and T20 | Schroeder backward integration of the wet IR per octave (500 Hz-4 kHz), each ear | within ±20 % of the preset RT60 (x strength x canopy) |
| R4 | Level neutrality | loudness (ITU-R BS.1770 K-weighted, both ears' power summed) of pink noise and of a mix of DCS-like signals (low-heavy engine noise, broadband wind, clicks) per source, Off vs each strength | within ±0.5 dB per strength, mean over sources; within ±1 dB per source |
| R5 | Rotation | yaw the head by θ: the early part of the IR at yaw θ equals the IR at 0 rendered with the room turned by -θ; bus head-frame directions follow -θ within 1° | correlation ≥ 0.98; direction error ≤ 1° |
| R6 | Click-free changes | strength, family, canopy 0→1 in 1 s, inside→outside, early off/on, kill switch, HRTF change, frame invalid→valid: sample-to-sample step and 2-20 kHz splatter against the steady render | no step above the steady signal's own maximum step; splatter ≤ -60 dB re signal |
| R7 | Off is exact | `CockpitSpace=0` vs a build without the room (module e2e) | bit-identical |
| R8 | CPU | thread CPU time per block, 48 voices + room, AVX2 and SSE2, static and head turning 90°/s | room p99 ≤ 0.15 ms AVX2, ≤ 0.30 ms SSE2 |
| R9 | Colouration | spectrum of direct+early vs direct (1/3 octave, 200 Hz-12 kHz) for each cockpit source | ripple ≤ ±2 dB at Subtle, ±3 dB at Normal |
| R10 | Stability | 10 min of white noise into the FDN at the longest RT60, Strong, canopy closed | no growth; energy decay matches RT60; no denormal slow-down (block time flat) |

Live (only on the user's go, never by an agent unasked), with LogVoices on: `audio_space.csv` shows inside 1 in the
cockpit and 0 in F2, the family and type, frame from sound with spread under 1°, yaw following the head; the
`audio: running` line's room p99 under 0.15 ms; kill switch on/off loudness of the cockpit within ±1 dB (existing
hrtf check) with the space at Subtle and Normal; the listener probe answers [A] of section 3.1.

### 10.1 Live validation plan

Two steps, both only on the user's go. Nothing of it has run yet.

**Step 1: the validation-08 harness, hrtf session (unattended, about 6 minutes per arm).** The harness in
`artifacts\validation-08` (`go-v8.ps1 -Sessions hrtf -NoRegress`) already flies `qvbench_v8_hrtf.miz` with 3D audio on,
LogVoices and Follow the head: WASAPI loopback FLY+6..+172 s, a head sweep, the kill switch at +85 / +150 s, analysis
by `audio\hrtf_audio.py` (run 20261125-1351: PASS, CPU p99 median 0.37 ms, max 0.40, budget 2.5 ms, 41 sounds). For
the space, three arms in one start, each with the profile override `cockpitSpace` (the app's `CockpitSpace`): 0 (the
baseline, equal to 20261125-1351), 1 (Subtle), 2 (Normal). The hrtf arm of `scenarios\make_scenarios.py` needs that
override and the arm name per level; `hrtf_audio.py` needs to read `audio_space.csv`. Pass per arm:
- the existing hrtf checks unchanged (owned voices, radio untouched, listener against the pose, kill switch on/off
  loudness within ±1 dB, no overrun);
- the `audio: running` line: `space Subtle|Normal bubble inside 1 ... early on`, its `room p99` under 0.15 ms (0.30 ms is
  the ceiling of the offline gates) and the module's p99 within 0.2 ms of the Off arm;
- `audio_space.csv`: `inside 1` while in the cockpit, `type` the aircraft's, `frame_src` sound with `spread_deg` under
  1°, `yaw_deg` following the head sweep (the [A] of section 3.2), `wet 1`, `early 1`, `dcomp_mean_db` between -0.1 and
  -0.4 dB;
- the `audio: ctx N listener gain=... lowpass=...` probe line logged once (the [A] of section 3.1, read, not judged);
- no click in the loopback at the kill switch (largest sample step against the steady signal's own, as offline).
A fourth arm with an F2 view switch in the schedule checks inside 0 and the room idle (`wet 0`, no room time).

**Step 2: the pilot's listening test (in the headset, about 10 minutes, section 11).** Only after step 1 passes. The
kit is `tools/listening-test/` (README): Part A offline stimuli made with `binaural_room_render.exe` and the pilot's own
HRTF set (DCS closed), Part B in DCS with the test mission made by `DcsVr.Cli audio-test mission` (the F-16C Cold and
Dark copy under `artifacts\cspace-listening\`, not committed) and the watcher `part-b.ps1`, results and `summary.md`
under `artifacts\listening\<date>\`. Decides: Subtle as the default (section 11's rule), the R1 shortfall of median
sources (section 13.1), colouration ("tin can") in the free comment.

## 11. Listening test (pilot, in the headset, about 10 minutes)

Blind, short, two parts. The kit (APP, `tools/listening-test/`) prepares everything; the pilot needs no screen during
the trials; answers are single keys, each confirmed by a short tick.

**Part A, offline stimuli (about 5 minutes, DCS not running).** The kit renders, with `binaural_room_render.exe` and
the user's own HRTF setting (the `[Audio] Hrtf` set; reading only), 1.5 s stimuli: a pink-noise burst train and a
"switch click + hum" complex (alternating by direction, the same signal at the three levels of a direction) at 8
directions at 0.5 m = cockpit distance: 0°, ±45°, ±135°, 180° azimuth (front/back pairs; ±90° has no front/back answer
and is left out), 45° up front and 30° down front, each as Off / Subtle / Normal of the `generic` family. Played through
the headset's headphones by the kit (Windows `SoundPlayer`, no new dependency) in a random order from the session seed
(`seed.txt`); the runner reads only opaque ids (`trials.json`), the unblinding is `key.json`.

- A1 (24 trials, about 3 minutes): after each stimulus, **externalisation 0-4** (0 inside the head, 1 at the head's
  surface, 2 just outside, 3 clearly outside, 4 where the source should be) with the number row, then **front or
  back** with ↑ / ↓.
- A2 (8 trials, about 2 minutes): A/B/X between Off and Subtle on random directions: A, B, X play in turn; key 1 = X
  is A, 2 = X is B.

**Part B, in DCS (about 5 minutes; the pilot starts DCS and flies the test mission, cold on the ramp of any
aircraft).** No key of ours: the kit makes a test mission from any mission DCS saved (`audio-test mission`, a copy next
to the results; nothing is written into DCS or Saved Games by the kit itself): one MISSION START trigger runs
`cockpit_space_test.lua` (DO SCRIPT FILE, as DCS's own missions do), which adds an F10 radio menu (Other > Cockpit space
test) and writes every choice to dcs.log as a `CSPACE ...` line (`env.info`). The kit's watcher (`audio-test watch`)
follows dcs.log, writes `[AudioTest] Active=1, Seed, Trials=12` when it starts and `Level` for each trial (section 7),
and removes `[AudioTest]` when the mission's End the test is chosen or on Ctrl+C. Each choice that changes the sound is
followed by a tick about 3 s later (time for the log line, the watcher and the module's one-second ini read):
- **Next cockpit trial** (9 trials, Off / Subtle / Normal in blocks of three from the seed, never shown or logged as a
  name): the pilot flips switches on the left console, the right console, the front panel and overhead, turns the head,
  and picks **Rate the cockpit trial** 0..4.
- **Next outside sound** (16): an explosion (`trigger.action.explosion`, power 3) 150 m from the aircraft, 15 m above the
  ground, at a bearing from two shuffled passes over 8 bearings relative to the nose, 4 s after the choice; Off and
  Subtle in blocks of two. The pilot looks straight ahead and picks **The outside sound came from** (8 directions):
  front/back reversals and the angle error for a world sound entering through the canopy.
- **A/B/X** (3 trials): Play A (Off), Play B (Subtle), Play X (one of them, from the seed), then X is A / X is B.
Mission lines: `CSPACE start`, `trial <n>`, `rate <n> <k>`, `outside <n> az <deg>`, `heard <n> <deg>`, `abxtrial <n>`,
`abx <n> <A|B|X>`, `abxanswer <n> <A|B>`, `end`. The watcher copies them to `part-b.log` in the results folder (dcs.log
starts anew with DCS); the analysis unblinds from the seed.

**Analysis (kit, automatic):** per condition: median externalisation and the share of ratings ≥ 3; front/back
reversal rate (A1 and Part B's outside sounds); Wilcoxon signed-rank (exact, two-sided) Off vs Subtle and Subtle vs
Normal (paired by direction in A1 and by block in Part B); A/B/X
correct count with its binomial p (8 trials: 7 correct p = 0.035). **Accept Subtle as default** when it rates higher
than Off (median +1 or more, p < 0.05 over A1+B) and its reversals are not worse than Off's; prefer Normal as default
only if it rates higher than Subtle and nobody reports colouration ("tin can") in the free comment.

## 12. Task split and file ownership

Branches from `dev/next` (each builder in its own worktree under `.claude\worktrees\`, build inputs copied from
`integ-next`, never junctions): `feat/cspace-core`, `feat/cspace-module`, `feat/cspace-app`; integration
`feat/cspace` (merge core, then module, then app). This document is the contract: an interface change updates this
file and the header in the same commit. Where two parts touch one file, the owner is named; others only append.

### CORE — `native/binaural` (branch `feat/cspace-core`)

- New `include/binaural_room.h`, `include/cockpit_space.h` exactly as section 8.1; `include/binaural_config.h` new keys
  and `AudioConfig` fields (owner).
- New `src/room.cpp` (Room: tap lines, bus panning, bus Renderer, FDN, coherence mixer, diffuse EQ, params crossfade),
  `src/room_fdn.h`; SSE2 scalar paths first, AVX2 only if C9 needs it (then only in `kernels_avx2.cpp`'s namespace).
- `RoomDirectGain`, `RoomSendDistanceGain`, `Preset`, `FamilyForType` (table of section 5.1; verify the names
  statically against `<DCS>\Mods\aircraft\*\entry.lua` and readable aircraft Lua, read-only; list unverified ones).
- `test/room_test.cpp` (tests C1-C14), `test/room_render.cpp` → `binaural_room_render.exe` (metrics R1-R6, R8-R10,
  WAV output for the kit, command line `--family --strength --source x,y,z --yaw --out <dir> --hrtf <file>`; with
  `--input <mono wav>` it renders that signal (direct path at strength 0, direct + room above) and writes
  `<dir>/render.wav`, stereo 48 kHz, which the kit plays: `CockpitSpaceTest.RenderArguments` builds this line),
  `test/contract_test.cpp` additions, `CMakeLists.txt` targets and ctest entries.
- Hand-off: library target unchanged (`binaural_core` gains `src/room.cpp`); the tool's command line for APP.

### MODULE — `native/dcsqvcull` audio + aircraft/canopy detection (branch `feat/cspace-module`)

- `src/audio_voice.h` (meta flags, `cockpitGain`), `src/audio_sim.h` (new reads of section 8.2, cockpit-context vote,
  `C_dcs`, through-canopy class), new `src/audio_space.h` (`SpaceState` seqlock, frame consistency, late rotation of
  `C`), `src/audio_render.h` (Room calls in `ProcessVoice` and the block, direct gain, budget ladder step 1),
  `src/audio_config.h` (new keys, `[AudioTest]`), `src/main/audio.inc` (Room lifetime next to the core, SetParams on
  changes, HRTF retire for both, log lines 8.5, CSV 8.6, listener probe, `[AudioTest] Level` as the effective level).
- Lua bridge: `src/loader.cpp`, `src/loader_api.h`, `src/main/exports.inc` (three exports of 8.3),
  `lua/DcsQvCull.lua` (callbacks, gated by `DcsQvCull_AudioWanted`).
- Tests: `test/audio_e2e_test.cpp`, `test/audio_fake_ed.h` (fake ctx `+0xe1`, `+0x58..+0x60`, sound `+0x1a8`,
  `+0x1b0`, `+0xe8`), `test/audio_fake_core.*` (a fake Room recording calls), `test/audio_rt_alloc.cpp`,
  `test/qv_fake_payload.cpp` (new exports); tests M1-M16.
- Docs: `docs/BINAURAL_AUDIO_HOOKS.md` new section 11 with the offsets of section 3 (owner); hooks byte checks for
  `sub_6f70`'s use of `+0xe1`/`+0x1a8` added to the enable checks (§6 there) so a DCS update that moves them turns the
  space's through-canopy class off (not binaural).

### APP — UI, config, docs, listening-test kit (branch `feat/cspace-app`)

- `src/DcsVr.Core/Models.cs` (`CockpitSpace` int 0..3 default 1, `CockpitSpaceFamily` "auto", `CockpitSpaceCanopy`
  true, `[WrittenWhenChanged]`), `src/DcsVr.Core/BinauralAudio.cs` (levels, family keys, labels, validation),
  `src/DcsVr.Core/ConfigurationWriters.cs` (`[Audio]` keys), `src/DcsVr.Core/LiveSettings.cs` (all three Live, only
  with a payload that has them), `src/DcsVr.Core/CockpitSpaceTest.cs` (`[AudioTest]`, orders, stimuli, the test
  mission, log lines, analysis) and `CockpitSpaceTestCli.cs`; `ControlService.SetAudioTest` (`[AudioTest]` as a live
  write; Launch DCS and Back to stock end it).
- `src/DcsVr.App/ui/app.js` (+ `smoke.js`): the control, Advanced rows, status line from the log line (8.5).
- CLI verb `audio-test start|level|stop|watch|stimuli|mission|analyze` in the app's CLI.
- `tools/listening-test/` (kit: `README.md`, `RATING_SHEET.md`, `make-stimuli.ps1` calling `binaural_room_render.exe`,
  `run-part-a.ps1`, `make-mission.ps1`, `part-b.ps1` (the watcher), `analyze.ps1`); results under
  `artifacts\listening\<date>\` and the test mission under `artifacts\cspace-listening\` (not committed).
- `tests/DcsVr.Tests/Program.cs` (tests P1-P10); `docs/USER_GUIDE.md` (Audio section); `docs/BINAURAL_AUDIO.md`
  (a short pointer to this document; owner of that file stays as before, append only).

## 13. Test lists

### CORE (ctest, `binaural_room` and `binaural_contract`)

- C1 contract: headers compile together; `RoomParams`/`RoomFrame`/`RoomSource` layouts; `Preset` for every family x
  1..3 is finite, all `R < 1`, RT60 > 0; `FamilyForType` on the table names, prefixes, unknown → Generic, empty/null.
- C2 image sources: a source at known positions gives the analytic delays and gains (±1 sample, ±0.01 dB).
- C3 outside source: excitation point on the box along its direction; through flag applies `throughDb`.
- C4 bus panning: energy-preserving (sum of squares = 1) for 10 000 random directions; a direction on a bus gives
  that bus only.
- C5 rotation: R5 of section 10.
- C6 FDN: impulse response decay matches RT60 per band (±10 %), stable at every preset x strength x canopy.
- C7 coherence: late IACC by band matches the two-band target (low ≥ 0.8 below 300 Hz, ≤ 0.2 above 1 kHz).
- C8 level: `RoomDirectGain` makes direct + wet energy equal to direct-only energy for white noise (±0.1 dB, with
  `neutralShare` = 1); R4 with the shipped `neutralShare`.
- C9 CPU: R8 (prints p50/p99, fails over the limits).
- C10 crossfades: R6.
- C11 real time: no allocation between BeginBlock and EndBlock (the harness's allocation hook), FTZ/DAZ restored,
  bounded time at 8192 frames and 64 sources.
- C12 non-finite: a NaN/Inf input or state resets that source / the FDN, nothing added, counted; output finite.
- C13 idle: wet 0 and silence for 3 x RT60 → `Idle()` true, Render adds nothing; a new send wakes it without a click.
- C14 HRTF swap: `SetHrtf` during sound crossfades the buses, retires the old set only once unused; diffuse EQ
  recomputed.

### 13.1 CORE results (2026-11-26, `scripts/build-binaural.ps1`, 7/7 ctest)

`binaural_room` (synthetic sphere) and `binaural_room_default_data` (SADIE II KU100) in `test/room_test.cpp`;
`binaural_room_test --cpu` repeats C9, `--long` runs R10 for 10 minutes, `--diag` prints the model's wet energies
against the rendered ones per source.

| Test | Result (synthetic; KU100 where run) | Design limit |
|---|---|---|
| C1 contract | headers, layouts (RoomParams 140 B, RoomFrame 32 B, RoomSource 20 B), presets, type table | pass |
| C2 image sources | 0.0000 samples, 0.0000 dB against the closed form | 1 sample, 0.01 dB |
| C3 outside sources | excitation on the box; throughDb -6: early and late -6.000 dB | |
| C4 VBAP | energy error 3e-7 over 10 000 directions, on a bus = that bus only | |
| C5 / R5 rotation | bus directions 0.000°; turned scene vs head-frame scene correlation 1.000 (head-locked room 0.13) | 1°, 0.98 |
| C6 FDN decay | T20 within 11.6 % of the band's RT60 range; 0 unstable lines over 6 x 3 x 3 | 10 % (gated 15 %) |
| C7 tail IACC | 100-300 Hz 0.99; 1-2 kHz 0.17, 2-4 kHz 0.26, 4-8 kHz 0.19 | >= 0.8; <= 0.2 (gated 0.3) |
| C8 level, white, share 1 | 0.6 m ahead: +0.02 / +0.08 / +0.20 dB (KU100 +0.06 / +0.16 / +0.35); 4 families worst 0.24 (KU100 0.45) | 0.1 (gated 0.5) |
| R4 loudness, share 0.7 | pink mean +0.11 / +0.20 / +0.35 dB, worst 0.80 (KU100 +0.42, worst 0.98); DCS-like -0.05 / -0.11 / -0.19 | 0.5 / 1.0 |
| R2 DRR 1-4 kHz | 15.1 / 10.8 / 7.4 dB (KU100 13.9 / 9.7 / 6.5) | 13-19 / 9-15 / 6-11 |
| R3 T20 | within 20 % of the preset per octave for all 5 cockpit sources (EDT 0.10-0.17 s, printed) | 20 % |
| R9 ripple | 1.9 / 2.7 / 3.3 dB (KU100 1.9 / 2.7 / 3.3) | 2 / 3 / - |
| R1 early IACC | lateral sources 0.93 → 0.88-0.90; median sources 1.00 → 0.98-0.99, front panel → 0.93 | median ≥ 0.05 lower: **not met** |
| R1 late IACC (5 ms-end) | 1 kHz 0.21-0.48, 2 kHz 0.33-0.35, 4 kHz 0.22 | <= 0.3: **not met** at 1-2 kHz |
| C9 / R8 CPU | section 9 (48 voices AVX2 p50 0.145, p99 0.19-0.21 ms) | p99 0.15: **missed** (gated 0.30) |
| C10 / R6 clicks | 15 transitions (strength, family, canopy at once and over 1 s, wet both ways, early off/on, kill switch, voice end, frame both ways, head 90°/s, source 1 m/s, HRTF change): worst 7.6e-4 (family) | 1e-3 |
| Block size | 480 vs random 1-2048: 1.6e-6 | 1e-5 |
| C11 real time | 64 sources x 8192 frames 2.7 ms; 0 allocations; MXCSR restored; bad calls refused | |
| C12 non-finite | NaN/Inf input, position, send refused and counted; NaN wet / rotation / canopy ignored | |
| C13 idle | idle 0.98 s after going outside (helicopter Strong), skipped blocks silent, wakes without a click | |
| C14 HRTF swap | old set retired once unused, diffuse EQ and calibrations recomputed | |
| R10 stability | 2 min (10 with `--long`) white noise at helicopter Strong: 0.000 dB spread, CPU flat | |

What the numbers mean for the design:
- **R1 median sources** cannot pass in a left/right-symmetric box at the early level of section 5: the reflections of
  a median source come in mirror pairs (decorrelated only above about 2 kHz by the per-tap offsets), and at -12 dB
  they move the early IACC by 0.01-0.02. Lateral sources and the front panel do drop. Settled by the listening test
  (A1 externalisation of frontal sources), or by an asymmetric box if it says so.
- **Late IACC (5 ms to the end)** includes the taps between 5 and 12 ms; the tail alone is C7. In 2-4 kHz a
  0.09 s decay leaves about 26 independent frequency regions per octave, so even a decorrelated pair measures
  about 0.2 plus the maximum over ±1 ms: 0.26 is at that floor.
- **C8 at share 1** is the incoherent energy model against the rendered sum: the taps' bus filters and taps of similar
  delay add partly in phase (more on the KU100). With the shipped `kNeutralShare` 0.7 the loudness (R4) is within
  0.5 dB on average for pink noise and DCS-like sound alike.

`binaural_room_render` (the kit's tool, `test/room_render.cpp`):

```
binaural_room_render --out <dir> [--name <base>] [--family bubble|framed|helicopter|warbird|transport|generic]
                     [--strength 0..3] [--source x,y,z]... [--yaw <deg>] [--canopy 0..1]
                     [--signal impulse|pink|pinkbursts|clickhum|dcs] [--seconds <s>] [--hrtf <file.dcshrtf>]
                     [--rate <Hz>] [--gain-db <dB>] [--seed <n>]
```

Writes `<dir>\<base>.wav` (16-bit PCM stereo, direct + room; `_direct.wav` and `_room.wav` too for `impulse`) and
`<dir>\<base>.json` (R1, R2, R3, R9 of the first source, R4 pink-noise loudness change at 48 kHz, clipped samples).
Strength 0 is Off (the direct path alone). A source outside the box is classed through the canopy. Default HRTF: the
bundled KU100 set, else the synthetic sphere. Exit 0 written, 1 bad arguments, 2 HRTF or write failure.

### MODULE (`native/dcsqvcull/test`)

- M1 Off is exact: `CockpitSpace=0` output bit-identical to the build without the room for the e2e scenes.
- M2 inside/outside from fake ctx `+0xe1`; wet ramps 0.3 s; F2-like toggles cause no click.
- M3 through-canopy class from `+0x1a8` and the context flag; `cockpitGain` logged.
- M4 frame: `C_dcs` from fake `+0xe8`; spread > 1° → unreliable, last good kept; no cockpit sound for 1 s and no pose
  → early off.
- M5 late rotation of the room: with a fake pose turning 90°/s, `C_now` follows within the late-correction limits.
- M6 sends follow the voice: ownership fade, kill switch, core refusal, voice end → `AddSource` weights match the HRTF
  weight; `ResetSource` with every `CoreResetSource`.
- M7 direct gain applied to the mid and the kept side of wide voices; diffuse voices late only.
- M8 level: e2e scene loudness with the space at Subtle/Normal within ±0.5 dB of Off (R4 through the module).
- M9 budget ladder: forced over-budget → early off first, then MaxObjects; recovery in reverse; log lines.
- M10 config Live: each key change applied at the next block, crossfaded; out-of-range values clamped and logged once.
- M11 family resolution: setting overrides the table; aircraft change via the Lua export re-publishes params.
- M12 Lua bridge: `qv_fake_payload` exports; `AudioWanted` false → the hook makes no Export call (counted).
- M13 HRTF change retires the set only after both the voices' Renderer and the Room released it.
- M14 real time: `audio_rt_alloc` covers the room path.
- M15 CSV/log: columns and lines of 8.5/8.6 exactly (header text compared).
- M16 test mode: `[AudioTest] Active=1` with `Level` plays that level (crossfaded, as a live change), without `Level`
  the user's; removing the section (or `Active=0`) restores the user's level; no hotkey is read for the test.

### APP (`tests/DcsVr.Tests`)

- P1 defaults: CockpitSpace 1, Family auto, Canopy 1; `[Audio]` written only when changed (`WrittenWhenChanged`).
- P2 key names and ranges equal `binaural_config.h`'s table (the existing table-parity test extended).
- P3 Live: all three keys Live, no restart prompt.
- P4 Back to stock removes `[AudioTest]` with `[Audio]`.
- P5 the `audio-test start|level|stop` verb writes and removes exactly `[AudioTest]`; the watcher sets each trial's
  level from the mission's log lines and removes the section at the end; the test mission gets one MISSION START
  trigger and its two resources and keeps everything else.
- P6 UI smoke: the control is disabled while 3D audio is off; labels and descriptions as section 7.
- P7 status line parsing of the log line (known type, unknown, setting).
- P8 kit: `make-stimuli.ps1` dry run builds the command lines; `analyze.ps1` on a fixture log/answers file gives the
  expected medians, reversal rates, Wilcoxon and binomial p.
- P9 profile round trip of the new properties (JSON).
- P10 personal-data check of the kit's outputs (no user paths in committed files).

### 13.2 Integration results (2026-11-26, `feat/cspace`: core, then module, then app)

The module's stand-in headers are gone; the payload links the real Room. `DcsQvCullAudioE2ETests` scenario 5 runs the
payload's audio module on the fake ed_sound image with the real BinauralCore and Room and the synthetic spherical head
(F-16C: bubble family, canopy closed, inside, the cockpit sounds carrying the aircraft frame). Scenarios 1-4 (the
direct path) run with `CockpitSpace=0`.

| Check | Result | Limit |
|---|---|---|
| R2 DRR 1-4 kHz, 0.6 m front (Subtle / Normal / Strong) | 15.1 / 10.7 / 7.1 dB (broadband 15.5 / 11.0 / 7.5) | 13-19 / 9-15 / 6-11 |
| R2 DRR 1-4 kHz, above-front 0.45 m | 18.2 / 13.8 / 10.3 dB | falls each step by ≥ 1 dB |
| R2 DRR 1-4 kHz, right console 0.5 m | 19.1 / 14.7 / 11.2 dB | falls each step by ≥ 1 dB |
| R1 IACC 1-4 kHz (Off → Subtle / Normal / Strong), front | 1.000 → 0.971 / 0.925 / 0.846 | never higher than Off; Strong lower |
| R1 IACC 1-4 kHz, above-front / right console | 1.000 → 0.986 / 0.965 / 0.928; 0.951 → 0.938 / 0.912 / 0.862 | as above |
| IACC 1-4 kHz of the room part alone | 0.19-0.42 | under 0.9 x the direct sound's |
| Direct gain d (module's figure; least-squares fit) | front -0.06 / -0.16 / -0.35 dB, fit equal within 0.01 dB | fit within 0.1 dB away from the canopy |
| Cockpit mix vs the kill switch (DCS's own output) | Off -0.32, Subtle -0.36, Normal -0.38, Strong -0.38 dB | ±1 dB |
| M8: cockpit mix vs Off | -0.04 / -0.06 / -0.06 dB | ±0.5 dB |
| R4 per sound vs Off (5 sounds) | worst -0.96 dB (a pure 700 Hz tone on the front panel at Strong); mean -0.10 / -0.16 / -0.20 dB | ±1 dB each, ±0.5 mean |
| Kill-switch halves vs DCS's own | 0.000 dB | 0.05 dB |
| R6: 13 live changes 0.4 s apart, head turning 90°/s | largest second difference 5.1e-4 (at the kill switch), steady 2.6e-4 | steady + 1e-3 |
| R7: `CockpitSpace=0` vs the module without a Room | bit-identical | bit-identical |
| R8: 48 voices (24 cockpit, 24 through the canopy), head 90°/s, Normal / Strong | module p99 0.57 / 0.61 ms per 10 ms block; room p50 0.087 ms, p99 0.115 / 0.121 ms; 48 owned and sent, early on, no back-off, 0 allocations | budget 2.5 ms; room 0.5 ms (gate), 0.15 ms (target) |

Notes:
- The above-front source's least-squares fit reads +0.3 to +0.55 dB against d: its canopy reflection arrives within
  the direct sound's own correlation time and the fit counts it as direct. The DRRs use the module's d (exact with one
  voice), not the fit.
- R8 here is wall time inside the module's room calls with this scene (QPC, the module's own `room p99`); CORE's C9
  (0.19-0.22 ms p99, section 9) is thread CPU time with its own scene, best of 5. Both under the 0.30 ms gate; the 0.15
  ms target is met here and missed in C9, so the live `room p99` decides (section 10.1).
- The "stereo above" sound of the mix reads -1.47 dB against DCS's own output already with the space Off: that is the
  direct path of a stereo voice 0.36 m above the head, not the space (which changes it by at most +0.10 dB).
- The other suites: `build-binaural.ps1` 7/7 ctest, `build-dcsqvcull.ps1` 6/6 ctest (`DcsQvCullTests` now reads back
  `CockpitSpace` and `CockpitSpaceCanopy` in its invariants), the .NET suite 423 passed 0 failed, `build-release.ps1
  -LocalOnly` and `test-release.ps1` pass, `check-contracts.ps1` 7 identical, `gen_audio_sigs.py --check` in step with
  ed_sound.dll 0x6acb8057, `check-personal-data.ps1 -All` clean.

## 14. Limits and open questions

- [A] cockpit hosts' orientation = the aircraft's (section 3.2): settled by the first live `audio_space.csv` (spread,
  yaw following the head). If wrong, fallback: aircraft attitude from `Export.LoGetSelfData()` and the camera from
  `Export.LoGetCameraPosition()` in the Lua bridge at the sim frame rate (a fourth export `DcsQvCull_AudioFrame`),
  then the same late correction.
- [A] whether canopy opening changes the world listener's gain or low-pass in DCS (probe line, section 8.5). If it
  does, nothing changes in our design (the direct path is DCS's); the canopy signal still drives the space.
- [A] unit type names not yet seen in a log (section 5.1): unknown names fall back to the generic family, never fail.
- 2D cockpit sounds of some modules get no space (never owned).
- Voices over MaxObjects get no space (the quietest).
- Head translation does not move the walls (section 4.4).
- No helmet model; no structure-borne sound; no per-aircraft geometry by design.
