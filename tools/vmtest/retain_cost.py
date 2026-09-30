"""What DERBY WATCH's compiled chunks keep in the guest heap, source-compiled
versus precompiled bytecode with and without line tables (host, WSL only).

  python3 tools/vmtest/retain_cost.py [DIR]
      the entry and the four chunks read while it evaluates (demo excluded),
      from apps/derby or from DIR (e.g. a `git show` export: WSL's git may not
      read a worktree's gitdir, CLAUDE.md); prints the three modes of
      tools/vmtest/retain_cost.c.
  python3 tools/vmtest/retain_cost.py --parts|--side [DIR]
      (--side: the side view's course() and plan strings instead)
      stage 3 of the panning cameras (the wip commit on vm/pan-camera, docs/apps/
      derby-pan-memory.md): each of its top-level functions and plan strings
      removed in turn from those chunks (apps/derby or DIR), and what the
      compiled chunks lose (the part's own cost).

Built like tools/vmtest/compile_peak.py (-m32, the device's object sizes).
Charged bytes are TLSF lengths as the device charges them; host figures.
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compile_peak  # noqa: E402

ROOT = compile_peak.ROOT
CHUNKS = ["derby_watch.js", "derby_prog.js", "derby_view.js", "derby_scene.js", "derby_play.js"]


def build():
    flags = subprocess.run(['bash', 'tools/vmtest/m32_sysroot.sh'], cwd=ROOT, check=True,
                           capture_output=True, text=True).stdout.split()
    compile_peak.build()          # the quickjs objects, -m32
    out = compile_peak.OUT
    objs = [str(out / 'obj' / f'{n}.o') for n in
            ('dtoa', 'libregexp', 'libunicode', 'quickjs', 'quickjs-libc', 'quickjs-vm')]
    binary = out / 'retain_cost'
    subprocess.run(['gcc', '-std=gnu11', '-O1', '-g', '-fno-pie', *flags, '-DQUICKJS_NG_BUILD', '-D_GNU_SOURCE',
                    '-I', str(out / 'include'), '-I', str(compile_peak.QJS),
                    '-I', str(ROOT / 'components/pocketjs_guest/include'), '-no-pie', '-Wall',
                    str(ROOT / 'tools/vmtest/retain_cost.c'), *objs, '-lm', '-lpthread', '-o', str(binary)],
                   cwd=ROOT, check=True)
    return binary


def files(src, d):
    for c in CHUNKS:
        (d / c).write_text(((Path(src) if src else ROOT / 'apps/derby') / c).read_text(encoding='utf-8'),
                           encoding='utf-8')
    return [str(d / c) for c in CHUNKS]


def charged(binary, mode, paths):
    r = subprocess.run([str(binary), mode, *paths], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"retain_cost {mode} failed: {r.stderr.strip()}")
    return int(re.search(r'charged (\d+)', r.stdout).group(1)), r.stdout.strip()


# Stage 3's parts: (chunk, how to find the part). A function is cut from its
# 'function NAME(' (or 'const NAME =') line to the next top-level line.
SIDE = [("derby_view.js", "function course("), ("derby_prog.js", "  rail: '"), ("derby_prog.js", "  turf: '"),
        ("derby_prog.js", "  stands: '"), ("derby_prog.js", "  crowd: '"), ("derby_prog.js", "  gate: '"),
        ("derby_prog.js", "  pole: '")]
PARTS = [("derby_scene.js", "function pan("), ("derby_scene.js", "function ser("), ("derby_scene.js", "function hl("),
         ("derby_scene.js", "function pj("), ("derby_scene.js", "function lim("), ("derby_scene.js", "function inr("),
         ("derby_view.js", "function wide("), ("derby_view.js", "const rin ="), ("derby_view.js", "function feed("),
         ("derby_prog.js", "  prail: '"), ("derby_prog.js", "  pt: '"), ("derby_prog.js", "  pc: '"),
         ("derby_prog.js", "  hl: '")]


def cut(text, start):
    """Remove a T-table line, a one-line function, a function up to its
    closing '}' line, or a const up to its ';' line."""
    i = text.index(start)
    if start.startswith('  '):
        return text[:i] + text[text.index('\n', i) + 1:]
    line_end = text.index('\n', i)
    if text[i:line_end].rstrip().endswith('}'):
        return text[:i] + text[line_end + 1:]
    end = text.index('\n}\n', i) + 3 if start.startswith('function') else text.index(';\n', i) + 2
    return text[:i] + text[end:]


def main():
    binary = build()
    with tempfile.TemporaryDirectory() as t:
        d = Path(t)
        if sys.argv[1:2] in (['--parts'], ['--side']):
            src = sys.argv[2] if len(sys.argv) > 2 else None
            paths = files(src, d)
            whole, _ = charged(binary, 'src', paths)
            print(f"stage 3 chunks compiled: {whole} B")
            for chunk, start in (PARTS if sys.argv[1] == '--parts' else SIDE):
                files(src, d)
                p = d / chunk
                p.write_text(cut(p.read_text(encoding='utf-8'), start), encoding='utf-8')
                n, _ = charged(binary, 'src', paths)
                print(f"  {start.strip():<18} {chunk:<16} {whole - n:6d} B")
            return
        paths = files(sys.argv[1] if len(sys.argv) > 1 else None, d)
        for mode in ('src', 'bc', 'strip'):
            print(charged(binary, mode, paths)[1])


if __name__ == '__main__':
    main()
