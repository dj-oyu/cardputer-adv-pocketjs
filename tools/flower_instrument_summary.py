#!/usr/bin/env python3
"""Parse a flower-scene serial capture: PERF / SPLIT / SPLIT2 / SPLIT3 lines.

    python3 tools/flower_instrument_summary.py <log>
"""
import re
import statistics as st
import sys


def kv(line):
    """Pull every key=value out of a log line (values are floats by default)."""
    out = {}
    for key, val in re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=([-+0-9.eE]+)", line):
        try:
            out[key] = float(val)
        except ValueError:
            pass
    return out


def collect(lines, marker):
    rows = [kv(l) for l in lines if marker in l]
    return [r for r in rows if r]


def stat(rows, key):
    vals = [r[key] for r in rows if key in r]
    if not vals:
        return None
    return min(vals), st.mean(vals), max(vals), len(vals)


def show(rows, keys, label):
    print(f"-- {label}  (n={len(rows)})")
    for k in keys:
        s = stat(rows, k)
        if s:
            print(f"   {k:<26} min={s[0]:9.3f} mean={s[1]:9.3f} max={s[2]:9.3f}  (n={s[3]})")


def main():
    lines = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
    perf = collect(lines, "background: PERF")
    split = [r for r in collect(lines, "garden: SPLIT ") if "decor" in r]
    split2 = collect(lines, "garden: SPLIT2")
    split3 = [r for r in collect(lines, "garden: SPLIT3") if "decor" in r]
    print(f"{sys.argv[1]}: {len(lines)} lines, PERF={len(perf)} SPLIT={len(split)} "
          f"SPLIT2={len(split2)} SPLIT3={len(split3)}")
    # Only mode 3 (flower) PERF samples, and only valley (species 0) samples are comparable.
    modes = sorted({int(r["mode"]) for r in perf if "mode" in r})
    print(f"PERF modes present: {modes}")
    flower = [r for r in perf if r.get("mode") == 3]
    show(flower, ["fps", "draw", "prep", "loop", "kernel", "hud", "ovl", "fmt", "fps_cy", "menu", "send"],
         "PERF mode=3 (flower)")
    if split:
        show(split, ["frames", "total", "garden", "pixels", "decor", "ray", "visits", "hits",
                     "sqrt", "shade", "bell", "span", "scan", "pre"], "garden SPLIT")
    if split2:
        show(split2, ["species", "view", "parts", "motes", "horror", "garden", "seeds", "build", "petals"],
             "garden SPLIT2")
    if split3:
        show(split3, ["frames", "decor", "veg", "rays", "rest", "dissolve"], "garden SPLIT3 (new)")
        print("   raw SPLIT3 samples:")
        for l in lines:
            if "SPLIT3" in l:
                print("     " + l.split("garden: ", 1)[-1])
    # species mix, because a species swap changes every number above
    species = sorted({int(r["species"]) for r in split2 if "species" in r})
    print(f"species seen in SPLIT2: {species}")
    # The gate A/B: adjacent SPLIT3 windows alternate the switch, so each pair is
    # three seconds apart in the same scene and the paired difference is the
    # switch. Reported per pair rather than as two group means, because the scene
    # drifts between windows and that drift is what a group mean would absorb.
    rows3 = [l for l in lines if "SPLIT3" in l and "decor" in l and "gate=" in l]
    if rows3:
        print("\n-- gate A/B (adjacent windows, same binary)")
        pairs = []
        for i in range(len(rows3) - 1):
            a, b = kv(rows3[i]), kv(rows3[i + 1])
            if a.get("gate") == b.get("gate"):
                continue
            pairs.append((a, b))
        print(f"   {'pair':>4} {'gateA':>5} {'raysA':>7} {'raysB':>7} {'d_rays':>7} "
              f"{'vegA':>6} {'vegB':>6} {'d_veg':>6} {'decorA':>7} {'decorB':>7} {'d_decor':>7}")
        drows, dveg, ddec = [], [], []
        for n, (a, b) in enumerate(pairs):
            dr = a["rays"] - b["rays"]
            dv = a["veg"] - b["veg"]
            dd = a["decor"] - b["decor"]
            drows.append(dr)
            dveg.append(dv)
            ddec.append(dd)
            print(f"   {n:>4} {int(a['gate']):>5} {a['rays']:>7.2f} {b['rays']:>7.2f} {dr:>+7.2f} "
                  f"{a['veg']:>6.2f} {b['veg']:>6.2f} {dv:>+6.2f} "
                  f"{a['decor']:>7.2f} {b['decor']:>7.2f} {dd:>+7.2f}")
        if drows:
            print(f"   pairs={len(pairs)}  rays: mean delta={st.mean(drows):+.2f} median={st.median(drows):+.2f} "
                  f"min={min(drows):+.2f} max={max(drows):+.2f}")
            print(f"   control veg (unchanged code): mean={st.mean(dveg):+.2f} median={st.median(dveg):+.2f}")
            print(f"   decor: mean={st.mean(ddec):+.2f} median={st.median(ddec):+.2f}")
    # Join each SPLIT3 report with the SPLIT2 line that precedes it (same 60-frame
    # window), so every row figure can be read beside the plant that produced it.
    print("\n-- SPLIT3 by species (SPLIT2 joined: same 60-frame window)")
    print(f"   {'species':>7} {'parts':>5} {'decor':>7} {'veg':>6} {'veg cy/row':>10} "
          f"{'rays':>7} {'ray cy/row':>10} {'rest':>6} {'dissolve':>8}")
    print(f"   {'-'*7} {'-'*5} {'-'*7} {'-'*6} {'-'*10} {'-'*7} {'-'*10} {'-'*6} {'-'*8}")
    for i, l3 in enumerate([l for l in lines if "SPLIT3" in l and "decor" in l]):
        r3 = kv(l3)
        prev2 = [l for l in lines[:lines.index(l3) + 1] if "SPLIT2" in l]
        r2 = kv(prev2[-1]) if prev2 else {}
        vegcy = r3.get("veg", 0)
        # cy/row is not a key of its own; it is the third field of the veg/rays groups
        m = re.search(r"veg=[-0-9.]+ \(([0-9]+) rows, ([0-9]+) passes, ([0-9]+) cy/row\)", l3)
        m2 = re.search(r"rays=[-0-9.]+ \(([0-9]+) rows, ([0-9]+) cy/row\)", l3)
        print(f"   {int(r2.get('species', -1)):>7} {int(r2.get('parts', -1)):>5} "
              f"{r3.get('decor', 0):>7.2f} {r3.get('veg', 0):>6.2f} "
              f"{(m.group(3) if m else '-'):>10} {r3.get('rays', 0):>7.2f} "
              f"{(m2.group(2) if m2 else '-'):>10} {r3.get('rest', 0):>6.2f} "
              f"{int(r3.get('dissolve', 0)):>8}")


if __name__ == "__main__":
    main()
