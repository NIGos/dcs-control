"""Self-test of compose.py and verdict.py on generated data with known answers.
   py selftest.py [--keep]        exit 0 = every expectation holds. Output: artifacts/rig/selftest-out/ (results.json, cases).

The scene: a textured world in tangent space moving by a known amount per real frame; a periphery layer and a focus
layer rendered from it (focus with a fade in alpha, unpremultiplied), a cursor spot moving at its own known speed and
a head-fixed HUD mark; captures written in the capture layer's file format. Synthetic frames are rendered at the exact
in-between time (good cases) or broken on purpose (each bad case breaks one thing)."""
import argparse
import json
import math
import os
import shutil
import sys

import numpy as np
from scipy import ndimage

import compose
import riglib
import verdict

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, '..', '..', 'artifacts', 'rig', 'selftest-out'))  # git-ignored, never under tools/
PW, PH, FW, FH = 270, 266, 488, 482                 # a quarter of the live sizes
PER_FOV = (-0.9, 0.9, 0.9, -0.9)
FOC_HALF = (0.28, 0.2766)                           # focus density 871.4 px per tangent, periphery 150
WORLD_PPT, WORLD_HALF = 900.0, 1.4
MOVE = (12.0 / (FW / (2 * FOC_HALF[0])), -4.0 / (FH / (2 * FOC_HALF[1])))   # world motion per real frame: 12 focus px right, 4 down
CURSOR_MOVE_PX = (20.0, 6.0)                        # cursor: its own motion per real frame, focus pixels
HUD_BOX = (-0.10, 0.10, 0.02, -0.02)                # head-fixed mark, tangent box l, r, u, d
CURSOR_BOX = (0.03, 0.26, -0.03, -0.17)             # the cursor's track stays inside this box

_rng = np.random.default_rng(7)
_n = int(2 * WORLD_HALF * WORLD_PPT)
_w = ndimage.gaussian_filter(_rng.standard_normal((_n, _n)).astype(np.float32), 2.5)
_w2 = ndimage.gaussian_filter(_rng.standard_normal((_n, _n)).astype(np.float32), 12)
_w = _w / _w.std() * 13 + _w2 / _w2.std() * 15 + 90
WORLD = np.clip(np.stack([_w, np.roll(_w, 37, 1) * 0.9 + 8, np.roll(_w, 71, 0) * 0.8 + 20], axis=2), 4, 250).astype(np.float32)
WORLD_BLUR = ndimage.gaussian_filter(WORLD, (5.0, 5.0, 0))      # for the periphery (6 world pixels per periphery pixel): no aliasing


def focus_alpha(w, h, fade=0.18):
    ny, nx = np.mgrid[0:h, 0:w]
    nx, ny = np.abs(2 * (nx + 0.5) / w - 1), np.abs(2 * (ny + 0.5) / h - 1)
    e = np.clip((1 - np.maximum(nx, ny)) / fade, 0, 1)
    return e * e * (3 - 2 * e)


def render(fov, w, h, t, blurred, cursor_at=None, hud=True, hud_sigma=0.0):
    """The world at time t (in real frames) over fov; cursor_at = tangent position of the cursor spot."""
    tx, ty = riglib.tangent_grid(fov, w, h)
    wx = (tx - MOVE[0] * t + WORLD_HALF) * WORLD_PPT - 0.5
    wy = (WORLD_HALF - (ty - MOVE[1] * t)) * WORLD_PPT - 0.5
    img = riglib.sample_bilinear(WORLD_BLUR if blurred else WORLD, wx, wy).astype(np.float32)
    ppt = w / (fov[1] - fov[0])
    over = np.zeros((h, w), np.float32)
    if cursor_at is not None:
        r2 = ((tx - cursor_at[0]) ** 2 + (ty - cursor_at[1]) ** 2) * ppt * ppt
        over = np.maximum(over, np.exp(-r2 / (2 * (4.0 * ppt / 871.4) ** 2 + 0.5)).astype(np.float32))
    if hud:
        l, r, u, d = HUD_BOX
        line = 1.2 / 871.4                                       # stroke half-width in tangents (about 1 focus pixel)
        horiz = np.clip(1.5 - np.abs(ty) / line, 0, 1) * ((tx > l) & (tx < r))
        ticks = np.clip(1.5 - np.abs(((tx - l) % 0.025) - 0.0125) / line, 0, 1) * ((ty < u) & (ty > d) & (tx > l) & (tx < r))
        m = np.maximum(horiz, ticks).astype(np.float32)
        if hud_sigma > 0:
            m = ndimage.gaussian_filter(m, hud_sigma * ppt / 871.4)
        over = np.maximum(over, m)
    img = img * (1 - over[..., None]) + np.array([40, 255, 60], np.float32) * over[..., None]
    return img


