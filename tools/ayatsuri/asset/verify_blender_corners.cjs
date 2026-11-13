'use strict';
const assert=require('node:assert/strict'),fs=require('fs'),path=require('path');
const {importGlb}=require('./import_glb.cjs'),{compileStatic}=require('./compile_static.cjs');
const dir=process.argv[2];assert(dir);const bound=Math.asin(Math.sqrt(3)*.5e-4)+1e-6;
const basis=([x,y,z])=>[x,-z,-y];
function angle(a,b){const c=[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];return Math.atan2(Math.hypot(...c),a.reduce((s,x,k)=>s+x*b[k],0));}
function verify(s,m){
 assert.equal(s.triangles.length,2);assert.equal(s.positions.length,6,'must preserve corner splits');
 const ids=s.positions.map(p=>{const ds=m.vertices.map(v=>Math.hypot(...basis(v).map((x,k)=>x-p[k]))),d=Math.min(...ds);assert(d<1e-6);return ds.indexOf(d);});
 let maximum=0;const used=new Set();
 for(const triangle of s.triangles){const mapped=triangle.map(i=>ids[i]);let match;
  for(let f=0;f<m.faces.length;f++){const expected=[m.faces[f][0],m.faces[f][2],m.faces[f][1]];for(let r=0;r<3;r++)if(mapped.every((v,k)=>v===expected[(r+k)%3]))match=f;}
  assert.notEqual(match,undefined,'winding/source face');assert(!used.has(match));used.add(match);
  for(const index of triangle){const vertex=ids[index],corner=m.faces[match].indexOf(vertex),raw=m.corners[match*3+corner];assert.equal(raw.vertex,vertex);assert(Array.isArray(raw.normal)&&raw.normal.length===3&&[0,1,2].every(k=>Number.isFinite(raw.normal[k])),'source normal finite');assert(Math.abs(Math.hypot(...raw.normal)-1)<1e-6,'source normal unit');const n=s.normals[index];assert(n.every(Number.isFinite));assert(Math.abs(Math.hypot(...n)-1)<1e-6);const error=angle(n,basis(raw.normal));assert(error<=bound,'corner direction');maximum=Math.max(maximum,error);}
 }
 return maximum;
}
for(const name of ['flat','custom']){
 const m=JSON.parse(fs.readFileSync(path.join(dir,name+'.json'))),ir=importGlb(fs.readFileSync(path.join(dir,name+'.glb')));assert.equal(ir.surfaces.length,1);const s=ir.surfaces[0],maximum=verify(s,m);
 for(const invalid of [[0,0,0],[NaN,0,1],[0,0,2]]){const badSource=structuredClone(m);badSource.corners[0].normal=invalid;assert.throws(()=>verify(s,badSource));}
 // Wrongly welded/averaged normals must not pass even when positions are equal.
 const averaged=structuredClone(s);averaged.normals=averaged.normals.map(()=>[0,-1,0]);assert.throws(()=>verify(averaged,m));
 const reversed=structuredClone(s);reversed.normals[0]=reversed.normals[0].map(x=>-x);assert.throws(()=>verify(reversed,m));
 const compiled=compileStatic(ir,{scale:40,scopeBySurface:new Map([[s.stableId,'rigid-corner']]),materials:{[s.materialId??'default']:{albedo565:0xffff,hone:[.6,.4,.1,16,0,.3,.3,.3,.2,.3,.4]}}});
 const bytes=compiled.bytes,sections=new Map();
 for(let i=0;i<bytes.readUInt32LE(12);i++){const p=32+i*20;sections.set(bytes.readUInt16LE(p),{offset:bytes.readUInt32LE(p+4),count:bytes.readUInt32LE(p+12),stride:bytes.readUInt32LE(p+16)});}
 const d=sections.get(9),v=sections.get(10),t=sections.get(11);assert.equal(d.count,1);assert.equal(d.stride,32);assert.equal(v.stride,28);assert.equal(t.stride,6);
 const first=bytes.readUInt16LE(d.offset),count=bytes.readUInt16LE(d.offset+2),face=bytes.readUInt16LE(d.offset+4),faces=bytes.readUInt16LE(d.offset+6),packed={positions:[],normals:[],triangles:[]};
 assert(first+count<=v.count&&face+faces<=t.count);
 for(let i=0;i<count;i++){const p=v.offset+(first+i)*28;packed.positions.push([0,1,2].map(k=>bytes.readFloatLE(p+4*k)/40));packed.normals.push([0,1,2].map(k=>bytes.readFloatLE(p+12+4*k)));}
 for(let i=0;i<faces;i++)packed.triangles.push([0,1,2].map(k=>bytes.readUInt16LE(t.offset+(face+i)*6+2*k)));
 const packedMaximum=verify(packed,m),badPacked=structuredClone(packed);badPacked.normals[0]=[0,0,0];assert.throws(()=>verify(badPacked,m));
 console.log(JSON.stringify({case:name,status:'PASS',vertices:s.positions.length,faces:s.triangles.length,maxNormalAngle:maximum,packedMaxNormalAngle:packedMaximum,bytes:compiled.bytes.length,parts:compiled.parts.length,scope:'synthetic split/custom source to imported and packed normals; pixels/device not checked'}));
}
