"""DERBY WATCH on the host: build and run tools/games/test_derby_host.c.

Builds the harness against the real QuickJS, the real pocket.kasane (view and
procedural), pocket.input.keys (keymap + keystate) and the Kasane renderer,
then runs:
  1. the full scripted game at the default load tier (MID): paddock, gate,
     race with camera changes, photo, result, a replay that must finish
     identically, the next race and the Back turn; every procedural draw
     through the plan / debug-step / single-step VM oracle and every frame
     compared pixel for pixel;
     before the game, the demo (attract) mode: the idle clock, three whole
     demos, a key in each scene, Back inside a demo;
  2. each of the first three demo races again, played by hand from the same
     stored race with the same pick: FINISH / PICK / RESULT must be the same
     strings (the demo runs the game, not a copy of it);
  3. one race at LIGHT and one at HEAVY (same cameras, no demo) for the
     statistics;
  4. each tier with the director alone (no camera keys), and LIGHT and HEAVY
     with the pick on #8 (the widest manual close-up);
  5. the panning units (docs/apps/derby-watch.md "首振りカメラ"): LIGHT and HEAVY
     with every WIDE a panning unit (f from 200: LIGHT has the widest Newton
     steps, HEAVY the most segments), and LIGHT with the screen moved to 700 m,
     150 m deep and f held at 200, so that a panning unit sees it (its face,
     feed and occlusion checked; the side units' faces are not, the screen
     being out of place for them).
Every run with a panning unit compares each point its VM series emit (Newton
reciprocals, float) with the exact projection (<0.1 px on the panel).
The player's race must finish identically in every run. Every run checks the
big screen: its face drawn exactly inside the bezel, nothing but the feed on
the face, the view's lettering inside it.
Seeds (pocket.random.seed() is DERBY_HW in the harness, fixed): every paddock
seed the MID run logs must be tools/games/derby_seeds.py's formula; the same
stored race under another hardware seed must be another race; the seed
analysis (stream overlaps, chi-square) must pass. Odds: node runs
tools/games/tune_derby.mjs 20000 --check (the bettor's return is 0.8 in every
chance bin and popularity rank).
--m32 builds for i386 with the device's 8-byte JSValue and 4-byte pointers
(tools/vmtest/m32_sysroot.sh) so the guest heap figures match the firmware's
object sizes; the default 64-bit build runs with ASan/UBSan. --ppm writes the
composited panels, docs/apps/derby-watch-preview.png (its paddock panel now
comes after the demos, so it differs from the committed sheet there) and
docs/apps/derby-watch-demo-preview.png and derby-watch-dissolve-preview.png (the
demo's curtain), and derby-pan-preview.png: each panning shot (near, far,
zoomed) beside the same frame with side units only (a second run, PAN[0] 1e9). WSL/Linux only; no
device and no serial port.
"""
from __future__ import annotations

import argparse
import os
import re
import struct
import subprocess
import zlib
from pathlib import Path

import derby_seeds

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / ".cache/derby_host"
QJS = "components/quickjs-ng/quickjs-ng"
EXTRA = ["main/pocket/pocket_input.c", "main/hal/keymap.c", "main/hal/keystate.c",
         # pocket.app.load and its table, read from apps/derby/chunks.txt
         "main/pocket/pocket_app_load.c", "main/pocket/app_registry.c", "tools/hostshim/app_chunks_host.c"]


def kasane_sources() -> list[str]:
    """The .c list of tools/build_kasane_test.sh (minus its test source)."""
    text = (ROOT / "tools/build_kasane_test.sh").read_text()
    return [s for s in re.findall(r"(?:main|tools)/[\w/]+\.c", text)
            if not s.startswith("tools/test_pocket_kasane")]


