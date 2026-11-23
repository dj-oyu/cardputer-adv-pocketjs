#!/usr/bin/env python3
"""Rename the project from "pocket" to "kobako", and the fork's engine options to QJS_LEAN.

    python3 tools/rename/kobako_rename.py plan  [--fork components/quickjs-ng/quickjs-ng]
    python3 tools/rename/kobako_rename.py apply [--fork ...]
    python3 tools/rename/kobako_rename.py check [--fork ...]

`plan` (the default) writes nothing: it prints the file moves and how many
times each rule fires, and writes the lines left for a person to review to
.cache/kobako_rename/review.txt. `apply` needs a clean work tree in both
repositories; it moves files with `git mv` and rewrites the rest in place,
then runs `check`. Nothing is committed.

Two repositories change together, because the fork reads the firmware's
Kconfig names (CONFIG_POCKET_VM_*) directly and a rename on one side only
would switch features off without a word (the ROM intrinsics among them):

- this firmware: pocket -> kobako (pocket.* in JS, pocket_* in C,
  CONFIG_POCKET_*), the components and directories named after it, and the
  engine's options to their new names (table below);
- the fork (--fork): POCKET_VM_* and CONFIG_POCKET_VM_* -> QJS_LEAN_*,
  JS_Pocket* -> JS_Lean*, pocket_trig -> lean_trig, and a new header,
  quickjs-lean-config.h, the one place that reads Kconfig: an ESP-IDF build
  sets CONFIG_QJS_LEAN_X, any other build passes -DQJS_LEAN_X=1.

Rules that keep their hands off:

- .js/.mjs/.cjs files get only replacements of the same length. A source's
  length moves the --fail-alloc numbers of the OOM corpus and an app's
  evaluation peak; pocket/Pocket/POCKET and kobako/Kobako/KOBAKO are all six
  bytes, and every changed .js file is checked for it.
- Not touched: third_party/, measurement logs (docs/**/logs/), upstream
  patches (*.patch), the repository's own name (cardputer-adv-pocketjs, in
  URLs) and this tool.
- "PocketJS"/"pocketjs" as words (not identifiers) can mean the third-party
  PocketJS project: they are left as they are and listed for review.

After it: hot/cold profiles name files and functions, so retrain
(tools/qjs_hotcold/train.sh, also TARGET=native); delete each build_*
directory's sdkconfig (the Kconfig names changed); rename the GitHub
repository if wanted (the URLs here still name the old one).
"""
import argparse, os, re, subprocess, sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
SELF = os.path.relpath(HERE, ROOT).replace(os.sep, '/') + '/'

# Where a macro's name starts: after anything but an identifier character, or
# right after -D (-DPOCKET_VM_X=1: \b does not see a boundary between D and P).
START = r'(?:(?<=-D)|(?<![A-Za-z0-9_]))'

# --- the engine's options --------------------------------------------------
# Read by the fork (as CONFIG_POCKET_VM_X today): firmware Kconfig X becomes
# QJS_LEAN_X (so CONFIG_QJS_LEAN_X), the fork reads QJS_LEAN_X, and
# quickjs-lean-config.h maps the one to the other.
ENGINE_KCONFIG = ['PROBE', 'RELOC', 'OOMPROBE', 'STRIP_DEBUG', 'ROM_INTRINSICS',
                  'FLATCALLS', 'SEGFRAMES', 'YIELD', 'STRIP_FN_SOURCE']
# The fork's internal switches (never Kconfig): POCKET_VM_X -> QJS_LEAN_X.
ENGINE_INTERNAL = ['ROM_ATOMS', 'LAZY_BUILTINS', 'LAZY_INTRINSICS', 'GEN_ROM_ATOMS',
                   'GEN_ROM_INTRINSICS', 'IMMORTAL_TEST', 'ROM_INTRINSICS_HOST']
