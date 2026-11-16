"""Shared helpers of the rig's offline tools (compose.py, verdict.py, selftest.py, capture-layer/check_harness.py).

A capture is a folder written by the rig capture layer: capture.jsonl (a header line, one line per xrEndFrame, an end
line) and one raw file per projection view (w*h*4 bytes, rows top-down, RGBA, or BGRA when the view says "bgra").
Field-of-view angles are radians as submitted; every tool works in tangents (l, r, u, d) = tan(angle)."""
import json
import math
import os

import numpy as np

FLAG_BLEND_ALPHA = 2          # XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
FLAG_UNPREMULTIPLIED = 4      # XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT


def load_capture(folder):
    path = os.path.join(folder, 'capture.jsonl')
    header, subs, end = None, [], None
    with open(path, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            o = json.loads(line)
            if 'header' in o:
                header = o
            elif 'end' in o:
                end = o
            else:
                subs.append(o)
    return {'dir': folder, 'header': header or {}, 'subs': subs, 'end': end}


def projection_layers(sub):
    return [(i, l) for i, l in enumerate(sub.get('layers', [])) if l.get('type') == 'projection' and l.get('views')]


def tan_fov(view):
    a = view['fov']
    return (math.tan(a[0]), math.tan(a[1]), math.tan(a[2]), math.tan(a[3]))


def pick_layers(sub):
    """(periphery, focus): the projection layer with the widest view 0 and, when there are two or more, the narrowest
    of the others. focus is None for a single composed layer (the twin with the option off)."""
    pl = [l for _, l in projection_layers(sub)]
    if not pl:
        return None, None
    width = lambda l: tan_fov(l['views'][0])[1] - tan_fov(l['views'][0])[0]
    per = max(pl, key=width)
    rest = [l for l in pl if l is not per]
    foc = min(rest, key=width) if rest else None
    return per, foc


def stream_view(view, stream=2):
    """The view as its second stream (file2, w2, ...: the whole sub-image at a scale), or None when it has none."""
    sfx = str(stream)
    if not view.get('file' + sfx):
        return None
    v = dict(view)
    for k in ('file', 'w', 'h', 'cropOffset', 'scale', 'bgra', 'gpuScaled'):
        if k + sfx in view:
            v[k] = view[k + sfx]
    v.pop('_px', None)
    return v


def view_pixels(cap, view):
    """uint8 (h, w, 4) RGBA of a captured view, or None when the layer wrote no pixels for it."""
    if view.get('_px') is not None:          # a view made by a tool (resampled twin window): pixels in memory
        return view['_px']
    name = view.get('file')
    if not name:
        return None
    w, h = int(view['w']), int(view['h'])
    path = os.path.join(cap['dir'], name)
    if not os.path.exists(path) or os.path.getsize(path) != w * h * 4:
        return None
    a = np.fromfile(path, dtype=np.uint8).reshape(h, w, 4)
    if view.get('bgra'):
        a = a[:, :, [2, 1, 0, 3]]
    return a


def view_fov_of_pixels(view):
    """Tangent FOV of the stored pixels: the sub-image's FOV narrowed by the layer's crop (a scale keeps it)."""
    l, r, u, d = tan_fov(view)
    rw, rh = view['rect'][2], view['rect'][3]
    cx, cy = view.get('cropOffset', [0, 0])
    s = int(view.get('scale', 1))
    w, h = int(view.get('w', rw)) * s, int(view.get('h', rh)) * s
    px, py = (r - l) / rw, (u - d) / rh
    return (l + cx * px, l + (cx + w) * px, u - cy * py, u - (cy + h) * py)


def luma(a):
    a = a.astype(np.float32)
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def save_png(path, a):
    from PIL import Image
    a = np.asarray(a)
    if a.dtype != np.uint8:
        a = np.clip(a + 0.5, 0, 255).astype(np.uint8)
    Image.fromarray(a).save(path)


def sample_bilinear(img, x, y):
    """img (h, w[, c]) float; x, y pixel coordinates of the same shape (0 = centre of the first texel), clamped at the
    borders: the sampling of Compose() in tools/runtime-probe/probe_run.cpp."""
    h, w = img.shape[:2]
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    fx = (x - x0).astype(np.float32)
    fy = (y - y0).astype(np.float32)
    xa, xb = np.clip(x0, 0, w - 1), np.clip(x0 + 1, 0, w - 1)
    ya, yb = np.clip(y0, 0, h - 1), np.clip(y0 + 1, 0, h - 1)
    if img.ndim == 3:
        fx, fy = fx[..., None], fy[..., None]
    return (img[ya, xa] * (1 - fx) + img[ya, xb] * fx) * (1 - fy) + (img[yb, xa] * (1 - fx) + img[yb, xb] * fx) * fy


def tangent_grid(fov, w, h):
    """Tangents of the pixel centres of a w x h image over fov (TanX / TanY of the probe)."""
    l, r, u, d = fov
    tx = l + (np.arange(w, dtype=np.float64) + 0.5) / w * (r - l)
    ty = u - (np.arange(h, dtype=np.float64) + 0.5) / h * (u - d)
    return np.meshgrid(tx, ty)


def resample(img, fov, tx, ty):
    """img over fov sampled bilinearly at tangents (tx, ty); second value: mask of the points inside fov."""
    l, r, u, d = fov
    h, w = img.shape[:2]
    un = (tx - l) / (r - l)
    vn = (u - ty) / (u - d)
    out = sample_bilinear(img.astype(np.float32), un * w - 0.5, vn * h - 0.5)
    return out, (un > 0) & (un < 1) & (vn > 0) & (vn < 1)


def write_capture(folder, subs, swapchains, note=''):
    """Test helper: writes a capture folder in the layer's format. subs: list of dicts as the layer's lines, each view
    carrying 'pixels' (uint8 h, w, 4) instead of 'file'."""
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, 'capture.jsonl'), 'w', encoding='utf-8') as f:
        f.write(json.dumps({'header': 1, 'version': 1, 'api': 12, 'frames': len(subs), 'scale': 1, 'crop': [0, 0, 1, 1], 'qpcFreq': 10000000, 'swapchains': swapchains, 'note': note}) + '\n')
        for n, s in enumerate(subs):
            o = json.loads(json.dumps(s, default=lambda x: None))
            for li, layer in enumerate(s['layers']):
                for vi, v in enumerate(layer.get('views', [])):
                    px = v.get('pixels')
                    ov = o['layers'][li]['views'][vi]
                    ov.pop('pixels', None)
                    ov.pop('pixels2', None)
                    if px is not None:
                        name = 's%04d_L%d_v%d.rgba' % (n, li, vi)
                        np.ascontiguousarray(px, dtype=np.uint8).tofile(os.path.join(folder, name))
                        ov.update({'file': name, 'w': int(px.shape[1]), 'h': int(px.shape[0]), 'cropOffset': list(v.get('cropOffset', [0, 0])), 'scale': int(v.get('scale', 1)), 'bgra': False})
                    if v.get('pixels2') is not None:      # the second stream: the whole sub-image at scale2
                        name = 's%04d_L%d_v%db.rgba' % (n, li, vi)
                        np.ascontiguousarray(v['pixels2'], dtype=np.uint8).tofile(os.path.join(folder, name))
                        ov.update({'file2': name, 'w2': int(v['pixels2'].shape[1]), 'h2': int(v['pixels2'].shape[0]), 'cropOffset2': [0, 0], 'scale2': int(v['scale2']), 'bgra2': False})
            o['n'] = n
            f.write(json.dumps(o) + '\n')
        f.write(json.dumps({'end': 1, 'reason': 'complete', 'submissions': len(subs), 'asked': len(subs)}) + '\n')
    with open(os.path.join(folder, 'done.txt'), 'w') as f:
        f.write('complete\n')


