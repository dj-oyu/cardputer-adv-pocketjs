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
const original=JSON.stringify(ir),reduced=reduceSurface(surface,{targetVertices:65}).surface,errors=sampleErrors(surface,reduced);
assert(errors.distance.max<=.01&&errors.normalDegrees.max<=8);
const materialId=surface.materialId??'default';
const compiled=compileStatic({...ir,surfaces:[reduced]},{scale:40,scopeBySurface:new Map([[surface.stableId,'rigid-grid']]),materials:{[materialId]:{albedo565:0xfc00,hone:[.6,.4,.1,16,0,.3,.3,.3,.2,.3,.4]}}});
assert.equal(JSON.stringify(ir),original);
const hash=x=>crypto.createHash('sha256').update(x).digest('hex');
console.log(JSON.stringify({status:'PASS',blenderVersion:metadata.blenderVersion,sourceVertices:81,sourceTriangles:128,exportMaxDistance:maxExportDistance,reducedVertices:65,sampledReductionError:errors,coldBytes:compiled.bytes.length,parts:compiled.parts.length,glbSha256:hash(bytes),modelSha256:hash(compiled.bytes),scope:'Synthetic Blender export/topology and sampled reduction only; no fox/normal-export oracle/pixels/device acceptance'}));