# Firmware-only Kconfig (the engine never reads them): CONFIG_POCKET_VM_X ->
# CONFIG_KOBAKO_<new>. Anything else named POCKET_VM_* in the firmware becomes
# KOBAKO_VM_* and is listed for review.
FIRMWARE_KCONFIG = {
    'SELFTEST': 'VM_SELFTEST',
    'GUEST_CHUNK_KB': 'GUEST_CHUNK_KB',
    'GUEST_ARENA_KB': 'GUEST_ARENA_KB',
    'HELPER_IRAM': 'QJS_HELPER_IRAM',
    'QJS_PGO': 'QJS_PGO',
    'QJS_HOTCOLD': 'QJS_HOTCOLD',
    'TURNPERF': 'TURNPERF',
}
# The fork's API and hooks the firmware calls or supplies.
FORK_API = [
    (r'\bJS_Pocket(\w+)', r'JS_Lean\1'),
    (r'\bJS_DEF_POCKET_', 'JS_DEF_LEAN_'),
    (START + r'POCKET_GC_STATS\b', 'QJS_LEAN_GC_STATS'),
    (START + r'POCKET_TRIG_WEAK\b', 'QJS_LEAN_TRIG_WEAK'),
    (r'pocket_trig', 'lean_trig'),       # also inside tools/check_pocket_trig_table.py
    (r'pocket_gc_', 'lean_gc_'),
    (r'\bpocket_(sin|cos|tan)(_q16)?\b', r'lean_\1\2'),
    (r'\bpocket_(once|data)_h\b', r'lean_\1_h'),
]

SKIP = [re.compile(p) for p in (
    r'^third_party/', r'(^|/)docs/(.*/)?logs/', r'\.patch$', '^' + re.escape(SELF))]
JS_EXT = ('.js', '.mjs', '.cjs')
PROTECT_REPO = 'cardputer-adv-pocketjs'
URL = re.compile(r'https?://[^\s)\]>"\'`]+')
# libpocketjs_*: the upstream's libraries, named by a check that they are not linked
THIRD_PARTY = re.compile(r'pocket-stack|Pocket Vapor|libpocketjs_\w+')
# "PocketJS"/"pocketjs" as a word; pocketjs_guest, include/pocketjs/ and
# cardputer_pocketjs are identifiers and paths, renamed by the rules.
WORD_PJS_ALONE = re.compile(r'(?i)(?<![_A-Za-z0-9])pocketjs(?![_/A-Za-z0-9])')

def kconfig_sub(fork):
    def sub(m):
        cfg, x = m.group(1) or '', m.group(2)
        if fork:
            return 'QJS_LEAN_' + x            # the fork never names CONFIG_ again
        if x in ENGINE_KCONFIG or x in ENGINE_INTERNAL:
            return cfg + 'QJS_LEAN_' + x
        if x in FIRMWARE_KCONFIG:
            return cfg + 'KOBAKO_' + FIRMWARE_KCONFIG[x]
        UNKNOWN_VM.add(x)
        return cfg + 'KOBAKO_VM_' + x
    return sub

UNKNOWN_VM = set()

def build_rules(fork):
    """(name, regex, replacement) in order. Placeholders keep what must not change."""
    r = [('kconfig', re.compile(START + r'(CONFIG_)?POCKET_VM_([A-Z0-9_]+)'), kconfig_sub(fork))]
    r += [('fork-api', re.compile(p), s) for p, s in FORK_API]
    if fork:
        r += [('fork-POCKET:', re.compile(r'\bPOCKET:'), 'QJS_LEAN:'),   # provenance tags in comments
              # the firmware's names, in the fork's comments
              ('fork-firmware-ref', re.compile(r'\bpocketjs_'), 'kobako_'),
              ('fork-firmware-ref', re.compile(r'_pocket'), '_kobako'),
              ('fork-firmware-ref', re.compile(r'\bpocket\.'), 'kobako.'),   # the firmware's JS API
              ('fork-POCKET_', re.compile(START + r'POCKET_'), 'QJS_LEAN_'),
              ('fork-_POCKET_', re.compile(r'_POCKET_'), '_LEAN_'),
              ('fork-Pocket', re.compile(r'Pocket(?!JS)'), 'Lean'),
              ('fork-pocket', re.compile(r'(?<!_)pocket(?!js)'), 'lean')]
    else:
        r += [('pocketjs-code', re.compile(r'\bpocketjs_'), 'kobako_'),
              ('pocketjs-code', re.compile(r'\bpocketjs/'), 'kobako/'),
              ('pocketjs-code', re.compile(r'_pocketjs\b'), '_kobako'),
              ('pocketjs-code', re.compile(r'\bPOCKETJS_'), 'KOBAKO_'),
              ('Pocket', re.compile(r'Pocket(?!JS)'), 'Kobako'),
              ('POCKET', re.compile(r'POCKET(?!JS)'), 'KOBAKO'),
              ('pocket', re.compile(r'pocket(?!js)'), 'kobako')]
    return r

