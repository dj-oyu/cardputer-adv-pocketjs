// Bounded static GLB adapter. No files, network, scripts or Blender are executed.
'use strict';
const {createHash}=require('node:crypto');
const LIMITS=Object.freeze({maxBytes:32*1024*1024,maxJsonBytes:4*1024*1024,maxNodes:4096,maxMeshes:4096,
  maxAccessors:16384,maxBufferViews:16384,maxMaterials:4096,maxSurfaces:16384,maxVertices:500000,
  maxTriangles:1000000,maxDecodedBytes:128*1024*1024,maxDepth:128,maxNameBytes:1024});
class ImportError extends Error{constructor(code,path,message){super(`${code} ${path}: ${message}`);this.name='ImportError';this.code=code;this.path=path;}}
function fail(code,path,message){throw new ImportError(code,path,message);}
function check(ok,path,message,code='INVALID_GLB'){if(!ok)fail(code,path,message);}
const own=(o,k)=>Object.prototype.hasOwnProperty.call(o,k);
function object(o,p){check(o!==null&&typeof o==='object'&&!Array.isArray(o),p,'object required');return o;}
function integer(n,lo,hi,p){check(Number.isSafeInteger(n)&&n>=lo&&n<=hi,p,'integer outside bounds');return n;}
function finite(n,p){check(typeof n==='number'&&Number.isFinite(n),p,'finite number required');return n;}
function vector(a,n,p){check(Array.isArray(a)&&a.length===n,p,`expected ${n} values`);a.forEach((v,i)=>finite(v,`${p}/${i}`));return a;}
function keys(o,allowed,p){object(o,p);for(const k of Object.keys(o))check(allowed.includes(k),`${p}/${k}`,'unsupported property','UNSUPPORTED');}
function extensions(o,p){if(own(o,'extensions')){object(o.extensions,p+'/extensions');check(!Object.keys(o.extensions).length,p+'/extensions','extensions unsupported','UNSUPPORTED');}}
function array(a,p,max,optional=false){if(a===undefined&&optional)return [];check(Array.isArray(a)&&a.length<=max,p,'array limit');return a;}
function name(o,p,limits){if(!own(o,'name'))return null;check(typeof o.name==='string',p+'/name','string required');
  check(Buffer.byteLength(o.name,'utf8')<=limits.maxNameBytes,p+'/name','name byte limit','LIMIT');
  for(const c of o.name){const n=c.codePointAt(0);check(n>=32&&n!==127&&!(n>=0xd800&&n<=0xdfff),p+'/name','control or unpaired surrogate');}return o.name;}
const IDENTITY=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];
const BASIS=[1,0,0,0,0,-1,0,0,0,0,1,0,0,0,0,1];
function multiply(a,b,p){const out=Array(16).fill(0);for(let c=0;c<4;c++)for(let r=0;r<4;r++){
  for(let k=0;k<4;k++)out[c*4+r]+=a[k*4+r]*b[c*4+k];finite(out[c*4+r],p);}return out;}
