"""Smooth-cursor records of a rig run, per second and per mouse stretch.
   py cursor_verdict.py <run folder> [--on <the option-on run folder, when this one is the twin>]
Reads the run's OFXR flight log (op=smooth_cursor; layouts in include/xrfg/bridge_flight_logger.hpp 472-505):
   result=1  every second, per synthesizer: pairs judged, drawn, searched; rejected not found / low score /
             look-alike / views disagree; jump, too much motion, mouse idle; detection time
   result=6  (two layers linked, OFXR round 8) every second per swapchain: a low byte 1 = periphery (its detection
             decides for both layers), 2 = focus (it follows), image width and height; b = pairs judged | accepted
             << 16; c on the focus swapchain = views redrawn from its own detection | from the periphery's record
             with the cursor inside the focus image << 16 | cut by the focus border << 32 | outside the focus
             image << 48
and run.json's "stretches" (the runner's mouse-zone actions: inside the focus image, periphery, across the border).
A record's second is its log time (the log file's start second + ms): accurate to about a second, so the first and
last second of a stretch may belong to its neighbour. The composed twin has result=1 only.
Writes cursor-verdict.json and cursor-verdict.txt in the run folder."""
import argparse
import datetime
import glob
import json
import os
import re
import sys


def f16(v, shift):
    return (v >> shift) & 0xFFFF


