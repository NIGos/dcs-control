"""Calibration of the cursor scenario from a `cursor-calib` flight.
   py cursor_calib.py <run folder of the cursor-calib flight> [--write]
For each capture c0..c8 (the mouse resting at a known place of DCS's window, mouse.csv) finds DCS's white cursor dot
in the focus layer (left and right eye) and, where it is outside the focus image, in the periphery layer; fits
    left-eye tangent (x, y) of the cursor = a * (window fraction x, y) + b
and reports the residuals, the right eye's offset (the cursor is on a near surface) and the dot's size.
--write stores the calibration where run-scenario.ps1's cursor-stretch action reads it: <repo>/artifacts/rig/
cursor-calibration.json (git-ignored: it belongs to one headset and window size), or the file given with --out."""
import argparse
import csv
import json
import os
import sys

import numpy as np

import riglib


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run')
    ap.add_argument('--write', action='store_true')
    ap.add_argument('--out', help='with --write: the file to store instead of <repo>/artifacts/rig/cursor-calibration.json')
    a = ap.parse_args()
    mouse = {}
    with open(os.path.join(a.run, 'mouse.csv')) as f:
        for row in csv.DictReader(f):
            mouse[row['label']] = (float(row['windowFx']), float(row['windowFy']))      # the last line of a label = where it came to rest
    pts, rows = [], []
    tap = os.path.join(a.run, 'tap')
    for d in sorted(os.listdir(tap)):
        if not os.path.isdir(os.path.join(tap, d)) or '_c' not in d:
            continue
        label = d.split('_', 1)[1]
        cap = riglib.load_capture(os.path.join(tap, d))
        if not cap['subs'] or label not in mouse:
            continue
        sub = cap['subs'][-1]
        per, foc = riglib.pick_layers(sub)
        found = {}
        for lname, layer in (('focus', foc), ('periphery', per)):
            if layer is None:
                continue
            for e, v in enumerate(layer['views'][:2]):
                img = riglib.view_pixels(cap, v)
                if img is None:
                    continue
                fov = riglib.view_fov_of_pixels(v)
                ppt = img.shape[1] / (fov[1] - fov[0])
                c = riglib.find_cursor_dot(img, ppt)
                if c:
                    cx, cy, w, h, area, kind = c[0]
                    found[(lname, e)] = {'px': [cx, cy], 'size': [w, h], 'kind': kind, 'candidates': len(c), 'tan': [fov[0] + cx / img.shape[1] * (fov[1] - fov[0]), fov[2] - cy / img.shape[0] * (fov[2] - fov[3])]}
        row = {'label': label, 'window': mouse[label], 'found': {'%s eye %d' % k: v for k, v in found.items()}}
        rows.append(row)
        best = found.get(('focus', 0))
        if best and best['candidates'] <= 3:
            pts.append((mouse[label][0], mouse[label][1], best['tan'][0], best['tan'][1], (found.get(('focus', 1)) or {}).get('tan')))
        print('%s window %.3f,%.3f | %s' % (label, mouse[label][0], mouse[label][1], '; '.join('%s eye %d: %s px %.0f,%.0f size %dx%d tan %.4f,%.4f (%d cand.)' % (k[0], k[1], v['kind'], v['px'][0], v['px'][1], v['size'][0], v['size'][1], v['tan'][0], v['tan'][1], v['candidates']) for k, v in sorted(found.items())) or 'no dot found'))
    res = {'run': os.path.abspath(a.run), 'points': rows}
    if len(pts) >= 3:
        P = np.array([p[:4] for p in pts])
        A = np.c_[P[:, 0], P[:, 1], np.ones(len(P))]
        kx = np.linalg.lstsq(A, P[:, 2], rcond=None)[0]
        ky = np.linalg.lstsq(A, P[:, 3], rcond=None)[0]
        rx, ry = A @ kx - P[:, 2], A @ ky - P[:, 3]
        off = [np.array(p[4]) - np.array(p[2:4]) for p in pts if p[4]]
        res.update({'a': [[float(kx[0]), float(kx[1])], [float(ky[0]), float(ky[1])]], 'b': [float(kx[2]), float(ky[2])], 'pointsUsed': len(pts),
                    'residualTanMax': float(max(np.abs(rx).max(), np.abs(ry).max())), 'rightEyeOffsetTan': None if not off else [float(np.median([o[0] for o in off])), float(np.median([o[1] for o in off]))],
                    'note': 'left-eye tangent of the cockpit cursor = a * (window fraction x, y) + b; fitted on the focus layer of the left eye'})
        print('fit on %d points: tan x = %.4f fx %+.4f fy %+.4f ; tan y = %.4f fx %+.4f fy %+.4f ; worst residual %.4f tangent (%.1f focus px); right eye offset %s' % (
            len(pts), kx[0], kx[1], kx[2], ky[0], ky[1], ky[2], res['residualTanMax'], res['residualTanMax'] * 2540, res['rightEyeOffsetTan']))
        if a.write:
            out = os.path.abspath(a.out or os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'artifacts', 'rig', 'cursor-calibration.json'))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            json.dump(res, open(out, 'w'), indent=1)
            print('written', out)
    else:
        print('fewer than 3 points with the dot in the left eye\'s focus image: no fit')
    json.dump(res, open(os.path.join(a.run, 'cursor-calib.json'), 'w'), indent=1)
    return 0 if len(pts) >= 3 else 1


if __name__ == '__main__':
    sys.exit(main())
