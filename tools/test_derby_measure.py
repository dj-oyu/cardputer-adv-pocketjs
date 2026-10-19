"""Host accounting and compile-out contract; no target timing claims."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    out=Path(directory)
    for flags in (['-O1','-g','-fsanitize=address,undefined'],['-O2']):
        exe=out/'measure'
        subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-DPOCKET_DERBY_MEASURE',
            *flags,'-I',str(ROOT/'main/pocket'),str(ROOT/'tools/test_derby_measure.c'),
            str(ROOT/'main/pocket/derby_measure.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    off=out/'off.c';off.write_text('#include "derby_measure.h"\nvoid f(void){DM_SCOPE(DM_VM);DM_BATCH(1);}\n')
    obj=out/'off.o'
    subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-O0','-I',str(ROOT/'main/pocket'),'-c',str(off),'-o',str(obj)],check=True)
    assert not subprocess.check_output(['nm','-u',str(obj)],text=True).strip()
    print('PASS OFF scopes/backend counters have no undefined symbols at O0')

    exe=out/'device'
    subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-O2','-DPOCKET_DERBY_MEASURE',
        '-I',str(ROOT/'tools/hostshim'),'-I',str(ROOT/'main/pocket'),'-I',str(ROOT/'main/hal'),
        str(ROOT/'tools/test_derby_measure_device.c'),str(ROOT/'main/pocket/derby_measure.c'),
        str(ROOT/'main/pocket/derby_measure_device.c'),'-Wl,--wrap=dm_read','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

    lifecycle=out/'lifecycle.c'
    lifecycle.write_text('#include "derby_measure.h"\nint main(int argc,char **argv){(void)argv;if(argc>1)(void)dm_enter(DM_VM);return 0;}\n')
    exe=out/'lifecycle'
    subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-O2','-DPOCKET_DERBY_MEASURE',
        '-I',str(ROOT/'main/pocket'),str(lifecycle),str(ROOT/'main/pocket/derby_measure.c'),
        str(ROOT/'tools/games/derby_measure_host.c'),'-o',str(exe)],check=True)
    assert subprocess.run([str(exe)],capture_output=True).returncode==0
    leak=subprocess.run([str(exe),'leak'],capture_output=True,text=True)
    assert leak.returncode!=0 and 'unbalanced scopes' in leak.stderr
    print('PASS host balanced exit succeeds; leaked scope forces nonzero process exit')
