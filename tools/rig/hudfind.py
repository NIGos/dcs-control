"""Is the HUD in view? Looks for saturated green thin strokes (riglib.find_hud).
   py hudfind.py --png <screenshot.png> [--out marked.png]
   py hudfind.py <capture dir> [--layer focus|periphery] [--eye 0] [--out marked.png]
Prints one JSON line (found, pixels, box in pixels, and for a capture the box in tangents l,r,u,d).
Exit 0 = found, 3 = not found, 2 = no image. Thresholds are guesses (see find_hud)."""
import argparse
import json
import sys

import numpy as np

import riglib


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('capture', nargs='?')
    ap.add_argument('--png')
    ap.add_argument('--layer', default='focus')
    ap.add_argument('--eye', type=int, default=0)
    ap.add_argument('--out')
    ap.add_argument('--fly', action='store_true', help='with --png: look for the briefing FLY button instead (exit 0 = seen)')
    a = ap.parse_args()
    if a.fly and a.png:
        from PIL import Image
        c = riglib.find_fly_button(np.asarray(Image.open(a.png).convert('RGB')))
        print(json.dumps({'flyButton': c}))
        return 0 if c else 3
    fov = None
    if a.png:
        from PIL import Image
        img = np.asarray(Image.open(a.png).convert('RGB'))
    elif a.capture:
        cap = riglib.load_capture(a.capture)
        img = None
        for sub in cap['subs']:
            per, foc = riglib.pick_layers(sub)
            layer = foc if (a.layer == 'focus' and foc is not None) else per
            if layer is None or a.eye >= len(layer['views']):
                continue
            img = riglib.view_pixels(cap, layer['views'][a.eye])
            if img is not None:
                fov = riglib.view_fov_of_pixels(layer['views'][a.eye])
                break
    else:
        ap.error('give --png or a capture folder')
    if img is None:
        print(json.dumps({'found': False, 'why': 'no image'}))
        return 2
    r = riglib.find_hud(img)
    if r['found'] and fov is not None:
        x0, y0, x1, y1 = r['box']
        h, w = img.shape[:2]
        r['boxTangents'] = [fov[0] + x0 / w * (fov[1] - fov[0]), fov[0] + x1 / w * (fov[1] - fov[0]), fov[2] - y0 / h * (fov[2] - fov[3]), fov[2] - y1 / h * (fov[2] - fov[3])]
    r['image'] = [int(img.shape[1]), int(img.shape[0])]
    if a.out:
        m = img[..., :3].copy()
        if r['found']:
            x0, y0, x1, y1 = r['box']
            m[y0:y0 + 2, x0:x1] = m[max(0, y1 - 2):y1, x0:x1] = (255, 0, 255)
            m[y0:y1, x0:x0 + 2] = m[y0:y1, max(0, x1 - 2):x1] = (255, 0, 255)
        riglib.save_png(a.out, m)
    print(json.dumps(r))
    return 0 if r['found'] else 3


if __name__ == '__main__':
    sys.exit(main())
