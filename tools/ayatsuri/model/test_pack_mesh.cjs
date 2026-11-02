// Offline mesh contracts. Real rendering and ownership use the C/QuickJS tests.
'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {compile}=require('./pack_model.cjs');
const base={index:0,name:'root',parent:-1,kind:'group',enabled:true,pose:[0,0,0,0,0,0,1,1,1]};
const patch={...base,index:1,name:'patch',parent:0,kind:'gouraudMesh',
  material:[.4,.6,.025,4,.08,1,.73,.44,0,0,0],color:0xe47b30,thickness:1,opacity:1,edgeSoftness:0,
  mesh:{vertices:[-10,-10,0,10,-10,0,-10,10,0,10,10,0],indices:[0,1,2,1,3,2],mask:[-10,-10,10,10],
    holes:[[-2,-2,0,2,-2,0,0,2,0]],order:'centroid-depth',albedoA:0xe3c6,albedoB:0xff57,thickness:175}};
const fixture={schema:'ayatsuri-native-model-design-v1',nodes:[base,patch]},bytes=compile(fixture);
assert.ok(bytes.equals(compile(structuredClone(fixture))));
function section(data,type){for(let i=0;i<data.readUInt32LE(12);i++){const d=32+i*20;if(data.readUInt16LE(d)===type)return data.subarray(data.readUInt32LE(d+4),data.readUInt32LE(d+4)+data.readUInt32LE(d+8));}assert.fail('missing section '+type);}
assert.equal(bytes.readUInt32LE(12),11);assert.equal(section(bytes,9).length,32);
assert.equal(section(bytes,10).length,112);assert.equal(section(bytes,11).length,12);assert.equal(section(bytes,12).length,36);
assert.equal(section(bytes,1)[18],3);
for(let i=0;i<4;i++)assert.deepEqual([12,16,20].map(at=>section(bytes,10).readFloatLE(i*28+at)),[0,0,1]);
let rejected=0;
for(const change of [
  m=>m.vertices.pop(),m=>m.vertices[0]=Infinity,m=>m.vertices[0]=4097,m=>m.vertices.fill(0),
  m=>m.indices.pop(),m=>m.indices[0]=4,m=>m.indices[0]=1,m=>m.indices[0]=.5,
  m=>m.mask.pop(),m=>m.mask[0]=NaN,m=>m.mask[0]=4097,
  m=>m.normals=Array(12).fill(0),m=>m.normals=Array(12).fill(1),m=>m.normals=Array(11).fill(0),
  m=>m.holes.push(m.holes[0],m.holes[0]),m=>m.holes[0].pop(),m=>m.holes[0][0]=NaN,
  m=>m.mask=new Array(4),m=>{delete m.vertices[0];m.normals=[0,0,1,0,0,1,0,0,1,0,0,1];},
  m=>m.holes=[new Array(9)],
  m=>m.holes[0][0]=4097,m=>m.order='reverse',m=>m.albedoA=65536,m=>m.albedoB=-1,m=>m.thickness=256]){
  const bad=structuredClone(fixture);change(bad.nodes[1].mesh);assert.throws(()=>compile(bad));rejected++;
}
for(const [field,values] of [['thickness',[0,.5]],['opacity',[0,.5]],['edgeSoftness',[.5,1]]])for(const value of values){
  const bad=structuredClone(fixture);bad.nodes[1][field]=value;assert.throws(()=>compile(bad),/mesh common/);rejected++;
}
const two=structuredClone(fixture);two.nodes.push({...structuredClone(patch),index:2,name:'second',pose:[20,0,0,0,0,0,1,1,1]});
const both=compile(two),descriptors=section(both,9);assert.equal(descriptors.readUInt16LE(32),4);
assert.equal(descriptors.readUInt16LE(36),2);assert.equal(descriptors.readUInt16LE(40),3);
const explicit=structuredClone(fixture);explicit.nodes[1].mesh.normals=[0,0,1,0,0,1,0,0,1,0,0,1];
assert.ok(bytes.equals(compile(explicit)),'computed and supplied F32 normals agree');
const none=structuredClone(fixture);none.nodes[1].mesh.holes=[];compile(none);
const mixed=structuredClone(fixture);mixed.nodes.push({...structuredClone(patch),index:2,name:'analytic',kind:'ellipsoid',radii:[1,2,3],
  pattern:{kind:'localPlane',normal:[0,1,0],offset:0,feather:0,color:0xff0000}});
const combined=compile(mixed);assert.equal(combined.readUInt32LE(12),12);assert.equal(section(combined,8).length,32);
const out=path.resolve('.cache/ayatsuri-native/mesh.aynm');fs.mkdirSync(path.dirname(out),{recursive:true});fs.writeFileSync(out,bytes);
console.log('AYNM_MESH_PACK_PASS',JSON.stringify({bytes:bytes.length,nodes:2,triangles:2,rejected,sha256:crypto.createHash('sha256').update(bytes).digest('hex')}));
