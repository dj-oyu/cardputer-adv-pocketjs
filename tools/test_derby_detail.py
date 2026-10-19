"""Accounting/compile-out checks and mutations of actual host fixture records."""
from pathlib import Path
import argparse,subprocess,tempfile,sys
R=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(R/'tools/games'))
from derby_detail_result import parse_detail
p=argparse.ArgumentParser();p.add_argument('--log',type=Path);args=p.parse_args()
with tempfile.TemporaryDirectory() as t:
    out=Path(t);(out/'freertos').mkdir()
    (out/'freertos/FreeRTOS.h').write_text('#pragma once\nint xPortInIsrContext(void);\n')
    (out/'freertos/task.h').write_text('#pragma once\ntypedef void *TaskHandle_t;\nTaskHandle_t xTaskGetCurrentTaskHandle(void);\n')
    for flags in (['-O1','-fsanitize=address,undefined'],['-O2']):
        exe=out/'detail'
        subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-DPOCKET_DERBY_MEASURE','-DPOCKET_DERBY_DETAIL_MEASURE','-DESP_PLATFORM',*flags,'-I',str(out),'-I',str(R/'main/pocket'),str(R/'tools/test_derby_detail.c'),str(R/'main/pocket/derby_measure.c'),'-o',str(exe)],check=True)
        subprocess.run([exe],check=True)
    off=out/'off.c';off.write_text('#include "derby_detail.h"\nvoid f(void){DD_OPEN(a,DD_VM_RUN);DD_ADD(DD_VM_ZERO_BYTES,unknown++);DD_CLOSE(a);DD_SCOPE(DD_RENDER);}\n')
    obj=out/'off.o'
    subprocess.run(['gcc','-O0','-fno-pie','-Wall','-Wextra','-Werror','-I',str(R/'main/pocket'),'-c',str(off),'-o',str(obj)],check=True)
    assert not subprocess.check_output(['nm','-u',str(obj)],text=True).strip()
    print('PASS detail OFF does not evaluate arguments or reference instrumentation at O0')
if args.log:
    text=args.log.read_text();parse_detail(text,expected=True)
    lines=text.splitlines(keepends=True)
    stage=next(i for i,l in enumerate(lines) if l.startswith('DERBY_DETAIL v='))
    end=next(i for i,l in enumerate(lines) if l.startswith('DERBY_DETAIL_END'))
    summary=next(i for i,l in enumerate(lines) if l.startswith('DERBY_FIXED v='))
    cases={
        'missing stage':lines[:stage]+lines[stage+1:],
        'duplicate stage':lines[:stage]+[lines[stage]]+lines[stage:],
        'reorder stage':lines[:stage]+[lines[stage+1],lines[stage]]+lines[stage+2:],
        'unknown field':lines[:stage]+[lines[stage].rstrip()+' extra=0\n']+lines[stage+1:],
        'duplicate field':lines[:stage]+[lines[stage].rstrip()+' v=1\n']+lines[stage+1:],
        'version':lines[:stage]+[lines[stage].replace('v=1','v=2')]+lines[stage+1:],
        'negative':lines[:stage]+[lines[stage].replace('stage=0','stage=-1')]+lines[stage+1:],
        'mixed rep':lines[:stage]+[lines[stage].replace('rep=1','rep=2')]+lines[stage+1:],
        'fault':lines[:end]+[lines[end].replace('faults=0','faults=1')]+lines[end+1:],
        'lost end':lines[:end]+lines[end+1:],
        'lost summary':lines[:summary]+lines[summary+1:],
        'unknown marker':lines+['DERBY_DETAIL_FUTURE v=1\n'],
        'partial tail':[text[:-1]],
        'lost STOP':[text.replace('APP_STOPPED\n','')],
        'lost HOME':[text.replace('HOME_READY\n','')],
    }
    for name,mutated in cases.items():
        try:parse_detail(''.join(mutated),expected=True)
        except ValueError:pass
        else:raise AssertionError(name+' accepted')
    try:parse_detail(text,expected=False)
    except ValueError:pass
    else:raise AssertionError('OFF accepted detailed stream')
    print('PASS actual detail stream; 16 missing/schema/order/quality/OFF failure cases rejected')
