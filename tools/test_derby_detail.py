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
    for detail in (False,True):
      for flags in (['-O1','-fsanitize=address,undefined'],['-O2']):
        exe=out/'output'
        subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-DPOCKET_DERBY_MEASURE','-DPOCKET_DERBY_FIXED_WORKLOAD',*(['-DPOCKET_DERBY_DETAIL_MEASURE'] if detail else []),*flags,'-I',str(R/'main/pocket'),str(R/'tools/test_derby_detail_output.c'),str(R/'main/pocket/derby_fixed_workload.c'),str(R/'main/pocket/derby_measure.c'),'-o',str(exe)],check=True)
        subprocess.run([exe],check=True)
    off=out/'off.c';off.write_text('#include "derby_detail.h"\nvoid f(void){DD_OPEN(a,DD_VM_RUN);DD_ADD(DD_VM_ZERO_BYTES,unknown++);DD_CLOSE(a);DD_SCOPE(DD_RENDER);}\n')
    obj=out/'off.o'
    subprocess.run(['gcc','-O0','-fno-pie','-Wall','-Wextra','-Werror','-I',str(R/'main/pocket'),'-c',str(off),'-o',str(obj)],check=True)
    assert not subprocess.check_output(['nm','-u',str(obj)],text=True).strip()
    print('PASS detail OFF does not evaluate arguments or reference instrumentation at O0')
if args.log:
    text=args.log.read_text();parse_detail(text,expected=True)
    tagged=text.replace('APP_STOPPED\n','I (206206) app: APP_STOPPED\n').replace('HOME_READY\n','I (206206) shell: HOME_READY\n')
    parse_detail(tagged,expected=True)
    parse_detail(tagged.replace('I (206206) app: APP_STOPPED','\x1b[0;32mI (206206) app: APP_STOPPED\x1b[0m'),expected=True)
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
        'runtime error after END':[text.replace('APP_STOPPED\n','InternalError: interrupted\nAPP_STOPPED\n')],
        'runaway after END':[text.replace('APP_STOPPED\n','FRAME RUNAWAY 251000\nAPP_STOPPED\n')],
        'LOADSTALL':[text.replace('APP_STOPPED\n','DERBY LOADSTALL\nAPP_STOPPED\n')],
        'lost STOP':[text.replace('APP_STOPPED\n','')],
        'lost HOME':[text.replace('HOME_READY\n','')],
        'wrong STOP tag':[tagged.replace('app: APP_STOPPED','shell: APP_STOPPED')],
        'wrong HOME tag':[tagged.replace('shell: HOME_READY','app: HOME_READY')],
        'warning STOP':[tagged.replace('I (206206) app:', 'W (206206) app:')],
        'marker suffix':[tagged.replace('APP_STOPPED\n','APP_STOPPED extra\n')],
        'fake payload':[text.replace('APP_STOPPED\n','print APP_STOPPED\n')],
        'duplicate tagged STOP':[tagged.replace('APP_STOPPED\n','APP_STOPPED\nAPP_STOPPED\n')],
        'duplicate tagged HOME':[tagged.replace('HOME_READY\n','HOME_READY\nHOME_READY\n')],
        'HOME before STOP':[tagged.replace('app: APP_STOPPED','TEMP_STOP').replace('shell: HOME_READY','app: APP_STOPPED').replace('TEMP_STOP','shell: HOME_READY')],
        'tagged runtime error':[tagged.replace('I (206206) app: APP_STOPPED','InternalError: interrupted\nI (206206) app: APP_STOPPED')],
        'runtime error after HOME':[tagged+'InternalError: interrupted\n'],
        'stop hook rejected':[tagged.replace('I (206206) app: APP_STOPPED','W (206205) pocket_app: hook rejected: failure\nI (206206) app: APP_STOPPED')],
        'stop hook threw':[tagged.replace('I (206206) app: APP_STOPPED','W (206205) pocket_app: hook threw: Error: failure\nI (206206) app: APP_STOPPED')],
        'stop hook timeout':[tagged.replace('I (206206) app: APP_STOPPED','W (206205) pocket_app: stop hook did not finish in 200ms\nI (206206) app: APP_STOPPED')],
        'missing owner exit':[''.join(l for l in lines if not l.startswith('DERBY_DETAIL_EXIT '))],
        'duplicate owner exit':[text.replace('DERBY_DETAIL_EXIT v=1','DERBY_DETAIL_EXIT v=1 session=1 clean=1\nDERBY_DETAIL_EXIT v=1')],
        'unclean owner exit':[text.replace('clean=1','clean=0')],
        'wrong owner session':[text.replace('DERBY_DETAIL_EXIT v=1 session=1','DERBY_DETAIL_EXIT v=1 session=2')],
        'owner exit after STOP':[text.replace('DERBY_DETAIL_EXIT v=1 session=1 clean=1\n','').replace('APP_STOPPED\n','APP_STOPPED\nDERBY_DETAIL_EXIT v=1 session=1 clean=1\n')],
        'owner exit after HOME':[text.replace('DERBY_DETAIL_EXIT v=1 session=1 clean=1\n','')+'DERBY_DETAIL_EXIT v=1 session=1 clean=1\n'],
    }
    for name,mutated in cases.items():
        try:parse_detail(''.join(mutated),expected=True)
        except ValueError:pass
        else:raise AssertionError(name+' accepted')
    try:parse_detail(text,expected=False)
    except ValueError:pass
    else:raise AssertionError('OFF accepted detailed stream')
    print(f'PASS bare/tagged/ANSI detail stream; {len(cases)+1} missing/schema/order/quality/runtime/OFF failure cases rejected')
