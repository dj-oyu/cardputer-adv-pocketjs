// Geometry/ownership preflight only. Actual pixels and heap use the native host.
'use strict';
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict'),crypto=require('node:crypto');
let commands=[],begins=0,commits=0,assets=0,scenes=0,reenter=null;
function record(name,...values){commands.push([name,...values.map(v=>ArrayBuffer.isView(v)?Array.from(v):v)]);}
const H={
  project(m,src,out){
    assert.equal(m.length,12);assert.equal(src.length/3,out.length/2);
    for(let k=0;k<out.length/2;k++){
      const x=src[k*3],y=src[k*3+1],z=src[k*3+2],w=m[8]*x+m[9]*y+m[10]*z+m[11];
      out[k*2]=(m[0]*x+m[1]*y+m[2]*z+m[3])/w;out[k*2+1]=(m[4]*x+m[5]*y+m[6]*z+m[7])/w;
    }
    record('project',out);
  },
  shade(n,a,t,l,m,out){
    assert.equal(n.length,a.length*3);assert.equal(t.length,a.length);assert.equal(out.length,a.length);
    for(let k=0;k<a.length;k++){assert.ok(n[k*3]||n[k*3+1]||n[k*3+2]);out[k]=a[k];}
    record('shade',n,l);
  },
  fill(points,poly,holes,color){
    assert.ok(poly[1]>=3&&poly[1]<=4);assert.ok(Number.isInteger(color)&&color>=0&&color<=65535);
    for(let k=0;k<poly[1]*2;k++)assert.ok(Number.isFinite(points[k]));
    let sign=0;
    for(let k=0;k<poly[1];k++){
      const a=k*2,b=((k+1)%poly[1])*2,c=((k+2)%poly[1])*2,
        cross=(points[b]-points[a])*(points[c+1]-points[b+1])-(points[b+1]-points[a+1])*(points[c]-points[b]);
      if(Math.abs(cross)>1e-3){if(sign)assert.ok(cross*sign>0,'convex color fragments');sign=cross>0?1:-1;}
    }
    assert.deepEqual(Array.from(holes),[4,8,12,8]);record('fill',points.slice(0,poly[1]*2),points.slice(8),poly[1],color);
    if(reenter)reenter();
  },
  gouraudPatch(xy,indices,a,b,mask,hxy,hcounts){assert.equal(xy.length,60);assert.equal(indices.length,120);assert.equal(a.length,30);assert.equal(b.length,30);assert.equal(mask.length,30);assert.equal(hxy.length,32);assert.deepEqual(Array.from(hcounts),[8,8]);record('gouraud',xy,indices,a,b,mask,hxy,hcounts);if(reenter)reenter();},
  ellipsoid(...args){assert.equal(args[0].length,8);assert.equal(args[1].length,9);record('nose',...args);},
  outline(points,start,count,color){assert.ok(count===3||count===5);record('line',Array.from(points.slice(start*2,(start+count)*2)),color);}
};
const P={resource(o){assert.equal(o.maxSegments,1024);return {};},beginFrame(){++begins;},commit(){++commits;}};
const context=vm.createContext({__foxPatchVariant:process.env.FOX_PATCH_VARIANT,pocket:{hone:H,kasane:{procedural:P,replace(fn){fn({background(){},image(){}});}},
  ayatsuri:{modelBuiltin(name){assert.equal(name,'fox-face');++assets;let dead=false;return {dispose(){if(!dead)--assets;dead=true;}};}}},
  AyatsuriClip:{nativeScene(){
    ++scenes;let dead=false,ready=false;const poses={},visible={};
    return {handle(i){return {place(p){assert.ok(!dead);poses[i]=[...p.position,...p.rotation,...p.scale];ready=false;},
      visible(v){assert.ok(!dead);visible[i]=v;ready=false;}};},
      aperture(h,v){assert.equal(v.length,15);return {bind(){return this;}};},
      light(p){record('native-light',p.direction);ready=false;},evaluate(){ready=true;},
      render(){assert.ok(ready&&!dead);record('native',poses,visible);},dispose(){if(!dead)--scenes;dead=true;}};
  }}});
vm.runInContext(fs.readFileSync('tools/character/fox_patch_fixture.js','utf8'),context);
const api=context.FoxPatchStudy;
function draw(yaw,light=0){commands=[];api.draw(yaw,light);return crypto.createHash('sha256').update(JSON.stringify(commands)).digest('hex');}
for(let i=0;i<=60;i++)draw(-35+70*i/60);
assert.equal(draw(0),draw(0),'same inputs repeat all primitive payloads');
const left=draw(-35),right=draw(35);assert.notEqual(left,right,'yaw produces projected geometry, not a fixed panel');
draw(0);const body=commands.find(row=>row[0]==='project')[1];draw(0,60);
assert.deepEqual(commands.find(row=>row[0]==='project')[1],body,'light-only leaves body geometry fixed');
for(const args of [[36,0],[NaN,0],[0,181],[0,Infinity]]){
  const old=begins;assert.throws(()=>api.draw(...args),/view range/);assert.equal(begins,old,'invalid view does not begin a candidate');
}
reenter=()=>{assert.throws(()=>api.draw(0),/busy/);assert.throws(()=>api.release(),/busy/);};draw(0);reenter=null;
const stable=draw(0),stats={};api.stats(stats);const dense=['dense-clay','dense','socket'].includes(process.env.FOX_PATCH_VARIANT);assert.equal(stats.vertices,dense?56:30);assert.equal(stats.triangles,dense?84:40);assert.equal(stats.nativePasses,1);
for(let i=0;i<3;i++){api.release();api.release();assert.equal(assets,0);assert.equal(scenes,0);assert.equal(draw(0),stable);}
api.release();assert.equal(assets,0);assert.equal(scenes,0);assert.equal(begins,commits);
console.log('FOX_PATCH_PREFLIGHT_PASS',JSON.stringify(stats));
