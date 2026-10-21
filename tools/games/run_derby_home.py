"""Complete HOME game matrix; the same source-only Node checks run once.

No persisted success receipt is read. Every invocation generates and executes
all normal ASan, diagnostic, stage-measure and device-sized m32 checks.
"""
from contextlib import redirect_stdout, redirect_stderr
from pathlib import Path
import argparse
import json
import os
import subprocess
import sys
import time
import traceback

import run_derby

MODES = (
    ('derby-asan', ()),
    ('derby-telemetry', ('--telemetry', '--heap-limit', '524288')),
    ('derby-measure', ('--measure', '--heap-limit', '524288')),
    ('derby-m32', ('--m32',)),
    ('derby-m32-telemetry', ('--m32', '--telemetry', '--heap-limit', '163840')),
)


class Tee:
    def __init__(self, stream, log):
        self.stream, self.log = stream, log

    def write(self, text):
        self.stream.write(text)
        self.log.write(text)
        return len(text)

    def flush(self):
        self.stream.flush()
        self.log.flush()


def complete(out):
    out.mkdir(parents=True, exist_ok=True)
    # Old outputs cannot stand in for an unexecuted mode after an early failure.
    for name in ('derby-node', *(name for name, _ in MODES)):
        (out / (name + '.log')).unlink(missing_ok=True)
    proof = out / 'derby-complete.json'
    proof.unlink(missing_ok=True)
    with (out / 'derby-node.log').open('w') as log:
        with redirect_stdout(Tee(sys.stdout, log)), redirect_stderr(Tee(sys.stderr, log)):
            validation = run_derby.validate_node_sources()
    results = []
    for name, options in MODES:
        began = time.monotonic()
        print(f'DERBY_MODE start {name}', flush=True)
        with (out / (name + '.log')).open('w') as log:
            with redirect_stdout(Tee(sys.stdout, log)), redirect_stderr(Tee(sys.stderr, log)):
                try:
                    run_derby.run_profile(run_derby.parse_args(options), validation)
                except BaseException:
                    traceback.print_exc()
                    raise
        seconds = round(time.monotonic() - began, 3)
        results.append(dict(name=name, options=list(options), seconds=seconds, status='PASS'))
        print(f'DERBY_MODE end {name} seconds={seconds}', flush=True)
    run_derby.require_node_inputs(validation)
    head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=run_derby.ROOT, text=True).strip()
    if os.environ.get('SOURCE_HEAD', head) != head:
        raise SystemExit('HOME checkout differs from expected source head')
    proof.write_text(json.dumps(dict(source_head=head, node_inputs_sha256=validation,
                                    node_checks=list(run_derby.NODE_TESTS), modes=results), indent=2)+'\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=run_derby.ROOT / 'artifacts')
    complete(parser.parse_args().out)
