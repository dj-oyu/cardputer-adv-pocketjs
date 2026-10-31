// Experimental offline compiler: generated artifacts are not accepted models.
'use strict';
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {reduceSurface,partition,sampleErrors}=require('./reduce.cjs');
const {compile}=require('../model/pack_model.cjs');
const hash=x=>crypto.createHash('sha256').update(x).digest('hex');
function fail(code){const e=Error(code);e.code=code;throw e;}
function number(v,a,b){if(!Number.isFinite(v)||v<a||v>b)fail('INVALID_PROFILE');return v;}
function project(p,view){const y=view.yaw*Math.PI/180,t=view.pitch*Math.PI/180,c=Math.cos(y),s=Math.sin(y),cy=Math.cos(t),sy=Math.sin(t),x=c*p[0]+s*p[2],z=-s*p[0]+c*p[2];return [120+view.scale*x,67+view.scale*(cy*p[1]-sy*z),sy*p[1]+cy*z];}
function coverage(surfaces,view){
  const pixels=new Uint8Array(240*135);let visits=0;
  for(const s of surfaces){const points=s.positions.map(p=>project(p,view));for(const ids of s.triangles){
    const [a,b,c]=ids.map(i=>points[i]),area=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);if(Math.abs(area)<1e-12)continue;
    const x0=Math.max(0,Math.ceil(Math.min(a[0],b[0],c[0]))),x1=Math.min(239,Math.floor(Math.max(a[0],b[0],c[0]))),y0=Math.max(0,Math.ceil(Math.min(a[1],b[1],c[1]))),y1=Math.min(134,Math.floor(Math.max(a[1],b[1],c[1])));
    visits+=Math.max(0,x1-x0+1)*Math.max(0,y1-y0+1);
    for(let y=y0;y<=y1;y++)for(let x=x0;x<=x1;x++){
      const u=((b[0]-x)*(c[1]-y)-(b[1]-y)*(c[0]-x))/area,v=((c[0]-x)*(a[1]-y)-(c[1]-y)*(a[0]-x))/area;
      if(u>=-1e-10&&v>=-1e-10&&1-u-v>=-1e-10)pixels[y*240+x]=1;
    }
  }}return {pixels,bboxPixelVisits:visits};
}
function run(ir,profile){
  if(ir?.schema!=='hone-scene-ir-v1'||!Array.isArray(ir.surfaces)||!ir.surfaces.length||!profile||profile.experimental!==true)fail('EXPERIMENTAL_PROFILE_REQUIRED');
  if(ir.surfaces.length>256)fail('PROTOTYPE_INPUT_LIMIT');
  if(ir.surfaces.reduce((n,s)=>n+(s.positions?.length||0),0)>4096||ir.surfaces.reduce((n,s)=>n+(s.triangles?.length||0),0)>8192)fail('PROTOTYPE_INPUT_LIMIT');
  const inputHash=hash(JSON.stringify(ir)),scale=number(profile.assetScale,1e-9,1e6),budget=profile.budgets,quality=profile.quality;
  if(!budget||!quality||!Array.isArray(profile.views)||!profile.views.length||profile.views.length>128)fail('INVALID_PROFILE');
  for(const key of ['maxColdBytes','maxArenaEstimateBytes','maxSegments','maxBboxPixelVisits','maxNodes'])if(!Number.isInteger(budget[key])||budget[key]<=0)fail('INVALID_PROFILE');
  if(budget.maxSegments>1024||budget.maxNodes>256||budget.maxColdBytes>512*1024||budget.maxBboxPixelVisits>65535)fail('BACKEND_LIMIT');
  number(quality.maxSampleDistance,0,1e6);number(quality.maxNormalDegrees,0,180);number(quality.minSilhouetteIoU,0,1);
  for(const view of profile.views){number(view.yaw,-180,180);number(view.pitch,-90,90);number(view.scale,1e-6,4096);}
  const reduced=[],parts=[],surfaceReports=[];
  for(const s of ir.surfaces){
    const r=reduceSurface(s,{targetVertices:profile.targetVertices,maxCollapses:profile.maxCollapses??10000,maxTriangleTests:profile.maxTriangleTests??2000000,protectedVertices:profile.protectedVertices?.[s.stableId]||[]}),errors=sampleErrors(s,r.surface);
    if(errors.distance.max>quality.maxSampleDistance||errors.normalDegrees.max>quality.maxNormalDegrees)fail('QUALITY_SURFACE');
    reduced.push(r.surface);const partitioned=partition(r.surface);if(parts.length+partitioned.length+1>budget.maxNodes)fail('BUDGET_NODES');parts.push(...partitioned);surfaceReports.push({id:s.stableId,...r.stats,parts:partitioned.length,errors});
  }
  const nodes=[{index:0,name:'source-root',parent:-1,kind:'group',enabled:true,pose:[0,0,0,0,0,0,1,1,1]}];
  for(const [i,s] of parts.entries()){
    const m=profile.materials?.[s.materialId??'default'];
    if(!m||!Number.isInteger(m.albedo565)||m.albedo565<0||m.albedo565>65535||!Array.isArray(m.hone)||m.hone.length!==11)fail('MATERIAL_MAPPING_REQUIRED');
    nodes.push({index:i+1,name:'part-'+i,parent:0,kind:'gouraudMesh',enabled:true,pose:[0,0,0,0,0,0,1,1,1],color:0xffffff,material:m.hone.slice(),thickness:1,opacity:1,edgeSoftness:0,
      mesh:{vertices:s.positions.flat().map(v=>v*scale),normals:s.normals.flat(),indices:s.triangles.flat(),mask:s.positions.map(()=>-1),albedoA:m.albedo565,albedoB:m.albedo565,thickness:255,order:'authored'}});
  }
  if(nodes.length>budget.maxNodes)fail('BUDGET_NODES');
  const manifest={schema:'ayatsuri-native-model-design-v1',nodes},bytes=compile(manifest);
  // Decode the actual final F32 attributes rather than evaluating pre-pack doubles.
  const sections=new Map();for(let i=0;i<bytes.readUInt32LE(12);i++){const o=32+i*20;sections.set(bytes.readUInt16LE(o),{offset:bytes.readUInt32LE(o+4),count:bytes.readUInt32LE(o+12),stride:bytes.readUInt32LE(o+16)});}
  const decoded=parts.map((s,i)=>{const d=sections.get(9).offset+i*32,start=bytes.readUInt16LE(d),nv=bytes.readUInt16LE(d+2),triangleStart=bytes.readUInt16LE(d+4),nt=bytes.readUInt16LE(d+6),p=[],n=[],triangles=[];
    for(let j=0;j<nv;j++){const at=sections.get(10).offset+(start+j)*28;p.push([0,1,2].map(k=>bytes.readFloatLE(at+4*k)/scale));n.push([0,1,2].map(k=>bytes.readFloatLE(at+12+4*k)));}
    for(let j=0;j<nt;j++){const at=sections.get(11).offset+(triangleStart+j)*6;triangles.push([0,1,2].map(k=>bytes.readUInt16LE(at+2*k)));}return {...s,positions:p,normals:n,triangles};});
  const quantization=parts.map((s,i)=>sampleErrors(s,decoded[i]));
  if(quantization.some(e=>e.distance.max>quality.maxSampleDistance||e.normalDegrees.max>quality.maxNormalDegrees))fail('QUALITY_PACKED');
  let firstPart=0;
  const finalErrors=ir.surfaces.map((source,i)=>{const group=decoded.slice(firstPart,firstPart+surfaceReports[i].parts);firstPart+=group.length;
    const joined={positions:[],normals:[],triangles:[]};for(const s of group){const start=joined.positions.length;joined.positions.push(...s.positions);joined.normals.push(...s.normals);joined.triangles.push(...s.triangles.map(t=>t.map(v=>v+start)));}
    const errors=sampleErrors(source,joined);if(errors.distance.max>quality.maxSampleDistance||errors.normalDegrees.max>quality.maxNormalDegrees)fail('QUALITY_SOURCE_TO_PACKED');return errors;});
  const views=profile.views.map(view=>{const a=coverage(ir.surfaces,view),b=coverage(decoded,view);let intersection=0,union=0;for(let i=0;i<a.pixels.length;i++){intersection+=a.pixels[i]&b.pixels[i];union+=a.pixels[i]|b.pixels[i];}const iou=union?intersection/union:1;if(iou<quality.minSilhouetteIoU)fail('QUALITY_SILHOUETTE');if(b.bboxPixelVisits>budget.maxBboxPixelVisits)fail('BUDGET_PIXEL_VISITS');return {...view,iou,bboxPixelVisits:b.bboxPixelVisits};});
  const segments=parts.reduce((sum,s)=>sum+1+Math.ceil((8+16*s.positions.length+6*s.triangles.length)/10),0),arenaEstimate=24*nodes.length+84+parts.reduce((sum,s)=>sum+Math.ceil((84+1040+12*s.positions.length)/8)*8,0);
  if(bytes.length>budget.maxColdBytes||segments>budget.maxSegments||arenaEstimate>budget.maxArenaEstimateBytes)fail('BUDGET_EXCEEDED');
  if(hash(JSON.stringify(ir))!==inputHash)fail('INPUT_MUTATED');
  return {bytes,manifest,decoded,report:{schema:'hone-asset-report-v1',status:'generated-experimental-not-accepted',source:ir.source,irHash:inputHash,profileHash:hash(JSON.stringify(profile)),binSha256:hash(bytes),profile,surfaceReports,quantization,finalErrors,views,
    coldBytes:bytes.length,nodes:nodes.length,parts:parts.length,segmentsUpperBound:segments,arenaConservativeEstimate:arenaEstimate,maxBboxPixelVisitsObserved:Math.max(...views.map(v=>v.bboxPixelVisits)),
    correspondence:parts.map(s=>({partId:s.stableId,sourceVertices:s.sourceVertexIds,sourceTriangles:s.sourceTriangleIds})),
    gates:{sampledGeometry:true,sampledNormal:true,sampledSilhouette:true,coldAndEstimatedBudgets:true,blenderExportLoss:'NOT_TESTED',nativeReader:'NOT_TESTED',nativeFinalColour:'NOT_TESTED',occlusionDepthAndCorrespondence:'NOT_TESTED',sourceArt:'NOT_ACCEPTED',guestPeak:'NOT_MEASURED',device:'NOT_TESTED'}}};
}
function readBounded(file,limit){
  const fd=fs.openSync(file,'r');try{const before=fs.fstatSync(fd);if(!before.isFile()||before.size>limit)fail('INPUT_BYTE_LIMIT');const data=Buffer.alloc(before.size);
    let offset=0;while(offset<data.length){const n=fs.readSync(fd,data,offset,data.length-offset,null);if(!n)fail('INPUT_CHANGED');offset+=n;}
    if(fs.readSync(fd,Buffer.alloc(1),0,1,null)!==0)fail('INPUT_CHANGED');const after=fs.fstatSync(fd);if(before.size!==after.size||before.mtimeMs!==after.mtimeMs)fail('INPUT_CHANGED');return data;
  }finally{fs.closeSync(fd);}
}
const FILE_LIMITS={'candidate.aynm':512*1024,'scene.ir.json':256*1024*1024,'manifest.json':32*1024*1024,'report.json':16*1024*1024};
function publish(directory,ir,result){
  // Exclusive reservation + final integrity marker avoids directory-rename races
  // and OneDrive directory locks. Incomplete directories are never consumable.
  fs.mkdirSync(path.dirname(path.resolve(directory)),{recursive:true});fs.mkdirSync(directory);
  const files={'candidate.aynm':result.bytes,'scene.ir.json':Buffer.from(JSON.stringify(ir,null,2)+'\n'),'manifest.json':Buffer.from(JSON.stringify(result.manifest,null,2)+'\n'),'report.json':Buffer.from(JSON.stringify(result.report,null,2)+'\n')},entries=[];
  for(const [name,data] of Object.entries(files)){if(data.length>FILE_LIMITS[name])fail('OUTPUT_BYTE_LIMIT');fs.writeFileSync(path.join(directory,name),data,{flag:'wx'});entries.push({name,bytes:data.length,sha256:hash(data)});}
  fs.writeFileSync(path.join(directory,'COMPLETE.json'),JSON.stringify({schema:'hone-experimental-candidate-v1',accepted:false,files:entries})+'\n',{flag:'wx'});
}
function readCandidate(directory){
  const marker=JSON.parse(readBounded(path.join(directory,'COMPLETE.json'),4096).toString('utf8'));
  if(marker.schema!=='hone-experimental-candidate-v1'||marker.accepted!==false||marker.files?.length!==4)fail('INCOMPLETE_CANDIDATE');
  const files={},names=new Set();for(const entry of marker.files){if(!Object.hasOwn(FILE_LIMITS,entry.name)||names.has(entry.name))fail('INCOMPLETE_CANDIDATE');names.add(entry.name);const data=readBounded(path.join(directory,entry.name),FILE_LIMITS[entry.name]);if(data.length!==entry.bytes||hash(data)!==entry.sha256)fail('CANDIDATE_INTEGRITY');files[entry.name]=data;}return files;
}
module.exports={run,coverage,publish,readCandidate};
if(require.main===module){
  try{const {importGlb,DEFAULT_LIMITS}=require('./import_glb.cjs'),[input,profileFile,output]=process.argv.slice(2);if(!input||!profileFile||!output)fail('USAGE_INPUT_PROFILE_NEW_OUTPUT_DIRECTORY');
    // Refuse replacement of an existing candidate or accepted artifact directory.
    if(fs.existsSync(output))fail('OUTPUT_EXISTS');
    const ir=importGlb(readBounded(input,DEFAULT_LIMITS.maxBytes)),result=run(ir,JSON.parse(readBounded(profileFile,256*1024).toString('utf8')));publish(output,ir,result);readCandidate(output);
    console.log(JSON.stringify({output,status:result.report.status,sha256:result.report.binSha256}));
  }catch(e){console.error(JSON.stringify({status:'FAIL',code:e.code||'INVALID_INPUT',message:e.message}));process.exitCode=1;}
}
