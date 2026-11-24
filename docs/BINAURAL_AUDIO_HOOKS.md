# Binaural audio: hook specification for DCS's ed_sound.dll

This is the contract for the DCS audio module (task B in [BINAURAL_AUDIO.md](BINAURAL_AUDIO.md)). It turns §1 of the
design (`artifacts/research/hrtf-audio-design-2026-11-24.md`) into exact hook points, checks and struct layouts.

Everything here comes from static reading of the DCS binaries (Python `pefile` + `capstone`, linear sweep over the
`.pdata` function ranges). DCS was not started and no file was changed.

Labels:
- **[V]** read in the disassembly (instruction addresses are given)
- **[A]** inferred, with what settles it

## 0. Build analysed and how to tell a different build

| Module | File version | TimeDateStamp | SizeOfImage | SHA-256 |
|---|---|---|---|---|
| `bin\ed_sound.dll` | 2.9.31.29434 | `0x6acb8057` | `0x0d0c4000` | `dc52bc85749a7dcb57e09c675858bfe727c760e558a7a73b69260495f3f79bdf` |
| `bin\WorldGeneral.dll` | 2.9.31.29434 | `0x6adbace0` | `0x0127b000` | `83d36b10205234da58b4a98dd06f75a84d6f3e13826d0a123e482eb53a7b8265` |

- ed_sound is a plain vc143 DLL: sections `.text 0x1000..0x2ca73`, `.rdata 0x2d000`, `.data 0x3a000` (virtual
  size `0xd084700`, static pools), `.pdata`, `.rsrc`, `.reloc`. 72 named exports. It is not packed, so every hook
  below sits in ed_sound and can be checked byte by byte.
- **Same build:** TimeDateStamp and SizeOfImage equal the table. Then the recorded RVAs are used directly, and the
  byte checks of §6 still run.
- **Other build:** every location is looked up by its pattern (§2, §3), the existing `reloc` way: a unique match,
  then a masked FNV of the whole function (the `hooksig` `fnHash` gate), then the layout checks of §6. Any failure
  leaves the feature off with one log line (`audio: <id> not found (<why>)`) and DCS audio exactly as stock.
- Add `ed_sound.dll` to `tools/hooks_curated.py` / `hooks_manifest.json` (module entry with the timestamp and size
  above), regenerate `src/hook_sigs.h` with `tools/gen_hook_sigs.py`, and extend `tools/dcs_patch_check.py` so a DCS
  update reports the audio locations like the others.

### Why no WorldGeneral hook (change from the design)

The design's P1/P2 swap two IAT slots of WorldGeneral.dll. Statically that cannot be verified:
- WorldGeneral is not UPX-packed but protected. `.text`, `.rdata`, `.data` and `.pdata` have a raw size of 0 and are
  filled at run time, and the entry point (`0x93a96d`) is an obfuscated stub (`pushfq`, junk arithmetic, `retf`).
