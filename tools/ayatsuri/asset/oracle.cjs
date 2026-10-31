// Independent F64 source-ray/Gouraud oracle, not a product depth buffer.
'use strict';
const fs=require('node:fs'),path=require('node:path'),zlib=require('node:zlib'),crypto=require('node:crypto');
const {readBounded}=require('./pipeline.cjs');
const hash=data=>crypto.createHash('sha256').update(data).digest('hex');
const W=240,H=135,N=W*H,sub=(a,b)=>a.map((x,i)=>x-b[i]),dot=(a,b)=>a.reduce((v,x,i)=>v+x*b[i],0),unit=a=>{const d=Math.hypot(...a);if(!(d>0))throw Error('ZERO_DIRECTION');return a.map(x=>x/d);};
function check(ok,code){if(!ok)throw Error(code);}
function basis(camera){const f=unit(sub(camera.slice(0,3),camera.slice(3,6)));let r=[f[2],0,-f[0]];if(Math.hypot(...r)<1e-6)r=[1,0,0];r=unit(r);return {f,r,d:[f[1]*r[2],f[2]*r[0]-f[0]*r[2],-f[1]*r[0]]};}
function world(p,n,scale,pose){
  const [tx,ty,tz,ax,ay,az,sx,sy,sz]=pose,cx=Math.cos(ax),cy=Math.cos(ay),cz=Math.cos(az),xx=Math.sin(ax),yy=Math.sin(ay),zz=Math.sin(az);
  const rotate=v=>{const a=[v[0],cx*v[1]-xx*v[2],xx*v[1]+cx*v[2]],b=[cy*a[0]+yy*a[2],a[1],-yy*a[0]+cy*a[2]];return [cz*b[0]-zz*b[1],zz*b[0]+cz*b[1],b[2]];};
  check([sx,sy,sz].every(v=>v!==0),'ZERO_SCALE');const q=rotate([p[0]*scale*sx,p[1]*scale*sy,p[2]*scale*sz]);return {p:[q[0]+tx,q[1]+ty,q[2]+tz],n:unit(rotate([n[0]/sx,n[1]/sy,n[2]/sz]))};
}
function channels(c){return [c>>11,(c>>5)&63,c&31];}
function shade(n,albedo,m,lighting,thickness=255){
  n=unit(n);const l=unit(lighting.slice(0,3)),v=unit(lighting.slice(3,6)),h0=l.map((x,i)=>x+v[i]),h=Math.hypot(...h0)>0?unit(h0):null,
    nd=dot(n,l),front=Math.max(0,nd),back=Math.max(0,-nd),nh=h?Math.max(0,dot(n,h)):0,spec=front>0&&nh>0?nh**m[3]:0,base=channels(albedo),max=[31,63,31];
  const c=base.map((a,i)=>{const light=lighting[6+i],value=a/max[i]*(m[0]+m[1]*front*light)+light*(m[2]*spec+m[4]*(1-thickness/255)*back*m[5+i]);return Math.floor(Math.max(0,Math.min(1,value))*max[i]+.5);});return c[0]*2048+c[1]*32+c[2];
}
function edge(a,b,x,y){let sign=1;if(a[0]>b[0]||(a[0]===b[0]&&a[1]>b[1])){[a,b]=[b,a];sign=-1;}const aa=a[1]-b[1],bb=b[0]-a[0],cc=(b[1]-a[1])*a[0]-(b[0]-a[0])*a[1];return sign*(aa*x+bb*y+cc);}
const closed=(a,b)=>b[1]<a[1]||(b[1]===a[1]&&b[0]>a[0]);
function weights(t,x,y){const a=edge(t[0],t[1],t[2][0],t[2][1]);if(!(a>1e-5))return null;const pairs=[[t[1],t[2]],[t[2],t[0]],[t[0],t[1]]],e=pairs.map(p=>edge(...p,x,y));if(e.some((v,i)=>v<0||(v===0&&!closed(...pairs[i]))))return null;return e.map(v=>v/a);}
function raster(triangles){
  const ids=new Int32Array(N).fill(-1),depth=new Float64Array(N).fill(-Infinity),overlaps=new Map(),colors=new Uint16Array(N),bary=new Float64Array(N*3);let visits=0;
  for(const [id,t] of triangles.entries()){
    const p=t.points,area=edge(p[0],p[1],p[2][0],p[2][1]);if(!(area>1e-5))continue;
    const x0=Math.max(0,Math.ceil(Math.min(...p.map(v=>v[0])))),x1=Math.min(W-1,Math.floor(Math.max(...p.map(v=>v[0])))),y0=Math.max(0,Math.ceil(Math.min(...p.map(v=>v[1])))),y1=Math.min(H-1,Math.floor(Math.max(...p.map(v=>v[1]))));
    visits+=Math.max(0,x1-x0+1)*Math.max(0,y1-y0+1);check(visits<=1000000,'SOURCE_RASTER_WORK_LIMIT');
    for(let y=y0;y<=y1;y++)for(let x=x0;x<=x1;x++){
      const e=[edge(p[1],p[2],x,y),edge(p[2],p[0],x,y),edge(p[0],p[1],x,y)],pairs=[[p[1],p[2]],[p[2],p[0]],[p[0],p[1]]];
      if(e.some((v,i)=>v<0||(v===0&&!closed(...pairs[i]))))continue;
      const w=e.map(v=>v/area),pos=y*W+x,z=w.reduce((v,a,i)=>v+a*p[i][2],0);
      if(ids[pos]>=0){if(!overlaps.has(pos))overlaps.set(pos,[[ids[pos],depth[pos]]]);overlaps.get(pos).push([id,z]);}
      if(z<=depth[pos])continue;
      ids[pos]=id;depth[pos]=z;bary.set(w,pos*3);const ch=t.colors.map(channels),out=[0,1,2].map(j=>Math.floor(w.reduce((v,a,i)=>v+a*ch[i][j],0)+.5));colors[pos]=(out[0]<<11)|(out[1]<<5)|out[2];
    }
  }return {ids,depth,overlaps,colors,bary,visits};
}
function components(surface){const p=surface.positions.map((_,i)=>i),find=i=>{while(p[i]!==i){p[i]=p[p[i]];i=p[i];}return i;};for(const t of surface.triangles)for(const v of t)p[find(v)]=find(t[0]);return p.map((_,i)=>find(i));}
function sourceTriangles(ir,profile,camera,lighting,pose){
  const b=basis(camera),triangles=[],surfaces=new Map();
  for(const s of ir.surfaces){const material=profile.materials?.[s.materialId??'default'];check(material,'MATERIAL_MAPPING_REQUIRED');
    const transformed=s.positions.map((p,i)=>world(p,s.normals[i],profile.assetScale,pose)),points=transformed.map(v=>{const p=sub(v.p,camera.slice(3,6));return [camera[7]+camera[6]*dot(b.r,p),camera[8]+camera[6]*dot(b.d,p),dot(b.f,p)];}),colors=transformed.map(v=>shade(v.n,material.albedo565,material.hone,lighting)),comp=components(s),sourceIndex=new Map((s.sourceVertexIds||s.positions.map((_,i)=>i)).map((id,i)=>[id,i]));
    surfaces.set(s.stableId,{surface:s,comp,sourceIndex,transformed});
    for(const [tid,t] of s.triangles.entries())triangles.push({points:t.map(i=>points[i]),colors:t.map(i=>colors[i]),sourceSurfaceId:s.stableId,semanticId:s.semanticId||s.extras?.semanticId||ir.nodes?.find(n=>n.stableId===s.nodeId)?.extras?.semanticId||s.stableId,sourceTriangleId:s.sourceTriangleIds?.[tid]??tid,sourceVertexIndices:t.slice(),component:comp[t[0]],world:t.map(i=>transformed[i].p)});
  }return {triangles,surfaces};
}
function summary(xs){if(!xs.length)return {count:0,mean:0,p95:0,max:0};const a=xs.slice().sort((a,b)=>a-b);return {count:xs.length,mean:xs.reduce((a,b)=>a+b,0)/xs.length,p95:a[Math.ceil(a.length*.95)-1],max:a.at(-1)};}
function sameFeature(a,b,point,epsilon){
  if(a.sourceSurfaceId!==b.sourceSurfaceId)return false;
  const shared=a.sourceVertexIndices.filter(i=>b.sourceVertexIndices.includes(i)).map(i=>a.world[a.sourceVertexIndices.indexOf(i)]);
  if(shared.length===1)return Math.hypot(...sub(point,shared[0]))<=epsilon;
  if(shared.length!==2)return false;
  const d=sub(shared[1],shared[0]),den=dot(d,d);if(!(den>0))return false;
  const t=Math.max(0,Math.min(1,dot(sub(point,shared[0]),d)/den)),q=shared[0].map((v,i)=>v+t*d[i]);return Math.hypot(...sub(point,q))<=epsilon;
}
function compare({sourceIR,compileReport,metadata,pixels,tags,coverage,thresholds,rootPose}){
  check(pixels.length===N&&tags.length===N&&coverage.length===N,'IMAGE_SIZE');
  const th={depthTieEpsilonSource:1e-6,...thresholds};for(const key of ['minIoU','maxColorMean','maxColorP95','maxColorMax','maxDepthSource','maxComponentMismatch','maxCorrespondenceDistanceSource','depthTieEpsilonSource'])check(Number.isFinite(th[key])&&th[key]>=0,'INVALID_THRESHOLDS');
  // The probe prints nine significant decimal digits: restore the actual F32
  // values before exact native-edge tests, rather than using decimal doubles.
  const camera=metadata.camera?.map(Math.fround),lighting=metadata.evaluatedLighting?.map(Math.fround),pose=(rootPose||metadata.rootPose||[0,0,0,0,0,0,1,1,1]).map(Math.fround),profile=compileReport.profile;
  check(camera?.length===9&&lighting?.length===9&&pose?.length===9,'METADATA_PARAMETERS');
  check([...camera,...lighting,...pose].every(Number.isFinite),'METADATA_NONFINITE');
  check(hash(JSON.stringify(sourceIR))===compileReport.irHash,'SOURCE_IR_HASH_MISMATCH');
  const tagRecords=metadata.triangleTags||metadata.packets.flatMap((p,packetIndex)=>p.triangles.map((indices,triangleIndex)=>({tag:p.triangleTags[triangleIndex],packetIndex,nodeId:p.nodeId,triangleIndex,indices})));
  const source=sourceTriangles(sourceIR,profile,camera,lighting,pose),ref=raster(source.triangles),mappings=new Map(compileReport.correspondence.map(x=>[x.nodeId,x])),tagMap=new Map(tagRecords.map(t=>[t.tag,t]));
  const colour=[],colourByChannel=[[],[],[]],depth=[],correspondence=[],errors=[],evidence=[],depthImage=new Uint16Array(N);let union=0,intersection=0,tagCoverageMismatch=0,tagPixelMismatch=0,semanticMismatch=0,componentMismatch=0,unknownTags=0,equivalentSourceTiePixels=0;
  for(let i=0;i<N;i++){
    const native=coverage[i]!==0,original=ref.ids[i]>=0;if((tags[i]!==0)!==native)tagCoverageMismatch++;
    union+=native||original?1:0;intersection+=native&&original?1:0;if(!native)continue;
    const actual=tagMap.get(tags[i]);if(!actual){unknownTags++;continue;}
    const packet=metadata.packets[actual.packetIndex];check(packet,'TAG_PACKET_MISSING');const mapping=mappings.get(packet.nodeId);check(mapping,'MAPPING_MISSING');
    check(!packet.holes?.length,'SOURCE_CLIP_UNSUPPORTED');const s=source.surfaces.get(mapping.sourceSurfaceId);check(s,'SOURCE_SURFACE_MISSING');
    const ids=actual.indices,pts=ids.map(j=>[packet.vertices[j].x,packet.vertices[j].y,packet.vertices[j].depth].map(Math.fround)),w=weights(pts,i%W,Math.floor(i/W));
    if(!w){tagPixelMismatch++;if(errors.length<64)errors.push({kind:'TAG_PIXEL_OUTSIDE',x:i%W,y:Math.floor(i/W),nativeNodeId:packet.nodeId,nativeTriangleTag:tags[i]});continue;}
    if(!original)continue;
    const expected=source.triangles[ref.ids[i]];
    const expectedWorld=[0,1,2].map(j=>expected.world.reduce((v,p,k)=>v+ref.bary[i*3+k]*p[j],0));
    let sharedTie=false;for(const [other,z] of ref.overlaps.get(i)||[])if(other!==ref.ids[i]&&(ref.depth[i]-z)/profile.assetScale<=th.depthTieEpsilonSource){check(sameFeature(expected,source.triangles[other],expectedWorld,profile.assetScale*th.depthTieEpsilonSource),'AMBIGUOUS_SOURCE_DEPTH');sharedTie=true;}if(sharedTie)equivalentSourceTiePixels++;
    const actualDepth=w.reduce((v,a,j)=>v+a*pts[j][2],0),d=Math.abs(actualDepth-ref.depth[i])/profile.assetScale;depth.push(d);
    const heat=th.maxDepthSource>0?Math.min(1,d/th.maxDepthSource):d>0?1:0;depthImage[i]=(Math.round(31*heat)<<11)|(Math.round(63*(1-heat))<<5);
    const anchors=ids.map(j=>s.sourceIndex.get(mapping.sourceVertices[j]));check(anchors.every(v=>v!==undefined),'SOURCE_ANCHOR_MISSING');
    const comps=anchors.map(j=>s.comp[j]),wrongComponent=comps.some(c=>c!==comps[0])||mapping.sourceSurfaceId!==expected.sourceSurfaceId||comps[0]!==expected.component;
    componentMismatch+=wrongComponent?1:0;semanticMismatch+=mapping.semanticId!==expected.semanticId?1:0;
    const actualSourceWorld=[0,1,2].map(j=>anchors.reduce((v,a,k)=>v+w[k]*s.transformed[a].p[j],0)),dist=Math.hypot(...sub(actualSourceWorld,expectedWorld))/profile.assetScale;correspondence.push(dist);
    const a=channels(pixels[i]),b=channels(ref.colors[i]);for(let k=0;k<3;k++){const d=Math.abs(a[k]-b[k]);colour.push(d);colourByChannel[k].push(d);}
    const coldTriangle=mapping.outputTriangles?.findIndex(t=>t.every((v,j)=>v===ids[j]));
    check(coldTriangle!==undefined&&coldTriangle>=0,'COLD_TRIANGLE_MAPPING_MISSING');
    const sample={x:i%W,y:Math.floor(i/W),sourceSurfaceId:expected.sourceSurfaceId,sourceTriangleId:expected.sourceTriangleId,sourceComponent:expected.component,nativeNodeId:packet.nodeId,nativeTriangleTag:tags[i],mappedSourceSurfaceId:mapping.sourceSurfaceId,mappedOriginalTriangleLineage:mapping.sourceTriangles[coldTriangle],sourceVertexAnchors:anchors.map(j=>s.surface.sourceVertexIds?.[j]??j),mappedComponent:comps[0],depthErrorSource:d,correspondenceDistanceSource:dist};
    if((wrongComponent||d>th.maxDepthSource||dist>th.maxCorrespondenceDistanceSource)&&errors.length<64)errors.push(sample);if(i%337===0&&evidence.length<64)evidence.push(sample);
  }
  const color=summary(colour),colorChannels=colourByChannel.map((values,i)=>({channel:['R5','G6','B5'][i],...summary(values)})),z=summary(depth),corr=summary(correspondence),iou=union?intersection/union:1;
  const gates={coverage:union>0&&intersection>0&&iou>=th.minIoU,diagnosticCoverage:tagCoverageMismatch===0&&tagPixelMismatch===0&&unknownTags===0,color:colorChannels.every(c=>c.mean<=th.maxColorMean&&c.p95<=th.maxColorP95&&c.max<=th.maxColorMax),depth:z.max<=th.maxDepthSource,sourceCorrespondence:componentMismatch<=th.maxComponentMismatch&&semanticMismatch===0&&corr.max<=th.maxCorrespondenceDistanceSource};
  const report={schema:'hone-native-image-oracle-v1',passed:Object.values(gates).every(Boolean),gates,thresholds:structuredClone(th),metrics:{pixelsCompared:intersection,iou,sourceRasterBBoxVisits:ref.visits,equivalentSourceTiePixels,tagCoverageMismatch,tagPixelMismatch,unknownTags,semanticMismatch,componentMismatch,colorLevels:color,colorChannels,depthSource:z,correspondenceSource:corr},errors,evidence,
    scope:'F64 high-density source mapped to Hone material; native final RGB565 and diagnostic native triangle tags; no Blender/export/physical display acceptance',camera,lighting,rootPose:pose};
  return {report,images:{source:ref.colors,native:pixels,depth:depthImage,diff:new Uint16Array(Array.from(pixels,(p,i)=>ref.ids[i]>=0&&coverage[i]?Math.abs(p-ref.colors[i])?0xf800:0:ref.ids[i]>=0||coverage[i]?0x001f:0))}};
}
function png(file,pixels){
  const crc=data=>{let v=0xffffffff;for(const b of data){v^=b;for(let j=0;j<8;j++)v=(v>>>1)^((v&1)?0xedb88320:0);}return (v^0xffffffff)>>>0;},chunk=(type,data)=>{const t=Buffer.from(type),h=Buffer.alloc(4),c=Buffer.alloc(4);h.writeUInt32BE(data.length);c.writeUInt32BE(crc(Buffer.concat([t,data])));return Buffer.concat([h,t,data,c]);};
  const raw=Buffer.alloc(H*(1+W*3));for(let y=0;y<H;y++)for(let x=0;x<W;x++){const c=channels(pixels[y*W+x]),at=y*(1+W*3)+1+x*3;raw[at]=Math.round(c[0]*255/31);raw[at+1]=Math.round(c[1]*255/63);raw[at+2]=Math.round(c[2]*255/31);}const header=Buffer.alloc(13);header.writeUInt32BE(W);header.writeUInt32BE(H,4);header[8]=8;header[9]=2;fs.writeFileSync(file,Buffer.concat([Buffer.from('89504e470d0a1a0a','hex'),chunk('IHDR',header),chunk('IDAT',zlib.deflateSync(raw)),chunk('IEND',Buffer.alloc(0))]));
}
function loadNative(directory,expectedHash){
  const json=file=>JSON.parse(readBounded(path.join(directory,file),16*1024*1024).toString('utf8')),proof=json('render-report.json');
  check(proof.passed&&proof.inputUnchanged&&proof.sourcesUnchanged&&proof.inputSha256===expectedHash,'NATIVE_PROVENANCE');
  const root=path.resolve(__dirname,'../../..'),required=['tools/ayatsuri/asset/render_native.c','main/ayatsuri/ayatsuri_model.c','main/ayatsuri/ayatsuri_model.h','main/ayatsuri/ayatsuri_transform.h','main/ayatsuri/ayatsuri_shade.h','main/ayatsuri/ayatsuri_visibility.c','main/ayatsuri/ayatsuri_visibility.h','main/ui/kasane/ksn_procedural.c','main/ui/kasane/ksn_procedural.h'];
  for(const file of required)check(proof.sourceSha256?.[file]===hash(readBounded(path.join(root,file),2*1024*1024)),'NATIVE_SOURCE_STALE');
  const files={};for(const name of ['metadata.json','pixels.u16le','coverage.u16le','visible-triangles.u16le']){const b=readBounded(path.join(directory,name),name==='metadata.json'?16*1024*1024:N*2);check(hash(b)===proof.outputs[name],'NATIVE_OUTPUT_HASH');files[name]=b;}
  const u16=name=>{const b=files[name];check(b.length===N*2,'IMAGE_SIZE');return Uint16Array.from({length:N},(_,i)=>b.readUInt16LE(i*2));};
  return {metadata:JSON.parse(files['metadata.json'].toString('utf8')),pixels:u16('pixels.u16le'),tags:u16('visible-triangles.u16le'),coverage:u16('coverage.u16le')};
}
module.exports={compare,sourceTriangles,raster,shade,png,basis,loadNative};
if(require.main===module){try{const [irFile,reportFile,nativeDir,thresholdFile,outDir]=process.argv.slice(2);check(outDir,'USAGE_IR_REPORT_NATIVE_THRESHOLDS_OUTPUT');const read=file=>JSON.parse(readBounded(file,64*1024*1024).toString('utf8')),compileReport=read(reportFile);
  const result=compare({sourceIR:read(irFile),compileReport,...loadNative(nativeDir,compileReport.binSha256),thresholds:read(thresholdFile)});
  fs.mkdirSync(outDir,{recursive:true});fs.writeFileSync(path.join(outDir,'oracle.json'),JSON.stringify(result.report,null,2)+'\n');for(const [name,data] of Object.entries(result.images))png(path.join(outDir,name+'.png'),data);console.log(JSON.stringify(result.report));if(!result.report.passed)process.exitCode=1;
}catch(e){console.error(JSON.stringify({passed:false,error:e.message}));process.exitCode=1;}}
