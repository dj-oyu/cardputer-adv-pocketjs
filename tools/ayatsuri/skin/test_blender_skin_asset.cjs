'use strict';
const fs=require('fs'),assert=require('node:assert/strict'),{build}=require('./compile_blender_skin_fixture.cjs');
const r=JSON.parse(fs.readFileSync(process.argv[2])),a=build(r),basis=([x,y,z])=>[x,-z,-y];
for(let s=0;s<a.samples.length;s++)for(let i=0;i<6;i++){
 const p=[...a.V.slice(i*6,i*6+3)],out=[0,0,0];
 for(let k=0;k<4;k++){const j=a.J[i*4+k],w=a.W[i*4+k]/65535,IB=a.IB.slice(j*12,j*12+12),G=a.samples[s].G.slice(j*12,j*12+12),q=[0,1,2].map(c=>IB[c*4+3]+p.reduce((v,x,n)=>v+IB[c*4+n]*x,0));for(let c=0;c<3;c++)out[c]+=w*(G[c*4+3]+q.reduce((v,x,n)=>v+G[c*4+n]*x,0));}
 const expected=basis(r.samples[s].positions[i]);assert(out.every((x,k)=>Math.abs(x-expected[k])<2e-5));const analytic=[p[0],p[1],s?-6*[0,1/3,1][i%3]:0];assert(out.every((x,k)=>Math.abs(x-analytic[k])<2e-5));
}
for(const mutate of [x=>x.authoring.constraints=1,x=>x.authoring.drivers=1,x=>x.authoring.shapeKeys=true,x=>x.samples[1].matrices[1][0][0]=0,x=>x.weights[0]=[0,0],x=>x.weights[0]=[-1,2],x=>x.weights[0]=[NaN,1],x=>x.faces[0].reverse(),x=>x.normals[0]=[0,0,0],x=>x.restMatrices[1][0][3]=16,x=>x.vertices.push([0,0,0])]){const bad=structuredClone(r);mutate(bad);assert.throws(()=>build(bad));}
console.log(JSON.stringify({status:'PASS',vertices:6,joints:2,poses:2,bytes:a.model.length,scope:'actual Blender evaluated position and analytic LBS; no QuickJS/native acceptance yet'}));
