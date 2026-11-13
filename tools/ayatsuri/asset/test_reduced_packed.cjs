'use strict';
// Read actual trusted compiler output, not a second copy of pre-pack geometry.
const assert=require('node:assert/strict');
const {fixture}=require('./synthetic_grid.cjs');
const {importGlb}=require('./import_glb.cjs');
const {reduceSurface,sampleErrors}=require('./reduce.cjs');
const {compileStatic}=require('./compile_static.cjs');
function decode(bytes,scale){
 const section=new Map();for(let i=0;i<bytes.readUInt32LE(12);i++){
  const at=32+20*i;section.set(bytes.readUInt16LE(at),{offset:bytes.readUInt32LE(at+4),count:bytes.readUInt32LE(at+12),stride:bytes.readUInt32LE(at+16)});
 }
 const descriptors=section.get(9),vertices=section.get(10),indices=section.get(11),out={positions:[],normals:[],triangles:[]};
 assert.equal(descriptors.stride,32);assert.equal(vertices.stride,28);assert.equal(indices.stride,6);
 for(let i=0;i<descriptors.count;i++){
  const at=descriptors.offset+i*32,start=bytes.readUInt16LE(at),count=bytes.readUInt16LE(at+2),first=bytes.readUInt16LE(at+4),faces=bytes.readUInt16LE(at+6),base=out.positions.length;
  assert(start+count<=vertices.count&&first+faces<=indices.count);
  for(let j=0;j<count;j++){
   const p=vertices.offset+(start+j)*28;
   out.positions.push([0,1,2].map(k=>bytes.readFloatLE(p+4*k)/scale));
   out.normals.push([0,1,2].map(k=>bytes.readFloatLE(p+12+4*k)));
  }
  for(let j=0;j<faces;j++)out.triangles.push([0,1,2].map(k=>{
   const v=bytes.readUInt16LE(indices.offset+(first+j)*6+2*k);assert(v<count);return base+v;
  }));
 }
 return out;
}
function gate(errors,profile){
 if(errors.distance.max>profile.distance||errors.normalDegrees.max>profile.normalDegrees)throw Error('PACKED_SOURCE_QUALITY');
}
const ir=importGlb(fixture(9,.1).glb),source=ir.surfaces[0],before=structuredClone(ir),scale=40;
const options={scale,scopeBySurface:new Map([[source.stableId,'rigid-source']]),materials:{'material/0':{albedo565:0xfc00,hone:[.6,.4,.1,16,0,.3,.3,.3,.2,.3,.4]}}};
const results=[];
for(const targetVertices of [81,65,49]){
 const reduced=reduceSurface(source,{targetVertices}).surface;
 const compiled=compileStatic({...ir,surfaces:[reduced]},options),packed=decode(compiled.bytes,scale);
 const reduction=sampleErrors(source,reduced),quantization=sampleErrors(reduced,packed),final=sampleErrors(source,packed);
 gate(final,{distance:.01,normalDegrees:8});
 if(targetVertices<81)assert.throws(()=>gate(final,{distance:0,normalDegrees:0}),/PACKED_SOURCE_QUALITY/);
 assert(final.distance.max>0);assert(quantization.distance.max<1e-6);
 const changed=structuredClone(packed);changed.positions[0][2]+=.1;
 assert.throws(()=>gate(sampleErrors(source,changed),{distance:.01,normalDegrees:8}),/PACKED_SOURCE_QUALITY/);
 results.push({targetVertices,coldBytes:compiled.bytes.length,parts:compiled.parts.length,reduction,quantization,final});
}
assert.deepEqual(ir,before);
console.log(JSON.stringify({status:'PASS',scope:'SYNTHETIC CURVED SAMPLED GEOMETRY, NOT PIXEL/BLENDER/ART ACCEPTANCE',results}));
