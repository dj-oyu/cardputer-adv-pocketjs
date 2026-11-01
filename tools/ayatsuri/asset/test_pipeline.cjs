'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),child=require('node:child_process');
const {reduceSurface,partition,sampleErrors}=require('./reduce.cjs');
const {importGlb}=require('./import_glb.cjs'),{run,readCandidate,publish}=require('./pipeline.cjs');
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
const profile={experimental:true,assetScale:40,targetVertices:49,maxCollapses:1000,
  budgets:{maxColdBytes:65536,maxArenaEstimateBytes:49152,maxSegments:1024,maxBboxPixelVisits:65535,maxNodes:32},
  quality:{maxSampleDistance:1e-6,maxNormalDegrees:.05,minSilhouetteIoU:1},
  views:[-35,0,35].flatMap(yaw=>[-10,0,10].map(pitch=>({yaw,pitch,scale:40}))),
  materials:{'material/0':{albedo565:0xfc00,hone:[.6,.4,.1,16,0,.3,.3,.3,.2,.3,.4]}}};
function tests(output){
  const {glb,editable}=fixture(),ir=importGlb(glb),before=JSON.stringify(ir),a=run(ir,profile),b=run(ir,profile);
  assert.deepEqual(a.bytes,b.bytes);assert.deepEqual(a.report,b.report);assert.equal(JSON.stringify(ir),before);
  assert.deepEqual(run(JSON.parse(JSON.stringify(ir)),profile).bytes,a.bytes);
  assert.equal(a.report.surfaceReports[0].inputVertices,81);assert.equal(a.report.surfaceReports[0].outputVertices,49);assert.equal(a.report.surfaceReports[0].collapses,32);
  assert(a.report.parts>1);assert(a.manifest.nodes.slice(1).every(n=>n.mesh.vertices.length/3<=32&&n.mesh.indices.length/3<=64));
  assert(a.report.views.every(v=>v.iou===1));assert(a.report.quantization.every(e=>e.distance.max<=1e-6));
  assert.equal(a.report.status,'generated-experimental-not-accepted');assert.equal(a.report.gates.nativeFinalColour,'NOT_TESTED');
  const s=ir.surfaces[0],identity=reduceSurface(s,{targetVertices:81}).surface;assert.deepEqual(identity.positions,s.positions);assert(sampleErrors(s,identity).distance.max<1e-12);
  assert.throws(()=>run(ir,{...profile,experimental:false}),/EXPERIMENTAL_PROFILE_REQUIRED/);
  assert.throws(()=>run(ir,{...profile,materials:{}}),/MATERIAL_MAPPING_REQUIRED/);
  for(const key of ['maxColdBytes','maxArenaEstimateBytes','maxSegments','maxBboxPixelVisits','maxNodes'])assert.throws(()=>run(ir,{...profile,budgets:{...profile.budgets,[key]:1}}),/BUDGET/);
  assert.throws(()=>run(ir,{...profile,targetVertices:3}),/REDUCTION_CONSTRAINTS/);
  assert.throws(()=>run(ir,{...profile,maxCollapses:0}),/SEARCH_BUDGET_EXCEEDED/);
  assert.throws(()=>run(ir,{...profile,maxTriangleTests:1}),/SEARCH_BUDGET_EXCEEDED/);
  const bumpy=importGlb(fixture(9,.2).glb);assert.throws(()=>run(bumpy,profile),/QUALITY_SURFACE/);
  const curvedSource=fixture(9,.1),curvedProfile={...profile,quality:{maxSampleDistance:.01,maxNormalDegrees:8,minSilhouetteIoU:.97}},curved=run(importGlb(curvedSource.glb),curvedProfile);
  assert(curved.report.finalErrors[0].distance.max>0);assert(curved.report.finalErrors[0].distance.max<.01);assert(curved.report.finalErrors[0].normalDegrees.max<8);assert.deepEqual(run(importGlb(curvedSource.glb),curvedProfile).bytes,curved.bytes);
  const offset=importGlb(fixture(3,1e-5).glb);for(const p of offset.surfaces[0].positions)p[2]+=.7;
  const broad={...profile,targetVertices:8,assetScale:1,quality:{...profile.quality,maxSampleDistance:1,maxNormalDegrees:180,minSilhouetteIoU:0}},composed=run(offset,broad).report;
  // A test-only threshold separates stage errors from their accumulated error.
  const stageMax=Math.max(...composed.surfaceReports.map(s=>s.errors.distance.max),...composed.quantization.map(s=>s.distance.max)),finalMax=Math.max(...composed.finalErrors.map(s=>s.distance.max));
  assert(finalMax>stageMax);assert.throws(()=>run(offset,{...broad,quality:{...broad.quality,maxSampleDistance:(stageMax+finalMax)/2}}),/QUALITY_SOURCE_TO_PACKED/);
  assert.throws(()=>partition(s,{maxVertices:33}),/INVALID_PARTITION/);
  const invalid=structuredClone(s);invalid.triangles[0][1]=invalid.triangles[0][0];assert.throws(()=>partition(invalid),/INVALID_INDEX/);
  const winding=structuredClone(s);winding.triangles[0].reverse();assert.throws(()=>reduceSurface(winding,{targetVertices:49}),/INCONSISTENT_WINDING/);
  const nonmanifold=structuredClone(s);nonmanifold.triangles.push(s.triangles[0].slice());assert.throws(()=>reduceSurface(nonmanifold,{targetVertices:49}),/NONMANIFOLD/);
  if(output){fs.mkdirSync(output,{recursive:true});fs.writeFileSync(path.join(output,'source.glb'),glb);fs.writeFileSync(path.join(output,'editable-source.json'),JSON.stringify(editable,null,2)+'\n');fs.writeFileSync(path.join(output,'profile.json'),JSON.stringify(profile,null,2)+'\n');
    fs.writeFileSync(path.join(output,'candidate.aynm'),a.bytes);fs.writeFileSync(path.join(output,'report.json'),JSON.stringify(a.report,null,2)+'\n');
    fs.writeFileSync(path.join(output,'curved-source.glb'),curvedSource.glb);fs.writeFileSync(path.join(output,'curved-candidate.aynm'),curved.bytes);fs.writeFileSync(path.join(output,'curved-profile.json'),JSON.stringify(curvedProfile,null,2)+'\n');fs.writeFileSync(path.join(output,'curved-report.json'),JSON.stringify(curved.report,null,2)+'\n');
    const target=path.join(fs.mkdtempSync(path.join(output,'cli-test-')),'candidate'),first=child.spawnSync(process.execPath,[path.join(__dirname,'pipeline.cjs'),path.join(output,'source.glb'),path.join(output,'profile.json'),target],{encoding:'utf8'});assert.equal(first.status,0,first.stderr||String(first.error)||first.stdout);
    assert.equal(child.spawnSync(process.execPath,[path.join(__dirname,'pipeline.cjs'),path.join(output,'source.glb'),path.join(output,'profile.json'),target],{encoding:'utf8'}).status,1);assert.deepEqual(fs.readFileSync(path.join(target,'candidate.aynm')),a.bytes);
    assert.deepEqual(JSON.parse(fs.readFileSync(path.join(target,'scene.ir.json'),'utf8')),ir);
    assert.deepEqual(readCandidate(target)['candidate.aynm'],a.bytes);
    const incomplete=path.join(path.dirname(target),'incomplete');fs.mkdirSync(incomplete);fs.writeFileSync(path.join(incomplete,'candidate.aynm'),a.bytes);assert.throws(()=>readCandidate(incomplete));
    const interrupted=path.join(path.dirname(target),'interrupted'),write=fs.writeFileSync;let writes=0;
    fs.writeFileSync=function(...args){if(++writes===3)throw Error('INJECTED_IO_FAILURE');return write.apply(fs,args);};
    try{assert.throws(()=>publish(interrupted,ir,a),/INJECTED_IO_FAILURE/);}finally{fs.writeFileSync=write;}
    assert.equal(fs.existsSync(path.join(interrupted,'COMPLETE.json')),false);assert.throws(()=>readCandidate(interrupted));
    const changed=Buffer.from(a.bytes);changed[0]^=1;fs.writeFileSync(path.join(target,'candidate.aynm'),changed);assert.throws(()=>readCandidate(target),/CANDIDATE_INTEGRITY/);
    fs.writeFileSync(path.join(target,'candidate.aynm'),a.bytes);assert.deepEqual(readCandidate(target)['candidate.aynm'],a.bytes);
    const folder=path.dirname(target),largeGlb=path.join(folder,'large.glb'),largeProfile=path.join(folder,'large.json');
    for(const [file,size] of [[largeGlb,32*1024*1024+1],[largeProfile,256*1024+1]]){const fd=fs.openSync(file,'w');try{fs.ftruncateSync(fd,size);}finally{fs.closeSync(fd);}}
    for(const [input,settings] of [[largeGlb,path.join(output,'profile.json')],[path.join(output,'source.glb'),largeProfile]]){const destination=path.join(folder,'rejected'),r=child.spawnSync(process.execPath,[path.join(__dirname,'pipeline.cjs'),input,settings,destination],{encoding:'utf8'});assert.equal(r.status,1);assert.match(r.stderr,/INPUT_BYTE_LIMIT/);assert.equal(fs.existsSync(destination),false);}
  }
  console.log(JSON.stringify({status:'PASS',sourceVertices:81,reducedVertices:49,triangles:a.report.surfaceReports[0].outputTriangles,parts:a.report.parts,coldBytes:a.bytes.length,segments:a.report.segmentsUpperBound,arenaEstimate:a.report.arenaConservativeEstimate,sha256:a.report.binSha256,blender:'NOT_TESTED',foxArt:'NOT_TESTED',nativeRenderer:'NOT_TESTED'}));
}
module.exports={fixture,profile,tests};if(require.main===module)tests(process.argv[2]);
