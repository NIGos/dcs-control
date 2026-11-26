# Cockpit space listening test

A blind test of about 10 minutes that tells whether **Cockpit space** (Audio page) puts cockpit sounds outside your
head, and which level should be the default. Design and the reasons behind it: [docs/COCKPIT_SPACE.md](../../docs/COCKPIT_SPACE.md),
section 11.

You need: headphones (the headset's own are fine), DCS Control with 3D headphone audio on, and for Part A the native
render tool `binaural_room_render.exe` (built with `native/binaural`). Everything is driven by `DcsVr.Cli audio-test`;
the scripts here only find it and pass the folder. The level of a trial is never shown while you test.

Nothing here writes into DCS or Saved Games by itself. The only file of DCS Control's that changes is the engine
module's `DcsQvCull.ini`, which the app already owns: Part B adds an `[AudioTest]` section while the test runs and
removes it at the end. Launch DCS and Back to stock DCS remove it too.

## 0. Prepare (2 minutes, once)

```powershell
cd tools\listening-test
.\make-stimuli.ps1                       # Part A's sounds, into artifacts\listening\<today>\
.\make-mission.ps1 -Source "<DCS>\Mods\aircraft\F-16C\Missions\QuickStart\F-16C - Caucasus - Cold and Dark.miz"
```

- `make-stimuli.ps1` takes `-Hrtf <your set.dcshrtf>` to use your own ears (without it the tool's generic set is
  used) and `-DryRun` to print the render commands only.
- `make-mission.ps1` reads any mission DCS saved and writes a copy with the test added to
  `artifacts\cspace-listening\`. A mission with your aircraft cold on the ramp suits best (switches to flip, engines
  off). Open the copy in DCS with Mission > Open (or copy it to your Missions folder yourself).
- Use one results folder per session (default `artifacts\listening\<today>`; `-Folder` on every script for another).
  Its `seed.txt` ties both parts together. Do not open `key.json` before you are done.

Print or open [RATING_SHEET.md](RATING_SHEET.md) to see the scale.

## Part A: short sounds, DCS closed (about 5 minutes)

```powershell
.\run-part-a.ps1
```

- **A1, 24 sounds.** After each sound: how far outside your head it was, on the number row: **0** inside my head,
  **1** at my head, **2** just outside, **3** clearly outside, **4** where it should be. Then **Up arrow** = it came from
  the front, **Down arrow** = from behind. **R** plays it again.
- **A2, 8 trials.** A, B and X play in turn. **1** = X sounds like A, **2** = X sounds like B. **R** plays all three again.
- **Esc** stops; running it again continues where you stopped. A short beep confirms each answer.

## Part B: in the cockpit (about 5 minutes)

1. Launch DCS from DCS Control (3D headphone audio on), open the test mission, and sit in the cockpit.
2. On the PC run `.\part-b.ps1`. It writes "Test mission ready." when it sees the mission.
3. In DCS open the radio menu (\\ key by default), **F10 Other**, **Cockpit space test**. Every choice is a menu item:
   no key of ours is needed. Each choice that changes the sound is followed by a tick about 3 seconds later: wait for it.
   - **Next cockpit trial** (9 trials): after the tick, flip switches on the left console, the right console, the front
     panel and overhead, and turn your head. Then **Rate the cockpit trial**, 0 to 4, the same scale as Part A.
   - **Next outside sound** (16): look straight ahead. A bang sounds about 150 m away a few seconds later. Then **The
     outside sound came from**: the direction you heard (8 choices).
   - **A/B/X** (3 trials): **Next A/B/X trial**, then **Play A**, **Play B**, **Play X** (flip a few switches after each
     tick), then **X is A** or **X is B**.
   - **End the test** when done. `part-b.ps1` stops and your own Cockpit space setting plays again (Ctrl+C in the
     PowerShell window does the same).
4. Note anything you heard on the rating sheet, for example a "tin can" sound or an echo you did not like.

## Results

```powershell
.\analyze.ps1
```

Writes `summary.md` and `summary.json` into the results folder and prints them: per level the median rating and the
share rated 3 or 4, front/back reversals (Part A and the outside sounds), Wilcoxon signed rank Off against Subtle and
Subtle against Normal (paired by direction in Part A and by block in Part B), A/B/X with its binomial p, and the
verdict: Subtle stays the default when it rates higher than Off (median +1 or more, p below 0.05) without more
front/back reversals. Send `summary.md` and your notes; the folder holds no personal data, but check before sharing.

## Files of a session

| File | Made by | What |
|---|---|---|
| `seed.txt` | first script run | the session's seed (orders, A/B/X) |
| `trials.json` | make-stimuli | what Part A plays, by opaque id |
| `key.json` | make-stimuli | the answers to Part A (directions, levels) |
| `dry\`, `render\`, `stimuli\` | make-stimuli | the dry signals, the tool's output, the sounds played |
| `answers.csv` | run-part-a | your Part A answers |
| `part-b.log` | part-b | the test mission's lines from dcs.log |
| `summary.md`, `summary.json` | analyze | the results |

## Command line

`DcsVr.Cli audio-test start [--seed N] | level 0..3 | stop | watch --folder F [--log dcs.log] | stimuli --folder F
[--tool exe] [--hrtf file] [--dry-run] | mission --source in.miz --out out.miz | analyze --folder F`

`start`, `level` and `stop` set `[AudioTest]` by hand (for a quick check of the levels in flight); `watch` is Part B.