def build(flags: list[str], cache: Path) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    subprocess.run(["python3", "tools/make_font.py", str(cache)], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)
    data = (ROOT / "apps/pet/assets/pets-compact.bin").read_bytes()
    (cache / "kasane_pet_test_data.h").write_text(
        "static const uint8_t pet_test_data[] = {" + ",".join(map(str, data)) + "};\n")
    # Production subscription semantics, verbatim (as build_keytest_app_test.sh).
    api = (ROOT / "main/pocket/pocket_api.c").read_text()
    a = api.index("// ----------------------------------------------------------- subscriptions")
    b = api.index("// ------------------------------------------------------- async completions", a)
    (cache / "pocket_sub_impl.inc").write_text(api[a:b])
    defs = ["-DQUICKJS_NG_BUILD", "-D_GNU_SOURCE"]
    objects = []
    for name in ("dtoa", "libregexp", "libunicode", "quickjs", "quickjs-vm"):
        obj = cache / f"{name}.o"
        src = ROOT / QJS / f"{name}.c"
        if not obj.exists() or src.stat().st_mtime > obj.stat().st_mtime:
            subprocess.run(["gcc", "-std=gnu11", "-c", "-O1", "-g", "-w", *flags, *defs, "-I", QJS,
                            "-I", "components/pocketjs_guest/include", str(src), "-o", str(obj)],
                           cwd=ROOT, check=True)
        objects.append(str(obj))
    binary = cache / "test-derby"
    includes = [f"-I{d}" for d in (QJS, "tools/hostshim", "main", "main/pocket", "main/ui",
                                   "main/ui/kasane", "main/text", "main/hal", str(cache))]
    subprocess.run(["gcc", "-std=gnu11", "-O1", "-g", *flags, "-DKSN_PROC_POINTS_PIE_MODEL",
                    "-Wall", "-Wextra", "-Werror", "-fno-omit-frame-pointer", *includes,
                    "tools/games/test_derby_host.c", *kasane_sources(), *EXTRA, *objects,
                    "-Wl,--wrap=calloc", "-Wl,--wrap=free", "-lm", "-o", str(binary)],
                   cwd=ROOT, check=True)
    return binary


def png(path: Path, images: list[bytes], columns: int) -> None:
    w, h, gap = 240, 135, 2
    rows = (len(images) + columns - 1) // columns
    width, height = columns * w + (columns - 1) * gap, rows * h + (rows - 1) * gap
    grey = bytes((0x30, 0x30, 0x30))
    lines = bytearray()
    for y in range(height):
        lines.append(0)
        row, sy = divmod(y, h + gap)
        for column in range(columns):
            i = row * columns + column
            if sy >= h or i >= len(images):
                lines.extend(grey * w)
            else:
                lines.extend(images[i][sy * w * 3:(sy + 1) * w * 3])
            if column + 1 < columns:
                lines.extend(grey * gap)
    chunk = lambda tag, data: (struct.pack(">I", len(data)) + tag + data +
                               struct.pack(">I", zlib.crc32(tag + data)))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" +
                     chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(bytes(lines), 9)) + chunk(b"IEND", b""))


ORDER = ["pad", "gate", "start", "wide", "close", "field", "lead", "slow", "photo", "photo_zoom", "result"]
DEMO_ORDER = ["title", "demo_on", "demo_off", "demo_race", "demo_res"]
# The demo's curtain: the idle paddock to black, the card, the demo coming in
# (fx01..10); a key in a demo race and the paddock fading back in (ex1..5).
CURTAIN = [f"fx{i:02d}" for i in range(1, 11)] + [f"ex{i}" for i in range(1, 6)]
# The big screen: switching on, the director's VISION shot and its pan, WIDE
# after it, the replay's CLOSE (screen out of frame) and FIELD (small).
VISION = ["vision_on", "vision", "pan", "wide_screen", "close_off", "field_screen", "lead", "head", "slow"]


def sheet(folder: Path, out: Path, order: list[str], columns: int) -> None:
    header = b"P6\n240 135\n255\n"
    shots = {}
    for f in sorted(folder.glob("derby_*.ppm")):
        tag = f.stem.split("_", 2)[2]
        shots.setdefault(tag, f.read_bytes()[len(header):])
    images = [shots[t] for t in order if t in shots]
    png(out, images, columns)
    print(f"Wrote {out} ({len(images)} panels: {', '.join(t for t in order if t in shots)})")