def focus_fov(cx=0.0, cy=0.0):
    return (cx - FOC_HALF[0], cx + FOC_HALF[0], cy + FOC_HALF[1], cy - FOC_HALF[1])


def angles(fov):
    return [math.atan(x) for x in fov]


def make_sub(t, kind, slot, seqs, display_ns, gaze=(0.0, 0.0), single=False, mutate=None, cursor_t=None, hud_sigma=0.0, pose_t=None):
    """One submission at scene time t. kind 'real' / 'synthetic' picks the swapchains (ids as OFXR creates them)."""
    ct = t if cursor_t is None else cursor_t
    ppt = FW / (2 * FOC_HALF[0])
    cursor = (0.05 + CURSOR_MOVE_PX[0] / ppt * ct, -0.10 + CURSOR_MOVE_PX[1] / ppt * ct)
    layers = []
    ffov = focus_fov(*gaze)
    for li, (fov, w, h, blurred) in enumerate([(PER_FOV, PW, PH, True), (ffov, FW, FH, False)]):
        base = li * 4
        sc = base + (3 if kind == 'synthetic' else 1 + slot)
        seqs[sc] = seqs.get(sc, 0) + 1
        views = []
        for eye in (0, 1):
            f = (fov[0] + 0.004 * eye, fov[1] + 0.004 * eye, fov[2], fov[3])
            rgb = render(f, w, h, t, blurred, cursor, True, hud_sigma)
            a = focus_alpha(w, h) * 255 if li == 1 else np.full((h, w), 255, np.float32)
            px = np.clip(np.dstack([rgb, a]) + 0.5, 0, 255).astype(np.uint8)
            views.append({'pose': [0.032 * (2 * eye - 1), 0.001 * (t if pose_t is None else pose_t), 0, 0, 0, 0, 1], 'fov': angles(f), 'depthInfo': False, 'sc': 4096 + sc, 'scId': sc, 'format': 29 if li == 0 else 28, 'dxgiFormat': 29 if li == 0 else 27,
                          'scW': w, 'scH': h, 'scArray': 2, 'rect': [0, 0, w, h], 'array': eye, 'imageIndex': seqs[sc] % 3, 'releaseSeq': seqs[sc], 'pixels': px})
        layers.append({'type': 'projection', 'flags': 6 if li == 1 else 0, 'space': 119, 'spaceType': 2, 'viewCount': 2, 'views': views})
    if single:      # the composed twin: one layer = the two composed over the periphery's field of view at 3x its size
        views = []
        for eye in (0, 1):
            pv, fv = layers[0]['views'][eye], layers[1]['views'][eye]
            fp = tuple(math.tan(x) for x in pv['fov'])
            r = compose.compose_eye(pv['pixels'], fp, fv['pixels'], tuple(math.tan(x) for x in fv['fov']), 6, fp, PW * 3, PH * 3)
            px = np.dstack([np.clip(r['rgb'] + 0.5, 0, 255), np.full((PH * 3, PW * 3), 255)]).astype(np.uint8)
            v = dict(pv)
            v.update({'scW': PW * 3, 'scH': PH * 3, 'rect': [0, 0, PW * 3, PH * 3], 'pixels': px})
            views.append(v)
        layers = [{'type': 'projection', 'flags': 0, 'space': 119, 'spaceType': 2, 'viewCount': 2, 'views': views}]
    sub = {'displayTime': display_ns, 'layerCount': len(layers), 'layers': layers, 'truth': kind}
    if mutate:
        mutate(sub)
    return sub


SWAPCHAINS = [{'id': i, 'handle': 4096 + i, 'w': PW if i < 4 else FW, 'h': PH if i < 4 else FH, 'array': 2, 'mips': 1, 'samples': 1, 'format': 29 if i < 4 else 28, 'dxgiFormat': 29 if i < 4 else 27, 'usage': 33, 'createFlags': 0, 'images': 3, 'createdQpc': i, 'alive': True} for i in range(8)]


