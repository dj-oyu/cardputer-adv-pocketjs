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


def armof(row):
    """The A/B arm of a SPLIT3 row, for captures from before `arm=` was printed.

    The four switches are enough to name it: arms differ from the shipping
    setting in exactly one field, and the flips this replaced moved all four at
    once (gate/tweaks/canopy one way, sq the other), which no arm of the
    one-at-a-time rotation produces -- so a row that names none of the five is
    reported as unknown rather than forced into one.
    """
    if "arm" in row:
        return int(row["arm"])
    key = (int(row.get("gate", -1)), int(row.get("tweaks", -1)),
           int(row.get("sq", -1)), int(row.get("canopy", -1)))
    return {(1, 1, 0, 1): 0, (0, 1, 0, 1): 1, (1, 0, 0, 1): 2,
            (1, 1, 0, 0): 3, (1, 1, 1, 1): 4}.get(key)


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
    # PERF prints fps= twice -- the frame rate after clock=, then the HUD's own
    # fps sub-term inside the parentheses -- and kv() keeps the last, so take the
    # first for the frame rate.
    for l, r in zip([x for x in lines if "background: PERF" in x], perf):
        m = re.search(r"\bfps=([0-9.]+)", l)
        if m:
            r["fps"] = float(m.group(1))
    # The panel-transfer A/B: the PERF report flips the mode every window and
    # prints which one it used, so adjacent windows are a paired measurement in
    # one binary (see docs/flower-optimisation-options.md A).
    if perf and "async" in perf[0]:
        print("-- panel transfer A/B (adjacent PERF windows, same binary)")
        pairs = [(a, b) for a, b in zip(perf, perf[1:]) if a.get("async") != b.get("async")]
        drows, dsend, dfps = [], [], []
        print(f"   {'pair':>4} {'A':>2} {'drawA':>7} {'drawB':>7} {'d_draw':>7} {'sendA':>6} {'sendB':>6} "
              f"{'d_send':>7} {'fpsA':>6} {'fpsB':>6}")
        for n, (a, b) in enumerate(pairs):
            dd = a.get("draw", 0) - b.get("draw", 0)
            ds = a.get("send", 0) - b.get("send", 0)
            drows.append(dd)
            dsend.append(ds)
            dfps.append(b.get("fps", 0) - a.get("fps", 0))
            print(f"   {n:>4} {int(a.get('async', -1)):>2} {a.get('draw', 0):>7.2f} {b.get('draw', 0):>7.2f} "
                  f"{dd:>+7.2f} {a.get('send', 0):>6.2f} {b.get('send', 0):>6.2f} {ds:>+7.2f} "
                  f"{a.get('fps', 0):>6.1f} {b.get('fps', 0):>6.1f}")
        if drows:
            print(f"   pairs={len(pairs)}  draw: mean={st.mean(drows):+.2f} median={st.median(drows):+.2f} "
                  f"min={min(drows):+.2f} max={max(drows):+.2f}   (A = async=1 minus async=0)")
            print(f"              send: mean={st.mean(dsend):+.2f} median={st.median(dsend):+.2f}")
            print(f"              fps : mean={st.mean(dfps):+.2f} (B minus A)")
        # group means, for the reader who wants the two modes side by side
        for label, want in (("async=1", 1), ("async=0", 0)):
            g = [r for r in perf if r.get("async") == want]
            if g:
                print(f"   {label}: n={len(g)} draw={st.mean([r['draw'] for r in g]):.2f} "
                      f"send={st.mean([r['send'] for r in g]):.2f} "
                      f"loop={st.mean([r['loop'] for r in g]):.2f} "
                      f"kernel={st.mean([r['kernel'] for r in g]):.2f} "
                      f"fps={st.mean([r['fps'] for r in g]):.1f}")
    perf = [r for r in perf if r.get("mode") == 3]
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
    # The decor A/B: one switch moves per 60-frame window and arm 0 is the
    # shipping setting of all four, so each non-zero window is paired with the
    # arm-0 window beside it (previous when it is arm 0, else next). Reported per
    # pair and then as a median per switch: the plant rotates on its own 40 s
    # hold, so the drift between two windows three seconds apart is a real
    # signal, and a single pair is not a measurement of the switch.
    rows3 = [l for l in lines if "SPLIT3" in l and "decor" in l and "gate=" in l]
    if rows3:
        print("\n-- decor A/B (each window against its arm-0 neighbour, same binary)")
        arms = [armof(kv(l)) for l in rows3]
        vals = [kv(l) for l in rows3]
        table = {}
        for i, arm in enumerate(arms):
            if arm == 0 or arm is None:
                continue
            j = i - 1 if i and arms[i - 1] == 0 else (i + 1 if i + 1 < len(arms) and arms[i + 1] == 0 else None)
            if j is None:
                continue
            base = vals[j]
            row = vals[i]
            for k in ("decor", "veg", "rays", "rest"):
                if k in row and k in base:
                    table.setdefault(arm, {}).setdefault(k, []).append(row[k] - base[k])
        names = {1: "gate off", 2: "tweaks off", 3: "canopy off", 4: "fixed sqrt on"}
        print(f"   {'arm':>3} {'switch':<14} {'pairs':>5} {'d_decor':>14} {'d_veg':>14} "
              f"{'d_rays':>14} {'d_rest':>14}")
        for arm in sorted(table):
            t = table[arm]
            cells = []
            for k in ("decor", "veg", "rays", "rest"):
                v = t.get(k, [])
                cells.append(f"{st.median(v):+.2f} (n={len(v)})" if v else "-")
            print(f"   {arm:>3} {names.get(arm, '?'):<14} {len(t.get('decor', [])):>5} "
                  + " ".join(f"{c:>14}" for c in cells))
        print("   (delta = arm minus arm 0, ms/frame; negative means the arm is faster. "
              "The row of arm 0 is the shipping setting.)")
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
