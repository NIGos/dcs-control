"""Verdict tools for a capture sequence of the rig capture layer (real and synthetic submissions below OFXR).

  py verdict.py <capture dir> [--flight-log ofxr-bridge-flight-*.log] [--eyes 0] [--out DIR]
                [--crop NAME:LAYER:l,r,u,d ...] [--band-from <two-layer capture dir>] [--max-triples N] [--grid 4]

Per triple real(n), synthetic, real(n+1) (at 3X: two synthetics, fractions 1/3 and 2/3):
 (a) seam         composed image (compose.py, encoded blend): gradient in the focus fade band against just inside and
                  just outside, and the two layers' disagreement in the band; synthetic against its real neighbours.
 (b) interpolation per layer: motion real->real and real->synthetic by phase correlation per region, corrected for a
                  moved field of view; ratio = where the synthetic sits (expected: the cadence fraction); and whether
                  the synthetic simply equals a neighbour.
 (c) alpha        synthetic focus alpha against the following real frame's alpha (moved by the field of view when they
                  differ); holes; the zone the previous real frame did not cover (alpha, black pixels, layer disagreement).
 (d) crops        for each --crop (a tangent box, LAYER = focus or periphery): sharpness and position of the bright
                  symbology / cursor in the synthetic against its neighbours.
 (e) duplicates   repeats (same image re-submitted: exact, from the layer's release counter), new images with the same
                  pixels, display-time steps, the cadence pattern.
Real or synthetic: from the flight log when given (op=presenter_submission, b = display time, c = 1 real / 2 synthetic
/ 0 repeat: exact); else from the order OFXR creates its private swapchains in (two "current", then the "synthetic"
ones, per application swapchain: read in the code, never checked on the headset); the pose rule (a synthetic carries
the pose of the real frame after it) is reported beside it. Without either the triple verdicts are NOT JUDGED.
Every threshold is a GUESS until a twin run (option off, same scenario) gives the floor: see THRESHOLDS.
Output: verdict.json and verdict.txt in --out (default: the capture folder)."""
import argparse
import json
import os
import re
import sys

import numpy as np
from scipy import ndimage

import compose
import riglib

THRESHOLDS = {   # all guesses; "twin" = to be replaced by the same number of the composed twin run
    'interp_ratio_tolerance': 0.15,       # |median ratio - cadence fraction| (recon: 0.15 of a frame's motion)
    'interp_min_motion_px': 1.0,          # a region counts when real->real moved at least this much (in the measured layer's pixels)
    'interp_min_regions': 3,
    'interp_min_quality': 0.10,           # correlation peak quality a region needs (flown 2026-11-15: below it flat regions read 0 or 1 at random;
                                          # at >= 0.10 the composed twin reads 0.51, p10 0.47, p90 0.55)
    'interp_min_regions_pooled': 8,       # fewer regions with motion in the whole capture = not judged
    'equals_neighbour_share': 0.98,       # byte-equal share that calls a synthetic "a copy of a neighbour"
    'still_mad': 0.5,                     # real->real mean abs luma difference below this = still scene, not judged
    'seam_synthetic_over_real': 1.25,     # band gradient ratio, synthetic / real neighbours
    'mismatch_synthetic_over_real': 1.5,  # layer disagreement in the band, synthetic / real (recon: 1.5)
    'alpha_mean_abs_max': 2.0,            # of 255, synthetic alpha against the expected alpha
    'alpha_hole_share_max': 0.0005,       # share of opaque-expected pixels whose alpha fell below half
    'uncovered_black_share_max': 0.01,    # visible (alpha > 8) black pixels in the uncovered zone
    'uncovered_mismatch_over_interior': 2.0,
    'uncovered_interior_floor': 2.0,      # of 255: the interior's mismatch is not taken below this (gaze flight 2026-11-15: a flat interior of 0.76
                                          # turned a strip mismatch of 2.15, lower than most, into a ratio of 2.85)
    'crop_sharpness_min': 0.9,            # synthetic / mean of the neighbours (recon: stroke energy >= 0.9)
    'crop_position_tolerance': 0.2,       # of the neighbours' displacement; or 2 px when they are still
    'duplicate_equal_share': 0.999,
}


# ---- classification ------------------------------------------------------------------------------------------------
def read_flight_log(path):
    kinds = {}
    rx = re.compile(r' op=presenter_submission result=(-?\d+) .* b=(\d+) c=(\d+)')
    with open(path, 'r', errors='replace') as f:
        for line in f:
            if ' op=presenter_submission ' in line:
                m = rx.search(line)
                if m:
                    kinds[int(m.group(2))] = {1: 'real', 2: 'synthetic', 0: 'repeat'}.get(int(m.group(3)), 'unknown')
    return kinds


def read_app_times(path):
    """Display times the application submitted its frames for (op=app_end_frame, b): the content clock of real frames."""
    out = []
    rx = re.compile(r' phase=B op=app_end_frame .* b=(\d+) ')
    with open(path, 'r', errors='replace') as f:
        for line in f:
            if ' op=app_end_frame ' in line:
                m = rx.search(line)
                if m:
                    out.append(int(m.group(1)))
    return sorted(set(out))


class Layers:
    """Which images play "periphery" and "focus" for the measures.
    Two-layer capture: the two submitted layers as they are.
    Single-layer capture (the twin) with --like <option-on capture>: the composed image resampled onto the option-on
    capture's periphery grid (its field of view and pixel size; from the scaled whole-image stream when there is one,
    low-passed first when that reduces it) and onto its focus grid (the same tangent window at the focus layer's pixel
    size; from the full-resolution window stream). So both arms are measured over the same angular window at the same
    angular resolution. Without --like: the single layer plays the periphery (its whole-image stream when it has one)."""

    def __init__(self, cap, like, eyes):
        self.cap, self.eyes, self.cache, self.virtual, self.ref = cap, eyes, {}, False, None
        two = any(riglib.pick_layers(s)[1] is not None for s in cap['subs'])
        self.note = 'two layers, measured as submitted'
        if two:
            return
        self.note = 'single layer, measured as submitted (no --like)'
        if like:
            rc = riglib.load_capture(like)
            rs = next((x for x in rc['subs'] if riglib.pick_layers(x)[1] is not None), None)
            if rs is not None:
                rp, rf = riglib.pick_layers(rs)
                self.ref = {e: {'per': (riglib.view_fov_of_pixels(rp['views'][e]), rp['views'][e]['w'], rp['views'][e]['h']),
                                'foc': (riglib.view_fov_of_pixels(rf['views'][e]), rf['views'][e]['w'], rf['views'][e]['h'])} for e in eyes if e < len(rp['views'])}
                self.virtual = True
                self.note = 'single layer resampled onto the periphery and focus grids of %s (same tangent windows, same pixels per tangent)' % os.path.abspath(like)

    def _native(self, sub):
        layer = riglib.pick_layers(sub)[0]
        if layer is None:
            return None, None
        whole = [riglib.stream_view(v) or v for v in layer['views']]
        return layer, dict(layer, views=whole)

    def _build(self, sub):
        key = sub['n']
        if key in self.cache:
            return self.cache[key]
        if len(self.cache) > 8:
            self.cache.pop(next(iter(self.cache)))
        layer, whole = self._native(sub)
        out = {'per': None, 'foc': None}
        if layer is not None:
            for name, srcl in (('per', whole), ('foc', layer)):
                views = []
                for e, v in enumerate(srcl['views']):
                    nv = dict(v)
                    if e in self.ref:
                        fov, w, h = self.ref[e][name]
                        img = riglib.view_pixels(self.cap, v)
                        if img is not None:
                            fsrc = riglib.view_fov_of_pixels(v)
                            ratio = (img.shape[1] / (fsrc[1] - fsrc[0])) / (w / (fov[1] - fov[0]))      # source pixels per target pixel
                            f = img.astype(np.float32)
                            if ratio > 1.2:
                                f = ndimage.gaussian_filter(f, (0.45 * ratio, 0.45 * ratio, 0))
                            tx, ty = riglib.tangent_grid(fov, w, h)
                            r, inside = riglib.resample(f, fsrc, tx, ty)
                            r = np.clip(r + 0.5, 0, 255).astype(np.uint8)
                            r[..., 3] = np.where(inside, 255, 0)
                            nv.update({'_px': r, 'fov': [float(np.arctan(x)) for x in fov], 'rect': [0, 0, w, h], 'w': w, 'h': h, 'cropOffset': [0, 0], 'scale': 1, 'bgra': False,
                                       'coveredShare': float(inside.mean()), 'resampledFromPixelsPerPixel': float(ratio)})
                        else:
                            nv.pop('file', None)
                    views.append(nv)
                out[name] = dict(srcl, views=views, virtual=True)
        self.cache[key] = out
        return out

    def _native_cached(self, sub):
        if sub['n'] not in self.cache:
            if len(self.cache) > 64:
                self.cache.pop(next(iter(self.cache)))
            self.cache[sub['n']] = self._native(sub)[1]
        return self.cache[sub['n']]

    def per(self, sub):
        if self.virtual:
            return self._build(sub)['per']
        per, foc = riglib.pick_layers(sub)
        if per is not None and foc is None:
            return self._native_cached(sub)
        return per

    def foc(self, sub):
        if self.virtual:
            return self._build(sub)['foc']
        return riglib.pick_layers(sub)[1]