def sequence(name, reals=5, per_gap=1, syn_time=None, mutate_syn=None, single=False, gaze_at=None, syn_kwargs=None, extra=None):
    """reals real frames with per_gap synthetics between; syn_time(k, per_gap) = the synthetic's scene time offset."""
    subs, seqs, slot, n, step = [], {}, 0, 0, 11111111
    for i in range(reals):
        g = gaze_at(i) if gaze_at else (0.0, 0.0)
        subs.append(make_sub(float(i), 'real', slot, seqs, 10 ** 9 + step * n, g, single))
        slot ^= 1
        n += 1
        if i == reals - 1:
            break
        g2 = gaze_at(i + 1) if gaze_at else (0.0, 0.0)
        for k in range(per_gap):
            frac = (k + 1) / (per_gap + 1)
            tt = i + (syn_time(k, per_gap) if syn_time else frac)
            kw = syn_kwargs(i) if callable(syn_kwargs) else dict(syn_kwargs or {})
            subs.append(make_sub(tt, 'synthetic', 0, seqs, 10 ** 9 + step * n, g2, single, mutate_syn, pose_t=float(i + 1), **kw))
            n += 1
    if extra:
        subs = extra(subs)
        for k, s in enumerate(subs):
            s['displayTime'] = 10 ** 9 + step * k
    folder = os.path.join(OUT, name)
    riglib.write_capture(folder, subs, SWAPCHAINS if not single else SWAPCHAINS[:4], note='selftest ' + name)
    with open(os.path.join(folder, 'fake-flight.log'), 'w') as f:      # the flight log's own line shape
        for k, s in enumerate(subs):
            f.write('seq=%d ms=%.3f tid=1 phase=I op=presenter_submission result=0 dur_us=0 a=%d b=%d c=%d\n' % (k + 10, k * 11.1, k, s['displayTime'], {'real': 1, 'synthetic': 2, 'repeat': 0}[s['truth']]))
    return folder


CHECKS = []


def expect(name, cond, detail):
    CHECKS.append({'check': name, 'ok': bool(cond), 'detail': detail})
    print(('ok   ' if cond else 'FAIL ') + name + ': ' + str(detail))


def V(folder, *extra):
    return verdict.run(argparse.Namespace(capture=folder, flight_log=None, eyes='0', out=None, crop=list(extra) or None, band_from=None, like=None, max_triples=0, grid=4, seam_density=0.5, quiet=True))


