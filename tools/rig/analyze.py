# A6: numbers of one run from its logs. Usage: py analyze.py <run folder> <HH:MM:SS start> <HH:MM:SS end> [--json out]
# Reads (whatever is there): ofxr-bridge-flight-*.log (newest), DcsQvCull.log, pimax-fps*.txt. Window = wall clock.
import sys, re, os, glob, json, collections, statistics as st
run, w0, w1 = sys.argv[1], sys.argv[2], sys.argv[3]
out = {}
def secs(hms):
    h, m, s = hms.split(':'); return int(h) * 3600 + int(m) * 60 + float(s)
W0, W1 = secs(w0), secs(w1)
def q(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p / 100))] if v else None
P = print
P(f'## {os.path.basename(os.path.normpath(run))}  window {w0}-{w1} ({W1 - W0:.0f} s)')
out['window'] = [w0, w1]; out['window_s'] = W1 - W0

# ---------------- engine module (DcsQvCull.log) ----------------
p = os.path.join(run, 'DcsQvCull.log')
if os.path.exists(p):
    fps = []; p50 = []; p95 = []; verd = collections.Counter(); gov = []; rt = []; gpu = []; lines = []
    rx = re.compile(r'^(\d\d:\d\d:\d\d\.\d+) perf: fps=([\d.]+) ft p50/p95=([\d.]+)/([\d.]+) ms render-thread busy=(\d+)% gpu util=(\d+)%.*?verdict=(.*?) \(')
    for i, l in enumerate(open(p, encoding='utf-8', errors='replace'), 1):
        m = rx.match(l)
        if m and W0 <= secs(m.group(1)) <= W1:
            fps.append(float(m.group(2))); p50.append(float(m.group(3))); p95.append(float(m.group(4))); rt.append(int(m.group(5))); gpu.append(int(m.group(6))); verd[m.group(7)] += 1; lines.append(i)
        elif 'governor: capped at' in l and re.match(r'\d\d:\d\d:\d\d\.', l) and W0 <= secs(l[:12]) <= W1: gov.append((i, l.strip()[:120]))
    P(f'engine perf lines: {len(fps)} (DcsQvCull.log L{lines[0] if lines else 0}-L{lines[-1] if lines else 0})')
    if fps:
        P(f'  fps min/median/max: {min(fps):.1f} / {st.median(fps):.1f} / {max(fps):.1f}')
        P(f'  frame time p50 (median of lines) {st.median(p50):.1f} ms; p95 (median of lines) {st.median(p95):.1f} ms; worst p95 {max(p95):.1f} ms')
        P(f'  render thread busy median {st.median(rt):.0f}%  gpu util median {st.median(gpu):.0f}%')
        P('  verdicts: ' + ', '.join(f'{n} x "{v}"' for v, n in verd.most_common()))
    P(f'  "governor: capped at" lines: {len(gov)}' + ''.join(f'\n    L{i}: {t}' for i, t in gov[:8]))
    out['engine'] = dict(lines=len(fps), fps_min=min(fps) if fps else None, fps_median=st.median(fps) if fps else None, fps_max=max(fps) if fps else None,
                         ft_p50=st.median(p50) if fps else None, ft_p95=st.median(p95) if fps else None, ft_p95_worst=max(p95) if fps else None,
                         verdicts=dict(verd), governor_capped=len(gov), governor_lines=[t for _, t in gov], fps_list=fps)

# ---------------- Pimax server ----------------
for p in glob.glob(os.path.join(run, 'pimax-*fps*.txt')):
    a = []; c = []; ma = []; sa = []
    rx = re.compile(r'^\[\d\d-\d\d-\d\d (\d\d:\d\d:\d\d\.\d+)\].*rendering fps:\(a:([\d.]+),c:([\d.]+)\) Missed:\(a:(\d+),c:\d+,bl:\d+\) Stale:\(a:(\d+)\)')
    for l in open(p, encoding='utf-8', errors='replace'):
        m = rx.match(l)
        if m and W0 <= secs(m.group(1)) <= W1: a.append(float(m.group(2))); c.append(float(m.group(3))); ma.append(int(m.group(4))); sa.append(int(m.group(5)))
    if a:
        P(f'Pimax server ({os.path.basename(p)}, {len(a)} s): app submissions a mean {st.mean(a):.2f} (min {min(a):.1f}), compositor c mean {st.mean(c):.2f}, Missed a mean {st.mean(ma):.2f}/s, Stale a mean {st.mean(sa):.2f}/s')
        out['pimax'] = dict(seconds=len(a), a_mean=st.mean(a), a_min=min(a), c_mean=st.mean(c), missed_mean=st.mean(ma), stale_mean=st.mean(sa))

# ---------------- OFXR flight log ----------------
logs = sorted(glob.glob(os.path.join(run, 'ofxr-bridge-flight-*.log')), key=os.path.getmtime)
if not logs:
    P('no OFXR flight log in the run folder (frame generation off or log off)')
