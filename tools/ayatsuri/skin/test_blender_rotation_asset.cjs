"use strict";
const fs=require('fs'),assert=require('node:assert/strict'),{build}=require('./compile_blender_rotation_fixture.cjs');
const r=JSON.parse(fs.readFileSync(process.argv[2])),a=build(r),basis=([x,y,z])=>[x,-z,-y];
let maxError=0;for(let s=0;s<3;s++)for(let i=0;i<6;i++){
 const p=[...a.V.slice(i*6,i*6+3)],out=[0,0,0];
 for(let k=0;k<4;k++){const j=a.J[i*4+k],w=a.W[i*4+k]/65535,IB=a.IB.slice(j*12,j*12+12),G=a.samples[s].G.slice(j*12,j*12+12),q=[0,1,2].map(c=>IB[c*4+3]+p.reduce((v,x,n)=>v+IB[c*4+n]*x,0));for(let c=0;c<3;c++)out[c]+=w*(G[c*4+3]+q.reduce((v,x,n)=>v+G[c*4+n]*x,0));}
 const expected=basis(r.samples[s].positions[i]);out.forEach((x,k)=>{maxError=Math.max(maxError,Math.abs(x-expected[k]));assert(Math.abs(x-expected[k])<2e-5);});
}
let rejected=0;for(const mutate of [x=>delete x.samples[2].matrices[1][1][1],x=>delete x.samples[2].positions[0][0],x=>delete x.samples[2].matrices[1],x=>x.schema='synthetic-blender-skin-v1',x=>x.samples.pop(),x=>x.samples[2].name='arbitrary',x=>x.samples[2].matrices[1][1][2]*=-1,x=>x.samples[2].matrices[1][0][3]=16,x=>x.samples[2].matrices[0][0][3]=1,x=>x.samples[2].matrices[1][1][1]=NaN,x=>x.samples[2].normals[0]=[0,0,0],x=>x.samples[2].positions[0]=[NaN,0,0],x=>x.authoring.constraints=1,x=>x.weights[0]=[0,0],x=>x.faces[0].reverse()]){const bad=structuredClone(r);mutate(bad);assert.throws(()=>build(bad));rejected++;}
console.log(JSON.stringify({status:'PASS',poses:3,maxPositionError:maxError,rejected,bytes:a.model.length,scope:'Blender evaluated positions vs typedarray LBS; independent normal oracle separate; no native acceptance'}));