- Its import directory and IAT directory both point into the section named `.UPX1` (IAT `0x80a000`, size `0x14a8`;
  ed_sound's 46 slots are `0x80a000..0x80a170`, e.g. `SND_process` `0x80a068`, `SND_set_listener` `0x80a108`).
  Whether the decrypted code calls through that IAT, or through a copy the stub rebuilds, cannot be read from the file.

The binaural path does not need those hooks:
- The listener of every context is stored by `SND_set_listener` into the context struct itself, and read back by the
  export `SND_listener_params(ctx)` (§4.1).
- `SND_process` keeps a frame counter (§4.4), so the P3 wrapper sees each frame boundary itself.

So **P1 and P2 are dropped from v1.** An IAT swap stays possible later as a diagnostic, with the slot found at run time
by scanning the loaded image for a pointer equal to `GetProcAddress(ed_sound, name)` and patching only when exactly
one 8-byte aligned match lies in a run of pointers into ed_sound.

## 1. The hook points

| Id | What | Site RVA | Thread | Patch | Needed |
|---|---|---|---|---|---|
| **P3a** | `call panner` in `SND_process`'s per-voice loop | `0x4d40` | simulation, ed mutex held | 8-byte aligned store over `0x4d40..0x4d47`, under the ed mutex | yes |
| **P3b** | `call panner` in voice creation (`sub_1b870`, called from `SND_process` at `0x4d53`) | `0x1ba00` | simulation, ed mutex held | 8-byte aligned store over `0x1ba00..0x1ba07`, under the ed mutex | yes |
| **P4** | `call voice_render` in the mix loop (`sub_1b180`) | `0x1b1fb` (rel32 at `0x1b1fc`) | render (MMCSS) | aligned 4-byte store of the rel32 | yes |
| **P5** | `call mix` in the WASAPI thread loop | `0x20183` (rel32 at `0x20184`) | render (MMCSS) | aligned 4-byte store of the rel32 | yes |

The panner (`sub_1c1c0`) has exactly two callers, P3a and P3b [V]. The voice render (`sub_165d0`) has exactly one,
P4 [V]. The mix (`sub_1b180`) has three: `SND_offline_render` (`0x48b3`), the WASAPI loop (`0x20183`, P5) and the
Windows-spatial loop (`0x21c40`) [V]. Only P5 is hooked; the other two paths stay stock (§5.3).

Every new call target is a thunk in a `DetourPage` within ±2 GB of ed_sound (`call rel32` reach). Thunks are never
freed, so a payload hot reload can never leave a call site pointing at freed code.

### 1.1 P3a: panner call in SND_process

```
00004d30  49 8B 1C 24              mov  rbx, [r12]            ; sound = activeSounds[i]   (array 0x3f1c0)
00004d34  48 8B 8B F0 01 00 00     mov  rcx, [rbx+0x1f0]      ; voice = sound->voice
00004d3b  48 85 C9                 test rcx, rcx
00004d3e  74 07                    je   0x4d47
00004d40  E8 7B 74 01 00           call 0x1c1c0               ; panner(voice)       <- P3a
00004d45  EB 32                    jmp  0x4d79
00004d47  83 BB BC 01 00 00 00     cmp  dword [rbx+0x1bc], 0
```
- Pattern (call at match + 16): `49 8B 1C 24 48 8B 8B F0 01 00 00 48 85 C9 74 07 E8 ?? ?? ?? ?? EB`. One match [V].
- Validation: the rel32 at `+17` must resolve to the panner (§2.1 prologue), the byte at `+21` is `EB`.
- Patch: `0x4d40` is 8-byte aligned, so `E8 <rel32> EB 32 83` (8 bytes, the last three unchanged) goes in one aligned
  8-byte store. Do it while holding ed_sound's own mutex (§4.4): `SND_process` holds it from `0x49b3` to `0x4d8e`, so
  no thread can be between those addresses during the store. Restore the same way.

### 1.2 P3b: panner call when a voice is created

```
0001b9f0  C7 83 BC 0E 00 00 00 00 80 3B   mov  dword [rbx+0xebc], 0x3b800000
0001b9fa  88 83 C2 0E 00 00               mov  [rbx+0xec2], al      ; voice->ctx2d = ctx->is2D
0001ba00  E8 BB 07 00 00                  call 0x1c1c0              ; panner(voice)       <- P3b
0001ba05  0F 10 4B 30                     movups xmm1, [rbx+0x30]   ; then pending -> snapshot copy
```
- Pattern (call at match + 16): `C7 83 BC 0E 00 00 00 00 80 3B 88 83 C2 0E 00 00 E8 ?? ?? ?? ?? 0F 10 4B 30`. One
  match [V].
- Patch: `0x1ba00` is 8-byte aligned; same rule as P3a, under the same mutex (`sub_1b870` is only called from
  `SND_process` at `0x4d53`, inside the locked body) [V].
- Why it matters: right after this call the voice copies the pending block into its render snapshot and sets every
  gain ramp's start = target (`0x1bb10..0x1bb81`) [V]. So the first block a voice plays uses exactly these gains. With
  P3b the voice is mono-tagged from its first sample, so the onset of a gunshot or a switch click is already binaural;
  without it the first ~25 ms would be DCS's stereo pan.

### 1.3 P4: voice render call in the mix

```
0001b1f0  45 8B C4        mov  r8d, r12d        ; frames
0001b1f3  48 8B D3        mov  rdx, rbx         ; float* out (interleaved, renderer channel count)
0001b1f6  48 8B CF        mov  rcx, rdi         ; voice
0001b1f9  FF C5           inc  ebp
0001b1fb  E8 D0 B3 FF FF  call 0x165d0          ; int voice_render(voice, out, frames)   <- P4
0001b200  85 C0           test eax, eax
0001b202  75 17           jne  0x1b21b          ; 0 = voice finished: unlinked and freed to the pool at 0xd0bdf50
```
- Pattern (call at match + 11): `45 8B C4 48 8B D3 48 8B CF FF C5 E8 ?? ?? ?? ?? 85 C0 75`. One match [V].
- Patch: the rel32 at `0x1b1fc` is 4-byte aligned: one atomic 32-bit store, safe while the render thread runs.
- The wrapper must return the original's return value unchanged. A return of 0 means DCS frees the voice right after;
  the wrapper drops its per-voice render state then.

### 1.4 P5: mix call in the WASAPI loop

```
00020173  E8 6C C2 00 00        call memset           ; out[0 .. frames * channels) = 0
00020178  48 8B 55 DF           mov  rdx, [rbp-0x21]  ; float* out (the WASAPI buffer from GetBuffer)
0002017c  48 8D 4B 08           lea  rcx, [rbx+8]     ; renderer = &SND_Wasapi->renderer
00020180  44 8B C7              mov  r8d, edi         ; frames = buffer size - padding
00020183  E8 F8 AF FF FF        call 0x1b180          ; void mix(renderer, out, frames)   <- P5
00020188  48 8B 8B 30 27 00 00  mov  rcx, [rbx+0x2730] ; IAudioRenderClient
```
- Pattern (call at match + 11): `48 8B 55 ?? 48 8D 4B 08 44 8B C7 E8 ?? ?? ?? ?? 48 8B 8B ?? ?? 00 00`. One match [V].
- Patch: the rel32 at `0x20184` is 4-byte aligned: one atomic 32-bit store.
- P5 brackets one device block: `BeginBlock` (pose read, block id, budget clock) before the original, `EndBlock`
  (stats) after it. P4 does binaural work only inside an armed P5 block (§5.3).

## 2. Functions behind the hooks

### 2.1 Panner `sub_1c1c0` (`void panner(Voice* v)`)
- Prologue pattern: `48 8B C4 53 55 56 48 81 EC E0 00 00 00 48 8B 59 10 48 8B E9 48 8B 71 08` at `0x1c1c0`, one
  match [V]. Function `0x1c1c0..0x1c6b9`.
- Microsoft x64 convention, one argument (`rcx` = voice); returns nothing. `rbx` = `voice->sound` (`+0x10`), `rsi` =
  `voice->renderer` (`+0x08`), `rbp` = voice [V].
- Layout checks inside the function (§6): the per-channel stores `F3 42 0F 11 44 FD 48` (`movss [rbp+r15*8+0x48]`,
  2 matches, `0x1c423`, `0x1c44c`) and `F3 42 0F 11 74 FD 4C` (`movss [rbp+r15*8+0x4c]`, `0x1c45c`), and the channel
  loop bound `44 3B BE 74 22 00 00` (`cmp r15d, [rsi+0x2274]`).

**What it computes [V].** For each output channel `ch < renderer->channels`:
```
base  = speedFactor * sound->gain                                  ; gain = sound+0x1c0 (float)
speedFactor = 1                     if sound+0x178 == 1.0
            = s + (1 - s) * (1 - t)  else, with s = sound+0x178, t = |relVel| / 200 m/s (relVel = sound+0x108), t <= 1
if sound is 3D (sound+0x18 != 0) and speaker ch is not LFE:
    c = u . spk[ch]                 ; u = unit direction listener->source = -(sound+0x1d8), spk = renderer+0x2278+0x30*ch
    x = crosstalk in [0,1]          ; 0 unless global 0x3a334 != 0 and sound+0x1a7 != 0 and ctx->ctThreshold > 0;
                                    ; then x = (base < thr) ? powf(base / thr, ctx->ctExponent) : 1
    p = (0.4 - 0.35 x) * (1 + c) + (0.2 + 0.7 x)
    pending.gain[ch].a = spk[ch].w3dA * p * base       ; voice+0x48+8*ch
    pending.gain[ch].b = spk[ch].w3dB * p * base       ; voice+0x4c+8*ch
else (2D sound or LFE):
    pending.gain[ch].a = base * spk[ch].w2dA
    pending.gain[ch].b = base * spk[ch].w2dB
```
Constants read from `.rdata` [V]: `0.4` (`0x2f488`), `0.35` (`0x2f480`), `0.7` (`0x2f4a8`), `0.2` (`0x2f468`), `200`
(`0x2f4e0`, `0x2f524`), `1.0` (`0x2d790`). After the channel loop it computes the doppler pitch (`pending+0x20`), a
distance blend (`+0x24`), filter parameters (`+0x28..+0x30`, `+0x40`, `+0x44`), the propagation delay term (`+0x3c`)
and calls `sub_1a750(sound, pending+0x20)` (a velocity/direction-dependent filter that also reads `sound+0x1d8`).

### 2.2 Voice render `sub_165d0` (`int voice_render(Voice* v, float* out, uint32_t frames)`)
- Prologue pattern: `40 53 48 83 EC 40 80 B9 C0 0E 00 00 00 48 8B D9` at `0x165d0`, one match [V]. The design's
  expected prologue check `cmp byte [rcx+0xec0],0` is the 7 bytes at `+6`. Function `0x165d0..0x16a9d`.
- What it does [V]:
  1. Copies the pending block `voice+0x20..+0xd0` into the snapshot `voice+0xd0..+0x180` (no lock: the simulation
     thread may be writing it; DCS accepts that).
  2. Per output channel sets up two linear gain ramps (`voice+0xa54+0x30*ch` and `+0xa6c+0x30*ch`, each
     `{float target, float start, float current, uint64 step}`) from the previous snapshot to the new one, over the
     `frames` of this call.
  3. Updates the filter coefficients (`sub_1a1a0`), then loops a kernel until `frames` are produced.
- **Kernels [V]** (the design listed eight; there are nine, `0x14de0` was missing). `ctx2d` = `voice+0xec2`, channels
  = `voice+0xec4`, SIMD = global byte `0x3a338`, `voice+0xf4` = the snapshot of `voice+0x44` (the panner copies it
  from `sound+0x1ac`):

  | ctx2d | wave channels | SIMD | kernel |
  |---|---|---|---|
  | 1 | 1 | 1 | `0x11c40` if `voice+0xf4 != 0`, else `0x10d50` |
  | 1 | 1 | 0 | `0x17920` |
  | 1 | 2 | 1 / 0 | `0x13e90` / `0x194d0` |
  | 0 | 1 | 1 / 0 | `0x12dd0` / `0x16aa0` |
  | 0 | 2 | 1 / 0 | `0x14de0` / `0x183e0` |

- **Kernels only add [V]** (`addss xmm0, [r10-8]; movss [r10-8], xmm0` in `0x16aa0`): `out[i*ch + c] += ...`. For
  the float kernels, rendering into a zeroed scratch buffer and adding it to `out` gives bit-identical output (`0 + x
  == x`, then the same single addition into `out`). The SIMD mono 3D kernel `0x12dd0` adds in double [V]
  (`cvtps2pd` of `out`, `addsd`, `cvtsd2ss` back, `0x13abb..0x13aca`): `out = float(double(out) + v)`, where the
  scratch path rounds `v` to float first. There the scratch path is within 1 ulp of stock, not always bit-identical.
- **Mono kernels use the sum of the pair [V]** (`0x16aa0`, `0x16ff0..0x17166`): for a mono wave, output channel `ch`
  gets `s * (rampA[ch] + rampB[ch])`, where `s` is the resampled and filtered sample. Stereo kernels use `a` for the
  wave's left channel and `b` for its right channel.

### 2.3 Mix `sub_1b180` (`void mix(Renderer* r, float* out, uint32_t frames)`)
- Prologue pattern: `48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 20 48 8B F1 4C 8D 79 20` at
  `0x1b180`, one match [V]. Function `0x1b180..0x1b45a`.
- Order [V]: pop new voices from a lock-free list into `renderer+0x20` (singly linked, `voice+0` = next) → P4 for each
  voice → `renderer+0x2270` = voice count → per frame the master processing `sub_1ca60(renderer+0x48, channels,
  out+frame*channels)` (the limiter) → the global gain ramp (`0x3a330`). **Our binaural output is added inside P4, so it
  is before the limiter and the master gain, as the design wants.**

## 3. The panner's two values per channel, settled [V]

`setSpeaker(renderer, ch, const double3* dir, float gain2d)` at `0x1b4b0` (pattern `83 FA 11 0F 83 ?? ?? ?? ?? 48 89
5C 24 10 56 48 83 EC 30`, one match) writes the speaker record `renderer+0x2278+0x30*ch`:

| Offset | Type | Meaning |
|---|---|---|
| `+0x00` | double3 | unit speaker direction in the listener frame (x forward, y up, z right) |
| `+0x18` | float | `w2dA`: 2D gain from the wave's left channel |
| `+0x1c` | float | `w2dB`: 2D gain from the wave's right channel |
| `+0x20` | float | `w3dA`: 3D weight for the wave's left channel |
| `+0x24` | float | `w3dB`: 3D weight for the wave's right channel |
| `+0x28` | u8 | LFE (direction was zero) |

- Speaker on the left (`dir.z < 0`): `w3d = (1, 0)`, `w2d = (g, 0)`. On the right (`z > 0`): `w3d = (0, 1)`, `w2d = (0,
  g)`. Centre (`z == 0`) and LFE: `(0.5, 0.5)`, `(g, g)`.
- **So the two floats per output channel are the gains from the wave's left and right channel.** A mono wave plays
  `a + b` on that channel (§2.2). For a 3D voice on a stereo device DCS always writes `ch0 = (gL, 0)` and `ch1 = (0, gR)`.
- **Stereo device layout [V]:** the WASAPI init (`0x20487`) calls `sub_1b620` when the device format has 2 channels
  and channel mask 3 (FL|FR). `sub_1b620` places speaker 0 at azimuth −60° and speaker 1 at +60°:
  `(cos −60°, 0, sin −60°) = (0.5, 0, −0.866)` and `(0.5, 0, +0.866)`. Other masks take directions from the table at
  `0x3a390`. The offline renderer uses the same ±60° pair (`SND_offline_start`, `0x4931`).

## 4. Data structures

Offsets are from the struct start. "V" = read in the code at the addresses given.

### 4.1 Context (array at RVA `0x3d370`, stride `0x388`, count at `0x3ad74`)

`SND_init` sets the count from `params+4` (clamped 1..8, default 8) [V `0x4651..0x467c`]. `SND_set_listener(int ctx,
const ListenerParams*)` validates and copies the params **into the context itself** (`sub_5800(ctx, flags, params)`,
`0x4f4c`), without the ed mutex [V]. `const ListenerParams* SND_listener_params(int ctx)` returns the context's address
[V `0x4830`].

| Offset | Type | Meaning | Conf. |
|---|---|---|---|
| `+0x00` | u32 | listener flags as last set | V |
| `+0x08` | double3 | listener position, world | V (`0x7327`) |
| `+0x20` | double4 | listener orientation quaternion `(x, y, z, w)`, listener → world | V (`0x4a1e`, `0x1c60`) |
| `+0x40` | double3 | listener velocity, world | V (`0x7380`) |
| `+0x58`, `+0x5c` | float | listener params; `+0x5c` scales the doppler distance term | V use, A meaning |
| `+0x68` | double | listener time (ED time, or samples / rate offline) | V (`0x4f8e`) |
| `+0x78` | double4 | inverse orientation `conj(q) / |q|^2`, computed in `SND_process` | V (`0x4a64..0x4a80`) |
| `+0x98`, `+0xa0` | double | listener dt, previous time | V |
| `+0xb0`, `+0xd0` | list heads | host lists | V |
| `+0xe0` | u8 | is 2D (`SND_context_set2d`) | V (`0x3fd2`) |
| `+0xe1` | u8 | use cockpit gain (`SND_context_use_cockpit_gain`) | V (`0x40f6`) |
| `+0xe4` | i32 | output index (`SND_context_set_output`); the voice's renderer is looked up from it, output 0 if missing | V (`0x4022`, `0x55a0`) |
| `+0xf8` | float | spatial crosstalk threshold (param id `0x79`) | V (`0x4080`, `0x1c3a0`) |
| `+0xfc` | float | spatial crosstalk exponent | V |

**Context ids [A].** The ids are chosen by WorldGeneral at run time and its code cannot be read statically (§0). The
module does not rely on ids: it decides by the context's own flags (`is2D`, output index) and logs one line per
context at enable time: `audio: ctx <i> 2d=<0/1> cockpitGain=<0/1> output=<n> crosstalk=<thr>/<exp>`. The first live
log settles which id is world, cockpit, headphones and GUI. Radio and intercom are expected to be 2D sounds or in a 2D
context; ownership rules out both (§5.1), and `[Audio] ExcludeContexts` can rule out any context by id.

### 4.2 Sound instance (`activeSounds` array at `0x3f1c0`, count `0x3ad84`; pool in `.data`)

| Offset | Type | Meaning | Conf. |
|---|---|---|---|
| `+0x000` | Context* | its context | V (`0x731e`, `0x1c39d`) |
| `+0x018` | ptr/qword | non-zero = 3D sound (zero: 2D, constant gain) | V (`0x1c333`) |
| `+0x068` | double3 | world position | V (`0x7312`) |
| `+0x080` | double4 | world orientation quaternion | V (`0x735d`) |
| `+0x0a0` | double3 | world velocity | V (`0x7371`) |
| `+0x0d0` | double3 | **position relative to the listener, in the listener frame, metres** | V (`0x7317..0x7358`) |
| `+0x0e8` | double4 | orientation in the listener frame | V (`0x736c`) |
| `+0x108` | double3 | velocity relative to the listener, listener frame | V (`0x7371..0x73b7`) |
| `+0x178` | float | speed factor at rest (1.0 = none) | V (`0x1c208`) |
| `+0x1a7` | u8 | crosstalk enabled for this sound | V (`0x1c394`) |
| `+0x1bc` | u32 | play state (0 = not playing) | V (`0x4cf0`) |
| `+0x1c0` | float | gain after distance attenuation and cones | V (`0x71ab`, `0x1c328`) |
| `+0x1c8` | double | distance `|pos+0x0d0|` | V (`0x7427`) |
| `+0x1d0` | double | attenuation distance (equals `+0x1c8`, or the doppler-corrected one) | V (`0x7584`) |
| `+0x1d8` | double3 | **unit vector from the source towards the listener** = `−(+0x0d0) / distance`; `(0,0,0)` under 0.01 m | V (`0x7446..0x7480`, constant `−1.0` at `0x2e330`) |
| `+0x1f0` | Voice* | its render voice, or null | V (`0x4d34`, `0x1b9cb`) |

Correction to the design: `+0x108` is the relative velocity, not the position; the listener-frame position is `+0x0d0`,
and `+0x1d8` points from the source to the listener (the panner uses `1 − d·s`).

### 4.3 Render voice (`sizeof` `0x1f10`; pool free list at `0xd0bdf60`, freed voices go to `0xd0bdf50`)

| Offset | Type | Meaning | Conf. |
|---|---|---|---|
| `+0x000` | Voice* | next in the renderer's list | V (`0x1b204`) |
| `+0x008` | Renderer* | its renderer | V (`0x1b946`) |
| `+0x010` | Sound* | its sound (back pointer, set at creation) | V (`0x1b9bd`) |
| `+0x018` | qword | copy of `sound+0x180` | V |
| `+0x020..+0x0d0` | block | **pending parameters**, written by the simulation thread | V |
| `+0x020` | float | pitch ratio (doppler) | V (`0x1c5ea`) |
| `+0x048 + 8*ch` | float[2] | **pending gains (a, b)** for output channel ch, ch < 17 | V |
| `+0x0d0..+0x180` | block | snapshot of the pending block, taken at the start of each render | V (`0x165f5`) |
| `+0x0f8 + 8*ch` | float[2] | **snapshot gains (a, b)**: the targets the block just rendered ramped to | V |
| `+0xa54 + 0x30*ch` | ramp | gain ramp a: `{target, start, current, u64 step}` | V |
| `+0xa6c + 0x30*ch` | ramp | gain ramp b | V |
| `+0xeb8`, `+0xebc` | float | fade level and fade step (start/stop envelope) | V |
| `+0xec0`, `+0xec1` | u8 | stop requested, free when silent | V |
| `+0xec2` | u8 | context is 2D (copied from `ctx+0xe0` at creation; selects the kernel family). The design's "stream flag" is wrong. | V (`0x1b9e0..0x1b9fa`) |
| `+0xec4` | u32 | **wave channels (1 = mono)** | V (`0x15f1d`, `0x16423`) |
| `+0xec8` | u32 | wave sample rate | V (`0x167ae`) |

### 4.4 Globals and the renderer

| What | RVA / offset | Found by | Conf. |
|---|---|---|---|
| ed mutex (`ed::mutex`) | `0x3ad90` | `SND_process` + `0x2c`: `48 8D 0D <rip>` then `FF 15` → `edCore!?lock@mutex@ed@@QEAAXXZ` | V |
| engine state | `0x3ad70` | pattern `83 3D ?? ?? ?? ?? 01 0F 8E ?? ?? ?? ?? E8` (+2, rip+1) | V: 0 none, 1 stopped, 2 offline (AVIMaker), 3 running |
| frame counter | `0x3ad68` | pattern `FF 05 ?? ?? ?? ?? 8B C6 89 05 ?? ?? ?? ?? 8B FE 4C 8D 25` (+2) | V: `inc` once per `SND_process` (`0x4b23`), before the panner loop |
| context array / count | `0x3d370` / `0x3ad74` | pattern `33 F6 44 8B FE 4C 8D 2D ?? ?? ?? ?? F2 44 0F 10 05` (+8) | V |
| active sounds / count | `0x3f1c0` / `0x3ad84` | `SND_process` | V |
| crosstalk enable | `0x3a334` (u32) | panner `0x1c34c` | V |
| SIMD kernels | `0x3a338` (u8) | voice render `0x169c2` | V |
| offline renderer | `0x3adc0` | `SND_offline_start` | V |
| `Renderer+0x0000` | double | sample rate | V (`0x167b8`) |
| `Renderer+0x0020` | Voice* | voice list head | V (`0x1b199`) |
| `Renderer+0x2270` | u32 | voices rendered last block | V |
| `Renderer+0x2274` | u32 | output channels (≤ 17) | V |
| `Renderer+0x2278 + 0x30*ch` | speaker | §3 | V |
| `SND_Wasapi+0x8` | Renderer | embedded renderer (P5 passes `&wasapi->renderer`) | V |
| `SND_Wasapi+0x2718/0x2728/0x2730` | handles | event, `IAudioClient`, `IAudioRenderClient` | V |

The WASAPI buffer is float32 interleaved with `Renderer+0x2274` channels (DCS zeroes `frames * channels * 4` bytes
before the mix) [V]. `frames` is whatever `GetBuffer` allows (buffer size − padding), so **it varies per callback**:
the module and BinauralCore accept any frame count.

## 5. Behaviour of the wrappers

### 5.1 P3 wrapper (`void PanWrap(Voice* v)`, two thunks: `initial = false` for P3a, `true` for P3b)

Runs on the simulation thread with the ed mutex held. No allocation, no locks of our own, no logging.

1. Call the original panner. DCS's state is then exactly stock (doppler, filters, `sub_1a750`, all from the real
   direction).