def run(binary: Path, env: dict) -> str:
    """Run the harness, echo its output, fail on a non-zero exit."""
    p = subprocess.run([str(binary)], cwd=ROOT, env=env, capture_output=True, text=True)
    print(p.stdout, end="", flush=True)
    if p.returncode:
        print(p.stderr, end="")
        raise SystemExit(f"harness failed ({p.returncode})")
    return p.stdout


HW = 0x2545F491          # the harness's pocket.random.seed() (test_derby_host.c)


def check_demo_matches_play(binary: Path, env: dict, out: str) -> None:
    """Each demo race, played again by hand from the same stored race with the
    same pick: the FINISH (order, full-precision finish times, margin), PICK
    and RESULT lines must be the same strings. The session's base mixes the
    hardware seed with the stored race count (3 in the scripted game, the demo
    race's number here), so the hardware seed given to the second run is the
    one that makes the same base."""
    for n in (1, 2, 3):
        demo = {k: re.search(rf"^DEMO_{k} {n} (.*)$", out, re.M).group(1) for k in ("FINISH", "PICK", "RESULT")}
        race = int(re.search(r"race=(\d+)", demo["FINISH"]).group(1))
        pick = int(re.match(r"PICK (\d+)", demo["PICK"]).group(1))
        print(f"==== demo {n} by hand: race {race}, pick {pick}", flush=True)
        hw = HW ^ derby_seeds.imul(3, derby_seeds.PHI) ^ derby_seeds.imul(race, derby_seeds.PHI)
        text = run(binary, dict(env, DERBY_NORMAL=f"{race},{pick}", DERBY_HW=str(hw)))
        for k, v in demo.items():
            mine = re.search(rf"^NORMAL_{k} (.*)$", text, re.M).group(1)
            if mine != v:
                raise SystemExit(f"demo {n} {k} differs from play:\n  demo {v}\n  play {mine}")
        print(f"demo {n} = play: {demo['FINISH']}")


