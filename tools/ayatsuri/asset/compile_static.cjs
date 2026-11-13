'use strict';
// Exact static IR -> bounded parts -> immutable AYNM. No reduction, authoring
// mutation, asset defaults, implicit batching scopes or sidedness overrides.
const {batchStaticSurfaces}=require('./compact_opaque/batch_static.cjs');
const {compile}=require('../model/pack_model.cjs');
function compileStatic(ir,{scopeBySurface,materials,scale=1}={}){
 if(!ir||!['hone-scene-ir-v1','hone-scene-ir-v2'].includes(ir.schema)||!Array.isArray(ir.materials)||!Array.isArray(ir.surfaces)||!Number.isFinite(scale)||scale<=0)throw Error('INVALID_IR');
 const ids=new Set();
 const sideById=new Map(ir.materials.map(m=>{
  if(typeof m.stableId!=='string'||!m.stableId||ids.has(m.stableId))throw Error('INVALID_MATERIAL_ID');ids.add(m.stableId);
  if(ir.schema==='hone-scene-ir-v1'&&m.sidedness!==undefined)throw Error('SIDEDNESS_REQUIRES_IR_V2');
  const side=ir.schema==='hone-scene-ir-v1'?'single':m.sidedness;
  if(!['single','double'].includes(side))throw Error('MISSING_MATERIAL_SIDEDNESS');
  return [m.stableId,side];
 }));
 const surfaces=ir.surfaces.map(s=>{
  const id=s.materialId??'default',side=s.materialId===null?(ir.schema==='hone-scene-ir-v1'?'single':ir.defaultMaterial?.sidedness):sideById.get(id);
  if(!side)throw Error('UNKNOWN_MATERIAL');
  return {...s,materialId:id,materialSidedness:side};
 });
 const parts=batchStaticSurfaces(surfaces,{scopeBySurface,preserveOrder:true});
 const root={index:0,name:'root',parent:-1,kind:'group',enabled:true,pose:[0,0,0,0,0,0,1,1,1]};
 const nodes=[root,...parts.map((p,i)=>{
  const m=materials?.[p.materialId];
  if(!m||!Array.isArray(m.hone)||m.hone.length!==11||!Number.isInteger(m.albedo565)||m.albedo565<0||m.albedo565>65535)throw Error('MATERIAL_MAPPING_REQUIRED');
  return {...root,index:i+1,name:'part-'+i,parent:0,kind:'gouraudMesh',materialSidedness:p.materialSidedness,
   material:m.hone.slice(),color:0xffffff,thickness:1,opacity:1,edgeSoftness:0,
   mesh:{vertices:p.positions.flat().map(v=>v*scale),normals:p.normals.flat(),indices:p.triangles.flat(),mask:p.positions.map(()=>-1),holes:[],order:'authored',albedoA:m.albedo565,albedoB:m.albedo565,thickness:255}};
 })];
 const manifest={schema:'ayatsuri-native-model-design-v2',nodes};
 return {manifest,parts,bytes:compile(manifest)};
}
module.exports={compileStatic};
