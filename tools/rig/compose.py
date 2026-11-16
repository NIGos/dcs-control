"""Offline composer: what the Pimax compositor shows for one captured submission.

Port of Compose() in tools/runtime-probe/probe_run.cpp (333-357) with the blend rule the runtime probe measured on
Pimax (measured 2026-11-15): the periphery layer enlarged bilinearly over its field of view,
the focus layer placed by its own field of view inside it, and
    out = focus * a + periphery * (1 - a)          on the ENCODED bytes, a = the focus layer's alpha (unpremultiplied)
(--blend linear gives the probe's original: the same blend in linear light. A focus layer without the
BLEND_TEXTURE_SOURCE_ALPHA flag is opaque; without UNPREMULTIPLIED_ALPHA it is added as premultiplied.)
A submission with a single projection layer (the twin with the option off) is resampled onto the same grid.

  py compose.py <capture dir> [--n 0] [--eyes 0,1] [--out DIR] [--roi focus|full|l,r,u,d] [--margin 0.15]
                [--ppt PX_PER_TANGENT | --density 1.0] [--blend encoded|linear] [--layers]
Output: <out>/s<n>_eye<e>_composed.png (+ _periphery.png, _focus.png, _alpha.png with --layers) and a JSON line per
image on stdout. --roi focus (default) = the focus field of view widened by --margin on each side, at the focus
layer's own pixel density (--density scales it); --roi full = the whole periphery field of view."""
import argparse
import json
import os
import sys

import numpy as np

import riglib

_DEC = None


def _srgb_tables():
    global _DEC
    if _DEC is None:
        v = np.arange(256) / 255.0
        _DEC = np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4).astype(np.float32)
    return _DEC


def _encode(lin):
    lin = np.clip(lin, 0, 1)
    return np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.power(lin, 1 / 2.4) - 0.055) * 255.0


def compose_eye(per, fper, foc, ffoc, flags, out_fov, out_w, out_h, blend='encoded'):
    """per / foc: uint8 (h, w, 4) images over the tangent FOVs fper / ffoc (foc may be None).
    Returns dict: rgb (float32 h, w, 3, encoded 0..255), alpha (float32, the focus alpha on the output grid, 0 outside
    its rectangle), inside (bool: output pixels inside the focus rectangle), per / foc (the two layers on the grid)."""
    tx, ty = riglib.tangent_grid(out_fov, out_w, out_h)
    p, _ = riglib.resample(per[..., :3], fper, tx, ty)
    res = {'per': p, 'foc': None, 'alpha': np.zeros((out_h, out_w), np.float32), 'inside': np.zeros((out_h, out_w), bool)}
    if foc is None:
        res['rgb'] = p
        return res
    l, r, u, d = ffoc
    nx = (tx - (l + r) / 2) / ((r - l) / 2)
    ny = (ty - (u + d) / 2) / ((u - d) / 2)
    inside = (np.abs(nx) < 1) & (np.abs(ny) < 1)
    fh, fw = foc.shape[:2]
    f4 = riglib.sample_bilinear(foc.astype(np.float32), (nx + 1) / 2 * fw - 0.5, (1 - ny) / 2 * fh - 0.5)
    a = f4[..., 3] / 255.0 if (flags & riglib.FLAG_BLEND_ALPHA) else np.ones((out_h, out_w), np.float32)
    a = np.where(inside, a, 0).astype(np.float32)
    f = f4[..., :3]
    premult = bool(flags & riglib.FLAG_BLEND_ALPHA) and not (flags & riglib.FLAG_UNPREMULTIPLIED)
    if blend == 'linear':
        dec = _srgb_tables()
        # the probe decodes the texels before the bilinear filter; so does this
        pl, _ = riglib.resample(dec[per[..., :3]], fper, tx, ty)
        fl = riglib.sample_bilinear(dec[foc[..., :3]], (nx + 1) / 2 * fw - 0.5, (1 - ny) / 2 * fh - 0.5)
        mix = (fl if premult else fl * a[..., None]) + pl * (1 - a[..., None])
        rgb = np.where(inside[..., None], _encode(mix), _encode(pl))
    else:
        mix = (f if premult else f * a[..., None]) + p * (1 - a[..., None])
        rgb = np.where(inside[..., None], mix, p)
    res.update({'rgb': np.clip(rgb, 0, 255).astype(np.float32), 'foc': f, 'alpha': a, 'inside': inside})
    return res