def test_compose():
    # 1. the vectorised port against a literal per-pixel port of Compose() (probe_run.cpp 334-357), both blend rules
    rng = np.random.default_rng(3)
    A = rng.integers(0, 256, (18, 20, 4), dtype=np.uint8)
    B = rng.integers(0, 256, (22, 24, 4), dtype=np.uint8)
    fa, fb, w, h = (-0.8, 0.9, 0.85, -0.75), (-0.21, 0.33, 0.3, -0.18), 53, 47
    dec = [(v / 255 / 12.92) if v / 255 <= 0.04045 else ((v / 255 + 0.055) / 1.055) ** 2.4 for v in range(256)]
    enc = lambda q: (q * 12.92 if q <= 0.0031308 else 1.055 * q ** (1 / 2.4) - 0.055) * 255

    def samp(im, u, v, linear):
        x, y = u * im.shape[1] - 0.5, v * im.shape[0] - 0.5
        x0, y0 = math.floor(x), math.floor(y)
        fx, fy = x - x0, y - y0
        cl = lambda q, hi: max(0, min(hi, q))
        ps = [im[cl(y0 + j, im.shape[0] - 1), cl(x0 + i, im.shape[1] - 1)] for j in (0, 1) for i in (0, 1)]
        out = []
        for c in range(4):
            val = [(dec[int(p[c])] if (c < 3 and linear) else float(p[c]) / (255.0 if (c == 3 or linear) else 1.0)) for p in ps]
            out.append((val[0] * (1 - fx) + val[1] * fx) * (1 - fy) + (val[2] * (1 - fx) + val[3] * fx) * fy)
        return out
    worst = {}
    for mode in ('linear', 'encoded'):
        ref = np.zeros((h, w, 3))
        for y in range(h):
            ty = fa[2] - (y + 0.5) / h * (fa[2] - fa[3])
            ny = (ty - (fb[2] + fb[3]) / 2) / ((fb[2] - fb[3]) / 2)
            for x in range(w):
                tx = fa[0] + (x + 0.5) / w * (fa[1] - fa[0])
                nx = (tx - (fb[0] + fb[1]) / 2) / ((fb[1] - fb[0]) / 2)
                ca = samp(A, (x + 0.5) / w, (y + 0.5) / h, mode == 'linear')
                col = ca[:3]
                if abs(nx) < 1 and abs(ny) < 1:
                    cb = samp(B, (nx + 1) / 2, (1 - ny) / 2, mode == 'linear')
                    col = [cb[c] * cb[3] + ca[c] * (1 - cb[3]) for c in range(3)]
                ref[y, x] = [enc(q) for q in col] if mode == 'linear' else col
        got = compose.compose_eye(A, fa, B, fb, 6, fa, w, h, mode)['rgb']
        worst[mode] = float(np.abs(got - ref).max())
    expect('compose: vectorised port equals the literal port of Compose()', worst['linear'] < 0.05 and worst['encoded'] < 0.01, 'max difference (of 255): linear-light rule %.4f, encoded rule %.4f' % (worst['linear'], worst['encoded']))
    # 2. the encoded rule on flat layers: 200 over 100 at alpha 64/255 = 125.1 (linear light would give 134.6)
    P = np.full((8, 8, 4), 100, np.uint8)
    F = np.full((8, 8, 4), 200, np.uint8)
    F[..., 3] = 64
    e = compose.compose_eye(P, (-1, 1, 1, -1), F, (-0.5, 0.5, 0.5, -0.5), 6, (-1, 1, 1, -1), 16, 16, 'encoded')['rgb'][8, 8, 0]
    li = compose.compose_eye(P, (-1, 1, 1, -1), F, (-0.5, 0.5, 0.5, -0.5), 6, (-1, 1, 1, -1), 16, 16, 'linear')['rgb'][8, 8, 0]
    op = compose.compose_eye(P, (-1, 1, 1, -1), F, (-0.5, 0.5, 0.5, -0.5), 0, (-1, 1, 1, -1), 16, 16, 'encoded')['rgb'][8, 8, 0]
    F2 = F.copy()
    F2[..., :3] = 60
    pm = compose.compose_eye(P, (-1, 1, 1, -1), F2, (-0.5, 0.5, 0.5, -0.5), 2, (-1, 1, 1, -1), 16, 16, 'encoded')['rgb'][8, 8, 0]
    expect('compose: blend on encoded values with unpremultiplied alpha', abs(e - (200 * 64 / 255 + 100 * (1 - 64 / 255))) < 0.01 and abs(li - 134.6) < 0.3 and op == 200 and abs(pm - (60 + 100 * (1 - 64 / 255))) < 0.01,
           'encoded %.2f (125.10 expected), linear-light %.2f, no alpha flag %.0f, premultiplied flag %.2f' % (e, li, op, pm))
    # 3. focus placed by its field of view: composed against the world rendered straight onto the same grid
    seqs = {}
    sub = make_sub(0.0, 'real', 0, seqs, 0, gaze=(0.11, -0.07))
    cap = {'dir': None}
    pv, fv = sub['layers'][0]['views'][0], sub['layers'][1]['views'][0]
    fp, ff = tuple(math.tan(x) for x in pv['fov']), tuple(math.tan(x) for x in fv['fov'])
    grid = (ff, FW, FH)
    truth = render(ff, FW, FH, 0.0, False, (0.05, -0.10))
    opaque = focus_alpha(FW, FH) >= 0.999
    errs = {}
    for shift_px in (0.0, 1.0):
        ffs = tuple(v + (shift_px / 871.4 if i < 2 else 0) for i, v in enumerate(ff))
        r = compose.compose_eye(pv['pixels'], fp, fv['pixels'], ffs, 6, grid[0], FW, FH)
        errs[shift_px] = float(np.abs(r['rgb'] - truth)[opaque].mean())
    expect('compose: focus placed by its field of view', errs[0.0] < 0.6 and errs[1.0] > 4 * errs[0.0], 'mean error in the opaque zone %.3f of 255; with the field of view moved by one focus pixel %.3f' % (errs[0.0], errs[1.0]))
    # 4. bilinear enlargement of the periphery: a ramp of 3 per texel must come out as a ramp of 1 per pixel at 3x
    ramp = np.zeros((10, 40, 4), np.uint8)
    ramp[..., :3] = (10 + 3 * np.arange(40))[None, :, None]
    r = compose.compose_eye(ramp, (-1, 1, 1, -1), None, None, 0, (-1, 1, 1, -1), 120, 30)['rgb']
    want = 10 + 3 * ((np.arange(120) + 0.5) / 3 - 0.5)
    err = float(np.abs(r[5, 3:-3, 0] - want[3:-3]).max())
    expect('compose: periphery enlarged bilinearly', err < 0.001, 'ramp of 3 per texel at 3x: max error %.5f of 255 away from the border' % err)
    return errs


