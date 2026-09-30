"""DERBY WATCH's seeds, recomputed on the host (apps/derby/derby_watch.js).

The app draws pocket.random.seed() (esp_random() on the device) once per
session, mixes it with the stored race count sn, and derives each race's seed
from that base and the race number:

    base = first mulberry32 output of (HW ^ sn*PHI)      (a hash of the two)
    seed = (base + race*PHI) mod 2^32                     field(seed)
    the race's noise: mulberry32(seed + NOISE)            race()

mulberry32 is a counter generator: its state steps by G = 0x6D2B79F5 and each
output is a hash of the state. Two streams are the same numbers, shifted, when
their states fall within one stream's length of each other on that counter, so
the distance between two streams is (s2 - s1) * G^-1 mod 2^32. With the
additive layout every race's stream sits at race*PHI*G^-1 (+ a constant) on
that circle whatever the base is, so the overlap check below holds for every
session, not just the one it happens to sample.

    python3 tools/games/derby_seeds.py        prints the analysis (host only)
run_derby.py imports it to check the SCENE log's seeds against the formula.
"""
from __future__ import annotations

import math

M32 = 1 << 32
PHI = 0x9E3779B9
G = 0x6D2B79F5
NOISE = 0x5BD1E995
G_INV = pow(G, -1, M32)
FIELD_DRAWS = 64           # 8 runners x 8 draws in field()
# A race's noise stream: 8 form draws, then 2 a runner a 0.05 s step until all
# eight finish. 20,000 Monte Carlo races drew 19,953 on average and 20,348 at
# most (longest race 63.65 s); 21,000 covers that. The code's own bound is
# settle()'s 200 s, 64,008 draws, which no race of this model comes near.
NOISE_MEASURED = 21_000
NOISE_MAX = 8 + 16 * 4000


def imul(a: int, b: int) -> int:
    return (a * b) & 0xFFFFFFFF


def mulberry32(seed: int):
    """The app's rng(): yields uint32 outputs (the app divides by 2^32)."""
    s = seed & 0xFFFFFFFF
    while True:
        s = (s + G) & 0xFFFFFFFF
        t = imul(s ^ (s >> 15), 1 | s)
        t = ((t + imul(t ^ (t >> 7), 61 | t)) & 0xFFFFFFFF) ^ t
        yield (t ^ (t >> 14)) & 0xFFFFFFFF


def session_base(hw: int, sn: int) -> int:
    return next(mulberry32(hw ^ imul(sn, PHI)))


def race_seed(hw: int, sn: int, race: int) -> int:
    return (session_base(hw, sn) + race * PHI) % M32


def check_log(out: str, hw: int, stored_race: int) -> int:
    """Every paddock seed the app logged is the formula's: sn is 0 until the
    stored game loads, then the stored race count (the load's own SCENE line
    comes just before its LOADED line). Returns how many SCENE pad lines were
    checked; raises on a mismatch."""
    import re
    sn, n = 0, 0
    lines = [l for l in out.splitlines() if "DERBY " in l]
    for k, line in enumerate(lines):
        if "DERBY LOADED" in line or k + 1 < len(lines) and "DERBY LOADED" in lines[k + 1]:
            sn = stored_race
        m = re.search(r"DERBY SCENE pad race=(\d+) seed=([0-9A-F]{8})", line)
        if m:
            race, seed = int(m.group(1)), int(m.group(2), 16)
            want = race_seed(hw, sn, race)
            if seed != want:
                raise SystemExit(f"race {race}: logged seed {seed:08X}, formula {want:08X} (hw {hw:08X}, sn {sn})")
            n += 1
    return n


def first_overlap(streams: list[tuple[int, int]]) -> tuple[int, int] | None:
    """streams: (start state, length). The first pair (by sorted position)
    whose runs of states meet on the counter circle, or None."""
    pos = sorted(((s * G_INV) % M32, n, i) for i, (s, n) in enumerate(streams))
    if len(pos) < 2:
        return None
    for k in range(len(pos)):
        p, n, i = pos[k]
        q, _, j = pos[(k + 1) % len(pos)]
        if (q - p) % M32 < n:
            return (i, j)
    return None