function matrix(n,p){
  if(own(n,'matrix')){check(!['translation','rotation','scale'].some(k=>own(n,k)),p,'matrix and TRS are exclusive');const m=vector(n.matrix,16,p+'/matrix').slice();
    check(m[3]===0&&m[7]===0&&m[11]===0&&m[15]===1,p+'/matrix','affine matrix required');
    // glTF local matrices must decompose to TRS. Normalized column dots use
    // absolute tolerance 1e-5 for authored rounding, independent of axis scale.
    // Reflections are valid; composed world matrices may shear and are not tested.
    const columns=[0,4,8].map(at=>{const c=m.slice(at,at+3),length=Math.hypot(...c);
      check(Number.isFinite(length)&&length>0,p+'/matrix','finite nonzero column length required');
      return c.map(v=>finite(v/length,p+'/matrix'));});
    for(let a=0;a<3;a++)for(let b=a+1;b<3;b++){
      const dot=columns[a].reduce((sum,v,i)=>sum+v*columns[b][i],0);
      check(Number.isFinite(dot)&&Math.abs(dot)<=1e-5,p+'/matrix','local matrix must be TRS-decomposable');}
    return m;}
  const t=vector(n.translation===undefined?[0,0,0]:n.translation,3,p+'/translation'),s=vector(n.scale===undefined?[1,1,1]:n.scale,3,p+'/scale');
  check(s.every(x=>x!==0),p+'/scale','singular scale');const q=vector(n.rotation===undefined?[0,0,0,1]:n.rotation,4,p+'/rotation');
  const len=Math.hypot(...q);check(Math.abs(len-1)<=1e-5,p+'/rotation','unit quaternion required');
  const [x,y,z,w]=q.map(v=>v/len),[sx,sy,sz]=s;
  return [(1-2*(y*y+z*z))*sx,2*(x*y+z*w)*sx,2*(x*z-y*w)*sx,0,
    2*(x*y-z*w)*sy,(1-2*(x*x+z*z))*sy,2*(y*z+x*w)*sy,0,
    2*(x*z+y*w)*sz,2*(y*z-x*w)*sz,(1-2*(x*x+y*y))*sz,0,...t,1];
}
function inverseTranspose(m,p){
  const a=m[0],b=m[4],c=m[8],d=m[1],e=m[5],f=m[9],g=m[2],h=m[6],i=m[10];
  const co=[e*i-f*h,f*g-d*i,d*h-e*g,c*h-b*i,a*i-c*g,b*g-a*h,b*f-c*e,c*d-a*f,a*e-b*d];
  const det=a*co[0]+b*co[1]+c*co[2];check(Number.isFinite(det)&&det!==0,p,'singular or nonfinite transform');
  const out=co.map(v=>v/det);out.forEach(v=>finite(v,p));return {normal:out,det};
}
function canonicalZeros(ir,normalize=true){
  // JSON preserves every finite number except signed zero. Normalize the owned
  // output tree, including retained metadata, without serializing or recursing.
  const stack=[{value:ir,keys:Object.keys(ir),at:0,path:''}];
  while(stack.length){const frame=stack[stack.length-1];
    if(frame.at===frame.keys.length){stack.pop();continue;}
    const key=frame.keys[frame.at++],value=frame.value[key],p=frame.path?frame.path+'/'+key:key;
    if(typeof value==='number'){finite(value,p);if(normalize&&Object.is(value,-0))frame.value[key]=0;}
    else if(value!==null&&typeof value==='object')stack.push({value,keys:Object.keys(value),at:0,path:p});
  }
  return ir;
}
function importGlb(input,options={}){
  object(options,'limits');for(const k of Object.keys(options))check(own(LIMITS,k),'limits/'+k,'unknown limit');
  const limits={...LIMITS,...options};for(const [k,v]of Object.entries(limits))integer(v,1,Math.floor(Number.MAX_SAFE_INTEGER/256),'limits/'+k);
  check(input instanceof Uint8Array,'input','Uint8Array or Buffer required');
  check(!(typeof SharedArrayBuffer!=='undefined'&&input.buffer instanceof SharedArrayBuffer),'input','shared input unsupported','UNSUPPORTED');
  check(input.byteLength<=limits.maxBytes,'input','GLB byte limit','LIMIT');
  const bytes=Buffer.from(input);check(bytes.length>=20,'header','truncated');
  check(bytes.readUInt32LE(0)===0x46546c67&&bytes.readUInt32LE(4)===2,'header','GLB version 2 required');
  check(bytes.readUInt32LE(8)===bytes.length,'header/length','length mismatch');
  let at=12,jsonBytes=null,bin=null;
  while(at<bytes.length){check(bytes.length-at>=8,'chunk','truncated header');const size=bytes.readUInt32LE(at),type=bytes.readUInt32LE(at+4);at+=8;
    check(size%4===0&&size<=bytes.length-at,'chunk','alignment or bounds');
    if(jsonBytes===null){check(type===0x4e4f534a,'chunk','JSON must be first');check(size<=limits.maxJsonBytes,'chunk/JSON','JSON byte limit','LIMIT');jsonBytes=bytes.subarray(at,at+size);}
    else{check(type===0x004e4942&&bin===null,'chunk','only one BIN chunk is supported','UNSUPPORTED');bin=bytes.subarray(at,at+size);}at+=size;}
  let g;try{g=JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(jsonBytes));}catch{fail('INVALID_GLB','JSON','invalid UTF-8 or JSON');}
  // JSON permits exponent overflow. Reject it even in currently unretained
  // extras, before an IR roundtrip could silently replace Infinity with null.
  if(g!==null&&typeof g==='object')canonicalZeros(g,false);
  keys(g,['asset','scene','scenes','nodes','meshes','buffers','bufferViews','accessors','materials','skins','animations','cameras','images','textures','samplers','extensionsUsed','extensionsRequired','extensions','extras'],'root');extensions(g,'root');
  for(const k of ['extensionsUsed','extensionsRequired','skins','animations','cameras','images','textures','samplers'])if(own(g,k))check(Array.isArray(g[k])&&g[k].length===0,k,'unsupported content','UNSUPPORTED');
  keys(g.asset,['version','minVersion','generator','copyright','extensions','extras'],'asset');extensions(g.asset,'asset');
  check(g.asset.version==='2.0'&&(g.asset.minVersion===undefined||g.asset.minVersion==='2.0'),'asset','version 2.0 required');
  const ns=array(g.nodes,'nodes',limits.maxNodes),ms=array(g.meshes,'meshes',limits.maxMeshes),acs=array(g.accessors,'accessors',limits.maxAccessors),views=array(g.bufferViews,'bufferViews',limits.maxBufferViews),mats=array(g.materials,'materials',limits.maxMaterials,true);
  check(ns.length&&ms.length,'root','nonempty static mesh scene required');
  const buffers=array(g.buffers,'buffers',1);check(buffers.length===1&&bin!==null,'buffers','one embedded buffer required');
  keys(buffers[0],['byteLength','name','uri','extensions','extras'],'buffers/0');extensions(buffers[0],'buffers/0');name(buffers[0],'buffers/0',limits);
  check(!own(buffers[0],'uri'),'buffers/0/uri','external/data URI unsupported','UNSUPPORTED');
  const bufferLength=integer(buffers[0].byteLength,1,bin.length,'buffers/0/byteLength');check(bin.length-bufferLength<=3,'BIN','unexpected trailing bytes');
  for(let i=bufferLength;i<bin.length;i++)check(bin[i]===0,'BIN/padding','zero padding required');
  const vs=views.map((v,i)=>{const p=`bufferViews/${i}`;keys(v,['buffer','byteOffset','byteLength','byteStride','target','name','extensions','extras'],p);extensions(v,p);name(v,p,limits);
    check(v.buffer===0,p+'/buffer','embedded buffer index required');const offset=integer(v.byteOffset===undefined?0:v.byteOffset,0,bufferLength,p+'/byteOffset'),length=integer(v.byteLength,1,bufferLength,p+'/byteLength');
    check(length<=bufferLength-offset,p,'view out of bounds');if(v.byteStride!==undefined)check(Number.isInteger(v.byteStride)&&v.byteStride>=4&&v.byteStride<=252&&v.byteStride%4===0,p+'/byteStride','invalid stride');
    if(v.target!==undefined)check(v.target===34962||v.target===34963,p+'/target','unsupported target','UNSUPPORTED');return {...v,offset,length};});
  const accessors=acs.map((a,i)=>{const p=`accessors/${i}`;keys(a,['bufferView','byteOffset','componentType','normalized','count','type','min','max','sparse','name','extensions','extras'],p);extensions(a,p);name(a,p,limits);
    check(!own(a,'sparse'),p+'/sparse','sparse unsupported','UNSUPPORTED');check(a.normalized===undefined||a.normalized===false,p+'/normalized','normalized attributes unsupported','UNSUPPORTED');
    const sizes={5121:1,5123:2,5125:4,5126:4},size=sizes[a.componentType];check(size!==undefined&&(a.type==='VEC3'||a.type==='SCALAR'),p,'unsupported accessor representation','UNSUPPORTED');
    const count=integer(a.count,1,Math.max(limits.maxVertices,limits.maxTriangles*3),p+'/count'),view=vs[integer(a.bufferView,0,vs.length-1,p+'/bufferView')],components=a.type==='VEC3'?3:1,element=size*components;
    const offset=integer(a.byteOffset===undefined?0:a.byteOffset,0,view.length,p+'/byteOffset'),stride=view.byteStride===undefined?element:view.byteStride;
    check(offset%size===0&&(view.offset+offset)%size===0&&stride>=element,p,'accessor alignment or stride');
    check(offset<=view.length-element&&count-1<=Math.floor((view.length-offset-element)/stride),p,'accessor out of bounds');
    for(const k of ['min','max'])if(a[k]!==undefined){vector(a[k],components,p+'/'+k);for(const v of a[k]){
      if(a.componentType===5126)check(Number.isFinite(Math.fround(v)),p+'/'+k,'finite F32 bound');
      else integer(v,0,2**(8*size)-1,p+'/'+k);}}
    if(a.min&&a.max)for(let j=0;j<components;j++)check((a.componentType===5126?Math.fround(a.min[j]):a.min[j])<=(a.componentType===5126?Math.fround(a.max[j]):a.max[j]),p,'invalid min/max');
    return {a,view,count,offset:view.offset+offset,stride,size,components,path:p};});
  let decoded=0,totalV=0,totalT=0,surfaceCount=0;
  function charge(n,p){check(Number.isSafeInteger(n)&&n>=0&&n<=limits.maxDecodedBytes-decoded,p,'decoded numeric payload budget','LIMIT');decoded+=n;}
  function decode(index,role){const p=`accessors/${index}`,d=accessors[integer(index,0,accessors.length-1,p)],{a,view,count,stride}=d;
    if(role==='indices')check(a.type==='SCALAR'&&[5121,5123,5125].includes(a.componentType)&&view.byteStride===undefined&&(view.target===undefined||view.target===34963),p,'unsigned scalar index required');
    else check(a.type==='VEC3'&&a.componentType===5126&&(view.target===undefined||view.target===34962),p,'FLOAT VEC3 vertex attribute required');
    if(role==='POSITION'){check(own(a,'min'),p+'/min','POSITION min required');check(own(a,'max'),p+'/max','POSITION max required');}
    charge(count*d.components*8,p);const result=Array(count),minimum=Array(d.components).fill(Infinity),maximum=Array(d.components).fill(-Infinity);
    for(let i=0;i<count;i++){const row=[];for(let j=0;j<d.components;j++){const off=d.offset+i*stride+j*d.size;let value;
      if(a.componentType===5126)value=bin.readFloatLE(off);else if(d.size===1)value=bin[off];else if(d.size===2)value=bin.readUInt16LE(off);else value=bin.readUInt32LE(off);
      finite(value,p);if(a.min)check(value>=(a.componentType===5126?Math.fround(a.min[j]):a.min[j]),p,'data below min');
      if(a.max)check(value<=(a.componentType===5126?Math.fround(a.max[j]):a.max[j]),p,'data above max');
      minimum[j]=Math.min(minimum[j],value);maximum[j]=Math.max(maximum[j],value);row.push(value);}result[i]=d.components===1?row[0]:row;}
    // Declared extrema describe actual binary values, not merely enclosing bounds.
    for(const [key,actual] of [['min',minimum],['max',maximum]])if(a[key])for(let j=0;j<d.components;j++)
      check(actual[j]===(a.componentType===5126?Math.fround(a[key][j]):a[key][j]),p+'/'+key,'bound must equal actual accessor extremum');
    return result;}
  const materials=mats.map((m,i)=>{const p=`materials/${i}`;keys(m,['name','pbrMetallicRoughness','normalTexture','occlusionTexture','emissiveTexture','emissiveFactor','alphaMode','alphaCutoff','doubleSided','extensions','extras'],p);extensions(m,p);
    for(const k of ['normalTexture','occlusionTexture','emissiveTexture','alphaCutoff'])check(!own(m,k),p+'/'+k,'unsupported material feature','UNSUPPORTED');
    check(m.alphaMode===undefined||m.alphaMode==='OPAQUE',p+'/alphaMode','alpha unsupported','UNSUPPORTED');check(m.doubleSided===undefined||m.doubleSided===false,p+'/doubleSided','double-sided unsupported','UNSUPPORTED');
    if(m.emissiveFactor!==undefined)check(vector(m.emissiveFactor,3,p+'/emissiveFactor').every(x=>x===0),p+'/emissiveFactor','emission unsupported','UNSUPPORTED');
    const pb=m.pbrMetallicRoughness===undefined?{}:m.pbrMetallicRoughness;keys(pb,['baseColorFactor','metallicFactor','roughnessFactor','baseColorTexture','metallicRoughnessTexture','extensions','extras'],p+'/pbrMetallicRoughness');extensions(pb,p+'/pbrMetallicRoughness');
    check(!own(pb,'baseColorTexture')&&!own(pb,'metallicRoughnessTexture'),p,'textures unsupported','UNSUPPORTED');const color=vector(pb.baseColorFactor===undefined?[1,1,1,1]:pb.baseColorFactor,4,p+'/baseColorFactor').slice();
    check(color.every(x=>x>=0&&x<=1)&&color[3]===1,p+'/baseColorFactor','opaque unit color required','UNSUPPORTED');
    const metallic=finite(pb.metallicFactor===undefined?1:pb.metallicFactor,p+'/metallicFactor'),roughness=finite(pb.roughnessFactor===undefined?1:pb.roughnessFactor,p+'/roughnessFactor');check(metallic>=0&&metallic<=1&&roughness>=0&&roughness<=1,p,'PBR factor range');
    return {stableId:`material/${i}`,name:name(m,p,limits),baseColor:color,sourceProperties:{metallicFactor:metallic,roughnessFactor:roughness},extras:m.extras??null};});
  const primitives=ms.map((m,i)=>{const p=`meshes/${i}`;keys(m,['name','primitives','weights','extensions','extras'],p);extensions(m,p);name(m,p,limits);check(!own(m,'weights'),p+'/weights','morph unsupported','UNSUPPORTED');
    const ps=array(m.primitives,p+'/primitives',limits.maxSurfaces);check(ps.length>0,p,'empty primitives');return ps.map((pr,j)=>{const q=p+`/primitives/${j}`;keys(pr,['attributes','indices','material','mode','targets','extensions','extras'],q);extensions(pr,q);
      check(pr.mode===undefined||pr.mode===4,q+'/mode','only TRIANGLES supported','UNSUPPORTED');check(!own(pr,'targets'),q+'/targets','morph unsupported','UNSUPPORTED');keys(pr.attributes,['POSITION','NORMAL'],q+'/attributes');
      integer(pr.attributes.POSITION,0,accessors.length-1,q+'/POSITION');integer(pr.attributes.NORMAL,0,accessors.length-1,q+'/NORMAL');
      if(pr.indices!==undefined)integer(pr.indices,0,accessors.length-1,q+'/indices');if(pr.material!==undefined)integer(pr.material,0,materials.length-1,q+'/material');return pr;});});
  const parent=Array(ns.length).fill(-1),local=ns.map((n,i)=>{const p=`nodes/${i}`;keys(n,['name','mesh','children','translation','rotation','scale','matrix','skin','weights','camera','extensions','extras'],p);extensions(n,p);name(n,p,limits);
    for(const k of ['skin','weights','camera'])check(!own(n,k),p+'/'+k,'unsupported node feature','UNSUPPORTED');
    if(n.mesh!==undefined)integer(n.mesh,0,ms.length-1,p+'/mesh');const children=array(n.children,p+'/children',limits.maxNodes,true),seen=new Set();
    for(const child of children){integer(child,0,ns.length-1,p+'/children');check(!seen.has(child)&&parent[child]===-1,p+'/children','duplicate or multiple parent');seen.add(child);parent[child]=i;}
    const mat=matrix(n,p);inverseTranspose(mat,p);return mat;});
  const scenes=array(g.scenes,'scenes',1);check(scenes.length===1,'scenes','single selected static scene required','UNSUPPORTED');check(g.scene===undefined||g.scene===0,'scene','invalid scene index');
  keys(scenes[0],['name','nodes','extensions','extras'],'scenes/0');extensions(scenes[0],'scenes/0');name(scenes[0],'scenes/0',limits);
  const roots=array(scenes[0].nodes,'scenes/0/nodes',limits.maxNodes),seenRoots=new Set();
  for(const id of roots){integer(id,0,ns.length-1,'scenes/0/nodes');check(!seenRoots.has(id)&&parent[id]===-1,'scenes/0/nodes','root duplicated or has parent');seenRoots.add(id);}
  const nodes=[],surfaces=[],visited=new Set(),stack=roots.slice().reverse().map(id=>({id,world:IDENTITY,depth:1}));
  while(stack.length){const {id,world,depth}=stack.pop(),n=ns[id],p=`nodes/${id}`;check(!visited.has(id),p,'cycle or repeated node');check(depth<=limits.maxDepth,p,'hierarchy depth limit','LIMIT');visited.add(id);
    const next=multiply(world,local[id],p),baked=multiply(BASIS,next,p),{normal,det}=inverseTranspose(baked,p),nodeId=`node/${id}`;
    nodes.push({stableId:nodeId,sourceIndex:id,name:name(n,p,limits),parentId:parent[id]<0?null:`node/${parent[id]}`,worldMatrix:baked,extras:n.extras??null});
    if(n.mesh!==undefined)for(let pi=0;pi<primitives[n.mesh].length;pi++){
      const pr=primitives[n.mesh][pi],sp=p+`/mesh/${n.mesh}/primitive/${pi}`;
      check(++surfaceCount<=limits.maxSurfaces,sp,'surface count limit','LIMIT');const pc=accessors[pr.attributes.POSITION].count,nc=accessors[pr.attributes.NORMAL].count;
      check(pc===nc,sp,'position/normal count mismatch');check(pc<=limits.maxVertices-totalV,sp,'expanded vertex budget','LIMIT');totalV+=pc;
      const ic=pr.indices===undefined?pc:accessors[pr.indices].count;check(ic%3===0,sp,'triangle index count');check(ic/3<=limits.maxTriangles-totalT,sp,'expanded triangle budget','LIMIT');totalT+=ic/3;
      const pos=decode(pr.attributes.POSITION,'POSITION'),norm=decode(pr.attributes.NORMAL,'NORMAL');
      const ix=pr.indices===undefined?(charge(pc*8,sp),Array.from({length:pc},(_,i)=>i)):decode(pr.indices,'indices');
      // Numeric payload allowance includes baked rows, triangle copies and correspondence.
      // JS object/array overhead is bounded by the separate count/input caps, not this estimate.
      charge(pc*56+ic*8+ic/3*8,sp);
      for(let i=0;i<pc;i++){const v=pos[i],o=[0,1,2].map(r=>baked[r]*v[0]+baked[4+r]*v[1]+baked[8+r]*v[2]+baked[12+r]);o.forEach(x=>finite(x,sp));pos[i]=o;
        const q=norm[i],sourceLength=Math.hypot(...q);check(Number.isFinite(sourceLength)&&Math.abs(sourceLength-1)<=1e-4,sp,'unit source normal required');
        const t=[0,1,2].map(r=>normal[r*3]*q[0]+normal[r*3+1]*q[1]+normal[r*3+2]*q[2]),length=Math.hypot(...t);check(Number.isFinite(length)&&length>0,sp,'invalid transformed normal');norm[i]=t.map(v=>v/length);}
      const triangles=[];for(let j=0;j<ic;j+=3){const t=ix.slice(j,j+3);t.forEach(v=>integer(v,0,pc-1,sp+'/indices'));check(new Set(t).size===3,sp,'duplicate triangle index');
        const [a,b,c]=t.map(i=>pos[i]),u=b.map((x,k)=>x-a[k]),v=c.map((x,k)=>x-a[k]),cross=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]];
        check(cross.every(Number.isFinite)&&Math.hypot(...cross)>0,sp,'degenerate or nonfinite triangle');if(det<0)[t[1],t[2]]=[t[2],t[1]];triangles.push(t);}
      surfaces.push({stableId:`${nodeId}/mesh/${n.mesh}/primitive/${pi}`,nodeId,sourceMesh:n.mesh,sourcePrimitive:pi,name:ms[n.mesh].name??null,positions:pos,normals:norm,triangles,
        materialId:pr.material===undefined?null:`material/${pr.material}`,sourceVertexIds:Array.from({length:pc},(_,i)=>i),sourceTriangleIds:Array.from({length:ic/3},(_,i)=>i)});
    }
    for(const child of (n.children??[]).slice().reverse())stack.push({id:child,world:next,depth:depth+1});
  }
  check(visited.size===ns.length,'nodes','unreachable nodes or hierarchy cycle','UNSUPPORTED');check(surfaces.length>0,'scene','no mesh instances');
  return canonicalZeros({schema:'hone-scene-ir-v1',source:{format:'glb2',sha256:createHash('sha256').update(bytes).digest('hex'),byteLength:bytes.length,basis:[1,0,0,0,-1,0,0,0,1],units:'metres',generator:g.asset.generator??null},nodes,surfaces,materials,
    diagnostics:[],limits,counts:{vertices:totalV,triangles:totalT,surfaces:surfaceCount,decodedNumericBytes:decoded},defaultMaterial:{baseColor:[1,1,1,1],metallicFactor:1,roughnessFactor:1}});
}
module.exports={importGlb,ImportError,DEFAULT_LIMITS:LIMITS};
