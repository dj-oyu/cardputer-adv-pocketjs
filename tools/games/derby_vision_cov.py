"""Where the turf vision is on screen, per camera, as a function of the leader's x.

Constants and the visibility test are copied from apps/derby (derby_view.js CAMS/VS,
derby_scene.js paint()): a face is drawn when its width is 8..160 px, it is not left of
x=0 / right of x=239, and its top is at y>=13. Lettering needs a width > 99.
Steady-state camera: cx = leader - offset (WIDE 880/f, FIELD 880/f, VISION clamp).
"""
import sys

D = 1000
CAMS = {'WIDE': (100, 9.7, 33), 'FIELD': (58, 15, 36), 'VISION': (130, 1.6, 84), 'FINISH': (170, 7, 22)}


def visible(cam, cx, sx, depth=34, hw=20, zb=6, zt=16):
    f, h, hy = CAMS[cam]
    p = f / depth
    x0 = 120 + (sx - hw - cx) * p
    x1 = 120 + (sx + hw - cx) * p
    y0 = hy + (h - zt) * p
    w = x1 - x0
    ok = not (x0 > 239 or x1 < 1 or y0 < 13 or w < 8 or w > 160)
    return ok, ok and w > 99, w


def cam_cx(cam, L):
    f = CAMS[cam][0]
    if cam == 'VISION':
        return max(810, min(870, L - 880 / f))
    if cam == 'FINISH':
        return min(L - 640 / f, D - 6)
    return L - 880 / f


def director(L):
    # derby_play.js: L<150 WIDE, <400 FIELD, <700 WIDE (CLOSE after a lead change),
    # <780 CLOSE, <900 VISION, else WIDE (HEAD ON in a close finish). CLOSE/HEAD ON never show a face.
    if L < 150: return 'WIDE'
    if L < 400: return 'FIELD'
    if L < 700: return 'WIDE'
    if L < 780: return 'CLOSE'
    if L < 900: return 'VISION'
    return 'WIDE'


def coverage(screens):
    res = {}
    for L in range(0, D + 1):
        cam = director(L)
        if cam == 'CLOSE':
            res[L] = (cam, 0, 0); continue
        n = 0
        lettered = 0
        for sx in screens:
            ok, let, w = visible(cam, cam_cx(cam, L), sx)
            n += ok
            lettered += let
        res[L] = (cam, n, lettered)
    return res


def report(name, screens):
    r = coverage(screens)
    per = {}
    for L, (cam, n, let) in r.items():
        s = per.setdefault(cam, [0, 0, 0, 0])
        s[0] += 1; s[1] += n > 0; s[2] += let > 0; s[3] = max(s[3], n)
    tot = sum(v[0] for v in per.values())
    vis = sum(v[1] for v in per.values())
    print(f'{name}: screens={screens}  visible {100*vis/tot:.0f}% of the course  ', end='')
    print('  '.join(f'{c}:{100*v[1]/v[0]:.0f}%(max {v[3]} on screen)' for c, v in sorted(per.items())))


# WIDE window: leader positions where a screen at x is seen by WIDE
f, h, hy = CAMS['WIDE']
p = f / 34
lo = 820 - 119 / p; hi = 860 + 119 / p
print(f'WIDE p={p:.2f} face {40*p:.0f}px; seen while cx in [{lo-840+840:.0f}-{hi-840+840:.0f}] -> leader window {hi-lo:.0f} m wide (cx offset 8.8)')
for name, s in [('now', [840]),
                ('2', [300, 840]),
                ('3', [180, 520, 840]),
                ('4', [150, 400, 650, 900]),
                ('8 @120m', [60 + 120 * i for i in range(8)])]:
    report(name, s)
