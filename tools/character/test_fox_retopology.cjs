// Node geometry/ownership preflight. selectVisible is an oracle stub; native pixels and memory are not measured.
'use strict';
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict'),crypto=require('node:crypto');
const expectedXYZ=[0,-28,2,16,-24,1,25,-13,-2,31,0,-3,26,10,-2,13,18,-2,0,21,-3,-13,18,-2,-26,10,-2,-31,0,-3,-25,-13,-2,-16,-24,1,-18,-13,7,-4,-13,12,-4,3,16,-18,3,7,4,-13,12,18,-13,7,18,3,7,4,3,16,0,-18,14,0,-6,18,0,5,28,0,12,25,0,17,10,-8,8,23,8,8,23,-8,14,14,8,14,14,0,14.5,22];
const expectedIndices=[10,11,12,12,11,13,12,13,14,8,9,15,9,10,15,10,12,15,7,8,15,12,14,15,1,2,17,16,1,17,2,3,18,17,2,18,3,4,18,4,5,18,16,17,18,16,18,19,11,0,20,13,11,20,0,1,20,1,16,20,16,13,20,14,13,21,13,16,21,16,19,21,19,14,21,14,19,22,7,15,25,15,14,25,14,22,25,22,23,25,18,5,26,19,18,26,22,19,26,23,22,26,6,7,27,24,6,27,25,23,27,7,25,27,5,6,28,6,24,28,26,5,28,23,26,28,24,27,29,27,23,29,23,28,29,28,24,29];
let lastSource=null,lastSelected=null,lastDepth=null,lastNormals=null;
let commands=[],begins=0,commits=0,assets=0,scenes=0,reenter=null;
function record(name,...values){commands.push([name,...values.map(v=>ArrayBuffer.isView(v)?Array.from(v):v)]);}
const H={
  project(m,src,out){
    assert.equal(m.length,12);assert.equal(src.length/3,out.length/2);
    for(let k=0;k<out.length/2;k++){
      const x=src[k*3],y=src[k*3+1],z=src[k*3+2],w=m[8]*x+m[9]*y+m[10]*z+m[11];
      out[k*2]=(m[0]*x+m[1]*y+m[2]*z+m[3])/w;out[k*2+1]=(m[4]*x+m[5]*y+m[6]*z+m[7])/w;
    }
    lastSource=Array.from(src);assert.deepEqual(lastSource.slice(0,90),expectedXYZ);record('project',out);
  },
  shade(n,a,t,l,m,out){
    assert.equal(n.length,a.length*3);assert.equal(t.length,a.length);assert.equal(out.length,a.length);
    for(let k=0;k<a.length;k++){assert.ok(n[k*3]||n[k*3+1]||n[k*3+2]);out[k]=a[k];}
    lastNormals=Array.from(n);record('shade',n,l);
  },
  gouraudPatch(xy,indices,a,b,mask,hxy,hcounts,count){
    assert.equal(xy.length,60);assert.equal(indices.length,138);assert.equal(a.length,30);assert.equal(b.length,30);
    assert.equal(mask.length,30);assert.equal(hxy.length,32);assert.deepEqual(Array.from(hcounts),[8,8]);
    assert.equal(count,lastSelected.length/3);assert.deepEqual(Array.from(indices.slice(0,count*3)),lastSelected);
    for(let i=0;i<30;i++)assert.ok(Math.abs(mask[i]-(expectedXYZ[i*3+1]+.3*Math.abs(expectedXYZ[i*3])-5))<1e-5);
    if(process.env.FOX_PATCH_VARIANT==='retopology-clay')assert.deepEqual(Array.from(a),Array.from(b));
    record('gouraud',xy,indices.slice(0,count*3),a,b,mask,hxy,hcounts,count);if(reenter)reenter();
  },
  ellipsoid(...args){assert.equal(args[0].length,8);assert.equal(args[1].length,9);record('nose',...args);},
  outline(points,start,count,color){assert.ok(count===3||count===5);record('line',Array.from(points.slice(start*2,(start+count)*2)),color);}
};
const P={resource(o){assert.equal(o.maxSegments,1024);return {};},beginFrame(){++begins;},commit(){++commits;}};
const context=vm.createContext({__foxPatchVariant:process.env.FOX_PATCH_VARIANT,pocket:{hone:H,kasane:{procedural:P,replace(fn){fn({background(){},image(){}});}},
  ayatsuri:{selectVisible(xy,depth,input,output,counts,order){
    assert.deepEqual(Array.from(input),expectedIndices,'source triples remain unchanged');
    const accepted=[];let back=0,degenerate=0;
    for(let k=0;k<input.length;k+=3){
      const a=input[k],b=input[k+1],c=input[k+2],area=(xy[b*2]-xy[a*2])*(xy[c*2+1]-xy[a*2+1])-(xy[b*2+1]-xy[a*2+1])*(xy[c*2]-xy[a*2]);
      if(area<0){back++;continue;}if(area===0){degenerate++;continue;}
      accepted.push({k,z:depth[a]+depth[b]+depth[c]});
    }
    if(order===1)accepted.sort((a,b)=>a.z-b.z||a.k-b.k);
    lastSelected=[];for(const face of accepted)for(let j=0;j<3;j++)lastSelected.push(input[face.k+j]);
    output.set(lastSelected);counts[0]=accepted.length;counts[1]=back;counts[2]=degenerate;
    lastDepth=Array.from(depth);
  },modelBuiltin(name){assert.equal(name,'fox-face');++assets;let dead=false;return {dispose(){if(!dead)--assets;dead=true;}};}}},
  AyatsuriClip:{nativeScene(){
    ++scenes;let dead=false,ready=false;const poses={},visible={};
    return {handle(i){return {place(p){assert.ok(!dead);poses[i]=[...p.position,...p.rotation,...p.scale];ready=false;},
      visible(v){assert.ok(!dead);visible[i]=v;ready=false;}};},
      aperture(h,v){assert.equal(v.length,15);return {bind(){return this;}};},
      light(p){record('native-light',p.direction);ready=false;},evaluate(){ready=true;},
      render(){assert.ok(ready&&!dead);record('native',poses,visible);},dispose(){if(!dead)--scenes;dead=true;}};
  }}});