2. Decide ownership (all must hold, else return):
   - module enabled, renderer bound and validated (§6), offline state not active (`0x3ad70 != 2`);
   - `sound->is3D` (`+0x18 != 0`), context not 2D (`ctx+0xe0 == 0`), context not in `ExcludeContexts`;
   - wave is mono (`voice+0xec4 == 1`); stereo waves stay DCS's (v1);
   - renderer channels == 2 and the voice's renderer is the bound one (`voice+0x08`);
   - distance ≥ 0.05 m (`sound+0x1c8`); closer means head-locked, left to DCS;
   - rank within the top `MaxObjects` by `base` gain (hysteresis: kept ≥ 250 ms, replaced only by a voice 3 dB louder).
3. For an owned voice compute the **mono gain `m`**: DCS's own gain for that voice with the source straight ahead
   (`c = 0.5` on the ±60° pair). DCS's gains follow `g_ch = WA·(1 + c_ch) + WB` with `c_ch = u·spk[ch]`, `u = −(sound+0x1d8)`:
   - if `|c_L − c_R| ≥ 0.05`: `WA = (g_L − g_R) / (c_L − c_R)`, `WB = g_L − WA·(1 + c_L)`; keep `k = WB / WA` for the voice;
   - else (source near the median plane): `WA = g_L / (1 + c_L + k)` with the voice's last `k` (initially 0.5, the
     `x = 0` ratio);
   - `m = 1.5·WA + WB`.
   - Self-check: `WA ≥ 0` and `WB ≥ 0` within 1e-4 relative, and `g_ch` recomputed from `WA`, `WB` matches DCS's within
     1e-4. A failure counts as a pan-law mismatch; more than 1 % of owned voices over 10 s turns the module off (log
     `audio: pan law mismatch`). This catches a changed pan law after an update.
   - Rationale: frontal sources keep exactly DCS's level; every other direction gets its level from the HRTF, so DCS's
     own front/back attenuation and crosstalk narrowing are not applied twice.
