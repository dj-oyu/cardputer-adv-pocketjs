"""Observer-only common-binary renderer selector outside each band call."""
from pathlib import Path
import hashlib,json,re,subprocess
ROOT=Path(__file__).resolve().parents[3]
BASE='e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267'
PATH='main/ui/kasane/ksn_procedural.c'
def source():
 baseline=subprocess.check_output(['git','show',BASE+':'+PATH],cwd=ROOT,text=True)
 candidate=(ROOT/PATH).read_text()
 pa=baseline.index('static inline __attribute__((always_inline)) void plain_band');pb=baseline.index('/* One pixel of a pattern line',pa)
 qa=candidate.index('static inline __attribute__((always_inline)) void plain_band');qb=candidate.index('/* One pixel of a pattern line',qa)
 assert baseline[:pa]==candidate[:qa] and baseline[pb:]==candidate[qb:]
 a=baseline.index('/* The Bresenham phase where line s ')
 b=baseline.index('#ifdef KASANE_BGCOST_TRACE',a)
 ca=candidate.index('/* The Bresenham phase where line s ');cb=candidate.index('#ifdef KASANE_BGCOST_TRACE',ca)
 # The surrounding VM and plain-only public renderer remain the product source.
 # Exact changed-function scope is checked again when the supplied patch arrives.
 assert baseline[:a]==candidate[:ca] and baseline[b:]==candidate[cb:]
 def rename(text,prefix):
  for name in ('band_entry','plain_band','pattern_band','render_band_ext','ksn_proc_render_band'):
   text=re.sub(r'\b'+name+r'\b',prefix+name,text)
  return text
 wrapper='''
bool plain_step_candidate;
bool ksn_proc_render_band(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height){
 return plain_step_candidate?cand_ksn_proc_render_band(frame,pixels,y,height):ref_ksn_proc_render_band(frame,pixels,y,height);
}
'''
 return candidate[:ca]+rename(baseline[a:b],'ref_')+rename(candidate[ca:cb],'cand_')+wrapper+candidate[cb:]
def install(game,out):
 dst=out/'ksn_procedural_selector.c';dst.write_text(source())
 original=game.kasane_sources
 game.kasane_sources=lambda:[str(dst) if x==PATH else x for x in original()]
 return dst
def engine_fingerprint(flags):
 q=ROOT/'components/quickjs-ng/quickjs-ng'
 sources=sorted(set([q/(n+'.c') for n in ('dtoa','libregexp','libunicode','quickjs','quickjs-vm')]+list(q.rglob('*.h'))+list((ROOT/'components/pocketjs_guest/include').rglob('*.h'))))
 return {'compiler':subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],'flags':flags,'defines':['QUICKJS_NG_BUILD','_GNU_SOURCE'],'inputs':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}}
def build(game,flags,cache,harness):
 cache.mkdir(parents=True,exist_ok=True);stamp=cache/'engine-build.json';want=engine_fingerprint(flags)
 if not stamp.exists() or json.loads(stamp.read_text())!=want:
  for n in ('dtoa','libregexp','libunicode','quickjs','quickjs-vm'):(cache/(n+'.o')).unlink(missing_ok=True)
 binary=game.build(flags,cache,harness=harness);stamp.write_text(json.dumps(want,indent=2)+'\n');return binary
def check_engine(out):
 s=json.loads((out/'build/engine-build.json').read_text())
 assert s['flags'] in (['-O2','-ffp-contract=off'],['-O2','-ffp-contract=off','-m32','-malign-double'])
 assert s==engine_fingerprint(s['flags'])