def read_run(run):
    logs = sorted(glob.glob(os.path.join(run, 'ofxr-bridge-flight-*.log')), key=os.path.getsize, reverse=True)
    if not logs:
        return None
    m = re.search(r'flight-(\d{8})-(\d{6})-pid', os.path.basename(logs[0]))
    t0 = datetime.datetime.strptime(m.group(1) + m.group(2), '%Y%m%d%H%M%S') if m else None
    rec = json.load(open(os.path.join(run, 'run.json'), encoding='utf-8-sig')) if os.path.exists(os.path.join(run, 'run.json')) else {}
    stretches = []
    for s in rec.get('stretches') or []:
        stretches.append((s['zone'], datetime.datetime.fromisoformat(s['start'][:26]).replace(tzinfo=None), datetime.datetime.fromisoformat(s['end'][:26]).replace(tzinfo=None), s.get('box')))
    rx = re.compile(r'ms=([\d.]+) .* op=smooth_cursor result=(\d+) .* a=(\d+) b=(\d+) c=(\d+)')
    seconds, other = {}, {'linked': [], 'templates': 0, 'weak': 0, 'captures': 0}
    with open(logs[0], 'r', errors='replace') as f:
        for line in f:
            if ' op=smooth_cursor ' not in line:
                continue
            m = rx.search(line)
            if not m:
                continue
            ms, res, a, b, c = float(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))
            when = t0 + datetime.timedelta(milliseconds=ms) if t0 else None
            key = when.replace(microsecond=0) if when else int(ms // 1000)
            sec = seconds.setdefault(key, {'r1': [], 'r6': {}})
            if res == 1:
                sec['r1'].append({'judged': f16(a, 0), 'drawn': f16(a, 16), 'acquired': f16(a, 32), 'tracked': f16(a, 48), 'notFound': f16(b, 0), 'lowScore': f16(b, 16), 'lookAlike': f16(b, 32),
                                  'viewsDisagree': f16(b, 48), 'jump': f16(c, 0), 'tooMuchMotion': f16(c, 16), 'idlePairs': f16(c, 32), 'detectUs': f16(c, 48)})
            elif res == 6:
                kind = {1: 'periphery', 2: 'focus'}.get(a & 0xFF, 'kind%d' % (a & 0xFF))
                d = sec['r6'].setdefault(kind, {'judged': 0, 'accepted': 0, 'own': 0, 'fromPeripheryInside': 0, 'cutByBorder': 0, 'outsideFocus': 0, 'size': [f16(a, 16), f16(a, 32)]})
                d['judged'] += b & 0xFFFF
                d['accepted'] += (b >> 16) & 0xFFFF
                if kind == 'focus':
                    d['own'] += f16(c, 0); d['fromPeripheryInside'] += f16(c, 16); d['cutByBorder'] += f16(c, 32); d['outsideFocus'] += f16(c, 48)
            elif res == 5:
                other['linked'].append({'swapchain': a, 'bits': b, 'peripheryLayer': c})
            elif res == 2:
                other['templates'] += 1
            elif res == 3:
                other['weak'] += 1
            elif res == 4:
                other['captures'] += 1
    rows = []
    for key in sorted(seconds):
        s = seconds[key]
        zone = 'outside the stretches'
        if isinstance(key, datetime.datetime):
            for z, a_, b_, _ in stretches:
                if a_ - datetime.timedelta(seconds=0.5) <= key <= b_ + datetime.timedelta(seconds=0.5):
                    zone = z
        r1 = {k: sum(x[k] for x in s['r1']) for k in ('judged', 'drawn', 'notFound', 'lowScore', 'lookAlike', 'viewsDisagree', 'jump', 'tooMuchMotion', 'idlePairs')} if s['r1'] else None
        rows.append({'second': key.strftime('%H:%M:%S') if isinstance(key, datetime.datetime) else key, 'zone': zone, 'result1Records': len(s['r1']), 'result1': r1, 'result6': s['r6'] or None})
    return {'run': os.path.abspath(run), 'log': logs[0], 'option': rec.get('option'), 'ofxrFlownSha256': rec.get('ofxrFlownSha256'), 'ofxrFlownFrom': rec.get('ofxrFlownFrom'),
            'stretches': [{'zone': z, 'start': a_.strftime('%H:%M:%S'), 'end': b_.strftime('%H:%M:%S'), 'box': bx} for z, a_, b_, bx in stretches], 'other': other, 'seconds': rows}


def totals(data):
    out = {}
    for r in data['seconds']:
        t = out.setdefault(r['zone'], {'seconds': 0, 'judged': 0, 'drawn': 0, 'viewsDisagree': 0, 'notFound': 0, 'lowScore': 0, 'idlePairs': 0,
                                       'periphery': {'judged': 0, 'accepted': 0}, 'focus': {'judged': 0, 'accepted': 0, 'own': 0, 'fromPeripheryInside': 0, 'cutByBorder': 0, 'outsideFocus': 0}})
        t['seconds'] += 1
        if r['result1']:
            for k in ('judged', 'drawn', 'viewsDisagree', 'notFound', 'lowScore', 'idlePairs'):
                t[k] += r['result1'][k]
        for kind, d in (r['result6'] or {}).items():
            if kind in t:
                for k in t[kind]:
                    t[kind][k] += d[k]
    for t in out.values():
        t['drawnOverJudged'] = None if not t['judged'] else t['drawn'] / t['judged']
        for kind in ('periphery', 'focus'):
            t[kind]['acceptedOverJudged'] = None if not t[kind]['judged'] else t[kind]['accepted'] / t[kind]['judged']
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run')
    ap.add_argument('--on')
    a = ap.parse_args()
    data = read_run(a.run)
    if data is None:
        print('no OFXR flight log in', a.run)
        return 2
    data['totals'] = totals(data)
    ratio = lambda x: '-' if x is None else '%.2f' % x
    L = ['run %s' % data['run'], 'OFXR DLL flown: %s (%s)' % (data['ofxrFlownSha256'], data['ofxrFlownFrom']),
         'stretches (run.json): %s' % ('; '.join('%s %s-%s' % (s['zone'], s['start'], s['end']) for s in data['stretches']) or 'none recorded'),
         'linked records (result=5): %d; result=6 seen: %s' % (len(data['other']['linked']), any(r['result6'] for r in data['seconds'])), '',
         '%-9s %-22s | result=1: judged drawn disagree notFound lowScore idle | result=6 periphery: judged accepted | focus: judged accepted | focus views: own fromPeriphery cut outside' % ('second', 'stretch')]
    for r in data['seconds']:
        r1, r6 = r['result1'] or {}, r['result6'] or {}
        p, f = r6.get('periphery', {}), r6.get('focus', {})
        g = lambda d, k: '-' if k not in d else str(d[k])
        L.append('%-9s %-22s | %6s %5s %8s %8s %8s %4s | %6s %8s | %6s %8s | %4s %13s %3s %7s' % (r['second'], r['zone'], g(r1, 'judged'), g(r1, 'drawn'), g(r1, 'viewsDisagree'), g(r1, 'notFound'), g(r1, 'lowScore'), g(r1, 'idlePairs'),
                                                                                              g(p, 'judged'), g(p, 'accepted'), g(f, 'judged'), g(f, 'accepted'), g(f, 'own'), g(f, 'fromPeripheryInside'), g(f, 'cutByBorder'), g(f, 'outsideFocus')))
    L += ['', 'per stretch:']
    for z, t in data['totals'].items():
        L.append('  %-22s %2d s | result=1 judged %d drawn %d (%s) views disagree %d | result=6 periphery %d / %d accepted (%s), focus %d / %d (%s); focus views: own %d, from the periphery inside %d, cut by the border %d, outside %d' % (
            z, t['seconds'], t['judged'], t['drawn'], ratio(t['drawnOverJudged']), t['viewsDisagree'], t['periphery']['accepted'], t['periphery']['judged'], ratio(t['periphery']['acceptedOverJudged']),
            t['focus']['accepted'], t['focus']['judged'], ratio(t['focus']['acceptedOverJudged']), t['focus']['own'], t['focus']['fromPeripheryInside'], t['focus']['cutByBorder'], t['focus']['outsideFocus']))
    # the twin against the option-on run, stretch by stretch (result=1 is what both have)
    pair = None
    if a.on:
        other = read_run(a.on)
        if other:
            other['totals'] = totals(other)
            on, twin = other, data
            pair = {'on': on['run'], 'twin': twin['run'], 'zones': {}}
            L += ['', 'option on (%s) against this twin, result=1 drawn / judged per stretch:' % os.path.basename(on['run'])]
            for z in [s['zone'] for s in on['stretches']] or ['inside', 'periphery', 'border']:
                o, t = on['totals'].get(z), twin['totals'].get(z)
                pair['zones'][z] = {'on': o, 'twin': t}
                L.append('  %-12s on %s (%s of %s) | twin %s (%s of %s)%s' % (z, ratio(o and o['drawnOverJudged']), o and o['drawn'], o and o['judged'], ratio(t and t['drawnOverJudged']), t and t['drawn'], t and t['judged'],
                                                                           '' if not (o and t and o['drawnOverJudged'] is not None and t['drawnOverJudged'] is not None) else '   on - twin %+.2f' % (o['drawnOverJudged'] - t['drawnOverJudged'])))
            L.append('  (no threshold: the twin is the floor. In the periphery and border stretches the option-on run can only match the twin if the periphery layer draws the cursor: see its result=6 columns.)')
    data['againstOn'] = pair
    text = '\n'.join(L) + '\n'
    json.dump(data, open(os.path.join(a.run, 'cursor-verdict.json'), 'w', encoding='utf-8'), indent=1)
    open(os.path.join(a.run, 'cursor-verdict.txt'), 'w', encoding='utf-8').write(text)
    print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
