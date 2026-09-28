"""Build and run the host reference for the procedural limits scene.

Writes .cache/kasane_proc_limits/expected.rgb565 (little-endian RGB565) from
apps/kasane/proc_limits_probe.js: VM single-step and scalar points, checked
against the real pocket_proc.c adapter on host. Like
run_pocket_proc_limits_qjs.py, QuickJS objects are cached and the adapter is
built under ASan/UBSan where the host supports it.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
QJS = ROOT / 'components/quickjs-ng/quickjs-ng'
OUT = ROOT / '.cache/kasane_proc_limits'
DEFS = ['-DQUICKJS_NG_BUILD', '-D_GNU_SOURCE']


def main() -> None:
    cc = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    if not cc:
        raise SystemExit('No host C compiler found')
    OUT.mkdir(parents=True, exist_ok=True)
    sanitize = [] if sys.platform == 'win32' else ['-fsanitize=address,undefined',
                                                   '-fno-omit-frame-pointer']
    objects = []
    for name in ('dtoa', 'libregexp', 'libunicode', 'quickjs', 'quickjs-vm'):
        src = QJS / f'{name}.c'
        obj = OUT / f'{name}.o'
        if not obj.exists() or src.stat().st_mtime > obj.stat().st_mtime:
            subprocess.run([cc, '-std=gnu11', '-c', '-O1', '-g', '-w', *DEFS, '-I', str(QJS),
                            str(src), '-o', str(obj)], cwd=ROOT, check=True)
        objects.append(str(obj))
    kasane = ROOT / 'main/ui/kasane'
    sources = [kasane / n for n in (
        'ksn_procedural.c', 'ksn_proc_analysis.c', 'ksn_proc_plan.c',
        'ksn_proc_plan_points.c', 'ksn_proc_points.c', 'ksn_proc_points_dispatch.c',
        'ksn_proc_points_pie.c')]
    sources += [ROOT / 'main/pocket/pocket_proc.c',
                ROOT / 'tools/kasane_contract/test_proc_limits_scene.c']
    binary = OUT / 'test_proc_limits_scene'
    subprocess.run([cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitize,
                    *DEFS, '-DKSN_PROC_HOST_TEST', '-I', str(QJS), '-I', str(ROOT / 'main'),
                    '-I', str(ROOT / 'main/pocket'), '-I', str(kasane),
                    '-I', str(ROOT / 'tools/kasane_contract'), '-I', str(ROOT / 'tools/hostshim'),
                    *(str(s) for s in sources), *objects, '-lm', '-o', str(binary)],
                   cwd=ROOT, check=True)
    env = os.environ.copy()
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=1:abort_on_error=1')
    env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
    subprocess.run([str(binary), str(ROOT / 'apps/kasane/proc_limits_probe.js'),
                    str(OUT / 'expected.rgb565')], cwd=ROOT, env=env, check=True)


if __name__ == '__main__':
    main()