4. Write the **mono tag** into the pending block: `ch0 = (m, 0)`, `ch1 = (m, 0)`. DCS never writes a non-zero `a` on
   channel 1 of a stereo device (§3), so the tag is unambiguous in the snapshot.
5. Publish the voice's metadata to the render side (a per-slot seqlock, one writer): voice pointer, a generation that
   changes when the voice's sound changes or `initial` is true, listener-frame position (`sound+0x0d0`, double →
   float), distance, `m`, context id, flags, the `SND_process` frame counter, and the pose tag (below).

Once per `SND_process` (first wrapper call whose frame counter differs from the last): read the listener of each owned
context (`ctx+0x20` quaternion, `ctx+0x68` time) and the DCS frame's pose display time (the engine's own
`DcsVrMotion_v1` record), and store the pose tag `q_used` (§ head pose, `head_pose_shm.h`).

### 5.2 P4 wrapper (`int RenderWrap(Voice* v, float* out, uint32_t frames)`)

Runs on the render thread (MMCSS). No allocation, locks, logging or file I/O.
- Not inside an armed P5 block (different `out` pointer or thread), module off, or not a candidate voice → `return
  original(v, out, frames)`.
- Candidate (has a metadata slot, mono, renderer bound): zero `scratch[frames*2]`, `r = original(v, scratch, frames)`,
  then read the snapshot gains just used (`v+0xf8..+0x104`) and the previous block's tag state:

  | previous block | this block | output into `out` |
  |---|---|---|
  | not tagged | not tagged | `out += scratch` (stock within 1 ulp: §2.2, the double-accumulating SIMD kernel) |
  | not tagged | tagged | fade in: `out += (1−w)·scratch + w·HRTF(½(L+R))`, `w` 0 → 1 over the block |
  | tagged | tagged | `out += HRTF(L)` (L == R == mono·m) |
  | tagged | not tagged | fade out: `w` 1 → 0 |

  DCS ramps its gains linearly from the previous snapshot to the new one over the block, so at the block's first sample
  the scratch is exactly what stock would output and at its last sample it is pure `mono·m`: the crossfade has no step.