def race_streams(races: list[int], noise_len: int) -> list[tuple[int, int]]:
    out = []
    for r in races:
        s = r * PHI % M32              # base 0: the layout does not depend on it
        out += [(s, FIELD_DRAWS), ((s + NOISE) % M32, noise_len)]
    return out


def clean_races(noise_len: int, demos: int) -> int:
    """The largest R such that races 1..R and the demo races 1e6+1..1e6+demos
    share no stretch of any stream (binary search: overlaps only grow with R)."""
    demo = [1_000_000 + d for d in range(1, demos + 1)]
    lo, hi = 0, 1
    while first_overlap(race_streams(list(range(1, hi + 1)) + demo, noise_len)) is None:
        lo, hi = hi, hi * 2
        if hi > 1 << 22:
            return hi
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if first_overlap(race_streams(list(range(1, mid + 1)) + demo, noise_len)) is None:
            lo = mid
        else:
            hi = mid
    return lo


def chi2(values: list[int], bins: int) -> tuple[float, float, float]:
    """Pearson chi-square of the top bits against uniform, with the 0.1% and
    99.9% points of its distribution (Wilson-Hilferty)."""
    c = [0] * bins
    shift = 32 - int(math.log2(bins))
    for v in values:
        c[v >> shift] += 1
    e = len(values) / bins
    x = sum((k - e) ** 2 / e for k in c)
    df = bins - 1
    wh = lambda z: df * (1 - 2 / (9 * df) + z * math.sqrt(2 / (9 * df))) ** 3
    return x, wh(-3.0902), wh(3.0902)


def analyse(hw: int = 0x2545F491, races: int = 2000, draws: int = 1000, firsts: int = 20_000) -> bool:
    ok = True
    print(f"mulberry32 counter step G^-1 = {G_INV:08X}; race spacing on the counter = {PHI * G_INV % M32:08X}")
    for name, n in (("measured", NOISE_MEASURED), ("the 200 s bound", NOISE_MAX)):
        r = clean_races(n, 1000)
        print(f"no shared stretch among races 1..{r} and 1000 demo races (noise stream {n} draws, {name})")
        # 10,000 races is some 140 hours of play at 50 s a race.
        ok &= r >= 10_000 or n == NOISE_MAX
    base = [session_base(hw, sn) for sn in range(20_000)]
    x, lo, hi = chi2(base, 256)
    print(f"session base over sn 0..19999 (one hw): chi2 {x:.1f} (df 255, 0.1%..99.9% {lo:.0f}..{hi:.0f})")
    ok &= lo < x < hi
    seeds = [race_seed(hw, 5, r) for r in range(1, races + 1)]
    for name, off, n in (("field", 0, FIELD_DRAWS), ("noise", NOISE, draws)):
        v = []
        for s in seeds:
            g = mulberry32(s + off)
            v += [next(g) for _ in range(n)]
        x, lo, hi = chi2(v, 256)
        print(f"{name} draws of races 1..{races} ({len(v)} values): chi2 {x:.1f} (0.1%..99.9% {lo:.0f}..{hi:.0f})")
        ok &= lo < x < hi
        # Race to race: the first draw of each race, and consecutive pairs of it.
        first = [next(mulberry32(race_seed(hw, 5, r) + off)) for r in range(1, firsts + 1)]
        x, lo, hi = chi2(first, 64)
        pairs = [((a >> 29) << 29) | ((b >> 29) << 26) for a, b in zip(first, first[1:])]
        y, plo, phi = chi2(pairs, 64)
        print(f"  first {name} draw of races 1..{firsts}: chi2 {x:.1f} ({lo:.0f}..{hi:.0f}); consecutive races' pairs "
              f"(8x8): chi2 {y:.1f} ({plo:.0f}..{phi:.0f})")
        ok &= lo < x < hi and plo < y < phi
    print("DERBY_SEEDS " + ("PASS" if ok else "FAIL"))
    return ok


if __name__ == "__main__":
    raise SystemExit(0 if analyse() else 1)
