'use strict';
const fs=require('node:fs'),path=require('node:path');
const {importGlb}=require('./import_glb.cjs'),{compileStatic}=require('./compile_static.cjs');
const [source,out]=process.argv.slice(2);
for(const name of ['nonuniform','mirrored']){
 const ir=importGlb(fs.readFileSync(path.join(source,name+'.glb'))),s=ir.surfaces[0];
 const options={scale:12,scopeBySurface:new Map([[s.stableId,'transform-native']]),materials:{[s.materialId??'default']:{albedo565:0xffff,hone:[1,0,0,16,0,0,0,0,0,0,0]}}};
 fs.writeFileSync(path.join(out,name+'.aynm'),compileStatic(ir,options).bytes);
 const flipped=structuredClone(ir);for(const t of flipped.surfaces[0].triangles)[t[1],t[2]]=[t[2],t[1]];
 fs.writeFileSync(path.join(out,name+'-wrong-winding.aynm'),compileStatic(flipped,options).bytes);
}