- `r == 0` → reset the voice's render state (the voice is freed). Return `r`.
- Budget: the block's measured binaural time is compared with `CpuBudgetPercent` of the block duration; see
  `binaural_core.h`.

### 5.3 P5 wrapper (`void MixWrap(Renderer* r, float* out, uint32_t frames)`)

- Bound renderer (validated: 2 channels, ±60° pair, not LFE) and module on: arm the block (`out`, thread id, frames,
  QPC), read the head pose once, compute the late-rotation for each context, call the original, disarm, update stats.
- Unknown renderer (a device switch makes a new `SND_Wasapi`): pass through, and hand the renderer pointer, rate and
  channel count to the module's control thread, which validates it and allocates a BinauralCore for it off the audio
  thread. The old binding is retired after 2 s without calls and freed only then.
- The offline path (`SND_offline_render`, AVIMaker) and the Windows-spatial path never go through P5, so P4 always
  passes them through: stock audio there.

## 6. Checks before enabling (all must pass)

1. Module identity: `ed_sound.dll` loaded; same-build fast path or relocated by pattern + masked function hash (§0).
2. Each site's pattern matches at the resolved RVA and its rel32 resolves to the expected function (P3a/P3b →
   panner prologue, P4 → voice render prologue, P5 → mix prologue).
