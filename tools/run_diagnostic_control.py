"""Domain and fixed-D1 codec host tests; no device or wire permissions."""
from pathlib import Path
import os,subprocess
R=Path(__file__).resolve().parents[1]
out=R/'.cache/diagnostic-control';out.mkdir(parents=True,exist_ok=True)
for label,flags in [('o2',['-O2','-fstrict-aliasing']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie'])]:
 for suite in ['control','wire']:
  cmd=['cc','-D_POSIX_C_SOURCE=200809L','-std=c11','-Wall','-Wextra','-Werror','-Wpedantic','-pthread',*flags,'-Imain/ui','main/ui/diagnostic_control.c']
  if suite=='wire':cmd+=['main/ui/diagnostic_wire.c']
  binary=out/(suite+'-'+label);cmd+=['tools/test_diagnostic_'+suite+'.c','-o',str(binary)]
  subprocess.run(cmd,cwd=R,check=True)
  subprocess.run([str(binary)],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