def find_hud(img, min_pixels=None):
    """Looks for HUD symbology in an image (uint8 h, w, 3 or 4): saturated green, thin strokes.
    green = G above 140 and above 1.8 x R and 1.8 x B; thin = what a k x k opening removes (k grows with the image); specks are dropped.
    Returns {'found', 'pixels', 'box' (x0, y0, x1, y1 with a margin), 'why'}. Thresholds are guesses: tested on a
    drawn HUD and, as negatives, on the flown captures without one; never on a captured real HUD."""
    from scipy import ndimage
    r, g, b = (img[..., i].astype(np.int32) for i in range(3))
    h, w = g.shape
    # Symbology green as flown (F-16 HUD in DCS's window, 2026-11-15 18:30: median 159, 249, 163; p10 120, 217, 127):
    # bright, and 60 above red and blue. Grass and the aircraft's paint stay below that margin.
    green = (g > 130) & (g > r + 60) & (g > b + 60)
    k = max(5, int(w / 270) | 1)
    thin = green & ~ndimage.binary_opening(green, np.ones((k, k), bool))
    lab, n = ndimage.label(ndimage.binary_dilation(thin, np.ones((3, 3), bool)))
    if n:
        sizes = ndimage.sum(thin, lab, index=np.arange(1, n + 1))
        thin &= np.isin(lab, 1 + np.flatnonzero(sizes >= max(8, k * 2)))
    px = int(thin.sum())
    need = min_pixels if min_pixels is not None else max(120, int(0.00006 * w * h))
    if px < need:
        return {'found': False, 'pixels': px, 'needed': need, 'why': '%d thin green pixels, %d needed' % (px, need)}
    # the largest group of strokes (the HUD), not the green pages of the displays below it
    glab, gn = ndimage.label(ndimage.binary_dilation(thin, np.ones((4 * k + 1, 4 * k + 1), bool)))
    if gn > 1:
        gs = ndimage.sum(thin, glab, index=np.arange(1, gn + 1))
        thin = thin & (glab == 1 + int(np.argmax(gs)))
    ys, xs = np.nonzero(thin)
    x0, x1, y0, y1 = np.percentile(xs, 1), np.percentile(xs, 99), np.percentile(ys, 1), np.percentile(ys, 99)
    mx, my = 0.08 * (x1 - x0) + 8, 0.08 * (y1 - y0) + 8
    box = [int(max(0, x0 - mx)), int(max(0, y0 - my)), int(min(w, x1 + mx)), int(min(h, y1 + my))]
    return {'found': True, 'pixels': px, 'needed': need, 'box': box, 'share': px / float(w * h)}