def ring_roles(cap):
    """scId -> 'app' | 'current' | 'synthetic' | None from the creation order (see the module text)."""
    table = {}
    for s in cap['header'].get('swapchains', []):
        table[s['id']] = (s['w'], s['h'], s['format'], s['array'])
    for sub in cap['subs']:
        for _, l in riglib.projection_layers(sub):
            for v in l['views']:
                if v.get('scId', -1) >= 0:
                    table.setdefault(v['scId'], (v['scW'], v['scH'], v['format'], v['scArray']))
    # Per group of swapchains of one shape, in creation order, OFXR's order is: [the application's own, when the
    # runtime has one of that shape], current, current, synthetic, [more synthetic slots]. Two readings are possible
    # for a group of four or more (with or without the application's own first); the one that fits the capture is
    # taken: by the pose rule when the pose moves, else "the first one is never submitted = the application's own".
    seq = []
    for sub in cap['subs']:
        views = [v for _, l in riglib.projection_layers(sub) for v in l['views']]
        seq.append(({v.get('scId') for v in views}, tuple(tuple(round(x, 7) for x in v['pose']) for v in views)))
    used = set().union(*[s[0] for s in seq]) if seq else set()
    groups = {}
    for sid in sorted(table):
        groups.setdefault(table[sid], []).append(sid)
    roles = {}
    usage = {s['id']: s.get('usage') for s in cap['header'].get('swapchains', [])}
    for ids in groups.values():
        # OFXR's private swapchains ask for another usage than the application's (flown 2026-11-15: 49 against 33; its
        # own test: 17 against 1). When a shape has both kinds, the private ones are the ring: two current, then the
        # synthetic slots (one, or two with the Smoothness buffer); the others are the application's (Quad Views makes
        # its swapchains more than once). This is what the creation order alone got wrong in the gaze flight.
        kinds = {}
        for sid in ids:
            kinds.setdefault(usage.get(sid), []).append(sid)
        if len(kinds) >= 2 and None not in kinds:
            private = [u for u in kinds if all(u != o and (u & o) == o for o in kinds if o != u)]      # the usage that holds every other one's bits
            ring = kinds[private[0]] if len(private) == 1 else []
            size = len(ring) if 3 <= len(ring) <= 6 else 4 if ring and len(ring) % 4 == 0 else 3 if ring and len(ring) % 3 == 0 else 0
            if size:      # one ring per application swapchain of that shape (the twin: one per eye), made one after the other
                for sid in ids:
                    roles[sid] = 'app' if sid not in ring else 'current' if ring.index(sid) % size < 2 else 'synthetic'
                continue
        if len(ids) < 3:
            for sid in ids:
                roles[sid] = 'app'                    # nothing private of this shape: the application's own images
            continue
        models = [{sid: ('current' if k < 2 else 'synthetic') for k, sid in enumerate(ids)}]
        if len(ids) >= 4:
            models.append({sid: ('app' if k == 0 else 'current' if k < 3 else 'synthetic') for k, sid in enumerate(ids)})
        if len(models) == 1:
            roles.update(models[0])
            continue
        scores = []
        for m in models:
            good = total = 0
            for (ia, pa), (ib, pb) in zip(seq, seq[1:]):
                ra, rb = {m.get(x) for x in ia if x in m}, {m.get(x) for x in ib if x in m}
                if len(ra) != 1 or len(rb) != 1 or ia == ib:
                    continue
                ra, rb = ra.pop(), rb.pop()
                if ra == 'synthetic' and rb in ('current', 'app'):
                    total += 1
                    good += pa == pb                  # the real frame after a synthetic keeps the synthetic's pose
                elif ra in ('current', 'app') and rb == 'synthetic':
                    total += 1
                    good += pa != pb
            scores.append((good / total) if total else None)
        poses_move = len({p for _, p in seq}) > 2
        if poses_move and scores[0] is not None and scores[1] is not None and scores[0] != scores[1]:
            roles.update(models[0] if scores[0] > scores[1] else models[1])
        else:
            roles.update(models[1] if ids[0] not in used else models[0])
    return roles


def classify(cap, flight):
    roles = ring_roles(cap)
    out, prev_key, prev_pose = [], None, None
    for sub in cap['subs']:
        views = [v for _, l in riglib.projection_layers(sub) for v in l['views']]
        key = tuple((v.get('scId'), v.get('releaseSeq'), v.get('imageIndex')) for v in views)
        pose = tuple(tuple(round(x, 7) for x in v['pose']) for v in views)
        e = {'n': sub['n'], 'displayTime': sub['displayTime'], 'projectionLayers': len(riglib.projection_layers(sub)), 'repeat': bool(views) and key == prev_key,
             'poseChanged': None if prev_pose is None else pose != prev_pose}
        rs = {roles.get(v.get('scId')) for v in views}
        e['byRing'] = 'none' if not views else 'synthetic' if rs == {'synthetic'} else 'real' if rs <= {'current', 'app'} and None not in rs else 'unknown'
        e['byLog'] = flight.get(sub['displayTime']) if flight is not None else None
        if e['repeat']:
            e['kind'], e['source'] = 'repeat', 'layer (same images, nothing released)'
        elif e['byLog'] in ('real', 'synthetic'):
            e['kind'], e['source'] = e['byLog'], 'flight log'
        elif e['byRing'] in ('real', 'synthetic'):
            e['kind'], e['source'] = e['byRing'], 'swapchain creation order (unverified live)'
        else:
            e['kind'], e['source'] = 'unknown', 'none'
        out.append(e)
        prev_key, prev_pose = key, pose
    return out, roles


