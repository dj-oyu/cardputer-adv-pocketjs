"""Finite, app-owned section fixtures and exact returned-double capture."""
import hashlib,json

def boundaries(binary,out,base,fast,run,replace):
    lines=[];queries=0
    for secs in ([{'x':110,'w':8}], [{'x':110,'w':8},{'x':114,'w':11},{'x':180,'w':6}], []):
        for amp in (-1e-7,0,1e-7,.25,1-1e-7,1,1+1e-7):
            xc=122-28.2+8*amp
            xs=[xc+q+e for q in (7,22,30) for e in (-1e-7,0,1e-7)]
            for sec in secs:
                xs += [sec['x']+q+e for q in (-4,0,sec['w'],sec['w']+4) for e in (-1e-7,0,1e-7)]
            queries+=len(xs)
            lines.append('(()=>{const S=bigWave.S;Object.assign(S,{st:1,set:0,t:60,x:110,xc:'+repr(xc)+',z:0,p:.4,roll:0,lean:0});S.course={rocks:[],riders:[],secs:'+json.dumps(secs,separators=(',',':'))+'};const before=JSON.stringify(S);__curl_values('+json.dumps(xs,separators=(',',':'))+'.map(bigWave.curlAt));if(JSON.stringify(S)!==before)throw Error("curlAt mutated state");const D=[];bigWave.test.camera();bigWave.test.draws(D);const P=pocket.kasane.procedural;P.beginFrame(0x0845);for(const d of D)P.draw(d[0],d[1]);P.commit();})()')
    fixture=out/'boundary.js';fixture.write_text('\n'.join(lines)+'\n');results={}
    for mode,source in [('base',base),('fast',fast)]:
        src=out/(mode+'-boundary.js');src.write_text(replace(source,'globalThis.bigWave = {S: S,','globalThis.bigWave = {test:{camera,draws}, S: S,'))
        results[mode]=run(binary,src,out/('boundary-'+mode),verify=True,fixture=fixture)
    for ext in ('panels','draws','states'):
        a=(out/('boundary-base.'+ext)).read_bytes();b=(out/('boundary-fast.'+ext)).read_bytes();assert a==b,ext
        results[ext]={'bytes':len(a),'sha256':hashlib.sha256(a).hexdigest()}
    values=[]
    for mode in ('base','fast'):
        values.append([line for line in (out/('boundary-'+mode+'.log')).read_text().splitlines() if line.startswith('CURL_VALUES ')])
    assert values[0]==values[1] and len(values[0])==len(lines)
    results.update(fixtures=len(lines),returned_double_comparisons=queries,values=values[0])
    (out/'boundary-result.json').write_text(json.dumps(results,indent=2)+'\n')
    return results
