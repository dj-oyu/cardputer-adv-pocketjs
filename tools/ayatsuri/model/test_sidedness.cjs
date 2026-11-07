'use strict';
const assert=require('node:assert/strict'),path=require('node:path');
const {compile}=require('./pack_model.cjs'),{design,write}=require('./sided_fixtures.cjs');
const {triangleFixture,makeGlb}=require('../asset/test_import.cjs');
const {importGlb}=require('../asset/import_glb.cjs'),{compileStatic}=require('../asset/compile_static.cjs');
const {batchStaticSurfaces}=require('../asset/compact_opaque/batch_static.cjs');
const baseline=process.argv[2]?require(path.resolve(process.argv[2])).compile:null;
const fixture=triangleFixture();fixture.json.materials[0].doubleSided=true;
const glb=makeGlb(fixture.json,fixture.bin),copy=Buffer.from(glb),ir=importGlb(glb),before=JSON.stringify(ir);
const scopes=new Map(ir.surfaces.map(s=>[s.stableId,'explicit-static-scope']));
const profile={'material/0':{albedo565:65535,hone:[0,1,0,1,0,0,0,0,0,0,0]}};
const invalidIr=structuredClone(ir);invalidIr.schema='hone-scene-ir-v1';
assert.throws(()=>compileStatic(invalidIr,{scopeBySurface:scopes,materials:profile}),/REQUIRES_IR_V2/);
const missingSide=structuredClone(ir);delete missingSide.materials[0].sidedness;
assert.throws(()=>compileStatic(missingSide,{scopeBySurface:scopes,materials:profile}),/MISSING_MATERIAL_SIDEDNESS/);
const result=compileStatic(ir,{scopeBySurface:scopes,materials:profile});
assert(glb.equals(copy));assert.equal(JSON.stringify(ir),before);assert.equal(result.manifest.nodes[1].materialSidedness,'double');
function sec(b,id){for(let j=0;j<b.readUInt32LE(12);j++){let at=32+20*j;if(b.readUInt16LE(at)===id)return b.subarray(b.readUInt32LE(at+4),b.readUInt32LE(at+4)+b.readUInt32LE(at+8));}return null;}
assert.equal(sec(result.bytes,13)[0],1);assert(sec(result.bytes,9).subarray(19).every(x=>x===0));
let mixed=design({side:'double',nodes:2});mixed.nodes[1].materialSidedness='single';
const bytes=compile(mixed);assert.equal(sec(bytes,5).length,144);assert.deepEqual([...sec(bytes,13)],[0,0,0,0,1,0,0,0]);
const s=ir.surfaces[0],surfaces=['single','double','single'].map((side,i)=>({...s,stableId:'s'+i,materialSidedness:side}));
const parts=batchStaticSurfaces(surfaces,{scopeBySurface:new Map(surfaces.map(s=>[s.stableId,'same'])),preserveOrder:true});
assert.equal(parts.length,3);assert.deepEqual(parts.map(p=>p.materialSidedness),['single','double','single']);
assert.deepEqual(parts.flatMap(p=>p.origins.map(o=>o.surface)),['s0','s1','s2']);
assert.throws(()=>compile({...design(),nodes:design({side:'double'}).nodes}),/sidedness schema/);
const invalid=design({side:'double'});delete invalid.nodes[1].materialSidedness;assert.throws(()=>compile(invalid),/explicit/);
let count=0;if(baseline){
 for(let j=0;j<100;j++){
  const m=design({nodes:1+j%3,nv:j%2?32:3,triangles:1+j%64});
  for(const n of m.nodes.slice(1)){n.pose[0]=j/7;n.pose[1]=-j/11;n.material[0]=(j%9)/10;n.mesh.albedoA=j*599%65536;n.mesh.albedoB=j*773%65536;}
  assert(compile(m).equals(baseline(m)));const v2=structuredClone(m);v2.schema='ayatsuri-native-model-design-v2';v2.nodes.slice(1).forEach(n=>n.materialSidedness='single');assert(compile(v2).equals(baseline(m)));count++;
 }
}
write();console.log('SIDED_PIPELINE_PASS import_batch_compile immutable mixed_material no_reorder reserved_zero legacy_byte_cases='+count);
