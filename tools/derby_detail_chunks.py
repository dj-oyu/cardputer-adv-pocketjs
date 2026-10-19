"""Opt-in scopes only; OFF leaves the generated fixture bytes untouched."""
from pathlib import Path
import re

def once(text,old,new):
    if text.count(old)!=1:raise ValueError('detail source drift: '+old[:80])
    return text.replace(old,new,1)

def function(text,name,stage):
    # Existing source formatting puts top-level closing braces in column zero.
    pat=rf'(^function {name}\([^\n]*\) \{{\n)(.*?)(^\}})'
    def wrap(m):return m[1]+f'  S.detail({stage});try {{\n'+m[2]+'  } finally { S.detail(-1); }\n'+m[3]
    result,n=re.subn(pat,wrap,text,count=1,flags=re.M|re.S)
    if n!=1:raise ValueError('detail function drift: '+name)
    return result

def instrument(name,text):
    if name=='watch':return function(text,'step',1)
    if name=='view':return function(text,'shot',2)
    if name=='play':
        text=once(text,'let dfRep=1,dfMode=0,dfTicks=0,dfTailTicks=0,dfNext=391;',
            'let ddFinish=0;\nlet dfRep=1,dfMode=0,dfTicks=0,dfTailTicks=0,dfNext=391;')
        text=once(text,'    if (dfMode===0) {',
            "    if(dfMode===5){\n      const state=S.fixture(3,dfRep,510,F.seed,cm,tier,ddFinish);\n      if(state===4)return;\n      if(state!==1&&state!==5)throw Error('DF_REPORT');\n      if(++dfRep>3){dfMode=4;pocket.app.exit();}else dfMode=0;\n      return;\n    }\n    if (dfMode===0) {")
        text=once(text,'      S.fixture(3,dfRep,510,F.seed,cm,tier,digest);\n      if(++dfRep>3){dfMode=4;pocket.app.exit();}else dfMode=0;',
            '      ddFinish=digest;dfMode=5;return;')
        text=once(text,'      const up = tx => {','      const up = tx => {\n        S.detail(5);try {')
        text=once(text,'      };\n      if (need)', '        } finally { S.detail(-1); }\n      };\n      if (need)')
        return once(text,'const dfFrame=globalThis.frame;',
            'const ddFrame=globalThis.frame;\nglobalThis.frame=function(b){S.detail(0);try{return ddFrame(b);}finally{S.detail(-1);}};\nconst dfFrame=globalThis.frame;')
    if name=='pan':
        text=once(text,'  const ps = xs.map(', '  S.detail(2);\n  const ps = xs.map(')
        text=once(text,'  zo.sort(', '  S.detail(-1);S.detail(3);\n  zo.sort(')
        text=once(text,'  for (const l of zo) {','  S.detail(-1);S.detail(4);try {\n  for (const l of zo) {')
        return once(text,'  S.ser(live.prail, DNR, k[4], 0, L, 0xffff, 4.9);',
             '  } finally { S.detail(-1); }\n  S.ser(live.prail, DNR, k[4], 0, L, 0xffff, 4.9);')
    return text

def apply(app:Path):
    for name in ('watch','view','play','pan'):
        path=app/f'derby_{name}.js'
        path.write_bytes(instrument(name,path.read_text()).encode())