# Same-length rules only, for JavaScript sources.
JS_RULES = [('js-Pocket', re.compile(r'Pocket(?!JS)'), 'Kobako'),
            ('js-POCKET', re.compile(r'POCKET(?!JS)'), 'KOBAKO'),
            ('js-pocket', re.compile(r'pocket(?!js)'), 'kobako')]

def js_engine_names(text, counts):
    """Engine option names in JS (comments of the OOM corpus): the real new
    name is shorter (POCKET_VM_X -> QJS_LEAN_X), so the line gets the missing
    bytes back as trailing spaces and the file keeps its length."""
    rx = re.compile(START + r'(CONFIG_)?POCKET_VM_([A-Z0-9_]+)')
    out = []
    for line in text.split('\n'):
        new = line
        def sub(m):
            if m.group(2) in ENGINE_KCONFIG or m.group(2) in ENGINE_INTERNAL:
                counts['js-engine-name'] += 1
                return 'QJS_LEAN_' + m.group(2)
            return m.group(0)
        new = rx.sub(sub, line)
        out.append(new + ' ' * (len(line.encode()) - len(new.encode())))
    return '\n'.join(out)

def git(repo, *args):
    return subprocess.run(['git', '-C', repo] + list(args), check=True,
                          capture_output=True, text=True).stdout

def tracked(repo):
    out = subprocess.run(['git', '-C', repo, 'ls-files', '-z', '-s'], check=True,
                         capture_output=True).stdout.decode('utf-8', 'surrogateescape')
    for rec in out.split('\0'):
        if not rec:
            continue
        meta, path = rec.split('\t', 1)
        if meta.startswith('160000'):
            continue                    # a gitlink: the fork is handled as its own repo
        yield path

def rewrite(text, rules, counts, review, where):
    """Apply rules; the repository name and PocketJS-as-a-word stay put."""
    keep = []
    def stash(m):
        keep.append(m.group(0))
        return '\0%d\0' % (len(keep) - 1)
    # URLs (the third-party PocketJS lives at github.com/pocket-stack/pocketjs)
    # and the names of other people's products stay exactly as written.
    text = URL.sub(stash, text)
    text = THIRD_PARTY.sub(stash, text)
    text = text.replace(PROTECT_REPO, '\0R\0')
    def hold(m):
        ln = text.count('\n', 0, m.start()) + 1
        line = text.split('\n', ln)[ln - 1] if ln <= text.count('\n') + 1 else ''
        review.append(f'{where}:{ln}: {line.strip()[:160]}')
        keep.append(m.group(0))
        return '\0%d\0' % (len(keep) - 1)
    text = WORD_PJS_ALONE.sub(hold, text)
    for name, rx, rep in rules:
        text, n = rx.subn(rep, text)
        if n:
            counts[name] += n
    text = re.sub(r'\0(\d+)\0', lambda m: keep[int(m.group(1))], text)
    return text.replace('\0R\0', PROTECT_REPO)

def new_path(path, fork):
    rules = build_rules(fork)
    parts = []
    for seg in path.split('/'):
        if seg.lower() == 'pocketjs':
            parts.append('kobako')      # include/pocketjs/ (the C includes say "pocketjs/...")
            continue
        for _, rx, rep in rules:
            seg = rx.sub(rep, seg)
        parts.append(seg)
    return '/'.join(parts)

BRIDGE = '''// QJS_LEAN_* options (docs/vm in the kobako firmware: builtin-floor-plan.md,
// rom-intrinsics.md, vm-L2-design.md). A build defines them directly
// (-DQJS_LEAN_X=1); an ESP-IDF build may instead set them in Kconfig, where
// they arrive as CONFIG_QJS_LEAN_X in sdkconfig.h. This header is the one
// place the engine looks at Kconfig names. sdkconfig.h is on the include
// path of every ESP-IDF component but never force-included, and host builds
// have none.
#pragma once

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

{maps}
'''

def bridge_text():
    maps = []
    for x in ENGINE_KCONFIG:
        maps.append(f'#if defined(CONFIG_QJS_LEAN_{x}) && !defined(QJS_LEAN_{x})\n'
                    f'#define QJS_LEAN_{x} 1\n#endif')
    return BRIDGE.replace('{maps}', '\n'.join(maps))

