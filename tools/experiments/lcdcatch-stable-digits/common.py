"""Pinned main A/B inputs and the repository native Kasane/input harness."""
from pathlib import Path
import hashlib,json,subprocess,sys
R=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(R/'tools/games'))
import run_big_wave as game
BASE='6db9244d585fa3caad899853ff8c7fbe96c5d7d3'
def once(s,a,b):
 assert s.count(a)==1,a[:100]
 return s.replace(a,b)
def renamed(data):
 # BASE predates the pocket -> kobako rename (f4eb867); the harness now
 # installs kobako.*. The rename's JS rules are same-length, so the renamed
 # baseline parses into the same heap as the original.
 sys.path.insert(0,str(R/'tools/rename'));import kobako_rename as k
 t=data.decode('utf8');n={}
 for _,rx,rep in k.JS_RULES:t=rx.sub(rep,t)
 out=t.encode('utf8');assert len(out)==len(data);return out
def inputs(out):
 # README.md changed only by the rename (pocket.* -> kobako.* in its prose).
 changed=set(subprocess.check_output(['git','diff','--name-only',BASE,'--','apps/lcdcatch'],cwd=R,text=True).splitlines());assert changed<={'apps/lcdcatch/lcd_catch.js','apps/lcdcatch/README.md'} and 'apps/lcdcatch/lcd_catch.js' in changed,changed
 out.mkdir(parents=True,exist_ok=True)
 rows={}
 for mode,data in [('baseline',renamed(subprocess.check_output(['git','show',BASE+':apps/lcdcatch/lcd_catch.js'],cwd=R))),('candidate',(R/'apps/lcdcatch/lcd_catch.js').read_bytes())]:
  p=out/(mode+'.js');p.write_bytes(data);rows[mode]=p
 return rows

def build(out,harness,m32=False,sanitize=False):
 out.mkdir(parents=True,exist_ok=True)
 flags=['-O2','-ffp-contract=off']+(['-m32','-malign-double'] if m32 else [])+(['-fsanitize=address,undefined'] if sanitize else [])
 subprocess.run(['python3','tools/make_font.py',str(out)],cwd=R,check=True,stdout=subprocess.DEVNULL)
 pets=(R/'apps/pet/assets/pets-compact.bin').read_bytes()
 (out/'kasane_pet_test_data.h').write_text('static const uint8_t pet_test_data[] = {'+','.join(map(str,pets))+'};\n')
 api=(R/'main/kobako/kobako_api.c').read_text();a=api.index('// ----------------------------------------------------------- subscriptions');b=api.index('// ------------------------------------------------------- async completions',a);(out/'kobako_sub_impl.inc').write_text(api[a:b])
 compiler=subprocess.check_output(['gcc','--version'],text=True).splitlines()[0]
 qjs=R/game.QJS
 sources=sorted(set([qjs/(n+'.c') for n in ('dtoa','libregexp','libunicode','quickjs','quickjs-vm')]+list(qjs.rglob('*.h'))+list((R/'components/kobako_guest/include').rglob('*.h'))))
 fingerprint={'compiler':compiler,'flags':flags,'defines':['QUICKJS_NG_BUILD','QJS_LEAN_LAZY_BUILTINS=1','_GNU_SOURCE'],'inputs':{str(p.relative_to(R)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}}
 stamp=out/'engine-build.json'
 if not stamp.exists() or json.loads(stamp.read_text())!=fingerprint:
  for name in ('dtoa','libregexp','libunicode','quickjs','quickjs-vm'):(out/(name+'.o')).unlink(missing_ok=True)
 objects=[]
 for name in ('dtoa','libregexp','libunicode','quickjs','quickjs-vm'):
  obj=out/(name+'.o')
  if not obj.exists():subprocess.run(['gcc','-std=gnu11','-c','-g','-w',*flags,'-DQUICKJS_NG_BUILD', '-DQJS_LEAN_LAZY_BUILTINS=1','-D_GNU_SOURCE','-I',game.QJS,'-I','components/kobako_guest/include',game.QJS+'/'+name+'.c','-o',str(obj)],cwd=R,check=True)
  objects.append(str(obj))
 stamp.write_text(json.dumps(fingerprint,sort_keys=True,indent=2)+'\n')
 binary=out/'test-lcd7'
 inc=[f'-I{d}' for d in (game.QJS,'tools/hostshim','main','main/kobako','main/ui','main/ui/kasane','main/text','main/hal',str(out))]
 subprocess.run(['gcc','-std=gnu11','-g',*flags,'-DKSN_PROC_POINTS_PIE_MODEL','-Wall','-Wextra','-Werror','-fno-omit-frame-pointer',*inc,str(harness),*game.kasane_sources(),*game.EXTRA,*objects,'-Wl,--wrap=calloc','-Wl,--wrap=free','-lm','-o',str(binary)],cwd=R,check=True)
 return binary