3. Panner layout: the three store/bound byte sequences of §2.1 are inside `[0x1c1c0, 0x1c6b9)` relative to the resolved
   panner, at the same offsets from its start (`+0x263`, `+0x28c`, `+0x29c`, and the bound at `+0x2a6`).
4. Voice render layout: `80 B9 C0 0E 00 00 00` at `+6` (kill flag), `0F 10 41 20` (`movups xmm0,[rcx+0x20]`) at
   `+0x10`, i.e. the snapshot copy starts at `pending+0x20`.
5. Mutex: the `lea rcx,[rip+x]; call [rip+y]` at `SND_process+0x2c`, `y` resolves to edCore's
   `?lock@mutex@ed@@QEAAXXZ` and the unlock at the end uses the same `x`.
6. At bind time (render side): `renderer+0x2274 == 2`, speaker 0 `z < −0.5`, speaker 1 `z > 0.5`, neither LFE,
   `22050 ≤ rate ≤ 96000` Hz (the range the core supports).
7. Runtime self-checks: the pan-law check of §5.1, non-finite output of the core (the source is reset and counted), and
   ED's own `render time = %fs exceeded allowed slice` count (`SND_Wasapi+0x2274` increments, `0x201c1`): 3 overruns
   within 10 s, each while our own work took half the CPU budget or more of that block or the one before (ED counts
   anybody's slow block), → binaural off for the session (pass-through), log and status bit; the kill switch off and on,
   or Binaural back to 1 in the settings, re-arms it (`audio: on (re-armed ...)` / `audio: on (re-enabled ...)`).

Any failure → nothing is patched (or everything is restored in reverse order), log `audio: <id> not found (<why>)` or
`audio: off (<reason>)`, DCS audio stock.

## 7. Axes and the listener frame [V]

- DCS listener frame: **x forward, y up, z right**, right-handed. Settled by the stereo speakers (left `z < 0`) and the
  vector rotation `sub_1c60` (`v' = v + 2·(w·(q×v) + q×(q×v))`, quaternion `(x, y, z, w)`), applied with the inverse
  listener quaternion to `sourcePos − listenerPos`.
- OpenXR: x right, y up, −z forward. DCS = `M · OpenXR` with `M = [[0,0,−1],[0,1,0],[1,0,0]]` (`det = +1`), i.e.
  `dcs = (−oxr.z, oxr.y, oxr.x)`, and for quaternions `(x, y, z, w)_dcs = (−q.z, q.y, q.x, w)`. `head_pose_shm.h` has
  the helpers and the contract test checks them.
- Whether DCS's listener orientation equals the HMD orientation each frame, and its lag in frames, cannot be read
  statically [A]: the module logs `audio: listener-vs-pose` (angle between the listener's yaw rate and the head's) once
  a minute so the first live run settles it.

## 8. Settled and open questions of the design

| Design question | Answer |
|---|---|
| Meaning of the two per-channel floats | Gains from the wave's left and right channel; a mono wave uses their sum [V §3, §2.2] |
| Context ids | Not readable statically (WorldGeneral is protected); decided by context flags at run time, logged [A] |
| Listener axis convention | x forward, y up, z right; quaternion `(x,y,z,w)`, listener → world [V §7] |
| Stereo vs mono voices | `voice+0xec4` = wave channels; kernels differ per channel count [V §4.3] |
| `+0x108` = relative position | No: relative velocity; position is `+0x0d0` [V §4.2] |
| "Stream flag" `+0xec2` | No: "context is 2D" [V §4.3] |
| Eight kernels | Nine (`0x14de0` added) [V §2.2] |
| WorldGeneral IAT hooks | Not verifiable (protected image); not needed [§0] |
| New voices panned before the first block | Yes, at `0x1ba00`; hooked as P3b [V §1.2] |
| Mono gain rule (design's S2 question) | DCS's own gain for a frontal source, solved from DCS's two gains [§5.1] |

## 9. As implemented (task B, `native/dcsqvcull`)

Files: `src/audio_sigs.h` (generated), `src/audio_hooks.h` (sites, checks, thunks, patching), `src/audio_voice.h`
(layouts, mono gain rule, VoiceMeta), `src/audio_sim.h` (ownership, tag), `src/audio_render.h` (output rule, re-pan,
budget, events), `src/audio_config.h` (`[Audio]`), `src/main/audio.inc` (wrappers, install / restore, control thread).
Tests: `test/audio_test.h` with a fake ed_sound image (`test/audio_fake_ed.h`) and a stand-in core
(`test/audio_fake_core.cpp`); `QV_AUDIO_ONLY=1` runs them alone.

Where the implementation differs from or adds to the sections above:

- **Signatures (§0).** ed_sound has its own table, `src/audio_sigs.h`, written by `tools/gen_audio_sigs.py` from the
  DLL (`--check`: in step with the installed DCS; `--report`: where each location is after a DCS update). Each
  location's masked hash covers its whole function (the `.pdata` entry plus its chained entries). It is not in
  `hooks_manifest.json` / `hook_sigs.h`: `extract_manifest.py` stops in this tree at `binding pacer.h:pacer::kSiteRva:
  not found by the source scan`, so the shared manifest cannot be regenerated today.
- **Mono gain rule (§5.1).** A third equation makes the check real: WA + WB/2 = base/2 for any crosstalk, with
  base = speed factor x `sound+0x1c0`, computed exactly as the panner does (`sound+0x178`, `|sound+0x108|`, single
  precision, panner `0x1c208..0x1c2ae`). Lateral sources are solved from the two channels and checked against base;
  near the median plane (|cL - cR| < 0.05) WA comes from base and the channel whose speaker is further away, so no
  earlier frame is needed (k of an earlier frame is used only when base is not positive). Both also require
  WA >= 0.05 base and WB >= 0.2 base (x in [0, 1]). In the tests this flags 99.8 % of pan laws 10 % off on one
  channel; with only the two channels it flagged 14 %.
- **The tag.** m is floored at 1e-20 so a tag is never all zero (a silent owned voice stays recognisable).
- **Pose tag (§5.1).** `q_used` is the newest `Local\DcsVrHeadPose_v1` entry at the frame's first PanWrap (fresh within
  200 ms), not `DcsVrMotion_v1`: that block exists only with the cockpit mask export and a reader. DCS locates its views
  once per frame on its simulation thread before simulating (R74), so the newest entry is the pose of that frame. The
  `audio: listener-vs-pose` line (once a minute) settles the remaining lag.
- **Output rule (§5.2).** The tagged part is T = h HRTF + (1 - h) RePan, RePan(x) = x gL/m, x gR/m with DCS's own gains
  of that frame (published in VoiceMeta). h ramps over one block; the ownership fade and h never move in the same
  block, so every weight stays linear. The kill switch, `Binaural=0`, off for the session, MaxObjects and every release
  reach DCS's own panning within one block this way; the fake measures RePan at -107 dB from DCS's mix. A voice owned at
  creation (P3b) skips the fade: its first block is the core's. A core refusal in the middle of a voice switches to
  RePan without a crossfade (the contract refuses only on misuse: frames > 8192, slot out of range, no set).
- **Binding (§5.3, §6 item 6).** Also requires the 3D weights (1, 0) / (0, 1) of the stereo pair. Channels and rate are
  re-checked every block (a change re-binds). Blocks over 8192 frames pass through. While the bound output's blocks
  pass through, the sim releases every voice (a tagged voice that is not rendered would play centred) and, for blocks
  too long, the module logs `audio: off (DCS's audio blocks are N frames, ...)`, and `audio: on (DCS's audio blocks fit
  again, ...)` once they do. Test: `LongBlocks`.
- **Torn snapshot (§5.2).** DCS's panner stores its gains and the sim overwrites them with the tag right after; the
  render thread does not take ED's mutex, so a voice render in between snapshots DCS's stereo gains for one block (and
  ramps back to the tag in the next). For an owned voice that is not a release: the block stays tagged, the mono signal
  is recovered from the known ramp of the gain sum (tag 2m to gL + gR and back), counted as `torn (snapshots N)` in
  the 10 s line. Test: `TornSnapshot`.
- **Late translation.** Only when both poses (DCS's frame and now) have a tracked position (`meta_flags::kPosValid`,
  `PoseNow::posValid`; a missing one is stored as zero), clamped to 30 cm.
- **Install** waits for ed_sound's engine state >= 1 (nothing locked or patched before its engine started), and refuses
  if the thunk page cannot be made executable. **Stop** frees the bindings only after a 50 ms grace period with no
  wrapper call in flight (a call that passed a site just before it was restored counts itself only inside the
  wrapper); otherwise it leaves them in place.
- **Two outputs.** ed_sound makes one or two outputs (`num_outputs` 1-2 at `0x1ed44`; the second is options.lua
  `sound.hp_output`), each an `SND_Wasapi` with its own thread calling P5. One renderer is bound at a time: while it
  delivered a block within 200 ms another renderer is bound only when the world's 3D sounds play on it, and then only
  after the voices the bound one renders binaurally faded back to DCS's panning there (the sim releases them; at most
  500 ms), so the old output never plays a tagged voice centred. Each binding logs `audio: output <address> bound
  (<why>)`, so the two outputs can be told apart in the log (test: `Handover`). The sim counts,
  per panner call, the renderer of every 3D sound in a non-2D context (`audsim::Sim::world`); the control thread adopts
  the busiest one after it led two seconds in a row (`WorldTick`) and binds it once. The other output's blocks then pass
  through without waking the control thread. Test: `TwoOutputs` (main bound first, and the headphones output first).