def fork_bridge(fork, apply, notes):
    """Add quickjs-lean-config.h and include it where the options are read."""
    path = os.path.join(fork, 'quickjs-lean-config.h')
    edits = [('quickjs.h', '#include <math.h>\n', '#include <math.h>\n#include "quickjs-lean-config.h"\n'),
             ('quickjs-vmstack.h', '#include "quickjs.h"\n', '#include "quickjs.h"\n#include "quickjs-lean-config.h"\n'),
             ('amalgam.js', 'const lean_once_h = [', 'const lean_once_h = ["quickjs-lean-config.h", ')]
    sdk = re.compile(r'#if defined\(__has_include\)\n#if __has_include\("sdkconfig.h"\)\n#include "sdkconfig.h"\n#endif\n#else\n#include "sdkconfig.h"\n#endif\n')
    notes.append('fork: add quickjs-lean-config.h (maps CONFIG_QJS_LEAN_X to QJS_LEAN_X for '
                 + ', '.join(ENGINE_KCONFIG) + ')')
    for f, old, new in edits:
        notes.append(f'fork: {f}: include/list quickjs-lean-config.h')
    notes.append('fork: quickjs-vmprobe.h: its sdkconfig.h include becomes the bridge include')
    if not apply:
        return
    with open(path, 'w') as fh:
        fh.write(bridge_text())
    git(fork, 'add', 'quickjs-lean-config.h')
    for f, old, new in edits:
        p = os.path.join(fork, f)
        s = open(p).read()
        if s.count(old) != 1:
            sys.exit(f'kobako_rename: {f}: expected one "{old.strip()}" (after the rename)')
        open(p, 'w').write(s.replace(old, new))
    p = os.path.join(fork, 'quickjs-vmprobe.h')
    s = open(p).read()
    s2, n = sdk.subn('#include "quickjs-lean-config.h"\n', s)
    if n != 1:
        sys.exit('kobako_rename: quickjs-vmprobe.h: the sdkconfig.h block was not found')
    open(p, 'w').write(s2)

# main/idf_component.yml names the guest by the upstream's namespace
# (pocket-stack/pocketjs_guest, an override of the registry component). Under
# its new name it is a local component, not the upstream's.
MANIFEST = ('main/idf_component.yml',
            re.compile(r'  pocket-stack/pocketjs_guest:\n    version: "[^"]*"\n    override_path: "\.\./components/pocketjs_guest"\n'),
            '  kobako_guest:\n    path: "../components/kobako_guest"\n')

def run(repo, fork, apply, counts, review, moves, notes, length_errors):
    rules = build_rules(fork)
    paths = [p for p in tracked(repo) if not any(rx.search(p) for rx in SKIP)]
    # moves first, so the contents are rewritten at their final paths
    for p in paths:
        q = new_path(p, fork) if re.search(r'(?i)pocket', p) else p
        if q != p:
            moves.append((repo, p, q))
    if apply:
        for _, p, q in moves:
            if _ != repo:
                continue
            os.makedirs(os.path.dirname(os.path.join(repo, q)) or repo, exist_ok=True)
            git(repo, 'mv', p, q)
    moved = {p: q for r, p, q in moves if r == repo}
    for p in paths:
        q = moved.get(p, p) if apply else p
        full = os.path.join(repo, q)
        try:
            raw = open(full, 'rb').read()
        except FileNotFoundError:
            continue
        if b'\0' in raw:
            continue
        try:
            text = raw.decode('utf-8')
        except UnicodeDecodeError:
            notes.append(f'not UTF-8, skipped: {repo}:{q}')
            continue
        if not re.search(r'(?i)pocket', text):
            continue
        # the firmware's JS is app source and OOM corpus; the fork's is tooling
        is_js = not fork and q.endswith(JS_EXT)
        if not fork and p == MANIFEST[0]:
            out, n = MANIFEST[1].subn(MANIFEST[2], text)
            if n != 1:
                sys.exit(f'kobako_rename: {p}: the pocket-stack/pocketjs_guest entry was not found')
            notes.append(f'{p}: the guest becomes the local component kobako_guest (path:)')
            if apply:
                open(full, 'w', newline='').write(out)
            continue
        src = js_engine_names(text, counts) if is_js else text
        out = rewrite(src, JS_RULES if is_js else rules, counts, review, f'{os.path.basename(repo)}:{q}')
        if is_js:
            if len(out.encode()) != len(raw):
                length_errors.append(q)
            for m in re.finditer(r'CONFIG_KOBAKO_VM_\w+|JS_Kobako\w+|KOBAKO_VM_\w+', out):
                review.append(f'{os.path.basename(repo)}:{q}: same-length name in JS, '
                              f'real new name differs: {m.group(0)}')
        if out != text and apply:
            open(full, 'w', newline='').write(out)