def triples(seq):
    """[(prev real index, [synthetic indexes], next real index)]; repeats are stepped over."""
    res, i, n = [], 0, len(seq)
    reals = [k for k, e in enumerate(seq) if e['kind'] == 'real']
    for a, b in zip(reals, reals[1:]):
        syn = [k for k in range(a + 1, b) if seq[k]['kind'] == 'synthetic']
        if syn and all(seq[k]['kind'] in ('synthetic', 'repeat') for k in range(a + 1, b)):
            res.append((a, syn, b))
    return res


# ---- image measures ------------------------------------------------------------------------------------------------
_PC = {}
WHITEN_FLOOR = 0.3      # chosen on the self-test: 0.3 reads 0.4993 on the enlarged twin (full whitening: 0.41), 0.4944 / 0.4999 on the layers


def phase_shift(a, b):
    """Translation (dx, dy) that takes a to b (content moves by +dx, +dy pixels), with a peak quality 0..1."""
    h, w = a.shape
    if (h, w) not in _PC:
        # Phase correlation on the low frequencies only (below about 0.12 cycles per pixel). An enlarged image (the
        # composed twin's periphery, any upscaled layer) carries the enlargement's own pixel grid in its high
        # frequencies, and a fully whitened correlation locks onto that grid instead of the content.
        fy, fx = np.fft.fftfreq(h)[:, None], np.fft.rfftfreq(w)[None, :]
        wgt = np.exp(-(fx * fx + fy * fy) / (2 * 0.12 ** 2)).astype(np.float32)
        _PC[(h, w)] = (np.outer(np.hanning(h), np.hanning(w)).astype(np.float32), wgt, float(np.fft.irfft2(wgt, s=(h, w))[0, 0]))
    win, wgt, ideal = _PC[(h, w)]
    fa = np.fft.rfft2((a - a.mean()) * win)
    fb = np.fft.rfft2((b - b.mean()) * win)
    cross = fb * np.conj(fa)
    mag = np.abs(cross)
    # Whitened only where the images have energy (a floor of the band's mean): the weak mirror spectra that an
    # enlargement leaves near its grid frequency would otherwise count as much as the content.
    floor = float((mag * wgt).sum() / wgt.sum()) * WHITEN_FLOOR
    corr = np.fft.irfft2(cross / (mag + max(floor, 1e-9)) * wgt, s=(h, w))
    py, px = np.unravel_index(np.argmax(corr), corr.shape)
    peak = float(corr[py, px]) / ideal

    def sub(cm, c0, cp):
        if cm > 0 and c0 > 0 and cp > 0:              # the peak is close to a Gaussian: fit the parabola to its logarithm
            cm, c0, cp = np.log(cm), np.log(c0), np.log(cp)
        den = cm - 2 * c0 + cp
        return 0.0 if abs(den) < 1e-12 else float(np.clip(0.5 * (cm - cp) / den, -0.5, 0.5))
    dx = px + sub(corr[py, (px - 1) % w], corr[py, px], corr[py, (px + 1) % w])
    dy = py + sub(corr[(py - 1) % h, px], corr[py, px], corr[(py + 1) % h, px])
    if dx > w / 2:
        dx -= w
    if dy > h / 2:
        dy -= h
    return dx, dy, peak


def region_shifts(a, b, grid, valid=None):
    """Per region of a grid x grid split: (dx, dy, quality) or None (flat, or less than 90 % of it inside `valid`)."""
    h, w = a.shape
    th, tw = h // grid, w // grid
    if th == 0 or tw == 0:      # an image smaller than the grid (the 4 x 4 views of OFXR's call-chain test): no region to measure
        return [None] * (grid * grid)
    out = []
    for gy in range(grid):
        for gx in range(grid):
            ta, tb = a[gy * th:(gy + 1) * th, gx * tw:(gx + 1) * tw], b[gy * th:(gy + 1) * th, gx * tw:(gx + 1) * tw]
            if valid is not None and valid[gy * th:(gy + 1) * th, gx * tw:(gx + 1) * tw].mean() < 0.9:
                out.append(None)
                continue
            if ta.std() < 1.0 or tb.std() < 1.0:
                out.append(None)
                continue
            out.append(phase_shift(ta, tb))
    return out


def fov_shift_px(view_a, view_b):
    """Pixel shift a world-fixed point shows between two views only because the field of view moved (b against a)."""
    fa, fb = riglib.view_fov_of_pixels(view_a), riglib.view_fov_of_pixels(view_b)
    ppx, ppy = view_a['w'] / (fa[1] - fa[0]), view_a['h'] / (fa[2] - fa[3])
    return -(fb[0] - fa[0]) * ppx, (fb[2] - fa[2]) * ppy


def grad_mag(l):
    return np.hypot(ndimage.sobel(l, axis=1, mode='nearest'), ndimage.sobel(l, axis=0, mode='nearest')) / 8.0


class Pixels:
    def __init__(self, cap):
        self.cap, self.cache = cap, {}

    def get(self, sub, layer, eye):
        key = (sub['n'], id(layer), eye)
        if key not in self.cache:
            if len(self.cache) > 24:
                self.cache.pop(next(iter(self.cache)))
            self.cache[key] = riglib.view_pixels(self.cap, layer['views'][eye]) if eye < len(layer['views']) else None
        return self.cache[key]


