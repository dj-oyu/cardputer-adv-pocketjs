'use strict';
const fs=require('fs'),path=require('path'),assert=require('node:assert/strict');
const {compile}=require('../model/pack_model.cjs');
const xyz=([x,y,z])=>[x,-z,-y],finite=(v,n)=>Array.isArray(v)&&v.length===n&&Array.from({length:n},(_,i)=>Number.isFinite(v[i])).every(Boolean);
function build(r){
 assert.equal(r.schema,'synthetic-blender-skin-v1');assert.deepEqual(r.authoring,{scope:'fixed-public-strip-only',objectTransforms:'identity',bones:['root','child'],modifiers:['ARMATURE'],constraints:0,drivers:0,shapeKeys:false});assert.equal(r.vertices.length,6);assert.equal(r.normals.length,6);assert.equal(r.weights.length,6);assert.equal(r.faces.length,4);
 const expected=[[-30,-15,0],[0,-15,0],[30,-15,0],[-30,15,0],[0,15,0],[30,15,0]];
 for(let i=0;i<6;i++){assert(finite(r.vertices[i],3)&&finite(r.normals[i],3));assert.deepEqual(xyz(r.vertices[i]).map(x=>x===0?0:x),expected[i]);assert(Math.abs(Math.hypot(...r.normals[i])-1)<1e-6);assert.deepEqual(xyz(r.normals[i]).map(x=>x===0?0:x),[0,0,1]);}
 assert.deepEqual(r.faces,[[0,4,1],[0,3,4],[1,5,2],[1,4,5]]);
 const matrix=m=>{assert(Array.isArray(m)&&m.length===4&&m.every(row=>finite(row,4)));assert.deepEqual(m[3],[0,0,0,1]);const B=[[1,0,0],[0,0,-1],[0,-1,0]],out=[];for(let i=0;i<3;i++){for(let j=0;j<3;j++)out.push(B[i].reduce((a,x,k)=>a+x*B[j].reduce((b,y,l)=>b+m[k][l]*y,0),0));out.push(B[i].reduce((a,x,k)=>a+x*m[k][3],0));}return out;};
 const identity=[1,0,0,0,0,1,0,0,0,0,1,0],rest=r.restMatrices.map(matrix);assert.equal(rest.length,2);assert.deepEqual(rest[0],identity);const child=identity.slice();child[3]=15;assert.deepEqual(rest[1],child);
 const IB=new Float32Array([...identity,...identity]);IB[15]=-15;const V=new Float32Array(36),J=new Uint16Array(24),W=new Uint16Array(24);
 for(let i=0;i<6;i++){V.set([...xyz(r.vertices[i]),...xyz(r.normals[i])],i*6);const w=r.weights[i];assert(finite(w,2)&&w.every(x=>x>=0)&&Math.abs(w[0]+w[1]-1)<1e-6);assert(Math.abs(w[1]-[0,1/3,1][i%3])<1e-6);const scaled=w.map(x=>x/(w[0]+w[1])*65535),q=scaled.map(Math.floor),order=[0,1].sort((a,b)=>(scaled[b]-q[b])-(scaled[a]-q[a])||a-b);for(let k=0;k<65535-q[0]-q[1];k++)q[order[k]]++;J[i*4+1]=1;W.set(q,i*4);assert.equal(W[i*4]+W[i*4+1],65535);}
 assert.deepEqual(r.samples.map(s=>s.name),['rest','child_y6']);const samples=r.samples.map((s,i)=>{assert.equal(s.matrices.length,2);assert.equal(s.positions.length,6);assert.equal(s.normals.length,6);assert(s.positions.every(p=>finite(p,3))&&s.normals.every(p=>finite(p,3)));const matrices=s.matrices.map(matrix),posed=child.slice();if(i)posed[11]=-6;assert.deepEqual(matrices[0],identity);assert.deepEqual(matrices[1],posed);return {name:s.name,G:new Float32Array(matrices.flat())};});
 const model=compile({schema:'ayatsuri-native-model-design-v1',nodes:[{index:0,name:'strip',parent:-1,kind:'gouraudMesh',enabled:true,pose:[0,0,0,0,0,0,1,1,1],material:[1,0,0,4,0,0,0,0,0,0,0],color:0,thickness:1,opacity:1,edgeSoftness:0,mesh:{vertices:expected.flat(),indices:r.faces.flatMap(([a,b,c])=>[a,c,b]),mask:Array(6).fill(-1),holes:[],order:'authored',albedoA:0xf800,albedoB:0xf800,thickness:255}}]});
 return {V,J,W,IB,samples,model};
}
module.exports={build};
if(require.main===module){const r=JSON.parse(fs.readFileSync(process.argv[2])),out=process.argv[3],a=build(r);fs.mkdirSync(out,{recursive:false});fs.writeFileSync(path.join(out,'model.aynm'),a.model);const arr=(n,v)=>`${n}:new ${v.constructor.name}(${JSON.stringify([...v])})`;fs.writeFileSync(path.join(out,'arrays.cjs'),`module.exports={${['V','J','W','IB'].map(n=>arr(n,a[n])).join(',')},samples:[${a.samples.map(s=>`{name:${JSON.stringify(s.name)},${arr('G',s.G)}}`).join(',')}]};\n`);}