- **Nothing to render with.** ed_sound is patched only when a set is there: the named one or the generic resource
  (`HrtfReady`); otherwise `audio: off (no HRTF set: ...)` and the four sites stay stock. Test: `NoSetNoHooks`.
- **Overruns (§6 item 7).** ED's counter is read at renderer - 8 + 0x2274, accepted only when the `lock inc` at P5 +
  0x3e reads `F0 FF 83 74 22 00 00`.
- **Threads.** The module has its own control thread (`AudioThread`, 20 ms loop): independent of the engine worker
  (which can wait minutes for Scene.dll) and of the engine kill switch. The real-time wrappers never log; they count and
  push events to a ring this thread logs from.
- **FTZ/DAZ** are set only around our own DSP in each P4 call and restored before ED's next kernel runs.
- **Status word** bits 53 (configured), 54 (active), 55 (configured but off); see `status_word.h`.
- **Build.** The payload always links BinauralCore, compiled from `native/binaural/src` by the module's own CMake
  (target `DcsQvBinauralCore`; `native/binaural` is not added as a subdirectory, its harness runs in
  `scripts/build-binaural.ps1`). The default set is embedded as RCDATA `HRTF_DEFAULT` when
  `native/binaural/data/sadie2_ku100.dcshrtf` exists; without it the module logs
  `audio: off (no HRTF set: this build has no default HRTF set)`, which the Audio page shows as the reason.
- **Tests.** `DcsQvCullTests` runs the module's scenarios against the stand-in core (`test/audio_fake_core.cpp`, a core
  whose output and refusals the tests control); `DcsQvCullAudioE2ETests` (`test/audio_e2e_test.cpp`) runs the module
  on the fake image with the real core and the harness's synthetic spherical-head set: a source circling the head
  gives the core's own ITD / ILD trajectory (Woodworth for the configured head), the radio stays centred, nothing is
  allocated on the audio threads. Both share the rig in `test/audio_rig.h` and the allocation counter in
  `test/audio_rt_alloc.cpp`.
- **Head pose.** Both reads (the pose DCS used, at the frame's first panner call; the newest pose predicted to the
  block's end, on the render thread) go through `head_pose_reader.h` `ReadPredicted`: 150 ms staleness, writer = this
  process, speed guards, 50 ms horizon. The running line ends `head pose on|off|unused`.
- **LogVoices** writes `audio_voices.csv` next to the payload (Back to stock removes `audio_*.csv`).