def test_verdicts():
    hud = 'hud:focus:%g,%g,%g,%g' % (HUD_BOX[0] - 0.01, HUD_BOX[1] + 0.01, HUD_BOX[2] + 0.012, HUD_BOX[3] - 0.012)
    cur = 'cursor:focus:%g,%g,%g,%g' % CURSOR_BOX
    res = {}
    # good 2X
    g = sequence('good_2x')
    r = res['good_2x'] = V(g, hud, cur)
    s, v = r['summary'], r['verdicts']
    expect('good 2X: cadence and classification from the swapchain order', r['cadence'] == 'RSRSRSRSR' and all(e['kind'] == t for e, t in zip(r['sequence'], ['real', 'synthetic'] * 4 + ['real'])), r['cadence'])
    expect('good 2X: focus interpolation ratio = 0.5', abs(s['interpFocus']['ratioMedian'] - 0.5) < 0.02 and v['interpFocus.ratio']['verdict'] == 'PASS', 'median %.4f, residual %.3f px (real-to-real motion 12.65 focus px)' % (s['interpFocus']['ratioMedian'], s['interpFocus']['residualPxMedian']))
    expect('good 2X: periphery interpolation ratio = 0.5', abs(s['interpPeriphery']['ratioMedian'] - 0.5) < 0.06 and v['interpPeriphery.ratio']['verdict'] == 'PASS', 'median %.4f, residual %.3f px (motion 2.18 periphery px)' % (s['interpPeriphery']['ratioMedian'], s['interpPeriphery']['residualPxMedian']))
    expect('good 2X: seam, alpha, duplicates pass', all(v[k]['verdict'] == 'PASS' for k in ('seam.gradient', 'seam.layerMismatch', 'alpha.equalsNext', 'alpha.noHoles', 'duplicates.none', 'interpFocus.notACopy')),
           'seam synthetic/real %.3f, layer mismatch synthetic/real %.3f, alpha mean abs %.3f' % (s['seam']['seamSyntheticOverRealMax'], s['seam']['mismatchSyntheticOverRealMax'], s['alpha']['meanAbsMax']))
    expect('good 2X: cursor at half way, HUD sharp and still', abs(s['crops']['cursor']['positionFractionMedian'] - 0.5) < 0.03 and v['crop.cursor.position']['verdict'] == 'PASS' and v['crop.hud.sharp']['verdict'] == 'PASS' and v['crop.hud.position']['verdict'] == 'PASS',
           'cursor fraction %.4f (its motion 20.9 px per real frame); HUD sharpness ratio %.3f, HUD position error %.3f px' % (s['crops']['cursor']['positionFractionMedian'], s['crops']['hud']['sharpnessRatioMedian'], s['crops']['hud']['stillPositionErrorPxMax']))
    # the flight-log join gives the same classes
    a = argparse.Namespace(capture=g, flight_log=os.path.join(g, 'fake-flight.log'), eyes='0', out=os.path.join(g, 'with-log'), crop=None, band_from=None, max_triples=2, grid=4, seam_density=0.5, quiet=True)
    rl = verdict.run(a)
    expect('flight-log join: same classes, ring order agrees', rl['classification']['ringOrderAgreesWithLog'] == 1.0 and rl['classification']['flightLogMatched'] == 9 and rl['classification']['source'] == ['flight log'], rl['classification'])
    expect('pose rule: the real frame after a synthetic keeps its pose', r['classification']['poseRuleRealAfterSyntheticUnchanged'] == 1.0, r['classification']['poseRuleRealAfterSyntheticUnchanged'])
    # synthetic = copy of the previous real frame
    def copy_prev(subs):
        for i, s in enumerate(subs):
            if s['truth'] == 'synthetic':
                for li in (0, 1):
                    for e in (0, 1):
                        s['layers'][li]['views'][e]['pixels'] = subs[i - 1]['layers'][li]['views'][e]['pixels'].copy()
        return subs
    r = res['copy_prev'] = V(sequence('copy_prev', reals=4, extra=copy_prev))
    expect('copy of the previous frame: caught', r['verdicts']['interpFocus.notACopy']['verdict'] == 'FAIL' and r['verdicts']['interpFocus.ratio']['verdict'] == 'FAIL' and abs(r['summary']['interpFocus']['ratioMedian']) < 0.02,
           'equal-to-previous share %.3f, ratio %.4f; also listed as duplicates: %d same-pixel entries' % (r['summary']['interpFocus']['equalPrevShareMedian'], r['summary']['interpFocus']['ratioMedian'], r['duplicates']['samePixels']))
    # synthetic at 0.3 instead of 0.5
    r = res['off_phase'] = V(sequence('off_phase', reals=4, syn_time=lambda k, n: 0.3), cur)
    expect('synthetic at 0.3: ratio reads 0.3 and fails', abs(r['summary']['interpFocus']['ratioMedian'] - 0.3) < 0.02 and r['verdicts']['interpFocus.ratio']['verdict'] == 'FAIL' and abs(r['summary']['crops']['cursor']['positionFractionMedian'] - 0.3) < 0.03,
           'focus %.4f, periphery %.4f, cursor %.4f' % (r['summary']['interpFocus']['ratioMedian'], r['summary']['interpPeriphery']['ratioMedian'], r['summary']['crops']['cursor']['positionFractionMedian']))
    # 3X
    r = res['good_3x'] = V(sequence('good_3x', reals=4, per_gap=2))
    fr = sorted({round(t['fraction'], 3) for t in r['triples']})
    r1 = np.median([t['eyes']['0']['interpFocus']['ratioMedian'] for t in r['triples'] if t['fraction'] < 0.5])
    r2 = np.median([t['eyes']['0']['interpFocus']['ratioMedian'] for t in r['triples'] if t['fraction'] > 0.5])
    expect('3X: ratios 1/3 and 2/3', r['cadence'] == 'RSSRSSRSSR' and fr == [0.333, 0.667] and abs(r1 - 1 / 3) < 0.02 and abs(r2 - 2 / 3) < 0.02 and r['verdicts']['interpFocus.ratio']['verdict'] == 'PASS', 'cadence %s, focus ratios %.4f and %.4f' % (r['cadence'], r1, r2))
    # alpha hole
    def hole(sub):
        for e in (0, 1):
            sub['layers'][1]['views'][e]['pixels'][200:240, 220:260, 3] = 0
    r = res['alpha_hole'] = V(sequence('alpha_hole', reals=3, mutate_syn=hole))
    expect('alpha hole of 40x40 px: caught', r['verdicts']['alpha.noHoles']['verdict'] == 'FAIL' and r['summary']['alpha']['holePixelsTotal'] == 2 * 1600, 'hole pixels %d over 2 synthetics, share %.5f' % (r['summary']['alpha']['holePixelsTotal'], r['summary']['alpha']['holeShareMax']))
    # seam: the synthetic focus layer 12 % brighter than the periphery
    def bright(sub):
        for e in (0, 1):
            p = sub['layers'][1]['views'][e]['pixels']
            p[..., :3] = np.clip(p[..., :3].astype(np.float32) * 1.12, 0, 255).astype(np.uint8)
    r = res['seam_step'] = V(sequence('seam_step', reals=3, mutate_syn=bright))
    expect('focus layer 12 % brighter on synthetics: seam caught', r['verdicts']['seam.layerMismatch']['verdict'] == 'FAIL', 'layer mismatch synthetic/real %.2f (limit 1.5), band gradient synthetic/real %.3f' % (r['summary']['seam']['mismatchSyntheticOverRealMax'], r['summary']['seam']['seamSyntheticOverRealMax']))
    # HUD blurred in the synthetic
    r = res['hud_blur'] = V(sequence('hud_blur', reals=3, syn_kwargs={'hud_sigma': 2.0}), hud)
    expect('HUD blurred (sigma 2 px) on synthetics: caught', r['verdicts']['crop.hud.sharp']['verdict'] == 'FAIL', 'sharpness ratio %.3f (limit 0.9)' % r['summary']['crops']['hud']['sharpnessRatioMedian'])
    # cursor left behind (drawn at the previous frame's place in the synthetic)
    r = res['cursor_stuck'] = V(sequence('cursor_stuck', reals=3, syn_kwargs=lambda i: {'cursor_t': float(i)}), cur)
    expect('cursor left at the previous place on synthetics: caught', r['verdicts']['crop.cursor.position']['verdict'] == 'FAIL' and abs(r['summary']['crops']['cursor']['positionFractionMedian']) < 0.03 and r['verdicts']['interpFocus.ratio']['verdict'] == 'PASS',
           'cursor fraction %.4f while the world reads %.4f' % (r['summary']['crops']['cursor']['positionFractionMedian'], r['summary']['interpFocus']['ratioMedian']))
    # duplicates: one repeat, one new image with the same pixels
    def dups(subs):
        rep = json.loads(json.dumps({k: v for k, v in subs[2].items() if k != 'layers'}))
        rep['layers'] = subs[2]['layers']
        rep['truth'] = 'repeat'
        same = {k: v for k, v in subs[4].items() if k != 'layers'}
        same['layers'] = [{**l, 'views': [{**v, 'releaseSeq': v['releaseSeq'] + 50} for v in l['views']]} for l in subs[4]['layers']]
        same['truth'] = 'real'
        return subs[:3] + [rep] + subs[3:5] + [same] + subs[5:]
    r = res['duplicates'] = V(sequence('duplicates', reals=5, extra=dups))
    expect('duplicates: one repeat and one same-pixels image found', r['duplicates']['repeats'] == 1 and r['duplicates']['samePixels'] == 1 and r['cadence'] == 'RSRrSRRSRSR' and r['verdicts']['duplicates.none']['verdict'] == 'FAIL', 'cadence %s, list %s' % (r['cadence'], [(d['n'], d['kind'][:6]) for d in r['duplicates']['list']]))
    # gaze jump between real 1 and real 2: good fill, and a black uncovered strip
    jump = lambda i: (0.0, 0.0) if i < 2 else (0.03, 0.01)
    r = res['gaze_jump'] = V(sequence('gaze_jump', reals=4, gaze_at=jump))
    s = r['summary']
    expect('gaze jump, true fill: ratio still 0.5 with the moved field of view, uncovered zone passes',
           abs(s['interpFocus']['ratioMedian'] - 0.5) < 0.02 and s['alpha']['withUncoveredZone'] == 1 and r['verdicts']['alpha.uncoveredNotBlack']['verdict'] == 'PASS' and r['verdicts']['alpha.uncoveredAgreesWithPeriphery']['verdict'] == 'PASS' and r['verdicts']['alpha.equalsNext']['verdict'] == 'PASS',
           'ratio %.4f (field of view moved 26 px), uncovered zone in %d triple, black share %.4f, uncovered/interior mismatch %.2f' % (s['interpFocus']['ratioMedian'], s['alpha']['withUncoveredZone'], s['alpha']['uncoveredBlackShareMax'], s['alpha']['uncoveredMismatchOverInteriorMax']))
    def black_strip(sub):
        for e in (0, 1):
            v = sub['layers'][1]['views'][e]
            f = tuple(math.tan(x) for x in v['fov'])
            if abs((f[0] + f[1]) / 2 - 0.004 * e) < 1e-6:
                continue                                      # the gaze has not moved for this synthetic
            tx, ty = riglib.tangent_grid(f, FW, FH)
            old = focus_fov(0.0, 0.0)
            unc = (tx > old[1] + 0.004 * e) | (ty > old[2])
            v['pixels'][unc, :3] = 0
    r = res['gaze_jump_black'] = V(sequence('gaze_jump_black', reals=4, gaze_at=jump, mutate_syn=black_strip))
    expect('gaze jump, black uncovered strip: caught', r['verdicts']['alpha.uncoveredNotBlack']['verdict'] == 'FAIL' and r['verdicts']['alpha.uncoveredAgreesWithPeriphery']['verdict'] == 'FAIL',
           'black share %.3f, uncovered/interior mismatch %.1f' % (r['summary']['alpha']['uncoveredBlackShareMax'], r['summary']['alpha']['uncoveredMismatchOverInteriorMax']))
    # the composed twin (one layer), the band taken from the two-layer capture
    t = sequence('twin_single', reals=3, single=True)
    a = argparse.Namespace(capture=t, flight_log=os.path.join(t, 'fake-flight.log'), eyes='0', out=None, crop=None, band_from=g, max_triples=0, grid=4, seam_density=0.5, quiet=True)
    r = res['twin_single'] = verdict.run(a)
    s = r['summary']
    expect('single composed layer (twin, one stream) with --like: both windows judged on the option-on grids, seam judged, alpha not judged',
           abs(s['interpPeriphery']['ratioMedian'] - 0.5) < 0.03 and abs(s['interpFocus']['deviationMedian']) < 0.03 and r['verdicts']['seam.gradient']['verdict'] == 'PASS' and r['verdicts']['alpha.equalsNext']['verdict'] == 'NOT JUDGED',
           'ratio %.4f on the composed layer, seam synthetic/real %.3f' % (s['interpPeriphery']['ratioMedian'], s['seam']['seamSyntheticOverRealMax']))
    # the twin as the runner captures it: the focus window (+15 %) at full resolution and the whole image at scale 3,
    # measured like with like on the option-on capture's grids
    def two_streams(subs):
        for s in subs:
            for v in s['layers'][0]['views']:
                full = v['pixels']
                H, W = full.shape[:2]
                e = v['array']
                f = tuple(math.tan(x) for x in v['fov'])
                ff = focus_fov(0.0, 0.0)
                win = (ff[0] + 0.004 * e - 0.15 * 2 * FOC_HALF[0], ff[1] + 0.004 * e + 0.15 * 2 * FOC_HALF[0], ff[2] + 0.15 * 2 * FOC_HALF[1], ff[3] - 0.15 * 2 * FOC_HALF[1])
                x0, x1 = int((win[0] - f[0]) / (f[1] - f[0]) * W), int((win[1] - f[0]) / (f[1] - f[0]) * W + 0.5)
                y0, y1 = int((f[2] - win[2]) / (f[2] - f[3]) * H), int((f[2] - win[3]) / (f[2] - f[3]) * H + 0.5)
                v['pixels2'] = ((full.astype(np.uint32).reshape(H // 3, 3, W // 3, 3, 4).sum(axis=(1, 3)) + 4) // 9).astype(np.uint8)
                v['scale2'] = 3
                v['pixels'] = full[y0:y1, x0:x1].copy()
                v['cropOffset'] = [x0, y0]
        return subs
    t2 = sequence('twin_two_streams', reals=4, single=True, extra=two_streams)
    a = argparse.Namespace(capture=t2, flight_log=os.path.join(t2, 'fake-flight.log'), eyes='0', out=None, crop=[hud.replace('hud:focus:', 'hud:focus:').rsplit(':', 1)[0] + ':auto', cur], band_from=None, like=g, max_triples=0, grid=8, seam_density=0.5, quiet=True)
    r = res['twin_two_streams'] = verdict.run(a)
    s = r['summary']
    on = res['good_2x']['summary']
    expect('twin in two streams, like with like: same grids as the option-on capture, ratio 0.5 on both, seam judged, cursor crop works',
           abs(s['interpPeriphery']['deviationMedian']) < 0.03 and abs(s['interpFocus']['deviationMedian']) < 0.03 and r['verdicts']['seam.gradient']['verdict'] == 'PASS' and r['verdicts']['alpha.equalsNext']['verdict'] == 'NOT JUDGED'
           and abs(s['crops']['cursor']['positionFractionMedian'] - 0.5) < 0.05 and r['hudLocator']['hud']['found'],
           'periphery %+.4f (ON %+.4f), focus window %+.4f (ON %+.4f), seam %.3f, cursor %.3f, HUD located %s; %s' % (
               s['interpPeriphery']['deviationMedian'], on['interpPeriphery']['deviationMedian'], s['interpFocus']['deviationMedian'], on['interpFocus']['deviationMedian'], s['seam']['seamSyntheticOverRealMax'],
               s['crops']['cursor']['positionFractionMedian'], r['hudLocator']['hud'].get('boxTangents'), r['likeWithLike'][:40]))
    # the HUD locator on the option-on capture: finds the drawn HUD where it is, and nothing without one
    cap = riglib.load_capture(g)
    v0 = riglib.pick_layers(cap['subs'][0])[1]['views'][0]
    img = riglib.view_pixels(cap, v0)
    hudr = riglib.find_hud(img)
    f = riglib.view_fov_of_pixels(v0)
    bt = [f[0] + hudr['box'][0] / FW * (f[1] - f[0]), f[0] + hudr['box'][2] / FW * (f[1] - f[0]), f[2] - hudr['box'][1] / FH * (f[2] - f[3]), f[2] - hudr['box'][3] / FH * (f[2] - f[3])] if hudr['found'] else None
    nohud = np.clip(render(f, FW, FH, 0.0, False, None, False) + 0.5, 0, 255).astype(np.uint8)
    expect('HUD locator: box around the drawn HUD (-0.10..0.10, 0.02..-0.02 tangents), nothing found without it',
           hudr['found'] and abs(bt[0] + 0.10) < 0.03 and abs(bt[1] - 0.10) < 0.03 and abs(bt[2] - 0.02) < 0.03 and abs(bt[3] + 0.02) < 0.03 and not riglib.find_hud(nohud)['found'],
           'found %s px, box tangents %s; without HUD: %s' % (hudr.get('pixels'), None if bt is None else [round(x, 3) for x in bt], riglib.find_hud(nohud).get('why', 'FOUND')))
    # nothing to tell real from synthetic: says so
    u = sequence('unknown_kind', reals=3)
    capu = riglib.load_capture(u)
    lines = open(os.path.join(u, 'capture.jsonl')).read().splitlines()
    hdr = json.loads(lines[0])
    hdr['swapchains'] = []
    body = [json.loads(x) for x in lines[1:]]
    for o in body:
        for l in o.get('layers', []):
            for vv in l.get('views', []):
                vv['scId'], vv['releaseSeq'] = -1, o['n']            # swapchains the layer did not see being created
    open(os.path.join(u, 'capture.jsonl'), 'w').write('\n'.join([json.dumps(hdr)] + [json.dumps(o) for o in body]) + '\n')
    r = res['unknown_kind'] = V(u)
    expect('nothing to tell real from synthetic by: every triple verdict NOT JUDGED', r['verdicts']['interpFocus.ratio']['verdict'] == 'NOT JUDGED' and not r['triples'], r['verdicts']['interpFocus.ratio'].get('why', '')[:90])
    return res


def main():
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    test_compose()
    res = test_verdicts()
    # the composer's files, on the good case
    rc = os.system('py "%s" "%s" --n 1 --eyes 0 --layers > "%s"' % (os.path.join(HERE, 'compose.py'), os.path.join(OUT, 'good_2x'), os.path.join(OUT, 'compose-cli.txt')))
    pngs = [f for f in os.listdir(os.path.join(OUT, 'good_2x', 'composed'))] if os.path.isdir(os.path.join(OUT, 'good_2x', 'composed')) else []
    expect('compose.py command line writes the composed image, both layers and the alpha', rc == 0 and len(pngs) == 4, sorted(pngs))
    json.dump({'checks': CHECKS, 'summaries': {k: {'cadence': v['cadence'], 'verdicts': v['verdicts'], 'summary': v['summary']} for k, v in res.items()}}, open(os.path.join(OUT, 'results.json'), 'w'), indent=1, default=float)
    bad = [c for c in CHECKS if not c['ok']]
    print('\n%d checks, %d failed' % (len(CHECKS), len(bad)))
    if '--keep' not in sys.argv:
        for d in os.listdir(OUT):                 # keep the numbers and one case's pictures; drop the raw pixels
            p = os.path.join(OUT, d)
            if os.path.isdir(p):
                for f in os.listdir(p):
                    if f.endswith('.rgba'):
                        os.remove(os.path.join(p, f))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