def interp_measure(px, subs, p, s, c, layer_of, eye, grid, fraction):
    lp, ls, lc = layer_of(subs[p]), layer_of(subs[s]), layer_of(subs[c])
    if lp is None or ls is None or lc is None:
        return None
    ip, is_, ic = px.get(subs[p], lp, eye), px.get(subs[s], ls, eye), px.get(subs[c], lc, eye)
    if ip is None or is_ is None or ic is None or ip.shape != is_.shape or ip.shape != ic.shape:
        return None
    a, m, b = riglib.luma(ip), riglib.luma(is_), riglib.luma(ic)
    r = {'equalPrevShare': float((ip == is_).all(axis=2).mean()), 'equalNextShare': float((ic == is_).all(axis=2).mean()),
         'madPrevNext': float(np.abs(a - b).mean()), 'madSynPrev': float(np.abs(m - a).mean()), 'madSynNext': float(np.abs(m - b).mean())}
    vp, vs, vc = lp['views'][eye], ls['views'][eye], lc['views'][eye]
    fp, fs, fc = riglib.view_fov_of_pixels(vp), riglib.view_fov_of_pixels(vs), riglib.view_fov_of_pixels(vc)
    fpc = fov_shift_px(vp, vc)
    r['fovShiftPrevNextPx'] = [round(fpc[0], 3), round(fpc[1], 3)]
    r['fovSizeRatioPrevNext'] = float((fc[1] - fc[0]) / (fp[1] - fp[0]))
    r['synFovIs'] = 'next' if np.allclose(vs['fov'], vc['fov'], atol=1e-7) else 'prev' if np.allclose(vs['fov'], vp['fov'], atol=1e-7) else 'other'
    r['fovMoved'] = bool(not np.allclose(fp, fc, atol=1e-6))
    # Motion is measured in view space (tangents): a frame whose field of view differs from the previous real frame's
    # (moved or resized focus box) is first resampled onto that frame's grid, so a world point that does not move in
    # the view reads zero whatever the box did. Regions not covered by all three frames are left out.
    valid = None
    if not np.allclose(fs, fp, atol=1e-9) or not np.allclose(fc, fp, atol=1e-9):
        tx, ty = riglib.tangent_grid(fp, a.shape[1], a.shape[0])
        m, v1 = riglib.resample(m, fs, tx, ty)
        b, v2 = riglib.resample(b, fc, tx, ty)
        valid = v1 & v2
        r['validShare'] = float(valid.mean())
    regions = []
    for k, (spc, sps) in enumerate(zip(region_shifts(a, b, grid, valid), region_shifts(a, m, grid, valid))):
        if spc is None or sps is None or min(spc[2], sps[2]) < THRESHOLDS['interp_min_quality']:
            continue
        dpc, dps = np.array(spc[:2]), np.array(sps[:2])     # motion in view space, in the previous frame's pixels
        n2 = float(dpc @ dpc)
        if n2 < THRESHOLDS['interp_min_motion_px'] ** 2:
            continue
        regions.append({'gx': k % grid, 'gy': k // grid, 'movePrevNext': [round(float(dpc[0]), 3), round(float(dpc[1]), 3)], 'movePrevSyn': [round(float(dps[0]), 3), round(float(dps[1]), 3)],
                        'ratio': float(dps @ dpc) / n2, 'residualPx': float(np.linalg.norm(dps - fraction * dpc)), 'motionPx': n2 ** 0.5, 'quality': round(min(spc[2], sps[2]), 3)})
    r['regions'] = len(regions)
    r['regionList'] = regions
    r['_luma'] = a                                   # for the estimator's control; removed before the JSON is written
    if regions:
        ratios = [x['ratio'] for x in regions]
        r.update({'ratioMedian': float(np.median(ratios)), 'ratioP10': float(np.percentile(ratios, 10)), 'ratioP90': float(np.percentile(ratios, 90)),
                  'residualPxMedian': float(np.median([x['residualPx'] for x in regions])), 'motionPxMedian': float(np.median([x['motionPx'] for x in regions]))})
    return r


def estimator_control(a, move, grid, fraction):
    """The estimator's own answer on this content: `a` shifted rigidly by `move` as the next frame and by
    fraction * move as the synthetic. Returns the per-region ratios (the true answer is `fraction`)."""
    F = np.fft.rfft2(a)
    fy, fx = np.fft.fftfreq(a.shape[0])[:, None], np.fft.rfftfreq(a.shape[1])[None, :]
    sh = lambda k: np.fft.irfft2(F * np.exp(-2j * np.pi * (fx * move[0] * k + fy * move[1] * k)), s=a.shape).astype(np.float32)
    b, m = sh(1.0), sh(fraction)
    out = []
    for spc, sps in zip(region_shifts(a, b, grid), region_shifts(a, m, grid)):
        if spc is None or sps is None or min(spc[2], sps[2]) < THRESHOLDS['interp_min_quality']:
            continue
        dpc, dps = np.array(spc[:2]), np.array(sps[:2])
        if float(dpc @ dpc) >= THRESHOLDS['interp_min_motion_px'] ** 2:
            out.append(float(dps @ dpc) / float(dpc @ dpc))
    return out


def seam_measure(cap, subs, idx, eye, grid_ref, band_ref):
    """Seam numbers of one submission on a fixed output grid. band_ref: (alpha, inside) from another capture for a twin."""
    r = compose.compose_submission(cap, subs[idx], eye, out_grid=grid_ref)
    if r is None:
        return None
    alpha, inside = (r['alpha'], r['inside']) if r['twoLayers'] else band_ref if band_ref is not None else (None, None)
    if alpha is None:
        return None
    band = inside & (alpha > 0.02) & (alpha < 0.98)
    if band.sum() < 50:
        return {'bandPixels': int(band.sum())}
    width = max(3, int(round(2 * ndimage.distance_transform_edt(band).max())))      # the band's own thickness
    near = ndimage.binary_dilation(band, iterations=width)
    inner, outer = near & inside & (alpha >= 0.98), near & ((~inside) | (alpha <= 0.02))
    g = grad_mag(riglib.luma(r['rgb']))
    out = {'bandPixels': int(band.sum()), 'gradBand': float(g[band].mean()), 'gradInside': float(g[inner].mean()) if inner.any() else None, 'gradOutside': float(g[outer].mean()) if outer.any() else None}
    ref = [x for x in (out['gradInside'], out['gradOutside']) if x is not None]
    out['seamRatio'] = out['gradBand'] / max(1e-6, float(np.mean(ref))) if ref else None
    if r['twoLayers']:
        # the layers' disagreement at the periphery's own resolution (the focus image holds detail the periphery cannot)
        per_l = riglib.pick_layers(subs[idx])[0]
        fper = riglib.view_fov_of_pixels(per_l['views'][eye])
        sigma = 0.6 * (r['w'] / (r['fov'][1] - r['fov'][0])) / (per_l['views'][eye]['w'] / (fper[1] - fper[0]))
        dis = np.abs(ndimage.gaussian_filter(riglib.luma(r['foc']), sigma) - ndimage.gaussian_filter(riglib.luma(r['per']), sigma))
        out['mismatchBand'] = float(dis[band].mean())
        out['mismatchInside'] = float(dis[inner].mean()) if inner.any() else None
    return out


def alpha_measure(px, subs, p, s, c, focus_of, per_of, eye):
    ls, lc, lp = focus_of(subs[s]), focus_of(subs[c]), focus_of(subs[p])
    if ls is None or lc is None or lp is None:
        return None
    is_, ic = px.get(subs[s], ls, eye), px.get(subs[c], lc, eye)
    if is_ is None or ic is None:
        return None
    vs, vc, vp = ls['views'][eye], lc['views'][eye], lp['views'][eye]
    fs, fc, fp = riglib.view_fov_of_pixels(vs), riglib.view_fov_of_pixels(vc), riglib.view_fov_of_pixels(vp)
    h, w = is_.shape[:2]
    tx, ty = riglib.tangent_grid(fs, w, h)
    a_s = is_[..., 3].astype(np.float32)
    if np.allclose(fs, fc, atol=1e-9) and ic.shape == is_.shape:
        expect, valid = ic[..., 3].astype(np.float32), np.ones((h, w), bool)
    else:
        expect, valid = riglib.resample(ic[..., 3], fc, tx, ty)
    d = np.abs(a_s - expect)[valid]
    opaque = valid & (expect >= 128)
    holes = opaque & (a_s < 0.5 * expect)
    r = {'alphaMin': int(a_s.min()), 'alphaMax': int(a_s.max()), 'meanAbs': float(d.mean()), 'p99': float(np.percentile(d, 99)), 'max': float(d.max()),
         'holePixels': int(holes.sum()), 'holeShare': float(holes.sum() / max(1, opaque.sum())), 'flagsSynthetic': int(ls['flags']), 'flagsNext': int(lc['flags'])}
    # the zone of the synthetic's focus image that the previous real frame's focus image did not cover
    unc = (tx < fp[0]) | (tx > fp[1]) | (ty > fp[2]) | (ty < fp[3])
    r['uncoveredShare'] = float(unc.mean())
    if unc.mean() > 0.002:
        lper = per_of(subs[s])
        ip = px.get(subs[s], lper, eye) if lper is not None else None
        vis = unc & (a_s > 8)
        r['uncoveredAlphaMean'] = float(a_s[unc].mean())
        r['uncoveredExpectedAlphaMean'] = float(expect[unc & valid].mean()) if (unc & valid).any() else None
        r['uncoveredBlackShare'] = float(((is_[..., :3].max(axis=2) <= 2) & vis).sum() / max(1, vis.sum()))
        if ip is not None:
            fper = riglib.view_fov_of_pixels(lper['views'][eye])
            pg, _ = riglib.resample(riglib.luma(ip), fper, tx, ty)
            sigma = 0.6 * (w / (fs[1] - fs[0])) / (ip.shape[1] / (fper[1] - fper[0]))      # at the periphery's resolution
            dis = np.abs(ndimage.gaussian_filter(riglib.luma(is_), sigma) - ndimage.gaussian_filter(pg, sigma))
            interior = (~unc) & (a_s >= 250)
            r['uncoveredMismatch'] = float(dis[vis].mean()) if vis.any() else None
            r['interiorMismatch'] = float(dis[interior].mean()) if interior.any() else None
    return r


