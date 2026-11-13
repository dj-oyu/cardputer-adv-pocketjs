"""Host cleanup boundary tests; optional second checkout is pinned PR #131.

python3 run_extended.py <baseline-checkout> [<pr131-checkout>]
No network, product writes, merge, or device access. Builds in a temporary directory.
"""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
BASE_SESSION = '0adf0d1ca1b088037d0d6438f508f16dcb1a7e4235fb6a1263f4a3841a433ad0'
BASE_HEAP = '1b0b7e8cd3232153831bc362a34c08d7bbe06c00a770f14632ed5942ecd0cc82'
BASE_HEADER = '093c4022967df5685e6b4f5166948b26b2b8963e44075b74bc33f0a12479519e'
PR_STOP = 'abeae399896470ff139c3f992c7a45ed07b0dfd5577955156b361f25291ed639'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def run(root, pr=False):
    session = (root / 'main/app_session.c').read_bytes()
    heap = root / 'main/pocket/shiki_heap.c'
    if digest((root / 'main/pocket/shiki_heap.h').read_bytes()) != BASE_HEADER:
        raise RuntimeError('Unexpected heap header; review the boundary contract first')
    if digest(heap.read_bytes()) != BASE_HEAP:
        raise RuntimeError('Unexpected heap source; review and update the boundary contract first')
    source = session.decode()
    stop = source[source.index('void app_stop(void) {'):
                  source.index('// ------------------------------------------------------ resident suspension')]
    stop = stop.rstrip() + '\n'
    if pr:
        if digest(stop.encode()) != PR_STOP:
            raise RuntimeError('Expected app_stop from PR #131 head 0befe3d4')
        tests = ['test_pr131_boundary.c']
        include = 'pr131-stop.inc'
    else:
        if digest(session) != BASE_SESSION:
            raise RuntimeError('Expected baseline app_session.c; this is not a whole-product acceptance test')
        tests = ['test.c', 'test_reset_matrix.c', 'heap_lifecycle.c']
        include = 'stop.inc'
    print(('PR131' if pr else 'baseline'), 'heap', digest(heap.read_bytes()),
          'stop', digest(stop.encode()), flush=True)
    with tempfile.TemporaryDirectory(prefix='session-cleanup-') as tmp:
        build = Path(tmp)
        (build / include).write_text(stop)
        for test in tests:
            shutil.copy2(HERE / test, build / test)
            for label, flags in [('O2', ['-O2']),
                                 ('san', ['-O1', '-g', '-fsanitize=address,undefined'])]:
                output = build / (Path(test).stem + '-' + label)
                subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', *flags,
                                '-I', str(root / 'main/pocket'), str(build / test), str(heap),
                                '-o', str(output)], check=True)
                subprocess.run([str(output)], check=True,
                               env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})


if __name__ == '__main__':
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    run(Path(sys.argv[1]).resolve())
    if len(sys.argv) == 3:
        run(Path(sys.argv[2]).resolve(), pr=True)
    else:
        print('PR131 comparison not requested; not executed')