def find_fly_button(img):
    """The briefing's green FLY button in a picture of DCS's window: a filled green rectangle about 62 x 18 px at
    1076 x 1060 (flown: median colour 115, 163, 81). Returns [(cx, cy, w, h)] of the candidates, largest first."""
    from scipy import ndimage
    r, g, b = (img[..., i].astype(np.int32) for i in range(3))
    k = img.shape[1] / 1076.0
    m = ndimage.binary_closing((g > 135) & (g > r + 25) & (g > b + 45) & (r > 70), np.ones((3, 3), bool))
    lab, n = ndimage.label(m)
    out = []
    for i, sl in enumerate(ndimage.find_objects(lab), 1):
        h, w = sl[0].stop - sl[0].start, sl[1].stop - sl[1].start
        area = int((lab[sl] == i).sum())
        if 35 * k <= w <= 110 * k and 9 * k <= h <= 30 * k and area >= 0.55 * w * h:
            out.append((area, (int((sl[1].start + sl[1].stop) / 2), int((sl[0].start + sl[0].stop) / 2), int(w), int(h))))
    return [c for _, c in sorted(out, reverse=True)]

CURSOR_DOT_TAN = 0.00866      # DCS's cockpit cursor (the white dot, image 149): 22 px across in the 1952-px focus image


def find_cursor_dot(img, pixels_per_tangent, near=None):
    """DCS's white cockpit cursor dot in an image (uint8 h, w, 3 or 4): a near-white, near-round blob of the expected
    size (CURSOR_DOT_TAN x pixels per tangent across; 3-4 px in the periphery layer, where it is a guess at best).
    Returns the candidates [(cx, cy, w, h, area, kind)] (kind 'dot' or 'cross'), crosses and then the roundest, best-sized dots first (with `near` = (x, y): the
    closest first)."""
    from scipy import ndimage
    r, g, b = (img[..., i].astype(np.int32) for i in range(3))
    mn, mx = np.minimum(np.minimum(r, g), b), np.maximum(np.maximum(r, g), b)
    d = CURSOR_DOT_TAN * pixels_per_tangent
    lab, n = ndimage.label((mn > (200 if d >= 8 else 185)) & (mx - mn < 45))
    out = []
    # the other shape of the same cursor: a yellow cross (over the HUD, the sky and whatever is not a cockpit surface;
    # flown 2026-11-15 19:16: about 26 px across in the focus image), kind 'cross'
    ylab, yn = ndimage.label(ndimage.binary_dilation((r > 190) & (g > 160) & (b < 130) & (r - b > 90), np.ones((3, 3), bool)))
    for i, sl in enumerate(ndimage.find_objects(ylab), 1):
        h, w = sl[0].stop - sl[0].start, sl[1].stop - sl[1].start
        if not (0.7 * d <= w <= 2.4 * d + 2 and 0.7 * d <= h <= 2.4 * d + 2) or abs(w - h) > 0.4 * max(w, h) + 2:
            continue
        area = int((ylab[sl] == i).sum())
        if not (0.15 <= area / float(w * h) <= 0.75):
            continue
        cx, cy = (sl[1].start + sl[1].stop) / 2.0, (sl[0].start + sl[0].stop) / 2.0
        score = 0.0 if near is None else ((cx - near[0]) ** 2 + (cy - near[1]) ** 2) ** 0.5
        out.append((score, (cx, cy, int(w), int(h), area, 'cross')))
    for i, sl in enumerate(ndimage.find_objects(lab), 1):
        h, w = sl[0].stop - sl[0].start, sl[1].stop - sl[1].start
        if not (0.5 * d - 1 <= w <= 1.7 * d + 1 and 0.5 * d - 1 <= h <= 1.7 * d + 1) or abs(w - h) > 0.35 * max(w, h) + 1:
            continue
        area = int((lab[sl] == i).sum())
        fill = area / float(w * h)
        if not (0.5 <= fill <= 0.97):
            continue
        cx, cy = (sl[1].start + sl[1].stop) / 2.0, (sl[0].start + sl[0].stop) / 2.0
        score = abs(fill - 0.785) + abs(w - d) / d + abs(h - d) / d
        if near is not None:
            score = ((cx - near[0]) ** 2 + (cy - near[1]) ** 2) ** 0.5
        out.append((score + (0.0 if near is not None else 1.0), (cx, cy, int(w), int(h), area, 'dot')))
    return [c for _, c in sorted(out, key=lambda x: x[0])]
