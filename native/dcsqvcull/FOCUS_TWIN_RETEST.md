# FocusTwin retest (R150)

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
