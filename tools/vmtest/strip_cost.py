"""What CONFIG_POCKET_VM_STRIP_DEBUG saves per app (host, WSL only).

Builds tools/vmtest/retain_cost.c twice against the vendored quickjs-ng
-m32 (the device's object sizes), once as the firmware builds it and once
with -DCONFIG_POCKET_VM_STRIP_DEBUG=1 (no pc2line tables), compiles each
shipping app's scripts (COMPILE_ONLY, the functions kept) and prints the
charged bytes of both (TLSF lengths, as the device charges them).

  python3 tools/vmtest/strip_cost.py [DIR_WITH_DERBY_CHUNKS]

The figures are the compiled scripts before they run: an app's top-level
body is freed once it has run, the functions it declares stay, so the
difference is what the evaluated app keeps less (docs/vm/strip-debug.md).
Host measurements of the device's allocation sizes, not device ones.
"""
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compile_peak  # noqa: E402

ROOT = compile_peak.ROOT
QJS = compile_peak.QJS
DERBY = ["derby_watch.js", "derby_prog.js", "derby_view.js", "derby_scene.js", "derby_play.js", "derby_demo.js"]
APPS = {
    "hello": ["apps/hello/main.js"],
    "kasane demo": ["apps/kasane/demo.js"],
    "MEGADEMO": ["apps/kasane/proc_megademo.js"],
    "news zoom": ["apps/kasane/proc_news_zoom.js"],
    "grid fold": ["apps/kasane/grid_fold.js"],
    "grid lab": ["apps/kasane/grid_lab.js"],
    "imucal": ["apps/imucal/imucal.js"],
    "pet": ["apps/pet/pet.js"],
    "companion": ["apps/companion/companion.js"],
    "deskclock": ["apps/deskclock/deskclock.js"],
    "player": ["apps/player/player.js"],
    "stress": ["apps/stress/stress.js"],
    "LCD CATCH": ["apps/lcdcatch/lcd_catch.js"],
    "BIG WAVE": ["apps/bigwave/big_wave.js"],
    "keytest": ["apps/keytest/keytest.js"],
}


def build(tag, extra):
    flags = subprocess.run(['bash', 'tools/vmtest/m32_sysroot.sh'], cwd=ROOT, check=True,
                           capture_output=True, text=True).stdout.split()
    out = ROOT / '.cache/strip_cost' / tag
    (out / 'include').mkdir(parents=True, exist_ok=True)
    (out / 'include/sdkconfig.h').write_text('/* host */\n')
    cflags = ['-std=gnu11', '-O1', '-g', '-fno-pie', *flags, *extra, '-DQUICKJS_NG_BUILD', '-D_GNU_SOURCE',
              '-I', str(out / 'include'), '-I', str(QJS), '-I', str(ROOT / 'components/pocketjs_guest/include')]
    objs = []
    for name in ('dtoa', 'libregexp', 'libunicode', 'quickjs', 'quickjs-libc', 'quickjs-vm'):
        o = out / f'{name}.o'
        src = QJS / f'{name}.c'
        if not o.exists() or o.stat().st_mtime < src.stat().st_mtime:
            subprocess.run(['gcc', '-c', '-w', *cflags, str(src), '-o', str(o)], cwd=ROOT, check=True)
        objs.append(str(o))
    binary = out / 'retain_cost'
    subprocess.run(['gcc', *cflags, '-no-pie', '-Wall', str(ROOT / 'tools/vmtest/retain_cost.c'), *objs,
                    '-lm', '-lpthread', '-o', str(binary)], cwd=ROOT, check=True)
    return binary


def charged(binary, files):
    r = subprocess.run([str(binary), 'src', *files], cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"{files}: {r.stderr.strip()}")
    return int(re.search(r'charged (\d+)', r.stdout).group(1)), int(re.search(r'pc2line \d+ \((\d+) B\)', r.stdout).group(1))


def main():
    off, on = build('off', []), build('on', ['-DCONFIG_POCKET_VM_STRIP_DEBUG=1'])
    derby = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'apps/derby'
    apps = dict(APPS, **{"DERBY WATCH": [str(derby / c) for c in DERBY]})
    print(f"{'app':<14} {'source B':>9} {'kept B':>8} {'stripped B':>10} {'saved B':>8} {'pc2line B':>9}")
    for name, fs in apps.items():
        src = sum((ROOT / f).stat().st_size for f in fs)
        a, p = charged(off, fs)
        b, _ = charged(on, fs)
        print(f"{name:<14} {src:9d} {a:8d} {b:10d} {a - b:8d} {p:9d}")


if __name__ == '__main__':
    main()