def crop_box(view, box):
    f = riglib.view_fov_of_pixels(view)
    w, h = view['w'], view['h']
    x0, x1 = (box[0] - f[0]) / (f[1] - f[0]) * w, (box[1] - f[0]) / (f[1] - f[0]) * w
    y0, y1 = (f[2] - box[2]) / (f[2] - f[3]) * h, (f[2] - box[3]) / (f[2] - f[3]) * h
    xi0, xi1, yi0, yi1 = int(round(x0)), int(round(x1)), int(round(y0)), int(round(y1))
    if xi0 < 0 or yi0 < 0 or xi1 > w or yi1 > h or xi1 - xi0 < 8 or yi1 - yi0 < 8:
        return None
    return xi0, yi0, xi1, yi1


def blob(l):
    """Centroid (x, y) and area of the bright marks of a crop: pixels above the midpoint between median and maximum."""
    med, mx = float(np.median(l)), float(l.max())
    if mx - med < 12:
        return None
    m = l > med + 0.5 * (mx - med)
    lab, n = ndimage.label(m)
    if n > 1:                                         # the mark that holds the brightest pixel, not stray bright texture
        m = lab == lab[np.unravel_index(np.argmax(l), l.shape)]
    wgt = (l - med) * m
    ys, xs = np.mgrid[0:l.shape[0], 0:l.shape[1]]
    return float((xs * wgt).sum() / wgt.sum()), float((ys * wgt).sum() / wgt.sum()), int(m.sum())


def crop_measure(px, subs, p, s, c, layer_of, eye, box, fraction):
    out = {}
    crops = []
    for i in (p, s, c):
        layer = layer_of(subs[i])
        img = px.get(subs[i], layer, eye) if layer is not None else None
        rect = crop_box(layer['views'][eye], box) if img is not None else None
        if rect is None:
            return None
        l = riglib.luma(img[rect[1]:rect[3], rect[0]:rect[2]])
        crops.append((l, rect))
    if len({c_[0].shape for c_ in crops}) != 1:
        return None
    sharp = [float((grad_mag(l) ** 2).mean()) for l, _ in crops]
    out['sharpness'] = {'prev': sharp[0], 'synthetic': sharp[1], 'next': sharp[2]}
    out['sharpnessRatio'] = sharp[1] / max(1e-9, (sharp[0] + sharp[2]) / 2)
    bl = [blob(l) for l, _ in crops]
    if all(b is not None for b in bl):
        # positions in the layer's pixel grid (the crop rectangle follows the field of view)
        # positions in view space (tangents), in units of the previous frame's pixels: a moved or resized box cancels out
        layer_views = [layer_of(subs[i])['views'][eye] for i in (p, s, c)]
        f0 = riglib.view_fov_of_pixels(layer_views[0])
        ppx, ppy = layer_views[0]['w'] / (f0[1] - f0[0]), layer_views[0]['h'] / (f0[2] - f0[3])
        pos = []
        for b, (_, r), v in zip(bl, crops, layer_views):
            f = riglib.view_fov_of_pixels(v)
            pos.append(np.array([(f[0] + (b[0] + r[0]) / v['w'] * (f[1] - f[0])) * ppx, -(f[2] - (b[1] + r[1]) / v['h'] * (f[2] - f[3])) * ppy]))
        dpc = pos[2] - pos[0]
        dps = pos[1] - pos[0]
        out['blobAreaRatio'] = bl[1][2] / max(1.0, (bl[0][2] + bl[2][2]) / 2)
        out['movePrevNextPx'] = float(np.linalg.norm(dpc))
        if np.linalg.norm(dpc) >= 0.5:
            out['positionFraction'] = float(dps @ dpc / (dpc @ dpc))
            out['positionErrorPx'] = float(np.linalg.norm(dps - fraction * dpc))
        else:
            out['positionErrorPx'] = float(np.linalg.norm(dps))
    return out


# ---- the run -------------------------------------------------------------------------------------------------------
def med(xs):
    xs = [x for x in xs if x is not None]
    return float(np.median(xs)) if xs else None


def judge(value, ok, why_none='no data'):
    if value is None:
        return {'verdict': 'NOT JUDGED', 'why': why_none}
    return {'verdict': 'PASS' if ok else 'FAIL', 'value': value}


