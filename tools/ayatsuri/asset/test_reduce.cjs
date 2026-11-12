'use strict';
const assert=require('node:assert/strict');
const {reduceSurface,partition,sampleErrors}=require('./reduce.cjs');
const {importGlb}=require('./import_glb.cjs');
const {compileStatic}=require('./compile_static.cjs');
function fixture(size=9,bump=0){
  const positions=[],normals=[],indices=[];
  for(let y=0;y<size;y++)for(let x=0;x<size;x++){
    const u=x/(size-1)*2-1,v=y/(size-1)*2-1;positions.push(u,v,bump*(1-u*u)*(1-v*v));
    const a=2*bump*u*(1-v*v),b=2*bump*v*(1-u*u),d=Math.hypot(a,b,1);normals.push(a/d,b/d,1/d);
  }
  for(let y=0;y<size-1;y++)for(let x=0;x<size-1;x++){const a=y*size+x,b=a+1,c=a+size,d=c+1;indices.push(a,b,d,a,d,c);}
  const p=Buffer.alloc(positions.length*4),n=Buffer.alloc(normals.length*4),t=Buffer.alloc(indices.length*2);
  positions.forEach((v,i)=>p.writeFloatLE(v,i*4));normals.forEach((v,i)=>n.writeFloatLE(v,i*4));indices.forEach((v,i)=>t.writeUInt16LE(v,i*2));
  const bounds=[0,1,2].map(j=>Array.from({length:size*size},(_,i)=>p.readFloatLE((i*3+j)*4))),bin=Buffer.concat([p,n,t]),doc={asset:{version:'2.0',generator:'synthetic editable grid fixture, not Blender or fox'},scene:0,scenes:[{nodes:[0]}],nodes:[{mesh:0,name:'synthetic-grid'}],meshes:[{primitives:[{attributes:{POSITION:0,NORMAL:1},indices:2,material:0,mode:4}]}],materials:[{name:'explicit-opaque',pbrMetallicRoughness:{baseColorFactor:[1,.5,.1,1],metallicFactor:0,roughnessFactor:.8}}],buffers:[{byteLength:bin.length}],bufferViews:[{buffer:0,byteOffset:0,byteLength:p.length},{buffer:0,byteOffset:p.length,byteLength:n.length},{buffer:0,byteOffset:p.length+n.length,byteLength:t.length}],accessors:[{bufferView:0,componentType:5126,count:size*size,type:'VEC3',min:bounds.map(v=>Math.min(...v)),max:bounds.map(v=>Math.max(...v))},{bufferView:1,componentType:5126,count:size*size,type:'VEC3'},{bufferView:2,componentType:5123,count:indices.length,type:'SCALAR'}]};
  const raw=Buffer.from(JSON.stringify(doc)),json=Buffer.alloc(Math.ceil(raw.length/4)*4,32);raw.copy(json);const data=Buffer.alloc(Math.ceil(bin.length/4)*4);bin.copy(data);
  const glb=Buffer.alloc(28+json.length+data.length);glb.writeUInt32LE(0x46546c67);glb.writeUInt32LE(2,4);glb.writeUInt32LE(glb.length,8);glb.writeUInt32LE(json.length,12);glb.writeUInt32LE(0x4e4f534a,16);json.copy(glb,20);const off=20+json.length;glb.writeUInt32LE(data.length,off);glb.writeUInt32LE(0x004e4942,off+4);data.copy(glb,off+8);return {glb,editable:{positions,normals,indices}};
}

