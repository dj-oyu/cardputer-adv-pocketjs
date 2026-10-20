"""Generate the finite diagnostic workload; ordinary app files stay unchanged."""
from pathlib import Path
import hashlib
import json
import sys
from derby_compare_chunks import instrument as compare_view

FIXTURE={"id":"oval-pan8-390-510-v1","seed":"9112A7AB","race":1000001,
         "oval":1,"tier":1,"camera":8,"start":390,"end":510,"contexts":120,
         "repetitions":3,"independent":False,"finish":"5DC86E18"}
MANIFEST_SHA256=hashlib.sha256(json.dumps(FIXTURE,sort_keys=True,separators=(',',':')).encode()).hexdigest()
CONTROLLER_MARKER='\n// DF fixture oval-pan8-390-510-v1: three dependent replays in one boot.'
CONTROLLER_LOAD="\npocket.app.load('fixed');"
CONTROLLER_STRICT="'use strict';"

def once(text,old,new):
    if text.count(old)!=1:raise ValueError('fixed workload source drift: '+old[:80])
    return text.replace(old,new,1)

WRAPPER=r'''
// DF fixture oval-pan8-390-510-v1: three dependent replays in one boot.
const dfFrame=globalThis.frame;
let dfRep=1,dfMode=0,dfTicks=0,dfTailTicks=0,dfNext=391;
function dfAbort() { S.fixture(4,0,0,0,0,0,0); pocket.app.exit(); }
function dfIdentity() {
  if (F.seed!==0x9112A7AB || !F.o || tier!==1 || cm!==8) throw Error('DF_IDENTITY');
}
globalThis.frame=function(b) {
  try {
    if (b || K.down().length || GK.some(k=>K.pressed(k))) return dfAbort();
    if (dfMode===0) {
      if (S.fixture(0,0,0,0,0,0,0)===6) return pocket.app.exit();
      drop(Object.keys(live));queue.length=0;
      pts=1500;raceNo=1000001;sn=19;tier=1;pick=0;stake=100;
      dm=0;idle=1e20;fe=-1e9;notes=[];A=null;enter('pad');
      if(F.seed!==0x9112A7AB||!F.o)throw Error('DF_SEED');
      dfTicks=0;dfTailTicks=0;dfNext=391;dfMode=1;
    }
    if(dfMode===1) {
      if(++dfTicks>800)throw Error('DF_WARMUP_LIMIT');
      dfFrame(0);
      if(scene==='pad'&&t>=20&&!queue.length)enter('gate');
      if(scene==='race'&&rnd(rs.t/DT)===390) {
        dfIdentity();if(queue.length)throw Error('DF_LOAD');
        S.fixture(1,dfRep,390,F.seed,cm,tier,0);dfMode=2;
      }
      return;
    }
    if(dfMode===2) {
      dfFrame(0);dfIdentity();
      if(scene!=='race'||rnd(rs.t/DT)!==dfNext||slow||disp!==0)throw Error('DF_STEP');
      S.fixture(2,dfRep,dfNext,F.seed,cm,tier,0);
      if(dfNext++===510)dfMode=3;
      return;
    }
    if(dfMode===3) {
      if(S.fixture(0,0,0,0,0,0,0)!==4)throw Error('DF_BOUNDARY');
      // DF bounded tail: return from this logical frame, not a VM continuation.
      // Same step order/DT/terminal predicate as settle(), outside measurement.
      // No dfFrame(), paint or capture; settlement runs once after its loop ends.
      if(++dfTailTicks>512)throw Error('DF_TAIL_LIMIT');
      for(let n=0;n<8&&rs.done<8&&rs.t<200;n++)step(F,rs,DT);
      if(rs.done<8&&rs.t<200)return;
      settle();
      const text=fin.o.map(i=>i+1).join(',')+'|'+fin.o.map(i=>rnd(rs.tc[i]*1e6)).join(',');
      let digest=2166136261;
      for(let i=0;i<text.length;i++)digest=M.imul(digest^text.charCodeAt(i),16777619)>>>0;
      if(digest!==0x5DC86E18)throw Error('DF_FINISH');
      S.fixture(3,dfRep,510,F.seed,cm,tier,digest);
      if(++dfRep>3){dfMode=4;pocket.app.exit();}else dfMode=0;
    }
  } catch(e) { dfAbort(); }
};
'''

