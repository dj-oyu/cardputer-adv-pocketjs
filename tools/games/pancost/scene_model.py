"""Frame-cost model for a panning DERBY WATCH camera
(docs/apps/derby-pan-camera-cost.md). Counts what one frame projects and
draws for a fixed camera turned toward the leader, and prices it with unit
costs measured on the device. This is a calculation, not a device run.

Geometry is DERBY WATCH's (apps/derby/derby_watch.js, derby_view.js): the
course runs along x, depth z from today's camera line; near rail 11 m, far
rail 22.6 m, stands and crowd 40 m, the leader's lane about 15.3 m.

  python tools/games/pancost/scene_model.py            # sweep over the race
  python tools/games/pancost/scene_model.py --newton   # VM Newton accuracy
"""
from __future__ import annotations

import argparse
import math

# ---- Unit costs, us of JS turn. M = measured on the device by this study
# (pancost_device.py, 2 runs agreeing within 0.5 %); B = measured in
# derby-background-cost.md; E = estimate built from those.
PJ = 41.8          # M projp: one point in JS (2 reads, rotate, clip, divide, 2 stores)
LIT = 22.7         # M lit8: one 8-element input array literal
DRAW = 37.3        # M nopd: one H.draw of a 1-instruction plan with 8 inputs
VM_STEP = 0.46     # M newton: 19 steps per post at 8.7 us of draw time
VM_LINE = 1.5      # B: LINE/PLOT ~ 1.05 us over an ALU step
POST_VM = 9.5 + 4 * VM_STEP + 2 * VM_LINE  # E: measured post + 3rd Newton step + 2 rail LINEs
BAY_VM = 9.5       # E: per-bay reciprocal in the crowd/stands plans (one Newton post)
STRIPE_VM = 2 * 9.5  # E: two Newton sequences (near and far rail) per stripe
DOT = (846 - 40) / 131.2  # B: crowd dot after the phase wrap, less one draw floor
STANDS_BAY = (370 - 40) / 11  # B: today's stands plan per bay (65 segments / ~11 bays)
TREG = 74.2        # M treg: typed batch re-registered per frame, per point
TREG0 = 540.0      # M treg intercept: prog() + register + unregister


