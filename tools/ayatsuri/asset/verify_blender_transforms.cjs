'use strict';
// Independent closed-form Rz/S oracle; does not use importer matrix helpers.
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {importGlb}=require('./import_glb.cjs'),{compileStatic}=require('./compile_static.cjs');
const {makeGlb}=require('./test_import.cjs');
const dir=process.argv[2];assert(dir);
const unit=v=>{const l=Math.hypot(...v);assert(l>0);return v.map(x=>x/l);};
const basis=([x,y,z])=>[x,-z,-y];
const rotate=(v,a)=>[Math.cos(a)*v[0]-Math.sin(a)*v[1],Math.sin(a)*v[0]+Math.cos(a)*v[1],v[2]];
const angle=(a,b)=>Math.atan2(Math.hypot(a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]),a.reduce((s,x,k)=>s+x*b[k],0));
const key=t=>[0,1,2].map(r=>[t[r],t[(r+1)%3],t[(r+2)%3]].join(':')).sort()[0];
function oracle(m){const scales=m.scale.map(Math.abs),condition=Math.max(...scales)/Math.min(...scales);
 // Blender4.3.2 rounds normal components to four decimals then normalizes.
 // Relative inverse-scale error <= condition * input error; bound is analytic,
 // not fit to observations. F32 arithmetic margins explicitly retained.
 return {positions:m.vertices.map(p=>basis(rotate(p.map((x,k)=>x*m.scale[k]),m.rotationZ).map((x,k)=>x+m.translation[k]))),normal:unit(basis(rotate([1,2,3].map((x,k)=>x/m.scale[k]),m.rotationZ))),face:m.scale.reduce((a,x)=>a*x,1)>0?[0,2,1]:[0,1,2],bound:Math.asin(condition*(Math.sqrt(3)*.5e-4+1e-6))+2e-6};}
function verify(s,o){assert.equal(s.positions.length,3);assert.equal(s.normals.length,3);assert.equal(s.triangles.length,1);
 const ids=s.positions.map(p=>{const d=o.positions.map(q=>Math.hypot(...p.map((x,k)=>x-q[k]))),best=Math.min(...d);assert(best<2e-6,'position');return d.indexOf(best);});assert.equal(new Set(ids).size,3);
 assert.equal(key(s.triangles[0].map(i=>ids[i])),key(o.face),'determinant winding');let max=0;
 for(const n of s.normals){assert(n.every(Number.isFinite));assert(Math.abs(Math.hypot(...n)-1)<2e-6,'unit');const a=angle(n,o.normal);assert(a<=o.bound,'inverse-transpose normal');max=Math.max(max,a);}return max;
}
function decode(b,scale){const sections=new Map();for(let i=0;i<b.readUInt32LE(12);i++){const a=32+20*i;sections.set(b.readUInt16LE(a),{offset:b.readUInt32LE(a+4),count:b.readUInt32LE(a+12),stride:b.readUInt32LE(a+16)});}const d=sections.get(9),v=sections.get(10),ix=sections.get(11);assert.equal(d.count,1);assert.equal(v.stride,28);assert.equal(ix.stride,6);const start=b.readUInt16LE(d.offset),count=b.readUInt16LE(d.offset+2),first=b.readUInt16LE(d.offset+4),faces=b.readUInt16LE(d.offset+6);assert(start+count<=v.count&&first+faces<=ix.count);return {positions:Array.from({length:count},(_,j)=>[0,1,2].map(k=>b.readFloatLE(v.offset+(start+j)*28+k*4)/scale)),normals:Array.from({length:count},(_,j)=>[0,1,2].map(k=>b.readFloatLE(v.offset+(start+j)*28+12+k*4))),triangles:Array.from({length:faces},(_,j)=>[0,1,2].map(k=>b.readUInt16LE(ix.offset+(first+j)*6+k*2)))};}
function sourceNormals(normals){assert(Array.isArray(normals)&&normals.length===3);
 for(let i=0;i<3;i++){const n=normals[i];assert(Array.isArray(n)&&n.length===3&&[0,1,2].every(k=>Number.isFinite(n[k])));assert(Math.abs(Math.hypot(...n)-1)<1e-6);assert(angle(n,unit([1,2,3]))<1e-6);}}
let negatives=0;for(const name of ['unit_scale','nonuniform','mirrored','double_mirror']){
 const m=JSON.parse(fs.readFileSync(path.join(dir,name+'.json'))),bytes=fs.readFileSync(path.join(dir,name+'.glb')),ir=importGlb(bytes),o=oracle(m);assert.equal(ir.surfaces.length,1);const s=ir.surfaces[0];
 // Independently check authored tilted triangle against its known plane normal.
 sourceNormals(m.cornerNormals);
 for(const invalid of [[],[[0,0,0],...m.cornerNormals.slice(1)],[[NaN,0,1],...m.cornerNormals.slice(1)]]){assert.throws(()=>sourceNormals(invalid));negatives++;}
 const imported=verify(s,o),c=compileStatic(ir,{scale:40,scopeBySurface:new Map([[s.stableId,'transform']]),materials:{[s.materialId??'default']:{albedo565:0xffff,hone:[.6,.4,.1,16,0,.3,.3,.3,.2,.3,.4]}}}),packed=decode(c.bytes,40),packedError=verify(packed,o);
 for(const mutate of [t=>t.normals[0]=t.normals[0].map(x=>-x),t=>[t.triangles[0][1],t.triangles[0][2]]=[t.triangles[0][2],t.triangles[0][1]],t=>t.normals[0][0]=NaN,t=>t.positions[0][0]+=.01,t=>t.normals[0]=t.normals[0].map(x=>x*2)]){const bad=structuredClone(packed);mutate(bad);assert.throws(()=>verify(bad,o));negatives++;}
 if(name!=='unit_scale'){const bad=structuredClone(s),wrong=unit(basis(rotate([1,2,3].map((x,k)=>x*m.scale[k]),m.rotationZ)));bad.normals=bad.normals.map(()=>wrong);assert.throws(()=>verify(bad,o));negatives++;}
 const jsonLength=bytes.readUInt32LE(12),g=JSON.parse(bytes.toString('utf8',20,20+jsonLength)),at=20+jsonLength,bin=bytes.subarray(at+8,at+8+bytes.readUInt32LE(at));
 const bad=structuredClone(g),node=bad.nodes.find(n=>n.mesh!==undefined);delete node.matrix;node.scale=[0,1,1];assert.throws(()=>importGlb(makeGlb(bad,bin)),e=>e.code==='INVALID_GLB');negatives++;
 const shear=structuredClone(g),sn=shear.nodes.find(n=>n.mesh!==undefined);delete sn.translation;delete sn.rotation;delete sn.scale;sn.matrix=[1,0,0,0,.2,1,0,0,0,0,1,0,0,0,0,1];assert.throws(()=>importGlb(makeGlb(shear,bin)),e=>e.code==='INVALID_GLB');negatives++;
 console.log(JSON.stringify({name,status:'PASS',sourceVersion:m.version,importedMaxAngle:imported,packedMaxAngle:packedError,bound:o.bound,bytes:c.bytes.length}));
}console.log(JSON.stringify({status:'PASS',negatives,scope:'4 public tilted-triangle object TRS exports, imported+packed normals/winding only; no arbitrary scene, hierarchy export, pixel, art or device acceptance'}));