def run(a):
    cap = riglib.load_capture(a.capture)
    subs = cap['subs']
    flight = read_flight_log(a.flight_log) if a.flight_log else None
    seq, roles = classify(cap, flight)
    eyes = [int(x) for x in a.eyes.split(',')]
    px = Pixels(cap)
    like = getattr(a, 'like', None) or a.band_from
    a.band_from = like
    src = Layers(cap, like, eyes)
    per_of, foc_of = src.per, src.foc
    res = {'likeWithLike': src.note,'capture': os.path.abspath(a.capture), 'thresholds': dict(THRESHOLDS), 'thresholdsAre': 'guesses until the twin run gives the floor',
           'submissions': len(subs), 'end': cap['end'], 'sequence': seq, 'ringRoles': {str(k): v for k, v in roles.items()}}
    kinds = [e['kind'] for e in seq]
    res['cadence'] = ''.join({'real': 'R', 'synthetic': 'S', 'repeat': 'r', 'unknown': '?'}[k] for k in kinds)
    both = [e for e in seq if e['byLog'] in ('real', 'synthetic') and e['byRing'] in ('real', 'synthetic')]
    res['classification'] = {'source': sorted({e['source'] for e in seq}), 'flightLogMatched': sum(1 for e in seq if e['byLog']) if flight is not None else None,
                             'ringOrderAgreesWithLog': None if not both else sum(1 for e in both if e['byLog'] == e['byRing']) / len(both),
                             # pose rule: a synthetic carries the pose of the real frame after it, so the real frame after a synthetic does not change the pose
                             'poseRuleRealAfterSyntheticUnchanged': None if len(seq) < 3 else float(np.mean([not seq[i]['poseChanged'] for i in range(1, len(seq)) if seq[i]['kind'] == 'real' and seq[i - 1]['kind'] == 'synthetic'] or [np.nan]))}
    # (e) duplicates and timing
    dts = np.diff([s['displayTime'] for s in subs]) / 1e6 if len(subs) > 1 else np.array([])
    dup = []
    for i in range(1, len(subs)):
        if seq[i]['repeat']:
            dup.append({'n': subs[i]['n'], 'kind': 'repeat (same image re-submitted)'})
            continue
        shares = []
        for layer_of in (per_of, foc_of):
            la, lb = layer_of(subs[i - 1]), layer_of(subs[i])
            if la is None or lb is None:
                continue
            ia, ib = px.get(subs[i - 1], la, eyes[0]), px.get(subs[i], lb, eyes[0])
            if ia is not None and ib is not None and ia.shape == ib.shape:
                shares.append(float((ia == ib).all(axis=2).mean()))
        if shares and min(shares) >= THRESHOLDS['duplicate_equal_share']:
            dup.append({'n': subs[i]['n'], 'kind': 'new image, same pixels as the submission before', 'equalShare': min(shares)})
    res['duplicates'] = {'list': dup, 'repeats': sum(1 for d in dup if d['kind'].startswith('repeat')), 'samePixels': sum(1 for d in dup if d['kind'].startswith('new')),
                         'displayStepMs': None if not len(dts) else {'min': float(dts.min()), 'median': float(np.median(dts)), 'max': float(dts.max())},
                         'irregularSteps': [int(subs[i + 1]['n']) for i in range(len(dts)) if abs(dts[i] - np.median(dts)) > 0.25 * np.median(dts)] if len(dts) else []}
    # triples
    tr = triples(seq)[:a.max_triples] if a.max_triples else triples(seq)
    band_ref = None
    crops = []
    res['hudLocator'] = {}
    first_real = next((i for i, e in enumerate(seq) if e['kind'] == 'real'), 0 if subs else None)
    for c in a.crop or []:
        name, layer, box = c.split(':')
        if box == 'auto':                 # the HUD found in the first real submission of that layer (riglib.find_hud)
            lay = (foc_of if layer == 'focus' else per_of)(subs[first_real]) if first_real is not None else None
            img = px.get(subs[first_real], lay, eyes[0]) if lay is not None else None
            hud = riglib.find_hud(img) if img is not None else {'found': False, 'why': 'no such layer'}
            if hud.get('found'):
                f = riglib.view_fov_of_pixels(lay['views'][eyes[0]])
                x0, y0, x1, y1 = hud['box']
                h_, w_ = img.shape[:2]
                hud['boxTangents'] = [f[0] + x0 / w_ * (f[1] - f[0]), f[0] + x1 / w_ * (f[1] - f[0]), f[2] - y0 / h_ * (f[2] - f[3]), f[2] - y1 / h_ * (f[2] - f[3])]
                crops.append((name, layer, tuple(hud['boxTangents'])))
            res['hudLocator'][name] = hud
            continue
        crops.append((name, layer, tuple(float(x) for x in box.split(','))))
    # display timing of the triples, and the application's own frame times from the flight log (the content clock)
    app_times = read_app_times(a.flight_log) if a.flight_log else []
    if subs:
        t0, t1 = subs[0]['displayTime'] - 300_000_000, subs[-1]['displayTime'] + 300_000_000
        steps = np.diff([t for t in app_times if t0 <= t <= t1]) / 1e6
        res['appFrameStepMs'] = None if not len(steps) else {'n': int(len(steps)), 'min': float(steps.min()), 'median': float(np.median(steps)), 'max': float(steps.max()),
                                                          'uneven': int((np.abs(steps - np.median(steps)) > 0.1 * np.median(steps)).sum())}
    out_tr = []
    for (p, syn, c) in tr:
        for k, s in enumerate(syn):
            frac = (k + 1) / (len(syn) + 1)
            t = {'prev': subs[p]['n'], 'synthetic': subs[s]['n'], 'next': subs[c]['n'], 'fraction': frac, 'eyes': {},
                 'displayFraction': (subs[s]['displayTime'] - subs[p]['displayTime']) / max(1, subs[c]['displayTime'] - subs[p]['displayTime']),
                 'displayStepMs': [(subs[s]['displayTime'] - subs[p]['displayTime']) / 1e6, (subs[c]['displayTime'] - subs[s]['displayTime']) / 1e6]}
            for eye in eyes:
                e = {}
                e['interpPeriphery'] = interp_measure(px, subs, p, s, c, per_of, eye, a.grid, frac)
                e['interpFocus'] = interp_measure(px, subs, p, s, c, foc_of, eye, a.grid, frac)
                e['alpha'] = None if src.virtual else alpha_measure(px, subs, p, s, c, foc_of, per_of, eye)
                # seam: one grid for the three (the synthetic's focus rectangle), so the band is the same pixels
                per_l, foc_l = riglib.pick_layers(subs[s])
                grid_ref = None
                if foc_l is not None:
                    grid_ref = compose.roi_for(per_l['views'][eye], foc_l['views'][eye], 'focus', 0.15, None, a.seam_density)
                elif a.band_from:
                    if band_ref is None:
                        rc = riglib.load_capture(a.band_from)
                        rs = next((x for x in rc['subs'] if riglib.pick_layers(x)[1] is not None), None)
                        if rs is not None:
                            rp, rf = riglib.pick_layers(rs)
                            g = compose.roi_for(rp['views'][eye], rf['views'][eye], 'focus', 0.15, None, a.seam_density)
                            rr = compose.compose_submission(rc, rs, eye, out_grid=g)
                            band_ref = (g, (rr['alpha'], rr['inside'])) if rr is not None else None
                    grid_ref = band_ref[0] if band_ref else None
                if grid_ref is not None:
                    br = band_ref[1] if band_ref else None
                    sm = {w: seam_measure(cap, subs, i, eye, grid_ref, br) for w, i in (('prev', p), ('synthetic', s), ('next', c))}
                    e['seam'] = sm
                    if all(sm[w] and sm[w].get('seamRatio') for w in sm):
                        e['seamSyntheticOverReal'] = sm['synthetic']['seamRatio'] / ((sm['prev']['seamRatio'] + sm['next']['seamRatio']) / 2)
                    if all(sm[w] and sm[w].get('mismatchBand') is not None for w in sm):
                        e['mismatchSyntheticOverReal'] = sm['synthetic']['mismatchBand'] / max(1e-6, (sm['prev']['mismatchBand'] + sm['next']['mismatchBand']) / 2)
                e['crops'] = {name: crop_measure(px, subs, p, s, c, foc_of if layer == 'focus' else per_of, eye, box, frac) for name, layer, box in crops}
                t['eyes'][str(eye)] = e
            out_tr.append(t)
    res['triples'] = out_tr

    # summary + verdicts
    T = THRESHOLDS
    allE = [(t, e) for t in out_tr for e in t['eyes'].values()]
    summ, ver = {}, {}
    judged = bool(out_tr)
    why = 'no real-synthetic-real triple (classification: %s; cadence %s)' % (', '.join(res['classification']['source']), res['cadence'][:60])
    for lname in ('interpPeriphery', 'interpFocus'):
        ms = [(t, e[lname]) for t, e in allE if e.get(lname)]
        moving = [(t, m) for t, m in ms if m['madPrevNext'] >= T['still_mad']]
        withr = [(t, m) for t, m in moving if m['regions'] >= 1]
        copies = [t['synthetic'] for t, m in moving if max(m['equalPrevShare'], m['equalNextShare']) >= T['equals_neighbour_share']]
        # Pooled over every region of every triple (a triple's own median of 2-5 noisy regions is not a measure):
        # deviation = ratio - cadence fraction. Split by whether the field of view moved in the triple.
        pool = [(x['ratio'] - t['fraction'], x['motionPx'], m['fovMoved']) for t, m in withr for x in m['regionList']]
        dev = np.array([x[0] for x in pool]) if pool else np.zeros(0)

        def group(sel):
            d = np.array([x[0] for x in pool if sel(x)])
            return None if not len(d) else {'regions': int(len(d)), 'deviationMedian': float(np.median(d)), 'p10': float(np.percentile(d, 10)), 'p90': float(np.percentile(d, 90)),
                                            'stdErr': float(1.2533 * d.std() / max(1, len(d)) ** 0.5)}
        # the estimator's own floor on this content: the first frames shifted rigidly by the capture's median motion
        control = []
        if len(pool) >= T['interp_min_regions']:
            mv = np.median(np.array([x['movePrevNext'] for _, m in withr for x in m['regionList']]), axis=0)
            for t, m in withr[:4]:
                control += [r_ - t['fraction'] for r_ in estimator_control(m['_luma'], mv, a.grid, t['fraction'])]
        summ[lname] = {'triples': len(ms), 'moving': len(moving), 'withMotionRegions': len(withr), 'regionsPooled': int(len(dev)),
                       'ratioMedian': None if not len(dev) else float(np.median(dev)) + (withr[0][0]['fraction'] if len({t['fraction'] for t, _ in withr}) == 1 else 0.0),
                       'deviationMedian': None if not len(dev) else float(np.median(dev)), 'all': group(lambda x: True),
                       'fieldOfViewStill': group(lambda x: not x[2]), 'fieldOfViewMoved': group(lambda x: x[2]),
                       'motionBelow2px': group(lambda x: x[1] < 2), 'motion2to5px': group(lambda x: 2 <= x[1] < 5), 'motionAbove5px': group(lambda x: x[1] >= 5),
                       'estimatorControl': None if not control else {'regions': len(control), 'deviationMedian': float(np.median(control)), 'p10': float(np.percentile(control, 10)), 'p90': float(np.percentile(control, 90))},
                       'residualPxMedian': med([x['residualPx'] for _, m in withr for x in m['regionList']]), 'motionPxMedian': med([x['motionPx'] for _, m in withr for x in m['regionList']]),
                       'equalsNeighbour': copies, 'equalPrevShareMedian': med([m['equalPrevShare'] for _, m in ms]), 'equalNextShareMedian': med([m['equalNextShare'] for _, m in ms])}
        enough = len(dev) >= T['interp_min_regions_pooled']
        ver[lname + '.ratio'] = judge(abs(float(np.median(dev))) if enough else None, enough and abs(float(np.median(dev))) <= T['interp_ratio_tolerance'],
                                      why if not judged else 'still scene, no layer, or fewer than %d regions with motion (%d)' % (T['interp_min_regions_pooled'], len(dev)))
        ver[lname + '.notACopy'] = judge(len(copies) if moving else None, not copies, why if not judged else 'still scene or no layer')
    dfr = [t['displayFraction'] - t['fraction'] for t in out_tr]
    res['timing'] = {'displayFractionMinusCadenceMedian': med(dfr), 'triplesShownUnevenly': sum(1 for x in dfr if abs(x) > 0.1), 'triples': len(dfr),
                     'note': 'the synthetic content is made for the cadence fraction of the real-to-real CONTENT interval (uniform when appFrameStepMs is even); when it was SHOWN is displayFraction: inside a capture window the capture itself delays submissions'}
    for t in out_tr:
        for e in t['eyes'].values():
            for k in ('interpPeriphery', 'interpFocus'):
                if e.get(k):
                    e[k].pop('_luma', None)
    sr = [e.get('seamSyntheticOverReal') for _, e in allE]
    mr = [e.get('mismatchSyntheticOverReal') for _, e in allE]
    summ['seam'] = {'seamSyntheticOverRealMedian': med(sr), 'seamSyntheticOverRealMax': max([x for x in sr if x is not None], default=None),
                    'mismatchSyntheticOverRealMedian': med(mr), 'mismatchSyntheticOverRealMax': max([x for x in mr if x is not None], default=None),
                    'seamRatioRealMedian': med([e['seam'][w]['seamRatio'] for _, e in allE if e.get('seam') for w in ('prev', 'next') if e['seam'][w] and e['seam'][w].get('seamRatio')]),
                    'seamRatioSyntheticMedian': med([e['seam']['synthetic']['seamRatio'] for _, e in allE if e.get('seam') and e['seam']['synthetic'] and e['seam']['synthetic'].get('seamRatio')])}
    ver['seam.gradient'] = judge(summ['seam']['seamSyntheticOverRealMax'], (summ['seam']['seamSyntheticOverRealMax'] or 0) <= T['seam_synthetic_over_real'], why if not judged else 'no focus band (single layer: give --band-from)')
    ver['seam.layerMismatch'] = judge(summ['seam']['mismatchSyntheticOverRealMax'], (summ['seam']['mismatchSyntheticOverRealMax'] or 0) <= T['mismatch_synthetic_over_real'], why if not judged else 'single layer')
    al = [e['alpha'] for _, e in allE if e.get('alpha')]
    unc = [x for x in al if 'uncoveredBlackShare' in x]
    summ['alpha'] = {'triples': len(al), 'meanAbsMax': max([x['meanAbs'] for x in al], default=None), 'p99Max': max([x['p99'] for x in al], default=None), 'holeShareMax': max([x['holeShare'] for x in al], default=None),
                     'holePixelsTotal': sum(x['holePixels'] for x in al) if al else None, 'withUncoveredZone': len(unc), 'uncoveredBlackShareMax': max([x['uncoveredBlackShare'] for x in unc], default=None),
                     'uncoveredMismatchOverInteriorMax': max([x['uncoveredMismatch'] / max(T['uncovered_interior_floor'], x['interiorMismatch']) for x in unc if x.get('uncoveredMismatch') is not None and x.get('interiorMismatch')], default=None),
                     'flagsDiffer': sum(1 for x in al if x['flagsSynthetic'] != x['flagsNext'])}
    ver['alpha.equalsNext'] = judge(summ['alpha']['meanAbsMax'], (summ['alpha']['meanAbsMax'] or 0) <= T['alpha_mean_abs_max'] and not summ['alpha']['flagsDiffer'], why if not judged else 'single layer')
    ver['alpha.noHoles'] = judge(summ['alpha']['holeShareMax'], (summ['alpha']['holeShareMax'] or 0) <= T['alpha_hole_share_max'], why if not judged else 'single layer')
    ver['alpha.uncoveredNotBlack'] = judge(summ['alpha']['uncoveredBlackShareMax'], (summ['alpha']['uncoveredBlackShareMax'] or 0) <= T['uncovered_black_share_max'], why if not judged else 'the focus field of view never moved')
    ver['alpha.uncoveredAgreesWithPeriphery'] = judge(summ['alpha']['uncoveredMismatchOverInteriorMax'], (summ['alpha']['uncoveredMismatchOverInteriorMax'] or 0) <= T['uncovered_mismatch_over_interior'], why if not judged else 'the focus field of view never moved')
    summ['crops'] = {}
    for name, layer, box in crops:
        cs = [(t, e['crops'][name]) for t, e in allE if e['crops'].get(name)]
        sh = [c['sharpnessRatio'] for _, c in cs]
        fr = [abs(c['positionFraction'] - t['fraction']) for t, c in cs if 'positionFraction' in c]
        st = [c['positionErrorPx'] for _, c in cs if 'positionFraction' not in c and 'positionErrorPx' in c]
        summ['crops'][name] = {'layer': layer, 'box': box, 'triples': len(cs), 'sharpnessRatioMedian': med(sh), 'sharpnessRatioMin': min(sh, default=None), 'positionFractionErrMedian': med(fr),
                               'positionFractionMedian': med([c['positionFraction'] for _, c in cs if 'positionFraction' in c]), 'stillPositionErrorPxMax': max(st, default=None), 'blobAreaRatioMedian': med([c.get('blobAreaRatio') for _, c in cs])}
        ver['crop.%s.sharp' % name] = judge(summ['crops'][name]['sharpnessRatioMedian'], (med(sh) or 0) >= T['crop_sharpness_min'], why if not judged else 'crop outside the layer')
        if fr:
            ver['crop.%s.position' % name] = judge(med(fr), med(fr) <= T['crop_position_tolerance'])
        else:
            # the median over the triples: the mark is the piece of symbology that holds the crop's brightest pixel, and on a
            # whole HUD that piece changes now and then (hud flight 18:45: 3 of 17 triples jumped 38-376 px while the strokes
            # themselves sat within 0.03 px); stillPositionErrorPxMax stays in the summary
            ver['crop.%s.position' % name] = judge(med(st), (med(st) if st else 9) <= 2.0, why if not judged else 'no bright mark in the crop')
    for name, hud in res['hudLocator'].items():
        if not hud.get('found'):
            ver['crop.%s.sharp' % name] = {'verdict': 'NOT JUDGED', 'why': 'HUD not found in the first real submission (%s)' % hud.get('why', '')}
    ver['duplicates.none'] = {'verdict': 'PASS' if not dup else 'FAIL', 'value': len(dup)}
    res['summary'], res['verdicts'] = summ, ver
    out = a.out or a.capture
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, 'verdict.json'), 'w', encoding='utf-8') as f:
        json.dump(res, f, indent=1, default=float)
    lines = ['capture %s' % res['capture'], 'submissions %d, cadence %s' % (len(subs), res['cadence']),
             'real/synthetic from: %s; ring order agrees with the flight log: %s' % ('; '.join(res['classification']['source']), res['classification']['ringOrderAgreesWithLog']),
             'triples %d; duplicates %d (repeats %d, same pixels %d); display step ms %s' % (len(out_tr), len(dup), res['duplicates']['repeats'], res['duplicates']['samePixels'], res['duplicates']['displayStepMs']), '',
             '%-38s %-11s %-14s %s' % ('check', 'verdict', 'value', 'threshold (guess)')]
    thr = {'interpPeriphery.ratio': 'abs(ratio - fraction) <= %.2f' % T['interp_ratio_tolerance'], 'interpFocus.ratio': 'abs(ratio - fraction) <= %.2f' % T['interp_ratio_tolerance'],
           'interpPeriphery.notACopy': 'no synthetic >= %.2f byte-equal to a neighbour' % T['equals_neighbour_share'], 'interpFocus.notACopy': 'same',
           'seam.gradient': 'synthetic / real <= %.2f' % T['seam_synthetic_over_real'], 'seam.layerMismatch': 'synthetic / real <= %.2f' % T['mismatch_synthetic_over_real'],
           'alpha.equalsNext': 'mean abs <= %.1f of 255, same flags' % T['alpha_mean_abs_max'], 'alpha.noHoles': 'share <= %g' % T['alpha_hole_share_max'],
           'alpha.uncoveredNotBlack': 'black share <= %.2f' % T['uncovered_black_share_max'], 'alpha.uncoveredAgreesWithPeriphery': 'uncovered / max(interior, %.0f) <= %.1f' % (T['uncovered_interior_floor'], T['uncovered_mismatch_over_interior']), 'duplicates.none': '0'}
    for k, v in ver.items():
        val = v.get('value')
        t = thr.get(k, 'ratio >= %.2f' % T['crop_sharpness_min'] if k.endswith('.sharp') else 'fraction error <= %.2f (or <= 2 px when still)' % T['crop_position_tolerance'])
        lines.append('%-38s %-11s %-14s %s' % (k, v['verdict'], '-' if val is None else ('%.4g' % val), t if v['verdict'] != 'NOT JUDGED' else v.get('why', '')))
    s = summ
    for ln in ('interpPeriphery', 'interpFocus'):
        g = s[ln]
        fmt = lambda x: '-' if not x else '%+.3f (p10 %+.2f p90 %+.2f, %d regions)' % (x['deviationMedian'], x['p10'], x['p90'], x['regions'])
        lines += ['', '%s: ratio minus cadence fraction, pooled over regions: %s' % (ln, fmt(g['all'])),
                  '   field of view still %s | moved %s' % (fmt(g['fieldOfViewStill']), fmt(g['fieldOfViewMoved'])),
                  '   motion < 2 px %s | 2-5 px %s | > 5 px %s' % (fmt(g['motionBelow2px']), fmt(g['motion2to5px']), fmt(g['motionAbove5px'])),
                  '   estimator on the same content shifted rigidly (should read 0): %s' % fmt(g['estimatorControl'])]
    lines += ['', 'timing: shown at (display fraction - cadence fraction) median %s, %s of %s triples shown unevenly; application frame steps ms %s' % (res['timing']['displayFractionMinusCadenceMedian'], res['timing']['triplesShownUnevenly'], res['timing']['triples'], res.get('appFrameStepMs')),
              'like with like: %s' % res['likeWithLike']]
    lines += ['', 'interpolation: periphery ratio median %s (regions in %s triples), focus ratio median %s (%s triples)' % (s['interpPeriphery']['ratioMedian'], s['interpPeriphery']['withMotionRegions'], s['interpFocus']['ratioMedian'], s['interpFocus']['withMotionRegions']),
              'seam ratio (band gradient / neighbours): real %s, synthetic %s' % (s['seam']['seamRatioRealMedian'], s['seam']['seamRatioSyntheticMedian'])]
    text = '\n'.join(lines) + '\n'
    with open(os.path.join(out, 'verdict.txt'), 'w', encoding='utf-8') as f:
        f.write(text)
    if not a.quiet:
        print(text)
    return res


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('capture')
    ap.add_argument('--flight-log')
    ap.add_argument('--eyes', default='0')
    ap.add_argument('--out')
    ap.add_argument('--crop', action='append')
    ap.add_argument('--band-from')
    ap.add_argument('--max-triples', type=int, default=0)
    ap.add_argument('--grid', type=int, default=8)
    ap.add_argument('--like', help='the option-on capture of the same scenario: a single-layer (twin) capture is resampled onto its periphery and focus grids, and the seam band is taken from it')
    ap.add_argument('--seam-density', type=float, default=0.5)
    ap.add_argument('--quiet', action='store_true')
    return run(ap.parse_args(argv))


if __name__ == '__main__':
    main()
    sys.exit(0)
