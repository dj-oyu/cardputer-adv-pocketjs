"""Synthetic initial JS failure -> real owner cleanup -> new runtime restart."""
import argparse
import subprocess
from run_meshbench_app import ROOT, build_harness


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--no-lsan', action='store_true',
                        help='Disable unavailable host LSan only; retain ASan/UBSan and allocation ledger')
    args = parser.parse_args()
    binary, out, env = build_harness(
        'tools/kasane_contract/test_session_eval_restart.c', 'session-eval-restart')
    if args.no_lsan:
        env['ASAN_OPTIONS'] = 'detect_leaks=0:abort_on_error=1'
        print('LSAN_DISABLED host ptrace limitation; ASan/UBSan and exact allocation ledger retained')
    result = subprocess.run([str(binary)], cwd=ROOT, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / 'results.log').write_text(result.stdout)
    print(result.stdout, end='')
    result.check_returncode()


if __name__ == '__main__':
    main()