def band_seg(px, vertical):
    """B: band drawing of one segment (0.85 scan + 0.35 entry + per pixel;
    a vertical one re-enters a band every 8 rows at 0.64)."""
    if vertical:
        return 1.2 + 0.19 * px + 0.64 * (px // 8)
    return 1.2 + 0.143 * px


DNR, DFR, DST = 11.0, 22.6, 40.0
KN = {"LIGHT": [3, 2, 3, 3, 8, 10], "MID": [4, 3, 4, 5, 5, 7], "HEAVY": [5, 4, 6, 8, 4, 5]}


class Cam:
    def __init__(self, f, h, xc, back, yaw):
        self.f, self.h, self.xc, self.zc = f, h, xc, -back
        self.c, self.s = math.cos(yaw), math.sin(yaw)

    def depth(self, x, z):
        dx, dz = x - self.xc, z - self.zc
        return dx * self.s + dz * self.c, dx * self.c - dz * self.s

    def sx(self, x, z):
        d, lat = self.depth(x, z)
        return 120 + self.f * lat / d if d > 0.5 else None


def visible_run(cam, z, step, lod_px, x0=-100.0, x1=1100.0, near=2.0, far=1e9):
    """(screen x, depth) of points every `step` m on the line at depth z that
    are in front, on screen (-5..245) and at least lod_px from the last kept
    one, walking away from the camera so LOD thins the far end only."""
    xs = [x0 + i * step for i in range(int((x1 - x0) / step) + 1)]
    k0 = min(range(len(xs)), key=lambda i: abs(xs[i] - cam.xc))
    out = []
    for side in (1, -1):
        last, i = None, k0 if side == 1 else k0 - 1
        while 0 <= i < len(xs):
            x = xs[i]
            i += side
            d, _ = cam.depth(x, z)
            p = cam.sx(x, z)
            if d < near or d > far or p is None or p < -5 or p > 245:
                continue
            if last is not None and abs(p - last) < lod_px:
                continue
            out.append((p, d))
            last = p
    return out


def frame(tier, cam, route="vm", lod_px=2.0, crowd_lod=False, far=1e9):
    """Cost (us) by element of the background of one frame."""
    k = KN[tier]
    f = cam.f
    posts = [visible_run(cam, z, k[4], lod_px, far=far) for z in (DNR, DFR)]
    stripes = visible_run(cam, DNR, k[5], lod_px, far=far)
    bays = visible_run(cam, DST, 12.0, lod_px, far=far)
    n_posts = sum(len(p) for p in posts)
    post_band = sum(band_seg(1.1 * f / d, True) + 2 * band_seg(k[4] * f / d * 0.5, False) for run in posts for _, d in run)
    stripe_band = sum(band_seg(11.6 * f / d * 0.3, False) for _, d in stripes)  # slanted, ~30 % of its span
    if crowd_lod:  # keep today's WIDE density: one dot per ~2.5 px of bay width per row
        dots = [k[1] * max(1, min(k[2], int(12 * f / d / 7.5))) for _, d in bays]
    else:
        dots = [k[1] * k[2]] * len(bays)
    c = {}
    if route == "vm":
        # JS sends each line's clip ends exactly (2 projections) and the VM
        # walks the rest with Newton reciprocals: one draw per line.
        c["rail"] = 2 * (2 * PJ + LIT + DRAW) + n_posts * POST_VM + post_band
        c["turf"] = 2 * PJ + LIT + DRAW + len(stripes) * (STRIPE_VM + VM_LINE) + stripe_band
        c["stands"] = 2 * PJ + LIT + DRAW + len(bays) * (BAY_VM + STANDS_BAY + 6 * 1.3)
        c["crowd"] = 2 * PJ + LIT + DRAW + len(bays) * BAY_VM + sum(dots) * (DOT + 1.2)
    elif route == "js":
        # JS projects every point and packs 4 points (8 inputs) per draw.
        pts = 2 * n_posts
        c["rail"] = pts * PJ + math.ceil(pts / 4) * (LIT + DRAW + 3 * VM_LINE) + n_posts * 2 * VM_LINE + post_band
        pts = 2 * len(stripes)
        c["turf"] = pts * PJ + math.ceil(pts / 4) * (LIT + DRAW + 2 * VM_LINE) + stripe_band
        c["stands"] = len(bays) * (PJ + LIT + DRAW + STANDS_BAY + 6 * 1.3)
        c["crowd"] = len(bays) * (PJ + LIT + DRAW) + sum(dots) * (DOT + 1.2)
    else:  # typed batches re-registered every frame (posts and stripes only)
        c["rail"] = 2 * n_posts * TREG + 2 * TREG0 + post_band
        c["turf"] = 2 * len(stripes) * TREG + TREG0 + stripe_band
        c["stands"] = len(bays) * (PJ + LIT + DRAW + STANDS_BAY + 6 * 1.3)
        c["crowd"] = len(bays) * (PJ + LIT + DRAW) + sum(dots) * (DOT + 1.2)
    # Runners already divide once each; yaw adds a rotation (4 mul/add).
    c["runners"] = 8 * 4 * 2.4
    return {"cost": c, "ms": sum(c.values()) / 1000, "posts": n_posts, "stripes": len(stripes),
            "bays": len(bays), "dots": sum(dots)}


def oval_extra(n, route):
    """Extra cost (us) of one visible corner drawn as polylines of n segments:
    both rails and the stands' front line (3 x (n+1) vertices)."""
    v = 3 * (n + 1)
    if route == "js":
        return v * PJ + math.ceil(v / 4) * (LIT + DRAW + 3 * VM_LINE) + 3 * n * 3.0
    # VM: arc points by 2 SIN (small arguments, ~1.5 us each), a rotation, a
    # Newton reciprocal and the screen mapping; 3 draws with exact seeds.
    return 3 * (2 * PJ + LIT + DRAW) + v * (2 * 1.5 + 9.5 + 8 * VM_STEP + VM_LINE) + 3 * n * 3.0


# The user's mixed WIDE (2026-09-30): n fixed cameras evenly along the
# straight, 25 m inside the inner rail at 6 m; each turns to the leader;
# f = 200 near, auto-zoom holding the leader at ZOOM_PX (2.4 m horse) up to
# 1500. The director takes the camera nearest the leader.
BACK, ZOOM_PX, LANE_Z = 14.0, 14.0, 15.3


def pan_camera(L, n):
    xs = [(i + .5) * 1000 / n for i in range(n)]
    xc = min(xs, key=lambda x: abs(x - L))
    dx, dz = L - xc, LANE_Z + BACK
    dd = math.hypot(dx, dz)
    f = min(1500.0, max(200.0, ZOOM_PX * dd / 2.4))
    return Cam(f, 6, xc, BACK, math.atan2(dx, dz)), f, math.degrees(math.atan2(dx, dz))


def lead(L, n):
    xc = min(((i + .5) * 1000 / n for i in range(n)), key=lambda x: abs(x - L))
    return math.hypot(L - xc, LANE_Z + BACK)


def newton_error():
    print("yaw deg | posts | worst |dx| px, 2 steps | 3 steps | first post depth m")
    for deg in (0, 20, 40, 60, 75, 83):
        cam = Cam(200, 6, 0, BACK, math.radians(deg))
        pts = sorted((d, lat) for x in range(-200, 1200, 5) for d, lat in [cam.depth(float(x), DNR)]
                     if d > 2 and -5 <= 120 + 200 * lat / d <= 245)
        res = []
        for steps in (2, 3):
            r, w = 1 / pts[0][0], 0.0
            for d, lat in pts:
                for _ in range(steps):
                    r = r * (2 - d * r)
                w = max(w, abs(200 * lat * (r - 1 / d)))
            res.append(w)
        print(f"{deg} | {len(pts)} | {res[0]:.3g} | {res[1]:.3g} | {pts[0][0]:.1f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--newton", action="store_true")
    a = ap.parse_args()
    if a.newton:
        newton_error()
        return
    print("## sweep: leader 0..1000 m every 5 m, straight course")
    print("tier | cams | route | crowd LOD | median ms | p90 | max | posts max | bays max | f range | yaw max")
    for tier in ("MID", "HEAVY"):
        for n in (2, 3, 4, 6):
            for route, lod in (("js", False), ("treg", False), ("vm", False), ("vm", True)):
                rs, fs, ys = [], [], []
                for L in range(0, 1001, 5):
                    cam, f, y = pan_camera(L, n)
                    rs.append(frame(tier, cam, route, crowd_lod=lod))
                    fs.append(f)
                    ys.append(abs(y))
                t = sorted(r["ms"] for r in rs)
                print(f"{tier} | {n} | {route} | {'on' if lod else 'off'} | {t[len(t) // 2]:.2f} | {t[int(len(t) * .9)]:.2f} | "
                      f"{t[-1]:.2f} | {max(r['posts'] for r in rs)} | {max(r['bays'] for r in rs)} | "
                      f"{min(fs):.0f}-{max(fs):.0f} | {max(ys):.0f}")
    print("\n## worst frame by element (vm, crowd LOD on)")
    for tier in ("MID", "HEAVY"):
        for n in (2, 4, 6):
            L, r = max(((L, frame(tier, pan_camera(L, n)[0], "vm", crowd_lod=True)) for L in range(0, 1001, 5)),
                       key=lambda x: x[1]["ms"])
            cam, f, y = pan_camera(L, n)
            print(f"{tier} {n} cams L={L} f={f:.0f} yaw={y:.0f}: " + " ".join(f"{k}={v / 1000:.2f}" for k, v in r["cost"].items())
                  + f" total={r['ms']:.2f} posts={r['posts']} stripes={r['stripes']} bays={r['bays']} dots={r['dots']}")
    print("\n## by f and yaw (MID, vm, crowd LOD on): posts/stripes/bays, ms")
    for f in (200, 500, 1000, 1500):
        row = []
        for deg in (0, 30, 60, 75, 83):
            r = frame("MID", Cam(f, 6, 0, BACK, math.radians(deg)), "vm", crowd_lod=True)
            row.append(f"{deg}:{r['posts']}/{r['stripes']}/{r['bays']} {r['ms']:.2f}")
        print(f"f={f} | " + " | ".join(row))
    print("\n## reductions (vm, crowd LOD on): share of frames over the 5.7 ms room, fps of the worst frame")
    print("far = the leader's distance + margin (nothing is clipped in front of the leader)")
    print("tier | cams | lod px | margin m | median | p90 | max | over 5.7 ms | worst fps")
    for tier in ("MID", "HEAVY"):
        for n in (2, 4, 6):
            for lod, far in ((2.0, 1e9), (4.0, 1e9), (2.0, 150.0), (4.0, 150.0), (4.0, 75.0)):
                t = sorted(frame(tier, pan_camera(L, n)[0], "vm", lod, True, lead(L, n) + far)["ms"] for L in range(0, 1001, 5))
                over = sum(x > 5.7 for x in t) / len(t)
                fps = 1000 / max(1000 / 30, 27.6 + t[-1])
                print(f"{tier} | {n} | {lod:.0f} | {'-' if far > 1e8 else f'{far:.0f}'} | {t[len(t) // 2]:.2f} | "
                      f"{t[int(len(t) * .9)]:.2f} | {t[-1]:.2f} | {100 * over:.0f}% | {fps:.1f}")
    print("\n## oval: one visible corner as polylines")
    for n in (10, 16, 24):
        print(f"n={n} | js {oval_extra(n, 'js') / 1000:.2f} ms | vm {oval_extra(n, 'vm') / 1000:.2f} ms")


if __name__ == "__main__":
    main()