vm.runInContext(fs.readFileSync('apps/lib/ayatsuri_visibility.js','utf8'),context);
vm.runInContext(fs.readFileSync('tools/character/fox_retopology_fixture.js','utf8'),context);
const api=context.FoxPatchStudy;
function draw(yaw,light=0){commands=[];api.draw(yaw,light);return crypto.createHash('sha256').update(JSON.stringify(commands)).digest('hex');}
function surfaceZ(x,y){
  for(let k=0;k<expectedIndices.length;k+=3){
    const a=expectedIndices[k]*3,b=expectedIndices[k+1]*3,c=expectedIndices[k+2]*3,
      ab=[expectedXYZ[b]-expectedXYZ[a],expectedXYZ[b+1]-expectedXYZ[a+1]],
      ac=[expectedXYZ[c]-expectedXYZ[a],expectedXYZ[c+1]-expectedXYZ[a+1]],
      det=ab[0]*ac[1]-ab[1]*ac[0],u=((x-expectedXYZ[a])*ac[1]-(y-expectedXYZ[a+1])*ac[0])/det,
      v=(ab[0]*(y-expectedXYZ[a+1])-ab[1]*(x-expectedXYZ[a]))/det;
    if(u>=-1e-7&&v>=-1e-7&&u+v<=1+1e-7)return expectedXYZ[a+2]+u*(expectedXYZ[b+2]-expectedXYZ[a+2])+v*(expectedXYZ[c+2]-expectedXYZ[a+2]);
  }
  assert.fail('attachment outside mesh');
}
const normalSums=Array.from({length:30},()=>[0,0,0]);
for(let k=0;k<expectedIndices.length;k+=3){
  const ids=expectedIndices.slice(k,k+3),a=ids[0]*3,b=ids[1]*3,c=ids[2]*3,
    u=[0,1,2].map(j=>expectedXYZ[b+j]-expectedXYZ[a+j]),v=[0,1,2].map(j=>expectedXYZ[c+j]-expectedXYZ[a+j]),
    n=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]];
  for(const id of ids)for(let j=0;j<3;j++)normalSums[id][j]+=n[j];
}
let minVisible=46,maxVisible=0;
for(let i=0;i<=60;i++){
  const yaw=-35+70*i/60;draw(yaw);const count=lastSelected.length/3;minVisible=Math.min(minVisible,count);maxVisible=Math.max(maxVisible,count);
  const angle=Math.fround(yaw*Math.PI/180),c=Math.fround(Math.cos(angle)),s=Math.fround(Math.sin(angle));
  for(let k=0;k<30;k++){
    assert.equal(lastDepth[k],Math.fround(-s*expectedXYZ[k*3]+c*expectedXYZ[k*3+2]));
    const n=normalSums[k],len=Math.hypot(...n),wanted=[(c*n[0]+s*n[2])/len,n[1]/len,(-s*n[0]+c*n[2])/len];
    for(let j=0;j<3;j++)assert.ok(Math.abs(lastNormals[k*3+j]-Math.round(wanted[j]*32767))<=1,'area-weighted normal');
  }
  for(let k=30;k<55;k++)assert.ok(Math.abs(lastSource[k*3+2]-surfaceZ(lastSource[k*3],lastSource[k*3+1])-(k<46?0:.15))<2e-6,'barycentric attachment');
}
assert.ok(minVisible<46);assert.equal(maxVisible,46);
for(const yaw of [-35,0,35]){draw(yaw);assert.equal(lastSelected.length/3,yaw===0?46:43);}

assert.equal(draw(0),draw(0),'same inputs repeat all primitive payloads');
const left=draw(-35),right=draw(35);assert.notEqual(left,right,'yaw produces projected geometry, not a fixed panel');
draw(0);const body=commands.find(row=>row[0]==='project')[1];draw(0,60);
assert.deepEqual(commands.find(row=>row[0]==='project')[1],body,'light-only leaves body geometry fixed');
for(const args of [[36,0],[NaN,0],[0,181],[0,Infinity]]){
  const old=begins;assert.throws(()=>api.draw(...args),/view range/);assert.equal(begins,old,'invalid view does not begin a candidate');
}
reenter=()=>{assert.throws(()=>api.draw(0),/busy/);assert.throws(()=>api.release(),/busy/);};draw(0);reenter=null;
const stable=draw(0),stats={};api.stats(stats);assert.equal(stats.vertices,30);assert.equal(stats.triangles,46);assert.equal(stats.nativePasses,1);
for(let i=0;i<3;i++){api.release();api.release();assert.equal(assets,0);assert.equal(scenes,0);assert.equal(draw(0),stable);}
api.release();assert.equal(assets,0);assert.equal(scenes,0);assert.equal(begins,commits);
console.log('FOX_RETOPOLOGY_PREFLIGHT_PASS',JSON.stringify(stats));
