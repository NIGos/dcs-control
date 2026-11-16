"""Byte check of the rig capture layer's output against rigtap_harness.exe's pattern.
   py check_harness.py <harness out dir>      exit 0 = every captured byte and field is as the harness submitted.
Five captures: full; scale 2 + crop; the hard cap; a tangent window per eye + a second stream at scale 3; a budget
that forces a scale. Scaled files are checked against the exact box average (GPU path and writer-thread path)."""
import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import riglib  # noqa: E402

WINDOWS = [(-0.3, 0.3, 0.2, -0.2), (-0.21, 0.35, 0.22, -0.2)]


def pattern(layer, eye, frame, x0, y0, w, h):
    y, x = np.mgrid[y0:y0 + h, x0:x0 + w].astype(np.int64)
    o = np.empty((h, w, 4), np.uint8)
    o[..., 0] = (x * 3 + frame * 5 + eye * 40) & 255
    o[..., 1] = (y * 7 + layer * 90 + frame) & 255
    o[..., 2] = ((x ^ y) + frame * 11) & 255
    o[..., 3] = ((x + 2 * y + frame * 3) & 255) if layer == 1 else 255
    return o


def main():
    out = sys.argv[1]
    exp = json.load(open(os.path.join(out, 'expected.json')))
    by_time = {f['displayTime']: f for f in exp['frames']}
    fails, checked_views, checked_bytes, repeats_seen, gpu_files, cpu_scaled = [], 0, 0, 0, 0, 0
    want = {'cap00_full': 12, 'cap01_small': 6, 'cap02_cap': 120, 'cap03_win': 6, 'cap04_budget': 20}
    for name, count in want.items():
        cap = riglib.load_capture(os.path.join(out, name))
        subs = cap['subs']
        if len(subs) != count or not cap['end'] or cap['end']['reason'] != 'complete':
            fails.append('%s: %d submissions (want %d), end %s' % (name, len(subs), count, cap['end']))
            continue
        if len(cap['header'].get('swapchains', [])) != 8:
            fails.append('%s: header lists %d swapchains (want 8)' % (name, len(cap['header'].get('swapchains', []))))
        cu = sorted(s['copyUs'] for s in subs)
        per = sum(v.get('w' + k, 0) * v.get('h' + k, 0) * 4 for l in subs[0]['layers'][:2] for v in l['views'] for k in ('', '2'))
        print('%s: %d submissions, inside xrEndFrame: median %d us, max %d us; bytes per submission on disk %d; scales %s' % (
            name, len(subs), cu[len(cu) // 2], cu[-1], per, sorted({(v.get('scale'), v.get('scale2')) for l in subs[0]['layers'][:2] for v in l['views']})))
        if name == 'cap04_budget':
            budget = (64 if exp.get('live') else 16) << 20
            if per * count > budget or 'autoScale' not in subs[0] or subs[0]['layers'][0]['views'][0]['scale'] < 2:
                fails.append('%s: budget not kept (%d bytes x %d, autoScale %s)' % (name, per, count, subs[0].get('autoScale')))
        times = [s['displayTime'] for s in subs]
        if any(b - a != 11111111 for a, b in zip(times, times[1:])):
            fails.append('%s: submissions are not consecutive' % name)
        prev_key = None
        for s in subs:
            e = by_time.get(s['displayTime'])
            if e is None:
                fails.append('%s: unknown display time %d' % (name, s['displayTime']))
                continue
            if s['layerCount'] != 3 or [l['type'] for l in s['layers']] != ['projection', 'projection', 'quad']:
                fails.append('%s n%d: layers %s' % (name, s['n'], [l['type'] for l in s['layers']]))
                continue
            if s['layers'][0]['flags'] != 0 or s['layers'][1]['flags'] != 6 or s['layers'][2]['scId'] != -1:
                fails.append('%s n%d: flags or the unknown swapchain wrong' % (name, s['n']))
            key = tuple((v['scId'], v['releaseSeq'], v['imageIndex']) for l in s['layers'][:2] for v in l['views'])
            repeats_seen += int(prev_key is not None and key == prev_key)
            if prev_key is not None and (key == prev_key) != e['repeat']:      # the first submission of a capture has nothing before it
                fails.append('%s n%d: repeat seen %s, submitted %s' % (name, s['n'], key == prev_key, e['repeat']))
            prev_key = key
            for li in (0, 1):
                rx, ry, rw, rh = exp['rects'][li]
                for ei, v0 in enumerate(s['layers'][li]['views']):
                    # ring 1, 2 = current, 3 = synthetic; ids: layer 0 -> 0..3, layer 1 -> 4..7
                    if v0['scId'] != li * 4 + e['ring'] or v0['array'] != ei or v0['rect'] != [rx, ry, rw, rh] or v0['format'] != (29 if li == 0 else 28):
                        fails.append('%s n%d L%d v%d: swapchain/rect/array/format fields wrong' % (name, s['n'], li, ei))
                    streams = [v0] + ([riglib.stream_view(v0)] if v0.get('file2') else [])
                    if name == 'cap03_win' and len(streams) != 2:
                        fails.append('%s n%d L%d v%d: no second stream' % (name, s['n'], li, ei))
                    for si, v in enumerate(streams):
                        px = riglib.view_pixels(cap, v)
                        if px is None:
                            fails.append('%s n%d L%d v%d stream %d: no pixels (%s)' % (name, s['n'], li, ei, si, v.get('noPixels')))
                            continue
                        scale = v['scale']
                        cx, cy = v['cropOffset']
                        if name == 'cap01_small' and (cx != int(0.25 * rw) or v['w'] * scale != (int(0.75 * rw + 0.5) - int(0.25 * rw)) // scale * scale):
                            fails.append('%s n%d L%d v%d: crop geometry %s %dx%d' % (name, s['n'], li, ei, v['cropOffset'], v['w'], v['h']))
                        if name == 'cap03_win' and si == 0:      # the window in tangents, clamped to the view
                            l, r, u, d = (math.tan(a) for a in v['fov'])
                            wl, wr, wu, wd = WINDOWS[ei]
                            f0, f1 = min(1, max(0, (wl - l) / (r - l))), min(1, max(0, (wr - l) / (r - l)))
                            g0, g1 = min(1, max(0, (u - wu) / (u - d))), min(1, max(0, (u - wd) / (u - d)))
                            ex0, ey0 = int(f0 * rw), int(g0 * rh)
                            if abs(cx - ex0) > 1 or abs(cy - ey0) > 1 or abs(v['w'] - (int(f1 * rw + 0.5) - ex0)) > 1 or abs(v['h'] - (int(g1 * rh + 0.5) - ey0)) > 1:
                                fails.append('%s n%d L%d v%d: window geometry offset %s size %dx%d, expected %d,%d %dx%d' % (name, s['n'], li, ei, v['cropOffset'], v['w'], v['h'], ex0, ey0, int(f1 * rw + 0.5) - ex0, int(g1 * rh + 0.5) - ey0))
                        if name == 'cap03_win' and si == 1 and (scale != 3 or [cx, cy] != [0, 0] or v['w'] != rw // 3):
                            fails.append('%s n%d L%d v%d: second stream %s' % (name, s['n'], li, ei, {k: v[k] for k in ('scale', 'cropOffset', 'w', 'h')}))
                        full = pattern(li, ei, e['shownFrame'], rx + cx, ry + cy, v['w'] * scale, v['h'] * scale).astype(np.uint32)
                        if scale > 1:
                            full = (full.reshape(v['h'], scale, v['w'], scale, 4).sum(axis=(1, 3)) + scale * scale // 2) // (scale * scale)
                            gpu_files += int(bool(v.get('gpuScaled')))
                            cpu_scaled += int(not v.get('gpuScaled'))
                        if not np.array_equal(px, full.astype(np.uint8)):
                            bad = int((px != full.astype(np.uint8)).any(axis=2).sum())
                            fails.append('%s n%d L%d v%d stream %d (scale %d, gpu %s): %d pixels differ, max %d' % (name, s['n'], li, ei, si, scale, v.get('gpuScaled'), bad, int(np.abs(px.astype(int) - full.astype(int)).max())))
                        checked_views += 1
                        checked_bytes += px.size
            pose_y = s['layers'][0]['views'][0]['pose'][1]
            if abs(pose_y - 0.001 * e['shownFrame']) > 1e-6:
                fails.append('%s n%d: pose not the submitted one' % (name, s['n']))
    if repeats_seen < 3:
        fails.append('only %d repeats seen' % repeats_seen)
    if gpu_files == 0:
        fails.append('no file was scaled on the GPU')
    print('checked %d files, %d bytes (colour and alpha), %d scaled on the GPU and %d on the writer thread, %d repeated submissions recognised, 5 captures: %s' % (
        checked_views, checked_bytes, gpu_files, cpu_scaled, repeats_seen, 'all equal' if not fails else '%d problems' % len(fails)))
    for f in fails[:20]:
        print('  FAIL', f)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
