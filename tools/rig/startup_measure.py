# Four-points round (2026-11-16), point 2: the start-up hitch at the mission entry, from OFXR's flight log. CPU only.
#   py startup_measure.py <run folder or log> [...]  [--n 180] [--json out.json]
# Definitions:
#   load frame   the application's first xrEndFrame after its last pause of over 5 s before the presenter starts
#   t0           start of the presenter's first runtime xrEndFrame;  tN = end of its 180th
#   stall        tN - t0 - 180 display periods
#   held         how long the layer held the application's xrEndFrame (app_end_frame E dur_us) for the frames between
#                the load frame and t0 (the two frames before the presenter: pass-through, prime)
#   runtime call the runtime's xrEndFrame of those frames (downstream_first_end_frame E dur_us)
import sys, re, json, glob, os

PERIOD = 1000.0 / 90.0
rx = re.compile(r'seq=(\d+) ms=([\d.]+) tid=(\d+) phase=(\w) op=(\w+) result=(-?\d+) dur_us=(\d+) a=(-?\d+) b=(-?\d+) c=(-?\d+)')
KEEP = ('app_end_frame', 'internal_end_frame', 'presenter_submission', 'downstream_first_end_frame', 'slow_step', 'presenter_transition')


def load(path):
    if os.path.isdir(path):
        logs = sorted(glob.glob(os.path.join(path, 'ofxr-bridge-flight-*.log')), key=os.path.getsize, reverse=True)
        if not logs:
            raise SystemExit(f'no flight log in {path}')
        path = logs[0]
    rec = []
    with open(path, 'r', errors='replace') as f:
        for line in f:
            if not any(k in line for k in KEEP):
                continue
            m = rx.search(line)
            if m:
                rec.append((float(m[2]), int(m[3]), m[4], m[5], int(m[6]), int(m[7]), int(m[8]), int(m[9]), int(m[10])))
    return path, rec


def measure(path, n=180):
    path, rec = load(path)
    subs = [r for r in rec if r[3] == 'presenter_submission']
    if len(subs) < n:
        return {'log': path, 'error': f'only {len(subs)} presenter submissions'}
    ptid = subs[0][1]
    pend = [r for r in rec if r[3] == 'internal_end_frame' and r[1] == ptid and r[2] == 'E']
    pbeg = [r for r in rec if r[3] == 'internal_end_frame' and r[1] == ptid and r[2] == 'B']
    t0 = pbeg[0][0]
    tN = subs[n - 1][0]
    appB = [r for r in rec if r[3] == 'app_end_frame' and r[2] == 'B']
    appE = {(r[1], r[7]): r for r in rec if r[3] == 'app_end_frame' and r[2] == 'E'}       # by tid and display time
    atid = appB[0][1]
    before = [r for r in appB if r[0] <= t0]
    li = 0
    for i in range(1, len(before)):
        if before[i][0] - before[i - 1][0] > 5000:
            li = i
    loadms = before[li][0]

    def frame(r):
        e = appE.get((r[1], r[7]))
        dn = [x for x in rec if x[3] == 'downstream_first_end_frame' and x[2] == 'E' and r[0] <= x[0] <= (e[0] if e else r[0] + 2000)]
        return {'begin': r[0], 'heldMs': (e[5] / 1000.0 if e else None), 'end': (e[0] if e else None), 'runtimeMs': (dn[0][5] / 1000.0 if dn else None)}
    briefing = [frame(r) for r in before[:li]]
    entry = [frame(r) for r in before[li:]]
    slow = [r for r in rec if r[3] == 'slow_step']
    s2 = [r for r in slow if r[4] == 2 and loadms <= r[0] <= tN]
    s2b = [r for r in slow if r[4] == 2 and r[0] < loadms]
    s9 = [{'ms': r[0], 'where': ('briefing' if r[0] < loadms else 'entry'), 'layersAdded': r[7], 'imagesReleased': r[8]} for r in slow if r[4] == 9]
    first = pend[:n]
    slowp = [r[5] / 1000.0 for r in first if r[5] >= 4000]
    # completions of runtime xrEndFrame calls from the load frame to tN: the entry frames on the application's thread, then the presenter's
    ev = sorted([x[0] for x in rec if x[3] == 'downstream_first_end_frame' and x[2] == 'E' and loadms <= x[0] < t0 and x[5] > 1000] + [r[0] for r in first])
    gaps_all = [(ev[i] - ev[i - 1], ev[i - 1] - loadms) for i in range(1, len(ev))]
    pe = [r[0] for r in first]
    gaps_p = [(pe[i] - pe[i - 1], pe[i - 1] - t0) for i in range(1, len(pe))]
    new = [s[0] for s in subs[:n] if s[8] != 0]
    gaps_new = [(new[i] - new[i - 1], new[i - 1] - t0) for i in range(1, len(new))]
    over = [g for g in gaps_p if g[0] > 2.5 * PERIOD]
    synth = [s[0] for s in subs if s[8] == 2]
    kinds = {0: 0, 1: 0, 2: 0}
    for s in subs[:n]:
        kinds[s[8]] = kinds.get(s[8], 0) + 1
    t406 = len([r for r in rec if r[3] == 'presenter_transition' and r[4] == 406 and t0 <= r[0] <= tN])
    return {
        'log': path, 'loadFrameMs': loadms, 't0': t0, 'tN': tN,
        'briefingFrames': briefing, 'entryFrames': entry,
        'dcsGapEntry1to2Ms': (entry[1]['begin'] - entry[0]['end'] if len(entry) > 1 and entry[0]['end'] else None),
        'runtimeSwapchainsRemadeAtEntry': {'count': len(s2), 'ms': sum(r[5] for r in s2) / 1000.0},
        'runtimeSwapchainsMadeAtBriefing': {'count': len(s2b), 'ms': sum(r[5] for r in s2b) / 1000.0},
        'warmRecords': s9,
        'presenterSlowCalls': {'count': len(slowp), 'sumMs': sum(slowp), 'list': [round(x) for x in slowp]},
        'stallMs': tN - t0 - n * PERIOD,
        'loadToFirstSubmissionMs': t0 - loadms, 'loadToNthMs': tN - loadms,
        'longestGapLoadToNth': max(gaps_all) if gaps_all else None,
        'longestGapPresenter': max(gaps_p) if gaps_p else None,
        'longestGapNewImages': max(gaps_new) if gaps_new else None,
        'lastGapOver2_5PeriodsEndsAt': (max(g[1] + g[0] for g in over) if over else 0),
        'gapsOver2_5Periods': len(over),
        'firstSyntheticAfterT0Ms': (min(x for x in synth if x >= t0) - t0 if synth else None),
        'first180': {'repeat': kinds[0], 'real': kinds[1], 'synthetic': kinds[2], 'held406': t406},
    }


