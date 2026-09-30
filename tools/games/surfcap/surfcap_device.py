"""Does a second procedural surface fit next to DERBY WATCH? (docs/kasane/surface-segment-cap.md)

  make N   write .cache/surfcap/derby_watch.js: apps/derby/derby_watch.js with a
           probe that, on the first frame after evaluation, creates surface 1
           with createSurface({maxSegments: N}) (N=0: createSurface(), the
           1,024 default), leases its image resource and allocates its frames
           (beginFrame(0, s) then beginFrame(0), as MEGADEMO does). N<0 is
           the baseline: the same logging, no surface 1. It logs
           SCAP lines: the heap before, the result, the heap one turn later,
           and every 300 frames the lowest turn-start free/largest since.
           Surface 1 is never committed or shown: this measures whether its
           frames fit and what the rest of the session has left, not drawing.
  size N   write a minimal app to the same path that measures what each step
           costs the native heap, one step per 8 frames (past info()'s
           100 ms sample): A -> beginFrame(0): surface 0's two frames,
           scratch and VM -> B -> createSurface+resource -> C ->
           beginFrame(0, s): surface 1's two frames alone -> D, E.
  sum LOG  print the SCAP lines and any failure markers of a bgcost_device.py log.

apps/derby is not modified. Build the copy into the DERBY WATCH row with
  idf.py -B build_surfcap "-DDERBY_BGCOST_SOURCE=../.cache/surfcap/derby_watch.js" build
and drive it with tools/games/bgcost/bgcost_device.py run --out <log>.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "apps/derby/derby_watch.js"
OUT = ROOT / ".cache/surfcap/derby_watch.js"
ANCHOR = "  globalThis.frame = function (b) {\n"
CALL = "    try { attract(b); frame_(); }"
PROBE = """  let scN = 0, scMin = 1e9, scLg = 1e9;
  function scap() {
    const m = pocket.memory.info(), f = m.internalFreeBytes, g = m.internalLargestBytes;
    if (f === null) return;
    if (++scN > 1) { if (f < scMin) scMin = f; if (g < scLg) scLg = g; }
    if (scN === 1) {
      log('SCAP pre n=%(n)d free=' + f + ' lg=' + g);
      try { const s = H.createSurface(%(arg)s); H.resource(s); H.beginFrame(0, s); H.beginFrame(0); log('SCAP ok'); }
      catch (e) { log('SCAP fail ' + e); }
    } else if (scN === 10) log('SCAP post free=' + f + ' lg=' + g);
    else if (!(scN %% 300)) log('SCAP t=' + scN + ' ' + scene + ' free=' + f + ' lg=' + g + ' min=' + scMin + ' minlg=' + scLg + ' reg=' + reg + ' q=' + queue.length);
  }
"""
SIZE = """(function () {
  const H = pocket.kasane.procedural, log = m => console.log('DERBY ' + m);
  let n = 0, st = 0, at = 0, s = 0;
  globalThis.frame = function () {
    ++n;
    const m = pocket.memory.info(), f = m.internalFreeBytes, g = m.internalLargestBytes;
    if (f === null || n - at < 8 || st > 4) return;
    at = n;
    log('SCAP size ' + 'ABCDE'[st] + ' free=' + f + ' lg=' + g);
    if (st === 0) H.beginFrame(0);
    else if (st === 1) { s = H.createSurface(%(arg)s); H.resource(s); }
    else if (st === 2) { H.beginFrame(0, s); H.beginFrame(0); }
    ++st;
  };
  log('READY size n=%(n)d');
})();
"""
BAD = ("Guru Meditation", "START_FAILED", "RUNAWAY", "abort()", "Uncaught", "app: OOM",
       "FRAMEFAIL", "LOADFAIL", "EVAL_ERROR")


def make(n: int) -> None:
    text = SOURCE.read_text(encoding="utf-8")
    if text.count(ANCHOR) != 1 or text.count(CALL) != 1:
        raise SystemExit("derby_watch.js no longer has the frame() this probe patches")
    arg = "{maxSegments: %d}" % n if n > 0 else ""
    probe = PROBE % {"n": n if n > 0 else 1024 if n == 0 else -1, "arg": arg}
    if n < 0:  # baseline: the same logging, no surface 1
        probe = re.sub(r"      try \{ const s = .*\n      catch .*\n", "", probe)
    text = text.replace(ANCHOR, probe + ANCHOR)
    text = text.replace(CALL, "    try { scap(); attract(b); frame_(); }")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUT} ({len(text.encode())} B, maxSegments={n if n > 0 else 'default' if n == 0 else 'none'})")


def size(n: int) -> None:
    arg = "{maxSegments: %d}" % n if n > 0 else ""
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(SIZE % {"arg": arg, "n": n if n > 0 else 1024}, encoding="utf-8", newline="\n")
    print(f"wrote {OUT} (size probe, maxSegments={n if n > 0 else 'default'})")


def summarise(path: Path) -> int:
    text = path.read_bytes().replace(b"\x00", b"").decode("utf-8", "replace")
    for line in text.splitlines():
        if m := re.search(r"DERBY (SCAP .*|READY.*|SCENE \w+|LOADED.*|LOADFAIL.*)", line):
            print(m[1])
    bad = [b for b in BAD if b in text]
    print("SCAP_LOG", "FAIL " + ",".join(bad) if bad else "PASS")
    return 2 if bad else 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make")
    m.add_argument("n", type=int, help="maxSegments, 0 for createSurface(), -1 for none")
    z = sub.add_parser("size")
    z.add_argument("n", type=int, help="maxSegments, 0 for createSurface()")
    s = sub.add_parser("sum")
    s.add_argument("log", type=Path)
    args = p.parse_args()
    if args.cmd == "make":
        make(args.n)
        return 0
    if args.cmd == "size":
        size(args.n)
        return 0
    return summarise(args.log)


if __name__ == "__main__":
    sys.exit(main())
