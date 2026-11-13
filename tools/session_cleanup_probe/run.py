from pathlib import Path
import subprocess,sys,hashlib
root=Path(sys.argv[1]).resolve(); here=Path(__file__).resolve().parent
source=(root/'main/app_session.c').read_text()
f=source[source.index('void app_stop(void) {'):source.index('// ------------------------------------------------------ resident suspension')]
(here/'stop.inc').write_text(f)
for p in [root/'main/app_session.c',root/'main/pocket/shiki_heap.c',here/'test.c',here/'stop.inc']:
 print(p.name,hashlib.sha256(p.read_bytes()).hexdigest(),flush=True)
for flags,name in [(['-O2'],'test'),(['-O1','-g','-fsanitize=address,undefined'],'test-san')]:
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'main/pocket'),str(here/'test.c'),str(root/'main/pocket/shiki_heap.c'),'-o',str(here/name)],check=True)
 import os
 subprocess.run([str(here/name)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
