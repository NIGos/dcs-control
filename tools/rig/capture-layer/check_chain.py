"""Reads a capture taken BELOW the real OFXR layer in OFXR's own call-chain test (fake runtime, WARP) and says what the
capture layer saw: layers per submission, pixels, real / synthetic from the flight log against the swapchain-order rule.
   py check_chain.py <capture dir>     (verdict.py must have run on it with --flight-log)
Exit 1 only when the capture itself is not whole (count, layers, pixels); the agreement numbers are findings."""
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import riglib  # noqa: E402


def main():
    d = sys.argv[1]
    cap = riglib.load_capture(d)
    v = json.load(open(os.path.join(d, 'verdict.json')))
    subs = cap['subs']
    two = sum(1 for s in subs if len(riglib.projection_layers(s)) == 2)
    views = [x for s in subs for _, l in riglib.projection_layers(s) for x in l['views']]
    with_px = sum(1 for x in views if riglib.view_pixels(cap, x) is not None)
    flags = sorted({(i, l['flags']) for s in subs for i, l in riglib.projection_layers(s)})
    seq = v['sequence']
    by_log = [e['byLog'] for e in seq]
    by_ring = [e['byRing'] for e in seq]
    both = [(a, b) for a, b in zip(by_log, by_ring) if a in ('real', 'synthetic')]
    agree = sum(1 for a, b in both if a == b)
    rep_log = sum(1 for e in seq if e['byLog'] == 'repeat')
    rep_layer = sum(1 for e in seq if e['repeat'])
    rep_both = sum(1 for e in seq if e['byLog'] == 'repeat' and e['repeat'])
    cu = sorted(s['copyUs'] for s in subs)
    print('capture below OFXR: %d submissions (%s), %d with two projection layers, %d of %d views with pixels, layer flags %s' % (len(subs), cap['end']['reason'] if cap['end'] else 'no end line', two, with_px, len(views), flags))
    print('swapchain roles by creation order: %s' % v['ringRoles'])
    print('cadence (flight log where it has the display time, else swapchain order): %s' % v['cadence'])
    print('flight log has the display time of %d of %d submissions; real/synthetic by swapchain order agrees with the log on %d of %d; repeats: log %d, layer %d, both %d' % (
        sum(1 for a in by_log if a), len(seq), agree, len(both), rep_log, rep_layer, rep_both))
    print('pose rule (real after synthetic keeps the pose): %s' % v['classification']['poseRuleRealAfterSyntheticUnchanged'])
    al = v['summary']['alpha']
    print('triples %d; synthetic focus alpha against the next real frame: mean abs max %s, holes %s; copy inside xrEndFrame median %d us' % (len(v['triples']), al['meanAbsMax'], al['holePixelsTotal'], cu[len(cu) // 2] if cu else -1))
    ok = len(subs) >= 30 and two >= len(subs) - 2 and with_px == len(views)
    print('capture whole: %s' % ok)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