def check(repo, fork_dir):
    """What is left after apply."""
    bad = []
    for repo_, is_fork in ((repo, False), (fork_dir, True)):
        if not repo_:
            continue
        for p in tracked(repo_):
            if any(rx.search(p) for rx in SKIP):
                continue
            full = os.path.join(repo_, p)
            try:
                text = open(full, encoding='utf-8').read()
            except (UnicodeDecodeError, FileNotFoundError, IsADirectoryError):
                continue
            text = THIRD_PARTY.sub('', URL.sub('', text.replace(PROTECT_REPO, '')))
            for ln, line in enumerate(text.split('\n'), 1):
                for m in re.finditer(r'(?i)pocket[A-Za-z0-9_]*', line):
                    if re.fullmatch(r'(?i)pocketjs', m.group(0)):
                        continue        # a word: in review.txt
                    bad.append(f'{os.path.basename(repo_)}:{p}:{ln}: {m.group(0)}')
            for m in re.finditer(r'KOBAKO_(?:VM_)?([A-Z0-9_]+)', text):
                if m.group(1) in ENGINE_KCONFIG or m.group(1) in ENGINE_INTERNAL:
                    bad.append(f'{os.path.basename(repo_)}:{p}: engine option under the firmware name {m.group(0)}')
            if is_fork and p != 'quickjs-lean-config.h':
                for m in re.finditer(r'CONFIG_[A-Z0-9_]*', text):
                    if m.group(0).startswith(('CONFIG_QJS_LEAN', 'CONFIG_POCKET')):
                        bad.append(f'{os.path.basename(repo_)}:{p}: reads Kconfig {m.group(0)}')
    return bad

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('mode', nargs='?', default='plan', choices=['plan', 'apply', 'check'])
    ap.add_argument('--fork', default='components/quickjs-ng/quickjs-ng',
                    help='the QuickJS fork checkout (default: the submodule)')
    args = ap.parse_args()
    fork = os.path.join(ROOT, args.fork) if not os.path.isabs(args.fork) else args.fork
    if args.mode == 'check':
        bad = check(ROOT, fork)
        print('\n'.join(bad[:200]))
        print(f'kobako_rename check: {len(bad)} line(s) left')
        sys.exit(1 if bad else 0)
    apply = args.mode == 'apply'
    if apply:
        for r in (ROOT, fork):
            if git(r, 'status', '--porcelain', '--untracked-files=no').strip():
                sys.exit(f'kobako_rename: {r} has uncommitted changes; commit or set them aside first')
    counts, review, moves, notes, length_errors = Counter(), [], [], [], []
    run(ROOT, False, apply, counts, review, moves, notes, length_errors)
    fcounts = Counter()
    run(fork, True, apply, fcounts, review, moves, notes, length_errors)
    fork_bridge(fork, apply, notes)

    print(f'files moved: firmware {sum(1 for r, *_ in moves if r == ROOT)}, '
          f'fork {sum(1 for r, *_ in moves if r == fork)}')
    for r, p, q in moves[:12]:
        print(f'  {os.path.basename(r)}: {p} -> {q}')
    if len(moves) > 12:
        print(f'  ... and {len(moves) - 12} more')
    print('replacements (firmware):', dict(counts))
    print('replacements (fork):', dict(fcounts))
    if UNKNOWN_VM:
        print('POCKET_VM_* names in no table (became KOBAKO_VM_*):', ', '.join(sorted(UNKNOWN_VM)))
    for n in notes:
        print(n)
    out = os.path.join(ROOT, '.cache', 'kobako_rename')
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, 'review.txt'), 'w') as fh:
        fh.write('\n'.join(review) + '\n')
    print(f'{len(review)} line(s) to review: {os.path.relpath(os.path.join(out, "review.txt"), ROOT)}')
    if length_errors:
        print('JS files whose length would change (refused):', *length_errors, sep='\n  ')
        sys.exit(1)
    if apply:
        bad = check(ROOT, fork)
        print(f'check: {len(bad)} line(s) still name pocket or read Kconfig in the fork')
        for b in bad[:40]:
            print('  ' + b)
        print('next: retrain hot/cold (train.sh, TARGET=native train.sh), delete build_*/sdkconfig, '
              'build, compare idf.py size with the build before')

if __name__ == '__main__':
    main()