def roi_for(per_view, foc_view, roi, margin, ppt, density):
    """(out_fov, w, h) for one eye."""
    fper = riglib.view_fov_of_pixels(per_view)
    ref = foc_view if foc_view is not None else per_view
    fref = riglib.view_fov_of_pixels(ref)
    if ppt is None:
        ppt = ref['w'] / (fref[1] - fref[0]) * density          # the stored pixels' own density
    if roi == 'full' or (roi == 'focus' and foc_view is None):
        fov = fper
    elif roi == 'focus':
        mx, my = (fref[1] - fref[0]) * margin, (fref[2] - fref[3]) * margin
        fov = (max(fper[0], fref[0] - mx), min(fper[1], fref[1] + mx), min(fper[2], fref[2] + my), max(fper[3], fref[3] - my))
    else:
        fov = tuple(float(x) for x in roi.split(','))
    w, h = max(1, int(round((fov[1] - fov[0]) * ppt))), max(1, int(round((fov[2] - fov[3]) * ppt)))
    return fov, w, h


def compose_submission(cap, sub, eye, roi='focus', margin=0.15, ppt=None, density=1.0, blend='encoded', out_grid=None):
    """Composes one eye of one submission. out_grid = (fov, w, h) overrides the ROI (to put several submissions on one
    grid). Returns (result dict of compose_eye + fov, w, h, flags, twoLayers) or None when pixels are missing."""
    per_l, foc_l = riglib.pick_layers(sub)
    if per_l is None or eye >= len(per_l['views']):
        return None
    pv = per_l['views'][eye]
    fv = foc_l['views'][eye] if foc_l is not None and eye < len(foc_l['views']) else None
    per = riglib.view_pixels(cap, pv)
    foc = riglib.view_pixels(cap, fv) if fv is not None else None
    if per is None or (fv is not None and foc is None):
        return None
    fov, w, h = out_grid if out_grid else roi_for(pv, fv, roi, margin, ppt, density)
    flags = int(foc_l['flags']) if foc_l is not None else 0
    res = compose_eye(per, riglib.view_fov_of_pixels(pv), foc, riglib.view_fov_of_pixels(fv) if fv is not None else None, flags, fov, w, h, blend)
    res.update({'fov': fov, 'w': w, 'h': h, 'flags': flags, 'twoLayers': fv is not None})
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('capture')
    ap.add_argument('--n', type=int, default=0)
    ap.add_argument('--eyes', default='0,1')
    ap.add_argument('--out')
    ap.add_argument('--roi', default='focus')
    ap.add_argument('--margin', type=float, default=0.15)
    ap.add_argument('--ppt', type=float)
    ap.add_argument('--density', type=float, default=1.0)
    ap.add_argument('--blend', choices=['encoded', 'linear'], default='encoded')
    ap.add_argument('--layers', action='store_true')
    a = ap.parse_args()
    cap = riglib.load_capture(a.capture)
    subs = [s for s in cap['subs'] if s['n'] == a.n]
    if not subs:
        print('no submission n=%d in %s (%d submissions)' % (a.n, a.capture, len(cap['subs'])), file=sys.stderr)
        return 2
    out = a.out or os.path.join(a.capture, 'composed')
    os.makedirs(out, exist_ok=True)
    rc = 0
    for eye in [int(x) for x in a.eyes.split(',')]:
        r = compose_submission(cap, subs[0], eye, a.roi, a.margin, a.ppt, a.density, a.blend)
        if r is None:
            print(json.dumps({'n': a.n, 'eye': eye, 'error': 'no pixels for this eye'}))
            rc = 1
            continue
        base = os.path.join(out, 's%04d_eye%d' % (a.n, eye))
        riglib.save_png(base + '_composed.png', r['rgb'])
        if a.layers:
            riglib.save_png(base + '_periphery.png', r['per'])
            if r['foc'] is not None:
                riglib.save_png(base + '_focus.png', r['foc'])
                riglib.save_png(base + '_alpha.png', r['alpha'] * 255)
        print(json.dumps({'n': a.n, 'eye': eye, 'file': base + '_composed.png', 'w': r['w'], 'h': r['h'], 'fov': [round(x, 6) for x in r['fov']], 'twoLayers': r['twoLayers'], 'focusFlags': r['flags'], 'blend': a.blend}))
    return rc


if __name__ == '__main__':
    sys.exit(main())
