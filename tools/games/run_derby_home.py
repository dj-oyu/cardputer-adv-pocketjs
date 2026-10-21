"""Complete HOME game matrix; the same source-only Node checks run once.

No persisted success receipt is read. Every invocation generates and executes
all normal ASan, diagnostic, stage-measure and device-sized m32 checks.
"""
from concurrent.futures import ProcessPoolExecutor
from contextlib import redirect_stdout, redirect_stderr
from multiprocessing import get_context
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


def _single_mode(name, options, validation, out):
    began = time.monotonic()
    print(f'DERBY_MODE start {name}', flush=True)
    with (out / (name + '.log')).open('w') as log:
        with redirect_stdout(Tee(sys.stdout, log)), redirect_stderr(Tee(sys.stderr, log)):
            try:
                run_derby.run_profile(run_derby.parse_args(options), validation)
            except BaseException as error:
                traceback.print_exc()
                raise RuntimeError('HOME mode did not complete: '+name) from error
    result = dict(name=name, options=list(options), seconds=round(time.monotonic()-began,3), status='PASS')
    print(f"DERBY_MODE end {name} seconds={result['seconds']}", flush=True)
    return result


def _profile_child(name, options, validation, out):
    began = time.monotonic()
    # Process-local FDs also capture compiler subprocess output. No redirect
    # of a shared Python thread's stdout, or shared mode log, is involved.
    with (Path(out) / (name + '.log')).open('w') as log:
        os.dup2(log.fileno(), 1)
        os.dup2(log.fileno(), 2)
        try:
            run_derby.run_profile(run_derby.parse_args(options), validation)
        except BaseException:
            traceback.print_exc()
            raise
        finally:
            sys.stdout.flush()
            sys.stderr.flush()
    return dict(name=name, options=list(options), seconds=round(time.monotonic()-began,3), status='PASS')


def _parallel_modes(modes, validation, out, worker=None):
    if len(modes) != 2 or any(len(run_derby.run_matrix(run_derby.parse_args(args))) != 1 for _,args in modes):
        raise ValueError('only two single-case profiles may share the two-process budget')
    results, failure = [], None
    with ProcessPoolExecutor(max_workers=2, mp_context=get_context('spawn')) as pool:
        futures = []
        for name, options in modes:
            print(f'DERBY_MODE start {name}', flush=True)
            futures.append(pool.submit(worker or _profile_child, name, options, validation, str(out)))
        for (name, _), future in zip(modes, futures):
            try:
                result = future.result()
                if result.get('name') != name or result.get('status') != 'PASS':
                    raise RuntimeError('mode process did not return successful completion')
                results.append(result)
            except BaseException as error:
                failure = failure or error
            log = out / (name + '.log')
            if log.exists():
                print(log.read_text(), end='', flush=True)
            if results and results[-1]['name'] == name:
                print(f"DERBY_MODE end {name} seconds={results[-1]['seconds']}", flush=True)
    if failure is not None:
        # Even an unexpected SystemExit(0) is an aborted mode, not completion.
        raise RuntimeError('HOME diagnostic process did not complete') from failure
    return results


def complete(out):
    out.mkdir(parents=True, exist_ok=True)
    # Two complete invocations using the same artifact root cannot delete or
    # replace one another's logs/proof, even across different profile stages.
    with run_derby.workspace_lock(out / '.derby-home.lock'):
        _complete_locked(out)


def _complete_locked(out):
    for name in ('derby-node', *(name for name, _ in MODES)):
        (out / (name + '.log')).unlink(missing_ok=True)
    proof = out / 'derby-complete.json'
    proof.unlink(missing_ok=True)
    with (out / 'derby-node.log').open('w') as log:
        with redirect_stdout(Tee(sys.stdout, log)), redirect_stderr(Tee(sys.stderr, log)):
            validation = run_derby.validate_node_sources()
    results = [_single_mode(*MODES[0], validation, out)]
    # Both diagnostic variants contain one case. Separate processes keep their
    # logs, generated fixtures and binaries isolated while using two cores.
    results.extend(_parallel_modes(MODES[1:3], validation, out))
    for name, options in MODES[3:]:
        results.append(_single_mode(name, options, validation, out))
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