else:
    path = logs[-1]; name = os.path.basename(path)
    m = re.search(r'flight-(\d{8})-(\d\d)(\d\d)(\d\d)-pid', name); L0 = int(m.group(2)) * 3600 + int(m.group(3)) * 60 + int(m.group(4))
    t0, t1 = (W0 - L0) * 1000, (W1 - L0) * 1000
    P(f'OFXR log {name} ({os.path.getsize(path) / 1e6:.0f} MB); log starts {m.group(2)}:{m.group(3)}:{m.group(4)}; window = log ms {t0:.0f}-{t1:.0f}')
    rx = re.compile(r'seq=(\d+) ms=([\d.]+) tid=(\d+) phase=(\w) op=(\w+) result=(-?\d+) dur_us=(-?\d+) a=(\d+) b=(\d+) c=(\d+)')
    trans = collections.Counter(); t410 = []; t411 = []; trans_all = collections.Counter()
    endB = None; lastE = None; subs = []; waits = []; appE = []; thr = []; switch = collections.Counter(); frames = collections.Counter(); sub_t = []
    U64 = 1 << 64
    with open(path, encoding='utf-8', errors='replace') as fh:
        for ln, line in enumerate(fh, 1):
            m = rx.match(line)
            if not m: continue
            seq, ms, tid, ph, op, res, dur, a, b, c = m.groups(); ms = float(ms); res = int(res)
            if op == 'presenter_transition':
                trans_all[res] += 1
                if res == 410: t410.append((ln, ms, int(a), int(b), int(c)))
                elif res == 411:
                    bb = int(b); bb = bb - U64 if bb >= (1 << 63) else bb
                    t411.append((ln, ms, int(a), bb, int(c)))
            if ms < t0 or ms > t1: continue
            if op == 'presenter_transition': trans[res] += 1
            elif op == 'internal_end_frame':
                if ph == 'B': endB = ms
                else: lastE = (endB, ms, int(dur))
            elif op == 'presenter_submission' and lastE: subs.append(lastE + (int(c),)); lastE = None
            elif op == 'internal_wait_frame' and ph == 'E': waits.append((ms, int(a), int(b)))
            elif op == 'app_end_frame' and ph == 'E': appE.append(ms)
            elif op == 'adaptive_throughput' and res == 6: thr.append(int(a))
            elif op == 'adaptive_switch': switch[res] += 1
            elif op == 'frame': frames[int(c) & 0xff] += 1
            elif op == 'submission_timing' and res == 1: sub_t.append((int(a), int(b)))
    o = out['ofxr'] = {'log': name}
    P('  presenter_transition in window: ' + (', '.join(f'{k} x{v}' for k, v in sorted(trans.items())) or 'none') + '   | whole log: ' + (', '.join(f'{k} x{v}' for k, v in sorted(trans_all.items())) or 'none'))
    o['transition_window'] = dict(trans); o['transition_log'] = dict(trans_all)
    P(f'  410 records (whole log): {len(t410)}' + ''.join(f'\n    L{ln} t={ms / 1000:.1f}s a={a} b={b} c={c}' for ln, ms, a, b, c in t410[:12]))
    codes = collections.Counter(x[4] for x in t411)
    P(f'  411 records (whole log): {len(t411)}; c: ' + (', '.join(f'{k} x{v}' for k, v in sorted(codes.items())) or 'none'))
    for ln, ms, a, b, c in [x for x in t411 if x[4] != 0][:20]: P(f'    L{ln} t={ms / 1000:.1f}s lead a={a} us lateness b={b} us c={c}')
    in411 = [x for x in t411 if t0 <= x[1] <= t1]
    if in411:
        P(f'    in window: {len(in411)}; lead a us min/median/max {min(x[2] for x in in411)} / {st.median([x[2] for x in in411]):.0f} / {max(x[2] for x in in411)}; lateness b us p10/p50/p90 {q([x[3] for x in in411], 10)} / {q([x[3] for x in in411], 50)} / {q([x[3] for x in in411], 90)}')
        o['lead_us_median'] = st.median([x[2] for x in in411]); o['late_us_p50'] = q([x[3] for x in in411], 50)
    o['n410'] = len(t410); o['t410'] = [dict(line=ln, t=ms / 1000, b=b) for ln, ms, a, b, c in t410]; o['n411'] = len(t411); o['c411'] = dict(codes)
    for kind, label in ((1, 'real (c=1)'), (2, 'synthetic (c=2)')):
        d = [s[2] / 1000 for s in subs if s[3] == kind]
        if d: P(f'  runtime xrEndFrame {label}: n {len(d)}  p50 {q(d, 50):.2f}  p90 {q(d, 90):.2f}  p99 {q(d, 99):.2f} ms; over 8 ms: {sum(1 for x in d if x > 8)}')
        o[f'end_kind{kind}'] = dict(n=len(d), p50=q(d, 50), p90=q(d, 90), over8=sum(1 for x in d if x > 8))
    first = [subs[j][2] / 1000 for j in range(1, len(subs)) if subs[j][3] == 2 and subs[j - 1][3] == 1]
    if first:
        n8 = sum(1 for x in first if x > 8)
        P(f'  FIRST synthetic xrEndFrame (c=2 right after c=1): n {len(first)}  p50 {q(first, 50):.2f}  p90 {q(first, 90):.2f}  p99 {q(first, 99):.2f} ms; over 8 ms: {n8} ({100 * n8 / len(first):.1f} %)')
        o['first_synth'] = dict(n=len(first), p50=q(first, 50), p90=q(first, 90), p99=q(first, 99), over8=n8)
    if len(waits) > 2:
        per = collections.Counter(w[2] for w in waits).most_common(1)[0][0] or 11111128
        steps = collections.Counter(round((waits[i + 1][1] - waits[i][1]) / per) for i in range(len(waits) - 1))
        tot = sum(steps.values())
        P(f'  presenter turns: {len(waits)}; display-time step in periods: ' + ', '.join(f'{k} x{v} ({100 * v / tot:.1f} %)' for k, v in sorted(steps.items())) + f'; period {per} ns')
        ps = collections.Counter(int(w[0] // 1000) for w in waits); v = [ps[s] for s in range(int(t0 // 1000) + 1, int(t1 // 1000))]
        P(f'  presenter turns per second p10/p50/p90: {q(v, 10)} / {q(v, 50)} / {q(v, 90)}')
        o['steps'] = {int(k): v for k, v in steps.items()}; o['turns_per_s_p50'] = q(v, 50)
    if appE:
        ps = collections.Counter(int(x // 1000) for x in appE); v = [ps[s] for s in range(int(t0 // 1000) + 1, int(t1 // 1000))]
        def lvl(n):
            if n >= 80: return '90 fps (1X)'
            if 40 <= n <= 50: return '45 fps (90/2)'
            if 33 <= n <= 39: return '33-39 fps (2X, slots lost)'
            if 27 <= n <= 32: return '30 fps (90/3)'
            if 21 <= n <= 24: return '22.5 fps (90/4, the lock)'
            return 'other'
        lv = collections.Counter(lvl(n) for n in v)
        P(f'  DCS frames per second (app xrEndFrame), {len(v)} s: min/median/max {min(v)} / {st.median(v)} / {max(v)}; mean {st.mean(v):.2f}')
        for k, n in lv.most_common(): P(f'    {k}: {n} s ({100 * n / len(v):.1f} %)')
        iv = [appE[i + 1] - appE[i] for i in range(len(appE) - 1)]
        P(f'  DCS frame interval (xrEndFrame to xrEndFrame): p50 {q(iv, 50):.2f}  p95 {q(iv, 95):.2f}  p99 {q(iv, 99):.2f} ms')
        o['dcs_fps'] = dict(seconds=len(v), median=st.median(v), mean=st.mean(v), levels=dict(lv), locked_s=lv.get('22.5 fps (90/4, the lock)', 0), iv_p50=q(iv, 50), iv_p95=q(iv, 95))
    if thr:
        tot = [0, 0, 0, 0]; pat = collections.Counter()
        for a in thr:
            fl = [(a >> s) & 0xffff for s in (0, 16, 32, 48)]
            for i in range(4): tot[i] += fl[i]
            pat['|'.join(map(str, fl))] += 1
        s = sum(tot) or 1
        P(f'  adaptive_throughput 6 ({len(thr)} s): frames 1X {tot[0]} ({100 * tot[0] / s:.1f} %) | cadence 2 {tot[1]} ({100 * tot[1] / s:.1f} %) | cadence 3 {tot[2]} ({100 * tot[2] / s:.1f} %) | late {tot[3]} ({100 * tot[3] / s:.1f} %)')
        P('    most frequent seconds (1X|c2|c3|late): ' + ', '.join(f'{k} x{v}' for k, v in pat.most_common(5)))
        o['throughput'] = dict(seconds=len(thr), x1=tot[0], c2=tot[1], c3=tot[2], late=tot[3], top=pat.most_common(5))
    if frames:
        tot = sum(frames.values()); per = sum(k * v for k, v in frames.items()) or 1
        P('  per-frame cadence (op=frame, c bits 0-7): ' + ', '.join(f'{k}: {v} frames ({100 * v / tot:.1f} % of frames, {100 * k * v / per:.1f} % of presenter turns)' for k, v in sorted(frames.items())))
        o['frame_cadence'] = {int(k): v for k, v in frames.items()}
    P('  adaptive_switch in window: ' + (', '.join(f'{k} x{v}' for k, v in sorted(switch.items())) or 'none'))
    o['switch'] = dict(switch)
    if sub_t:
        r = [a & 0xffff for a, b in sub_t]; g1 = [(a >> 16) & 0xffff for a, b in sub_t]; g2 = [(a >> 32) & 0xffff for a, b in sub_t]
        P(f'  submission_timing per second: real mean {st.mean(r):.1f}, gen1 {st.mean(g1):.1f}, gen2 {st.mean(g2):.1f}')
if '--json' in sys.argv: json.dump(out, open(sys.argv[sys.argv.index('--json') + 1], 'w'), indent=1)
