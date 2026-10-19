"""Host proof for the owner-task controller and compile-out contract."""
from pathlib import Path
import subprocess
import tempfile
import os
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent/'games'))
from derby_fixed_result import parse_trial
ROOT=Path(__file__).resolve().parents[1]

def main():
    with tempfile.TemporaryDirectory() as tmp:
        out=Path(tmp);binary=out/'test'
        subprocess.run(['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-DPOCKET_DERBY_MEASURE','-DPOCKET_DERBY_FIXED_WORKLOAD',
            '-I',str(ROOT/'main/pocket'),str(ROOT/'tools/test_derby_fixed_workload.c'),
            str(ROOT/'main/pocket/derby_fixed_workload.c'),str(ROOT/'main/pocket/derby_measure.c'),'-o',str(binary)],check=True)
        log=out/'trial.log'
        subprocess.run([str(binary)],check=True,env=dict(os.environ,DF_TEST_OUTPUT=str(log)))
        raw=log.read_bytes();assert parse_trial(raw,transport_complete=True)['all_repetitions_valid']
        for bad in (raw[:-1],raw.replace(b'rep=2 seq=2',b'rep=3 seq=3'),
                    raw.replace(b'quality=0',b'quality=1',1),raw.replace(b'finish=5DC86E18',b'finish=00000001',1),
                    raw.replace(b'contexts=120',b'contexts=119',1),raw+raw,
                    b'\n'.join(raw.splitlines()[1:])+b'\n',raw.replace(b'reps=3 valid=1',b'reps=2 valid=0')):
            try:parse_trial(bad,transport_complete=True)
            except ValueError:pass
            else:raise AssertionError('invalid/partial/gap/duplicate trial accepted')
        try:parse_trial(raw,transport_complete=False)
        except ValueError:pass
        else:raise AssertionError('unproved transport accepted')
        print('PASS parser: a missing, invalid or partial repetition makes the whole trial indeterminate')
        source=out/'off.c';source.write_text('#include "derby_fixed_workload.h"\nextern int side_effect(void);\nvoid probe(void){DF_COUNT(DF_VIEW);DF_ADD(DF_ROWS,side_effect());DF_OWNER_BEGIN(side_effect());DF_WORK_END(side_effect());DF_PACE_BEGIN(DF_PACE_CAP,side_effect(),side_effect());DF_PACE_END(side_effect());DF_OWNER_END(side_effect(),side_effect());}\n')
        obj=out/'off.o'
        subprocess.run(['gcc','-std=gnu11','-O0','-Wall','-Wextra','-Werror','-I',str(ROOT/'main/pocket'),'-c',str(source),'-o',str(obj)],check=True)
        assert not subprocess.check_output(['nm','-u',str(obj)],text=True).strip()
        print('PASS O0 OFF: no diagnostic references or evaluated clock arguments')

if __name__=='__main__':main()
