'use strict';
// Synthetic authored source -> actual Blender export -> current importer.
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {importGlb}=require('./import_glb.cjs'),{reduceSurface,sampleErrors}=require('./reduce.cjs'),{compileStatic}=require('./compile_static.cjs');
const directory=process.argv[2];assert(directory,'usage: node verify_blender_grid.cjs OUTPUT_DIRECTORY');
const metadata=JSON.parse(fs.readFileSync(path.join(directory,'blender-source.json'))),bytes=fs.readFileSync(path.join(directory,'source.glb'));
assert.equal(metadata.scope,'SYNTHETIC GRID, NOT PRIVATE ART OR FOX ACCEPTANCE');assert.equal(metadata.vertices.length,81);assert.equal(metadata.faces.length,128);
const ir=importGlb(bytes);assert.equal(ir.surfaces.length,1);const surface=ir.surfaces[0];assert.equal(surface.positions.length,81);assert.equal(surface.triangles.length,128);
// Blender Z-up -> glTF Y-up -> Hone down-positive; compare source, not exports.
const expected=metadata.vertices.map(([x,y,z])=>[x,-z,-y]);let maxExportDistance=0;
const ids=surface.positions.map(p=>{
 const errors=expected.map(q=>Math.hypot(...p.map((v,k)=>v-q[k]))),distance=Math.min(...errors),id=errors.indexOf(distance);
 assert(distance<1e-6);maxExportDistance=Math.max(maxExportDistance,distance);return id;
});assert.equal(new Set(ids).size,81);
const key=t=>[0,1,2].map(offset=>[t[offset],t[(offset+1)%3],t[(offset+2)%3]].join(':')).sort()[0];
// Basis reflection reverses winding; cyclic rotations preserve the same face.
assert.deepEqual(surface.triangles.map(t=>key(t.map(i=>ids[i]))).sort(),metadata.faces.map(([a,b,c])=>key([a,c,b])).sort());
// Independent bounded direction oracle, not an exporter reimplementation.
// Blender 4.3.2 exporter rounds corner normal components to 4 decimals, then
// normalizes. Component error <= 0.5e-4: angular error <= asin(sqrt(3)*0.5e-4).
// Add 1e-6 rad for Float32 source/export arithmetic. Never fit to observed errors.
const normalAngleBound=Math.asin(Math.sqrt(3)*.5e-4)+1e-6;
function verifyNormals(normals, corners){
 assert.equal(corners.length,384);assert.equal(normals.length,81);
 const seen=new Set();let maxNormalAngle=0;
 for(let i=0;i<corners.length;i++){
  const corner=corners[i];assert.equal(corner.vertex,metadata.faces[Math.floor(i/3)][i%3]);
  const raw=corner.normal,actual=normals[ids.indexOf(corner.vertex)];seen.add(corner.vertex);
  assert(Array.isArray(raw)&&raw.length===3&&raw.every(Number.isFinite));
  assert(actual.length===3&&actual.every(Number.isFinite));
  assert(Math.abs(Math.hypot(...raw)-1)<1e-6,'source unit normal');
  assert(Math.abs(Math.hypot(...actual)-1)<1e-6,'imported unit normal');
  const expected=[raw[0],-raw[2],-raw[1]],dot=expected.reduce((v,x,k)=>v+x*actual[k],0);
  const cross=[expected[1]*actual[2]-expected[2]*actual[1],expected[2]*actual[0]-expected[0]*actual[2],expected[0]*actual[1]-expected[1]*actual[0]];
  const angle=Math.atan2(Math.hypot(...cross),dot);assert(angle<=normalAngleBound,'normal transform/direction');
  maxNormalAngle=Math.max(maxNormalAngle,angle);
 }
 assert.equal(seen.size,81);return maxNormalAngle;
}
const maxNormalAngle=verifyNormals(surface.normals,metadata.cornerNormals);
for(const mutate of [n=>n.map(v=>-v),n=>[n[1],n[0],n[2]],n=>n.map(v=>v*2),n=>[NaN,n[1],n[2]],n=>[n[0]*Math.cos(.001)-n[1]*Math.sin(.001),n[0]*Math.sin(.001)+n[1]*Math.cos(.001),n[2]]]){
 const changed=structuredClone(surface.normals);changed[0]=mutate(changed[0]);
 assert.throws(()=>verifyNormals(changed,metadata.cornerNormals));
}
const badCorner=structuredClone(metadata.cornerNormals);badCorner[0].vertex=80;
assert.throws(()=>verifyNormals(surface.normals,badCorner));
const original=JSON.stringify(ir),reduced=reduceSurface(surface,{targetVertices:65}).surface,errors=sampleErrors(surface,reduced);
assert(errors.distance.max<=.01&&errors.normalDegrees.max<=8);
const materialId=surface.materialId??'default';
const compiled=compileStatic({...ir,surfaces:[reduced]},{scale:40,scopeBySurface:new Map([[surface.stableId,'rigid-grid']]),materials:{[materialId]:{albedo565:0xfc00,hone:[.6,.4,.1,16,0,.3,.3,.3,.2,.3,.4]}}});
assert.equal(JSON.stringify(ir),original);
const hash=x=>crypto.createHash('sha256').update(x).digest('hex');
console.log(JSON.stringify({status:'PASS',blenderVersion:metadata.blenderVersion,sourceVertices:81,sourceTriangles:128,exportMaxDistance:maxExportDistance,maxNormalAngleRadians:maxNormalAngle,normalAngleBoundRadians:normalAngleBound,reducedVertices:65,sampledReductionError:errors,coldBytes:compiled.bytes.length,parts:compiled.parts.length,glbSha256:hash(bytes),modelSha256:hash(compiled.bytes),scope:'Synthetic Blender export/topology and sampled reduction only; pre-export normal transform oracle; no independent smooth-normal algorithm/fox/pixels/device acceptance'}));