def generate(source:Path,out:Path,detail=False):
    out.mkdir(parents=True,exist_ok=True)
    # A changed settlement predicate must be reviewed with the bounded tail.
    once((source/'derby_scene.js').read_text(),
         '  while (rs.done < 8 && rs.t < 200) step(F, rs, DT);',
         '  while (rs.done < 8 && rs.t < 200) step(F, rs, DT);')
    view=compare_view((source/'derby_view.js').read_text())
    # Keep the app's one READY after enter('pad') succeeds, before any fixture
    # frame. Per-repetition enter('pad') calls do not emit this startup marker.
    view=once(view,"log = m => console.log('DERBY ' + m)",
              "log = m => { if(m==='READY tones=true'||m==='READY tones=false') console.log('DERBY ' + m); else if(m.indexOf('FAIL')>=0||m.indexOf('LOADSTALL')>=0||m.indexOf('DEGRADE')>=0) pocket.derby.fixture(4,0,0,0,0,0,0); }")
    a=view.index("try {\n  ST.get('derby.v1')")
    b=view.index('// The screen',a)
    view=view[:a]+'// Finite fixture: no player storage read or write.\n\n'+view[b:]
    play=(source/'derby_play.js').read_text()
    play=once(play,'    attract(b);','    nw=0;dm=0;dk=0;idle=1e20;fe=-1e9;')
    a=play.index('        let m = slow ? 3 :')
    b=play.index('\n        if (!m &&',a)
    play=play[:a]+'        let m = 8;'+play[b:]
    play=once(play,"};\nenter('pad');",'};\n'+WRAPPER+"\nenter('pad');")
    watch=once((source/'derby_watch.js').read_text(),'const OV = [.5,','const OV = [1,')
    for name,text in [('view',view),('play',play),('watch',watch)]:
        (out/f'derby_{name}.js').write_bytes(text.encode())
    if detail:
        from derby_detail_chunks import instrument
        for name in ('watch','view','play','pan'):
            path=out/f'derby_{name}.js'
            text=(source/path.name).read_text() if name=='pan' else path.read_text()
            path.write_bytes(instrument(name,text).encode())
    # Compile the controller after the normal frame's parser temporaries die.
    # Keep its global bindings, strict semantics and position before enter/READY;
    # a failed nested load must escape the original play load, without fallback.
    path=out/'derby_play.js';play=path.read_text()
    begin=play.index(CONTROLLER_MARKER)
    end=play.index("\nenter('pad');",begin)
    (out/'derby_fixed.js').write_bytes((CONTROLLER_STRICT+play[begin:end]).encode())
    path.write_bytes((play[:begin]+CONTROLLER_LOAD+play[end:]).encode())
    lines=['app local.derby']
    for line in (source/'chunks.txt').read_text().splitlines():
        cols=line.split()
        if len(cols)!=2 or cols[0]=='app' or line.lstrip().startswith('#'):continue
        path=out/f'derby_{cols[0]}.js' if cols[0] in ('view','play') or detail and cols[0]=='pan' else source/cols[1]
        lines.append(cols[0]+' '+path.resolve().as_posix())
    lines.append('fixed '+(out/'derby_fixed.js').resolve().as_posix())
    (out/'chunks.txt').write_text('\n'.join(lines)+'\n')
    (out/'fixture.json').write_text(json.dumps({**FIXTURE,'manifest_sha256':MANIFEST_SHA256},indent=2)+'\n')

if __name__=='__main__':
    if len(sys.argv) not in (3,4):raise SystemExit('usage: derby_fixed_workload_chunks.py SOURCE OUT')
    generate(Path(sys.argv[1]).resolve(),Path(sys.argv[2]).resolve(),len(sys.argv)==4 and sys.argv[3].upper() in ('ON','1','YES','TRUE','Y'))