const ir=importGlb(fixture().glb),s=ir.surfaces[0],before=structuredClone(ir);
const reduced=reduceSurface(s,{targetVertices:49});
assert.equal(reduced.stats.outputVertices,49);assert.equal(reduced.stats.collapses,32);
assert.deepEqual(reduceSurface(s,{targetVertices:49}),reduced);assert.deepEqual(ir,before);
assert(sampleErrors(s,reduced.surface).distance.max<1e-12);
for(let i=0;i<s.positions.length;i++)if(s.positions[i].some((v,k)=>k<2&&Math.abs(v)===1))assert(reduced.surface.sourceVertexIds.includes(i));
assert.deepEqual(reduceSurface(s,{targetVertices:81}).surface.positions,s.positions);
assert.throws(()=>reduceSurface(s,{targetVertices:3}),/REDUCTION_CONSTRAINTS/);
for(const extra of [{maxCollapses:0},{maxTriangleTests:1}])assert.throws(()=>reduceSurface(s,{targetVertices:49,...extra}),/SEARCH_BUDGET_EXCEEDED/);
const locked=reduceSurface(s,{targetVertices:49,protectedVertices:[40]}).surface;assert(locked.sourceVertexIds.includes(40));
const parts=partition(reduced.surface);assert(parts.every(p=>p.positions.length<=32&&p.triangles.length<=64));
const curved=importGlb(fixture(9,.1).glb).surfaces[0],curvedReduced=reduceSurface(curved,{targetVertices:49}).surface,errors=sampleErrors(curved,curvedReduced);
assert(errors.distance.max>0&&errors.distance.max<.01);assert(errors.normalDegrees.max<8);
// Current main's compiler owns partitioning, material sidedness and AYNM version.
const options={scopeBySurface:new Map([[s.stableId,'rigid-source']]),materials:{'material/0':{albedo565:0xfc00,hone:[.6,.4,.1,16,0,.3,.3,.3,.2,.3,.4]}},scale:40};
const selected={...ir,surfaces:[reduced.surface]},compiled=compileStatic(selected,options);
assert.deepEqual(compileStatic(selected,options).bytes,compiled.bytes);
assert.equal(compiled.manifest.schema,'ayatsuri-native-model-design-v2');
assert(compiled.manifest.nodes.slice(1).every(n=>n.materialSidedness==='single'));
const double={...selected,schema:'hone-scene-ir-v2',materials:ir.materials.map(m=>({...m,sidedness:'double'}))};
assert(compileStatic(double,options).manifest.nodes.slice(1).every(n=>n.materialSidedness==='double'));
assert.throws(()=>compileStatic(selected,{...options,scopeBySurface:new Map()}),/INVALID_SURFACE_ID_OR_SCOPE/);
assert.throws(()=>compileStatic(selected,{...options,materials:{}}),/MATERIAL_MAPPING_REQUIRED/);
for(const field of ['positions','normals','triangles'])for(let k=0;k<3;k++){
 const bad=structuredClone(s);delete bad[field][0][k];const before=structuredClone(bad);
 assert.throws(()=>reduceSurface(bad,{targetVertices:49}),/INVALID/);assert.deepEqual(bad,before);
}
const winding=structuredClone(s);winding.triangles[0].reverse();assert.throws(()=>reduceSurface(winding,{targetVertices:49}),/INCONSISTENT_WINDING/);
const nonmanifold=structuredClone(s);nonmanifold.triangles.push(s.triangles[0].slice());nonmanifold.sourceTriangleIds.push(999);assert.throws(()=>reduceSurface(nonmanifold,{targetVertices:49}),/NONMANIFOLD/);
const named=structuredClone(s);named.sourceVertexIds=s.positions.map((_,i)=>100+i*2);named.sourceTriangleIds=s.triangles.map((_,i)=>1000+i*3);
const namedReduced=reduceSurface(named,{targetVertices:49}).surface;
assert(namedReduced.sourceVertexIds.every(id=>named.sourceVertexIds.includes(id)));
assert(partition(namedReduced).every(p=>p.sourceTriangleIds.every(id=>named.sourceTriangleIds.includes(id))));
const lineage=structuredClone(s);lineage.sourceVertexIds=[0];assert.throws(()=>reduceSurface(lineage,{targetVertices:49}),/INVALID_LINEAGE/);
const large=structuredClone(s);large.positions=Array(4097).fill([0,0,0]);large.normals=Array(4097).fill([0,0,1]);assert.throws(()=>reduceSurface(large,{targetVertices:49}),/PROTOTYPE_INPUT_LIMIT/);
assert.deepEqual(ir,before);
console.log(JSON.stringify({status:'PASS',sourceVertices:81,reducedVertices:49,parts:compiled.parts.length,coldBytes:compiled.bytes.length,curvedSampleErrors:errors,blender:'NOT_TESTED',nativeRenderer:'NOT_TESTED',foxArt:'NOT_ACCEPTED'}));
const {composeLineage}=require('./source_lineage.cjs');
const namedIR={...ir,surfaces:[namedReduced]},namedCompiled=compileStatic(namedIR,options),lineageReport=composeLineage(namedIR,namedCompiled);
assert.equal(lineageReport.flatMap(p=>p.triangles).length,namedReduced.triangles.length);
for(const p of lineageReport)for(const t of p.triangles)assert.equal(t.sourceTriangleId,namedReduced.sourceTriangleIds[t.reducedTriangle]);
const corrupted=structuredClone(namedCompiled);corrupted.parts[0].origins[0].triangle=9999;assert.throws(()=>composeLineage(namedIR,corrupted),/INVALID_TRIANGLE_ORIGIN/);
const sparse=structuredClone(namedCompiled);delete sparse.parts[0].origins[0];assert.throws(()=>composeLineage(namedIR,sparse),/INVALID_TRIANGLE_ORIGIN/);
assert.throws(()=>composeLineage({surfaces:[]},{parts:Array(1)}),/INVALID_PART_LINEAGE/);
if(process.argv[2]){
 const fs=require('node:fs'),path=require('node:path'),output=process.argv[2];fs.mkdirSync(output,{recursive:true});
 const cases={identity:compileStatic(ir,options),reduced:compiled};
 for(const [name,result] of Object.entries(cases))fs.writeFileSync(path.join(output,name+'.aynm'),result.bytes);
 fs.writeFileSync(path.join(output,'source.ir.json'),JSON.stringify(ir,null,2)+'\n');
 fs.writeFileSync(path.join(output,'reduced.ir.json'),JSON.stringify(selected,null,2)+'\n');
 fs.writeFileSync(path.join(output,'lineage.json'),JSON.stringify(composeLineage(selected,compiled),null,2)+'\n');
}
