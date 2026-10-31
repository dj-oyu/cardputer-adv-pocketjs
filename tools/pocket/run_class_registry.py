"""Exercise exact production class-owner helper/reset, including the old assert.

Only unrelated promise-reset dependencies are stubbed. This isolates registry
bookkeeping; it does not claim to install every native namespace in QuickJS.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BASELINE = '791c95df6ff90a4e15819bc11c8a2790df77f80a'
BASELINE_FRAGMENT_SHA = 'f8dfa56305913117158e7ab449d422710c41e8afb4395dfcc535d4696c2d520e'
LEGACY_FIXTURE = ROOT / 'tools/pocket/fixtures/class_registry_16.inc'
SCENARIOS = ('reuse', 'history1-instance17', 'history2-model17', 'full-ledger', 'reset-bounded', 'overflow')
LEDGER_FILES = ('main/app_session.c', 'main/pocket/pocket_api.c', 'main/pocket/pocket_api.h',
                'main/pocket/pocket_class_registry.h', 'main/pocket/pocket_kasane.c',
                'main/pocket/pocket_fs.c', 'main/pocket/pocket_storage.c', 'main/pocket/pocket_net.c',
                'main/pocket/pocket_text.c', 'main/pocket/pocket_workspace.c',
                'main/pocket/app_music_view.c', 'main/pocket/pocket_ayatsuri_follow.inc',
                'main/pocket/pocket_ayatsuri_model.inc')
EXPECTED_OWNERS = {
    'main/pocket/pocket_api.c': ('hub_owner', 'token_owner'),
    'main/pocket/pocket_kasane.c': ('tx_owner', 'modal_owner', 'ref_owner', 'template_owner',
                                  'instance_owner', 'ticket_owner', 'image_owner', 'animation_owner',
                                  'schema_class_owner', 'source_cap_owner', 'scene_owner'),
    'main/pocket/pocket_fs.c': ('file_owner',),
    'main/pocket/pocket_storage.c': ('hub_owner',),
    'main/pocket/pocket_net.c': ('lease_owner', 'response_owner'),
    'main/pocket/pocket_text.c': ('session_owner',),
    'main/pocket/pocket_workspace.c': ('ref_owner',),
    'main/pocket/app_music_view.c': ('music_owner',),
    'main/pocket/pocket_ayatsuri_follow.inc': ('pocket_follow_class_owner', 'pocket_drive_class_owner'),
    'main/pocket/pocket_ayatsuri_model.inc': ('pocket_model_class_owner', 'pocket_model_instance_class_owner'),
}


def owner_ledger():
    """Reject both changed known declarations and a new surface outside the list."""
    found = {}
    tokens = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
    for path in (ROOT / 'main').rglob('*'):
        if not path.is_file() or path.suffix not in ('.c', '.h', '.inc'):
            continue
        code = tokens.sub(' ', path.read_text(encoding='utf8'))
        names = []
        for declaration in re.findall(r'\bstatic\s+pocket_class_owner_t\s+([^;]+);', code):
            for item in declaration.split(','):
                match = re.fullmatch(r'\s*(\w+)\s*(?:=\s*\{\s*0\s*\})?\s*', item)
                if not match:
                    raise ValueError('Review changed owner declaration: ' + str(path) + ': ' + item)
                names.append(match.group(1))
        if names:
            found[path.relative_to(ROOT).as_posix()] = sorted(names)
    expected = {path: sorted(names) for path, names in EXPECTED_OWNERS.items()}
    if found != expected or sum(map(len, found.values())) != 24:
        raise ValueError('Production owner ledger changed; update the reviewed regression ledger: ' + json.dumps(found, sort_keys=True))
    return found


def sha(data):
    return hashlib.sha256(data).hexdigest()


def function_end(source, start):
    """Find the matching C brace, ignoring comments and quoted literals."""
    at = source.index('{', start)
    depth, state = 0, 'code'
    while at < len(source):
        c, pair = source[at], source[at:at + 2]
        if state == 'line':
            if c == '\n':
                state = 'code'
        elif state == 'comment':
            if pair == '*/':
                state = 'code'
                at += 1
        elif state in ('"', "'"):
            if c == '\\':
                at += 1
            elif c == state:
                state = 'code'
        elif pair == '//':
            state = 'line'
            at += 1
        elif pair == '/*':
            state = 'comment'
            at += 1
        elif c in ('"', "'"):
            state = c
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if not depth:
                return at + 1
        at += 1
    raise ValueError('Unclosed production function')


def extract(raw):
    source = raw.decode('utf8').replace('\r\n', '\n')
    array = re.search(r'static\s+JSRuntime\s*\*\s*\*\s*class_owners\s*\[[^\]]+\]\s*;', source)
    reset = re.search(r'void\s+pocket_api_reset\s*\(\s*void\s*\)\s*\{', source)
    intrusive = re.search(r'static\s+pocket_class_registry_t\s+\w+\s*(?:=\s*\{[^}]*\})?\s*;', source)
    registry = array or intrusive
    if not registry or not reset or registry.start() >= reset.start():
        raise ValueError('Production registry layout changed: update extraction, never substitute a copied helper')
    defines = '\n'.join(re.findall(r'^#define\s+\w*(?:CLASS|OWNER)\w*[^\n]*', source, re.M))
    include = '#include "pocket_class_registry.h"\n' if intrusive else ''
    fragment = include + defines + '\n' + source[registry.start():reset.start()] + source[reset.start():function_end(source, reset.start())] + '\n'
    if len(re.findall(r'bool\s+pocket_api_class_ready\s*\(', fragment)) != 1:
        raise ValueError('Expected exactly one actual class-ready implementation')
    return fragment.encode(), bool(array)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-source', type=Path, help='Pinned baseline pocket_api.c when worktree Git is unavailable')
    parser.add_argument('--baseline-only', action='store_true')
    parser.add_argument('--no-sanitizers', action='store_true', help='Explicit native compiler fallback; does not claim ASan coverage')
    args = parser.parse_args()
    cache = ROOT / '.cache/class-owner-regression'
    cache.mkdir(parents=True, exist_ok=True)
    result_path = cache / ('baseline-result.json' if args.baseline_only else 'result.json')
    result_path.unlink(missing_ok=True)
    # This exact fragment was compared with Git BASELINE before being vendored.
    # A squash or shallow checkout must not remove the old negative control.
    baseline = (args.baseline_source or LEGACY_FIXTURE).read_bytes()
    if sha(extract(baseline)[0]) != BASELINE_FRAGMENT_SHA:
        raise ValueError('Baseline helper/reset differs from the pinned production commit')
    current_path = ROOT / 'main/pocket/pocket_api.c'
    current = current_path.read_bytes()
    ledger = None if args.baseline_only else owner_ledger()
    dependency_paths = [] if args.baseline_only else [ROOT / name for name in LEDGER_FILES]
    dependency_paths.append(LEGACY_FIXTURE)
    sources = {'baseline': baseline}
    if not args.baseline_only:
        sources['current'] = current
    test = ROOT / 'tools/pocket/test_class_registry.c'
    test_before = test.read_bytes()
    runner_before = Path(__file__).read_bytes()
    qjs = ROOT / 'components/quickjs-ng/quickjs-ng'
    dependency_paths += [p for p in qjs.rglob('*') if p.is_file() and p.suffix in ('.c', '.h', '.inc')]
    dependencies = {str(p.relative_to(ROOT)): sha(p.read_bytes()) for p in dependency_paths}
    flags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fno-omit-frame-pointer']
    if not args.no_sanitizers:
        flags += ['-fsanitize=address,undefined']
    includes = ['-I' + str(qjs), '-I' + str(ROOT / 'main/pocket')]
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
    runs = []
    for version, raw in sources.items():
        folder = cache / version
        folder.mkdir(parents=True, exist_ok=True)
        fragment, legacy = extract(raw)
        (folder / 'class_registry_under_test.inc').write_bytes(fragment)
        binary = folder / ('test-class-registry.exe' if os.name == 'nt' else 'test-class-registry')
        defines = ['-DCLASS_REGISTRY_LEGACY'] if legacy else []
        objects = []
        if not legacy:
            definitions = ['-DQUICKJS_NG_BUILD', '-D_GNU_SOURCE']
            upstream = sorted(p for p in qjs.rglob('*') if p.is_file() and p.suffix in ('.c', '.h', '.inc'))
            compiler_version = subprocess.check_output(['gcc', '--version'], text=True)
            key = sha(json.dumps([flags, definitions, includes, compiler_version,
                                 [(str(p.relative_to(qjs)), sha(p.read_bytes())) for p in upstream]]).encode())
            objdir = cache / 'qjs' / key
            objdir.mkdir(parents=True, exist_ok=True)
            for name in ('dtoa', 'libregexp', 'libunicode', 'quickjs', 'quickjs-vm'):
                obj = objdir / (name + '.o')
                if not obj.exists():
                    subprocess.run(['gcc', *flags, '-w', *definitions, *includes, '-c', str(qjs / (name + '.c')), '-o', str(obj)], cwd=ROOT, check=True)
                objects.append(str(obj))
            defines += ['-DCLASS_REGISTRY_REAL_QJS', *definitions]
        compile_command = ['gcc', *flags, *defines, *includes, '-I' + str(folder), str(test), *objects, '-lm', '-o', str(binary)]
        subprocess.run(compile_command, cwd=ROOT, check=True)
        cases = []
        for scenario in (*SCENARIOS, *(("forget", "real-qjs") if not legacy else ())):
            command = [str(binary), scenario]
            result = subprocess.run(command, cwd=ROOT, env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
            (folder / (scenario + '.log')).write_text(result.stdout)
            expect_abort = legacy and scenario != 'reuse'
            assertion = ('class_owner_count' in result.stdout and 'Assertion' in result.stdout)
            # UCRT abort uses fail-fast status 0xC0000409 on this native host;
            # require the exact production assertion text as well as the code.
            abort_code = result.returncode == -signal.SIGABRT or os.name == 'nt' and result.returncode in (3, 0xC0000409)
            passed = (abort_code and assertion) if expect_abort else (
                result.returncode == 0 and 'CLASS_REGISTRY_PASS scenario=' + scenario in result.stdout)
            if scenario == 'history1-instance17':
                passed = passed and 'MODEL_READY_COMPLETE historical_slots=1' in result.stdout
            cases.append(dict(scenario=scenario, command=command, exit_code=result.returncode,
                              expected_assert_abort=expect_abort, production_assert_observed=assertion, passed=passed,
                              model_completed_before_instance='MODEL_READY_COMPLETE historical_slots=1' in result.stdout,
                              last_registration=next((line for line in reversed(result.stdout.splitlines()) if line.startswith('REGISTER ')), None)))
            print('CLASS_REGISTRY', version, scenario, 'PASS' if passed else 'FAIL', 'exit=' + str(result.returncode), flush=True)
        runs.append(dict(version=version, source_sha256=sha(raw), extracted_sha256=sha(fragment),
                         binary_sha256=sha(binary.read_bytes()), compile_command=compile_command, cases=cases))
    unchanged = test.read_bytes() == test_before and Path(__file__).read_bytes() == runner_before
    if not args.baseline_only:
        unchanged = unchanged and current_path.read_bytes() == current
        unchanged = unchanged and owner_ledger() == ledger
    unchanged = unchanged and dependencies == {str(p.relative_to(ROOT)): sha(p.read_bytes()) for p in dependency_paths}
    proof = dict(kind='HOST REGISTRY UNIT CONTRACT', baseline_origin_commit=BASELINE,
                 baseline_fragment_sha256=BASELINE_FRAGMENT_SHA,
                 baseline_origin='Exact production helper/reset fragment matched Git origin before vendoring; default run requires no Git history',
                 production_helper_and_reset_extracted=True, native_owner_ledger_count=24,
                 production_owner_declarations=ledger,
                 runtime_type='opaque identities for reset/reuse; real QuickJS synthetic 24-class registration/finalizer test for current header; no physical namespace installation claim',
                 sanitizers=not args.no_sanitizers,
                 dependency_sha256=dependencies,
                 passed=unchanged and all(c['passed'] for r in runs for c in r['cases']),
                 sources_unchanged=unchanged, test_sha256=sha(test_before), runner_sha256=sha(runner_before),
                 compiler=subprocess.check_output(['gcc', '--version'], text=True).splitlines()[0], flags=flags, runs=runs)
    result_path.write_text(json.dumps(proof, indent=2) + '\n')
    if not proof['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
