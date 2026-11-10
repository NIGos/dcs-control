# FocusTwin retest (R150)

> **Outcome (R151, 2026-11-11): SHELVED.** G1 failed and the cost was then measured directly; see
> [R151 results](#r151-results-2026-11-11) at the end. Defaults are unchanged (`FocusTwin=0`).

One live session decides whether FocusTwin stays shelved. Everything here is opt-in; the defaults are unchanged
(`[Model] FocusTwin=0`, `FocusTwinInstancer=0`).

## What changed since R124 (why it never engaged)

R124 (airfield and germany, headset on the table, sim paused) saw twin rate 0 %: every focus pass was
"no prediction (no history)", and every periphery item was "not twinnable: other work".

1. **No history.** The R124 note blamed the collect-sequence test before `LearnFocus`. R124's own counters rule
   that out. A sequence mismatch is counted as "stale frame", and there were none: every pass got past that
   test. So `LearnFocus` ran on every focus pass and failed. It needs the CollectionInfo view-projection
   (`ci+0x198`) to be an affine of the uploaded TerrainContext rows. The offline tests built those cameras *from the
   uploads* (`CiFromTc`), so this was never checked against DCS. Live it never held, even with a still pose.
   - **Fix:** the decision no longer needs that camera. When there is no prediction, the decision uses DCS's own
     focus upload. It is already uploaded when the twin decides, and the twin's draws read GPU copies of DCS's own
     focus buffers. The checks are: shared rows byte-equal, and `RelateFocusTc` (the focus rows are one consistent
     magnified affine of this eye's periphery: VP, inverse P, side planes, within float round-off). When a camera
     prediction does exist, it is still checked as before.
   - Exactness is still guarded by the descriptors, the trust and periodic audits, and `FocusTwinVerify`.
2. **Pairing.** A focus pass now pairs with its eye's periphery stream through a per-frame latch, not by comparing
   collect sequences. This covers the audit's hypothesis, a collect published between the periphery and the
   focus. The diag line counts how often that actually happens.
3. **"Other work".** This reason used to lump together descriptor read failures, duplicate descriptors and any
   non-draw call. These are now separate reasons, and the diag line names the context call behind each "other
   work" item. Calls that set state already bound are now let through, because they are exact no-ops:
   - `SetPredication(NULL)` with no predicate bound;
   - the same viewports or scissors;
   - the same render targets with no UAVs bound.

   Before, each of these made the item untwinnable, and in a focus pass it stopped the twin.
4. If the focus targets alternate between two sets, the command lists are built for the set of two frames back.
   They are still checked when used.

## Setup

- Build and install the payload the usual way (DCS closed while installing). Keep the Pimax client closed until the
  run.
- Scene: **`qvbench_airfield` F2, yaw 285, sim paused (CMD 52), pose held**. This is R124's CPU-bound scene: render
  thread 96-100 %, about 50 fps. The decision is made on render-thread ms, so a GPU-bound scene cannot show it.
- Run ini through `[Dev] IniPath` (R124 method). Keys per phase:

| Phase | Keys | Length |
|---|---|---|
| G1 engage + shadow | `[Model] FocusTwin=1`, `FocusTwinInstancer=1`, `[Suite] FocusTwinShadow=1` | 2 min |
| G2 verify | `FocusTwin=1`, `FocusTwinInstancer=1`, `[Suite] FocusTwinVerify=8` (Shadow 0) | 2 min |
| P perf ABBA | `FocusTwin` 0 / 1 (Instancer 1, Shadow 0, Verify 0), hot switch, 40 s blocks, 25 s settle | 8+ pairs |

## Log lines (DcsQvCull.log, every 30 s)

- `focus twin: <s> s, <n> focus passes: twin <n> (<%>) ...`: the existing full line (audits, verify bytes, GPU).
- `focus twin summary: twin rate X % of N focus passes, Y items twinned per focus pass (Z % of the focus items);
  render thread per frame: saved A us, added B us (tee T, focus F), net +-C us; fallbacks: <top 3>`
- `focus twin diag: decided by camera prediction a, by own-row relation b (relation failed c, ...); prediction
  unavailable: ...; camera history learned n, not: <reason counts>; collect sequence moved between periphery and
  focus m; other work by call: <call counts>; no-op calls let through: ...; lists for alternating targets k`

## GO / NO-GO, in this order (stop at the first fail)

1. **Engages (G1).** The full line shows `audits <n> ok / 0 failed`, with n > 0, so items are being compared. The
   diag line shows `by own-row relation` > 0 (or camera prediction > 0), and `relation failed` stays under 1 % of
   the focus passes.
   - If audits stay at 0, read `other work by call` and the `periphery items ... not twinnable` reasons. They name
     the blocking call or reason. That is the next fix, and the result is still NO-GO.
2. **Exact (G2).** `verify <n> (0 failed, 0 of <bytes> bytes differ ...)` with n > 0. There must be no line
   `focus twin: audit of eye ... drawn with other inputs` and no line `verify of eye ... differ`.
   - Any difference latches the feature off, and the result is NO-GO.
3. **Twin rate (P, FocusTwin=1 blocks).** `twin rate` > 90 % in the summary line. With the default
   `FocusTwinAuditEvery=64` and `FocusTwinTrust=4`, about 2 % of passes are audits.
   - If the rate is lower, the summary's `fallbacks` name the cause (e.g. `targets`, `late`, `periphery unusable`).
4. **Net.** On the summary line, `net` must be ≥ +300 us per frame, averaged over the FocusTwin=1 blocks.
   - It must be confirmed by the ABBA render-thread ms: Δ ≤ −0.3 ms with the 95 % CI below 0, over 8+ pairs.
   - GPU ms Δ must be ≤ +0.05 ms (the full line's `GPU focus terrain: twin ... stock ...`, plus the ABBA GPU ms).
   - Otherwise NO-GO: the tee (R124: 0.9-1.2 ms) eats the saving. Record `tee` and `saved` per frame for the
     per-eye geometry decision.

Record for the report: the three lines from one steady 30 s window of each phase, the ABBA table, and the
`other work by call` list even on GO (it shows what still stays DCS's).

## Restore

Remove the run ini keys (or restore the ini by hash), close DCS and the Pimax client. Nothing is written outside the
DcsQvCull ini and log.

## R151 results (2026-11-11)

Three short runs: DCS 2.9.31.29434, `qvbench_airfield`, F2, sim paused, the user's profile with frame generation off,
dev payload and dev ini. One PC, one scene.

**1. G1 fails: twin rate 0 %, 0 audits.** Over 4 windows of 30 s, about 98 % of the focus passes were "periphery
unusable" and the rest "no periphery". R151 added diagnostics (`focus twin listen:` line every 30 s), which name two
independent blockers. Neither involves the camera, which R150 fixed.

- **Periphery unusable = "shared write", 100 %.** A draw outside the patch items reads a constant buffer a patch
  item wrote.
  - The suspected false positive turned out not to be the cause. That suspect was a `Map(WRITE_DISCARD)` outside
    items not ending an item's claim; it is now handled, but the counter stayed at 0.
  - With `FocusTwinEarlyStop=1`, every periphery tee stops before the first instancer item. So the shared read sits
    between the last terrain patch and the first tree/building item.
- **Descriptors: every failure is the geometry stream vector.** About 220 patch and 290 instancer items per pass
  fail, at `[p0 + 8, p0 + 0x10)` of the MetaShaderRenderItem.
  - The spans are random 64-bit values (min −9.2·10¹⁸, max 9.2·10¹⁸, one exactly 2³²). The R123 layout assumption
    for the geometry object does not hold on 2.9.31.
  - edterrainGraphics41 itself is unchanged, and every other descriptor read passes.

**2. Listening cost, measured directly.** `FocusTwinTapBaseline=1` runs every other periphery pass untapped and
compares, with `FocusTwinEarlyStop=0` and about 1,160 passes of each kind per 30 s:

| | teed | untapped |
|---|---|---|
| patch item | 1.36–1.44 µs | 0.99–1.11 µs |
| instancer item | 1.20–1.30 µs | 0.99–1.15 µs |
| periphery terrain window (~745 items) | 1,371–1,447 µs | 938–1,043 µs |

- Listening costs **404–433 µs per periphery pass, about 0.85 ms per frame**. This includes R151's hashed writer
  lookup.
- The R124/R150 "taps" estimate (1.1–1.5 ms/frame) was inflated: periphery items are slower than focus items even
  untapped (cold caches, about 1.05 vs 0.85 µs).
- The saving ceiling is about 0.9 ms/frame: per eye, about 237 patch and 293 instancer focus items at about
  0.85 µs each.
- **The net is about zero before the focus-side cost.** GO (+0.3 ms) would need listening 3–4× cheaper.

**Decision: shelved.** The code stays opt-in and off.

If it is ever reopened, these are the steps, in order:

1. Re-derive the 2.9.31 geometry layout behind the MetaShaderRenderItem.
2. Name the shared buffer and slot read between the patches and the instancer.
3. Find a cheaper tee (state deltas only for candidate items).

Logs: `artifacts/r150/` (local, not tracked).

New suite keys (default behaviour unchanged):

- `[Suite] FocusTwinTapBaseline` (0): untapped, timed periphery passes for the cost comparison.
- `[Suite] FocusTwinEarlyStop` (1): an unusable periphery stops its tee for the rest of the window.