def check_seeds(binary: Path, env: dict, out: str) -> None:
    """The logged seeds are the formula's; another hardware seed makes the
    same stored race (3, sn 3) another race; the same one, the same race."""
    n = derby_seeds.check_log(out, HW, 3)
    print(f"seeds: {n} paddock seeds in the MID run match the formula", flush=True)
    finish = {}
    for hw in (HW, HW ^ 1):
        print(f"==== race 3 by hand with hardware seed {hw:08X}", flush=True)
        text = run(binary, dict(env, DERBY_NORMAL="3,1", DERBY_HW=str(hw)))
        derby_seeds.check_log(text, hw, 3)
        finish[hw] = re.search(r"^NORMAL_FINISH (.*)$", text, re.M).group(1)
    if finish[HW] == finish[HW ^ 1]:
        raise SystemExit(f"another hardware seed ran the same race: {finish[HW]}")
    if not out.count(finish[HW].split(" t=")[0]):
        raise SystemExit(f"race 3 by hand differs from the scripted game's race 3: {finish[HW]}")
    print("seeds: another hardware seed is another race; the same one is the same race")
    if not derby_seeds.analyse(HW):
        raise SystemExit("seed analysis failed")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--m32", action="store_true", help="device-sized i386 build, no sanitizers")
    parser.add_argument("--ppm", action="store_true", help="write panels and the preview sheet")
    parser.add_argument("--heap-limit", type=int, help="run once with this guest heap limit (bytes)")
    args = parser.parse_args()
    if args.m32:
        env = dict(os.environ, M32_SYSROOT=str(ROOT / ".cache/kasane_megademo_app/m32sys"))
        flags = subprocess.run(["bash", "tools/vmtest/m32_sysroot.sh"], cwd=ROOT, check=True, env=env,
                               capture_output=True, text=True).stdout.split()
        binary = build(flags, CACHE / "m32")
    else:
        binary = build(["-fsanitize=address,undefined"], CACHE / "asan")
    env = os.environ.copy()
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0:abort_on_error=1")
    env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    # (tier, extra env): the scripted game at each tier, then the director
    # alone (no camera keys) and the widest manual close-up (pick #8).
    runs = [(1, {})] if args.heap_limit else [(1, {}), (0, {}), (2, {})] + [
        (t, {"DERBY_NOCAM": "1"}) for t in (0, 1, 2)] + [(t, {"DERBY_PICK": "7"}) for t in (0, 2)] + [
        (t, {"DERBY_NOCAM": "1", "DERBY_PAN": "0"}) for t in (0, 2)] + [
        (0, {"DERBY_NOCAM": "1", "DERBY_PAN": "0,1", "DERBY_PANFACE": "1", "DERBY_JS": "VS[0]=700;VS[1]=150"})]
    finish = {}
    for tier, extra in runs:
        e = dict(env, DERBY_TIER=str(tier), DERBY_CSV=str(CACHE / f"frames_tier{tier}{''.join(extra)}.csv"), **extra)
        if args.heap_limit:
            e["DERBY_HEAP_LIMIT"] = str(args.heap_limit)
        if args.ppm and tier == 1 and not extra:
            ppm = CACHE / "ppm"
            ppm.mkdir(parents=True, exist_ok=True)
            for old in ppm.glob("*.ppm"):
                old.unlink()
            e["DERBY_PPM"] = str(ppm)
        print(f"==== tier {tier} ({['LIGHT', 'MID', 'HEAVY'][tier]}) {extra or ''}", flush=True)
        out = run(binary, e)
        finish[(tier, *extra)] = re.search(r"^finish: (.*)$", out, re.M).group(1)
        if args.ppm and tier == 1 and not extra:
            pan_shots = re.findall(r"^PANSHOT (\w+) (\d+)", out, re.M)
        if tier == 1 and not extra and not args.heap_limit:
            check_demo_matches_play(binary, env, out)
            check_seeds(binary, env, out)
    # MID ran ten demos before its first race, the others none; neither the
    # tier, the camera keys nor the pick may move the player's race.
    if len(set(finish.values())) > 1:
        raise SystemExit(f"the player's race differs between runs: {finish}")
    if not args.heap_limit:
        print("==== odds calibration (node tools/games/tune_derby.mjs 20000 --check)", flush=True)
        if subprocess.run(["node", "tools/games/tune_derby.mjs", "20000", "--check"], cwd=ROOT).returncode:
            raise SystemExit("odds calibration check failed")
    if args.ppm:
        sheet(CACHE / "ppm", ROOT / "docs/apps/derby-watch-preview.png", ORDER, 4)
        sheet(CACHE / "ppm", ROOT / "docs/apps/derby-watch-demo-preview.png", DEMO_ORDER, 3)
        sheet(CACHE / "ppm", ROOT / "docs/apps/derby-watch-vision-preview.png", VISION, 4)
        sheet(CACHE / "ppm", ROOT / "docs/apps/derby-watch-dissolve-preview.png", CURTAIN, 5)
        # The same frames with side units only: WIDE stays the side camera.
        side = CACHE / "ppm_side"
        side.mkdir(parents=True, exist_ok=True)
        for old in side.glob("*.ppm"):
            old.unlink()
        shots = ",".join(f"{t}:side_{n[4:]}" for n, t in pan_shots)
        run(binary, dict(env, DERBY_TIER="1", DERBY_PAN="1e9", DERBY_PPM=str(side), DERBY_SHOTS=shots,
                         DERBY_CSV=str(CACHE / "frames_side.csv")))
        for f in side.glob("derby_*_side_*.ppm"):
            f.replace(CACHE / "ppm" / f.name)
        order = [x for n, _ in pan_shots for x in (f"side_{n[4:]}", n)]
        sheet(CACHE / "ppm", ROOT / "docs/apps/derby-pan-preview.png", order, 2)


if __name__ == "__main__":
    main()