def f(x, d=0):
    return '-' if x is None else f'{x:.{d}f}'


if __name__ == '__main__':
    args = sys.argv[1:]
    out = None; n = 180
    if '--json' in args:
        i = args.index('--json'); out = args[i + 1]; del args[i:i + 2]
    if '--n' in args:
        i = args.index('--n'); n = int(args[i + 1]); del args[i:i + 2]
    res = {}
    for p in args:
        name = os.path.basename(os.path.normpath(p))
        r = measure(p, n); res[name] = r
        print(f'== {name}')
        if 'error' in r:
            print('   ' + r['error']); continue
        e = r['entryFrames']
        print(f"   briefing frames, held ms (runtime call ms): " + ', '.join(f"{f(x['heldMs'])} ({f(x['runtimeMs'])})" for x in r['briefingFrames']))
        print(f"   entry frames before the presenter, held ms (runtime call ms): " + ', '.join(f"{f(x['heldMs'])} ({f(x['runtimeMs'])})" for x in e) + f"; DCS's own pause between the first two {f(r['dcsGapEntry1to2Ms'])} ms")
        print(f"   runtime swapchains made again at entry: {r['runtimeSwapchainsRemadeAtEntry']['count']} x, {f(r['runtimeSwapchainsRemadeAtEntry']['ms'])} ms")
        print(f"   warm records (slow_step 9): " + (', '.join(f"{w['where']} +{w['layersAdded']} layers / {w['imagesReleased']} images" for w in r['warmRecords']) or 'none'))
        print(f"   presenter runtime calls of 4 ms or more in the first {n}: {r['presenterSlowCalls']['count']}, {f(r['presenterSlowCalls']['sumMs'])} ms: {r['presenterSlowCalls']['list']}")
        print(f"   stall of the first {n} submissions: {f(r['stallMs'])} ms; load frame to first submission {f(r['loadToFirstSubmissionMs'])} ms, to the {n}th {f(r['loadToNthMs'])} ms")
        g = r['longestGapPresenter']; g2 = r['longestGapNewImages']; g3 = r['longestGapLoadToNth']
        print(f"   longest gap between presenter submissions {f(g[0])} ms (at +{f(g[1])}); between new images {f(g2[0])} ms (at +{f(g2[1])}); between runtime calls from the load frame {f(g3[0])} ms (at load +{f(g3[1])}); gaps over 2.5 periods {r['gapsOver2_5Periods']}, the last ends at +{f(r['lastGapOver2_5PeriodsEndsAt'])}")
        k = r['first180']
        print(f"   first {n}: real {k['real']}, synthetic {k['synthetic']}, repeat {k['repeat']}, 406 x{k['held406']}; first synthetic at +{f(r['firstSyntheticAfterT0Ms'])} ms")
    if out:
        json.dump(res, open(out, 'w'), indent=1)
