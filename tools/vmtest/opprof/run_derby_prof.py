"""DERBY WATCH's frame() under the bytecode profiler (opprof_impl.c).

    wsl bash -lc 'cd <repo> && python3 tools/vmtest/opprof/run_derby_prof.py [--only NAME]'

Builds tools/games/test_derby_host.c for i386 with the device's 8-byte
JSValue (tools/vmtest/m32_sysroot.sh) against a profiling copy of quickjs.c
(patch_quickjs.py), then runs the scripted game once per configuration in
CONFIGS. A wrapper put around globalThis.frame by DERBY_JS turns the
profiler on for the player's first race (not a demo, not the replay):
one window per frame whose leader (before the step) is inside the
configuration's band, tagged 'at<m>' for the first frame at or past each
mark and 'win' for the others. apps/derby is not changed: the harness runs
run_derby.py's lowered copy, the course forced as there, and the camera is
held with DERBY_EACH (cam=8: WIDE 2) and DERBY_JS (CAMS[8][5]: WIDE 2's w on
the oval). The harness's own wrappers (prelude.js) are hidden from the
counts (OPPROF_HIDE; the device API they call stays the app's, OPPROF_KEEP).
Also runs bench_ops.js (the device bench's bodies) for the prices. Output:
.cache/opprof/<name>.txt (opprof), <name>.csv (the harness's per-tick draws,
VM steps and segments) and bench.txt; analyze.py reads them
(docs/apps/derby-wide2-profile.md).
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/games"))
import run_derby  # noqa: E402  (lower, force_course, rom_table, kasane_sources, EXTRA, QJS)

CACHE = ROOT / ".cache/opprof"
HERE = Path(__file__).resolve().parent

# name: (course, tier, DERBY_EACH, extra DERBY_JS, band lo, hi, marks)
CONFIGS = {
    "oval_w2_mid_w100": ("oval", 1, "cam=8;man=9;", "", 273, 650, (300, 400, 500, 600)),
    "oval_w2_mid_w3": ("oval", 1, "cam=8;man=9;", "CAMS[8][5]=-3;", 273, 650, (300, 400, 500, 600)),
    "oval_w2_heavy_w100": ("oval", 2, "cam=8;man=9;", "", 273, 650, (300, 400, 500, 600)),
    "straight_w2_heavy": ("straight", 2, "cam=8;man=9;", "", 300, 620, (300, 400, 500, 600)),
    "straight_side_mid": ("straight", 1, "cam=0;man=9;", "wide=()=>0;", 300, 620, (300, 400, 500, 600)),
}

# The device API behind the harness's wrappers (prelude.js): a call into one
# of these from a hidden wrapper is the app's call (opprof_impl.c).
KEEP = ("draw,commit,beginFrame,patch,replace,register,unregister,setRect,setVisible,"
        "setClip,setText,setColor,image,rect,text")

WRAP = """(()=>{const F=frame,T=[%s];let k=0,n=0;
globalThis.frame=function(b){const L=scene==='race'&&!dm&&!replay&&rs?Math.max.apply(null,rs.x):-1;let g=null;
if(L>=%d&&L<%d){g='win';if(k<T.length&&L>=T[k]){g='at'+T[k];while(k<T.length&&L>=T[k])++k;}}
if(g)__opprof(1);try{return F(b);}finally{if(g)__opprof(0,g+' tick='+n+' L='+L.toFixed(1)+' cm='+cm+' pan='+(pc?1:0));++n;}};})()"""


def m32_flags() -> list[str]:
    env = dict(os.environ, M32_SYSROOT=str(CACHE / "m32sys"))
    return subprocess.run(["bash", "tools/vmtest/m32_sysroot.sh"], cwd=ROOT, check=True, env=env,
                          capture_output=True, text=True).stdout.split()


def build(flags: list[str]) -> Path:
    obj_dir = CACHE / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run(["python3", str(HERE / "patch_quickjs.py"), str(obj_dir)], check=True)
    table = run_derby.rom_table(obj_dir)
    subprocess.run(["python3", "tools/make_font.py", str(obj_dir)], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
    data = (ROOT / "apps/pet/assets/pets-compact.bin").read_bytes()
    (obj_dir / "kasane_pet_test_data.h").write_text(
        "static const uint8_t pet_test_data[] = {" + ",".join(map(str, data)) + "};\n")
    api = (ROOT / "main/pocket/pocket_api.c").read_text()
    a = api.index("// ----------------------------------------------------------- subscriptions")
    b = api.index("// ------------------------------------------------------- async completions", a)
    (obj_dir / "pocket_sub_impl.inc").write_text(api[a:b])
    defs = ["-DQUICKJS_NG_BUILD", "-D_GNU_SOURCE"]
    qjs = run_derby.QJS
    objects = []
    for name, src in (("dtoa", ROOT / qjs / "dtoa.c"), ("libregexp", ROOT / qjs / "libregexp.c"),
                      ("libunicode", ROOT / qjs / "libunicode.c"), ("quickjs", obj_dir / "quickjs.c"),
                      ("quickjs-vm", ROOT / qjs / "quickjs-vm.c")):
        obj = obj_dir / f"{name}.o"
        subprocess.run(["gcc", "-std=gnu11", "-c", "-O1", "-g", "-w", *flags, *defs, "-I", qjs,
                        "-I", "components/pocketjs_guest/include", str(src), "-o", str(obj)], cwd=ROOT, check=True)
        objects.append(str(obj))
    binary = obj_dir / "test-derby-opprof"
    includes = [f"-I{d}" for d in (qjs, "tools/hostshim", "main", "main/pocket", "main/ui",
                                   "main/ui/kasane", "main/text", "main/hal", str(obj_dir))]
    subprocess.run(["gcc", "-std=gnu11", "-O1", "-g", *flags, "-DKSN_PROC_POINTS_PIE_MODEL",
                    "-Wall", "-Wextra", "-Werror", "-fno-omit-frame-pointer", *includes,
                    "tools/games/test_derby_host.c", *run_derby.kasane_sources(), *run_derby.EXTRA, str(table), *objects,
                    "-Wl,--wrap=calloc", "-Wl,--wrap=free", "-Wl,--wrap=JS_NewContext", "-lm", "-o", str(binary)],
                   cwd=ROOT, check=True)
    return binary


def bench(flags: list[str]) -> None:
    """bench_ops.js on the same profiling objects: .cache/opprof/bench.txt."""
    obj_dir = CACHE / "obj"
    binary = obj_dir / "bench-ops"
    objects = [str(obj_dir / f"{n}.o") for n in ("dtoa", "libregexp", "libunicode", "quickjs", "quickjs-vm")]
    subprocess.run(["gcc", "-std=gnu11", "-O1", "-g", *flags, "-I", run_derby.QJS, str(HERE / "bench_ops.c"), *objects,
                    "-Wl,--wrap=JS_NewContext", "-lm", "-o", str(binary)], cwd=ROOT, check=True)
    out = CACHE / "bench.txt"
    out.unlink(missing_ok=True)
    subprocess.run([str(binary), str(HERE / "bench_ops.js")], cwd=ROOT, check=True, env=dict(os.environ, OPPROF_OUT=str(out)))
    print(f"bench: {out.read_text().count('FRAME ')} windows", flush=True)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--only", action="append", help="run only these configurations")
    ap.add_argument("--no-build", action="store_true")
    args = ap.parse_args()
    flags = m32_flags()
    binary = CACHE / "obj/test-derby-opprof" if args.no_build else build(flags)
    bench(flags)
    apps = {c: run_derby.force_course(run_derby.lower(ROOT / "apps/derby", CACHE / f"lowered_{c}/apps/derby"), c)
            for c in ("oval", "straight")}
    for name, (course, tier, each, js, lo, hi, marks) in CONFIGS.items():
        if args.only and name not in args.only:
            continue
        out, csv = CACHE / f"{name}.txt", CACHE / f"{name}.csv"
        out.unlink(missing_ok=True)
        env = dict(os.environ, DERBY_APP_DIR=str(apps[course]), DERBY_TIER=str(tier), DERBY_NOCAM="1",
                   DERBY_EACH=each, DERBY_JS=js + WRAP % (",".join(map(str, marks)), lo, hi),
                   DERBY_CSV=str(csv), OPPROF_OUT=str(out), OPPROF_HIDE="prelude.js,frame.js,env.js", OPPROF_KEEP=KEEP)
        p = subprocess.run([str(binary)], cwd=ROOT, env=env, capture_output=True, text=True)
        tail = [line for line in p.stdout.splitlines() if line.startswith(("FAIL", "DERBY_HOST", "finish"))]
        frames = out.read_text().count("FRAME ") if out.exists() else 0
        print(f"{name}: exit {p.returncode}, {frames} frames profiled; {' | '.join(tail)}", flush=True)
        if not frames:
            print(p.stdout[-3000:], p.stderr[-3000:])


if __name__ == "__main__":
    main()
