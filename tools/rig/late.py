# late synthetics: per-submission reconstruction from an OFXR flight log, written as a pickle for further analysis.
# Usage: py late.py <flight log> <t0 s> <t1 s> <out.pickle>
# The fourth argument is the OUTPUT. (The scratch version documented it as "[pimax log]" and wrote the pickle over
# whatever file was named there.) It must end in .pickle or .pkl and is never an existing file of another kind.
import sys, os, re, collections, pickle
if len(sys.argv) != 5:
    sys.exit('usage: py late.py <flight log> <t0 s> <t1 s> <out.pickle>')
path, T0, T1, out = sys.argv[1], float(sys.argv[2]) * 1000, float(sys.argv[3]) * 1000, sys.argv[4]
if not out.lower().endswith(('.pickle', '.pkl')):
    sys.exit(f'late.py: the fourth argument is the output pickle and must end in .pickle or .pkl, not: {out}')
if os.path.abspath(out) == os.path.abspath(path):
    sys.exit('late.py: the output is the flight log itself')
rx = re.compile(r'seq=(\d+) ms=([\d.]+) tid=(\d+) phase=(\w) op=(\w+) result=(-?\d+) dur_us=(-?\d+) a=(\d+) b=(\d+) c=(\d+)')
want = {'internal_wait_frame', 'internal_begin_frame', 'presenter_pace', 'internal_end_frame', 'presenter_submission', 'app_end_frame', 'app_wait_frame',
        'presenter_pair_release', 'synthesis_pair', 'synthesis_gpu_span', 'submission_timing', 'frame_gpu', 'presenter_transition', 'latency_status'}
cyc = None; subs = []; app_endB = []; app_endE = []; releases = []; level = 0; serial = None; gpu_end = {}; tim = []; told = 0
last_appB = None; last_rel = None; last_appE = None; trans = collections.Counter(); lat = []
with open(path, encoding='utf-8', errors='replace') as fh:
    for line in fh:
        i = line.find(' op='); j = line.find(' ', i + 4); op = line[i + 4:j]
        if op not in want: continue
        m = rx.match(line)
        if not m: continue
        seq, ms, tid, ph, op, res, dur, a, b, c = m.groups(); ms = float(ms); a = int(a); b = int(b); c = int(c); res = int(res)
        if op == 'internal_wait_frame' and ph == 'E':
            cyc = dict(wr=ms, disp=a, per=b, pace=0.0, wdur=int(dur) / 1000)
        elif op == 'internal_begin_frame' and ph == 'B' and cyc: cyc['bb'] = ms
        elif op == 'presenter_pace' and cyc: cyc['pace'] = a / 1000.0; cyc['pres'] = res
        elif op == 'internal_end_frame' and cyc:
            if ph == 'B':
                cyc['eb'] = ms; cyc['level'] = level; cyc['appB'] = last_appB; cyc['rel'] = last_rel; cyc['serial'] = serial; cyc['appE'] = last_appE
            else: cyc['edur'] = int(dur) / 1000
        elif op == 'presenter_submission' and cyc and 'eb' in cyc:
            cyc['kind'] = c; subs.append(cyc); cyc = None
        elif op == 'app_end_frame':
            if ph == 'B': last_appB = ms; app_endB.append(ms)
            else: last_appE = ms
        elif op == 'app_wait_frame' and ph == 'E': told = b
        elif op == 'presenter_pair_release': last_rel = ms; releases.append((ms, a))
        elif op == 'synthesis_pair' and ph == 'B': serial = b
        elif op == 'synthesis_gpu_span': gpu_end[c] = max(gpu_end.get(c, 0), b / 1000.0)
        elif op == 'submission_timing' and res == 1: tim.append((ms, a, b, c))
        elif op == 'presenter_transition' and T0 <= ms <= T1: trans[res] += 1
        elif op == 'latency_status': lat.append((ms, res, a, b))
        if op == 'app_wait_frame' and ph == 'E' and subs:
            level = round(told / subs[-1]['per']) if subs[-1]['per'] else 0
with open(out, 'wb') as fh:
    pickle.dump(dict(subs=subs, tim=tim, gpu_end=gpu_end, app_endB=app_endB, trans=trans, lat=lat), fh)
print('subs', len(subs), 'trans', dict(trans))
