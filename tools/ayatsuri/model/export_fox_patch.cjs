// Offline HOST STUDY assets. The default fox model is never replaced here.
'use strict';
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),crypto=require('node:crypto');
const {exportFoxFace,ROOT}=require('./export_fox_face.cjs');
const SOURCE='tools/character/fox_retopology_fixture.js';
const VARIANTS=['baseline','eye','mask','shape','combined'];
const KEEP=[0,1,2,3,9,10,11,12,16,17,18,19,25,26,27,28];
const SHAPE={3:[29,1,0],4:[26,8,3],5:[16,15,3],6:[0,18,4],7:[-16,15,3],8:[-26,8,3],9:[-29,1,0],
  23:[0,10.5,25],24:[0,15.5,17],25:[-10,7,23],26:[10,7,23],27:[-10,12.5,20],28:[10,12.5,20],29:[0,13,23]};
function sha(data){return crypto.createHash('sha256').update(data).digest('hex');}
function rgb565(v){return ((v>>>19)<<11)|(((v>>>10)&63)<<5)|((v>>>3)&31);}
function capture(variant){
  if(!VARIANTS.includes(variant))throw Error('unknown fox patch variant');
  let code=fs.readFileSync(path.join(ROOT,SOURCE),'utf8');
  if(variant==='shape'||variant==='combined'){
    const rx=/source\.set\(new Float32Array\(\[([^\]]+)\]\)\);/g,matches=[...code.matchAll(rx)];
    if(matches.length!==1)throw Error('expected one authored source literal');
    const xyz=JSON.parse('['+matches[0][1]+']');if(xyz.length!==90)throw Error('expected 30 vertices');
    for(const [id,p]of Object.entries(SHAPE))for(let k=0;k<3;k++)xyz[id*3+k]=p[k];
    code=code.replace(rx,'source.set(new Float32Array('+JSON.stringify(xyz)+'));');
  }
  if(variant==='mask'||variant==='combined'){
    const old='patchMask[i]=source[j+1]+.3*Math.abs(source[j])-5;';
    if(code.split(old).length!==2)throw Error('expected one baseline mask expression');
    code=code.replace(old,'patchMask[i]=source[j+1]-(1.8+3.2*Math.exp(-((source[j]/10)**2)));');
  }
  const marker='return {draw,stats,release};';
  if(code.split(marker).length!==2)throw Error('expected one fixture closure return');
  code=code.replace(marker,'return {offline:{source,indices,restNormals,patchMask,material,eyeDepth:[depth(-11,-5)-.2,depth(11,-5)-.2],noseZ:depth(0,6)+1}};');
  const forbidden=()=>{throw Error('offline capture must not draw or allocate an instance');};
  const context=vm.createContext({pocket:{hone:{},kasane:{procedural:{resource(){return 1;}},replace:forbidden}}});
  vm.runInContext(code,context,{filename:SOURCE});
  const x=context.FoxPatchStudy.offline;
  const result={vertices:Array.from(x.source.slice(0,90)),extra:Array.from(x.source.slice(90)),indices:Array.from(x.indices),
    normals:Array.from(x.restNormals),mask:Array.from(x.patchMask),material:Array.from(x.material).concat([0,0,0]),
    eyeDepth:Array.from(x.eyeDepth),noseZ:x.noseZ};
  if(result.vertices.length!==90||result.indices.length!==138||result.extra.length!==75||result.normals.length!==90||result.mask.length!==30)
    throw Error('unexpected offline geometry layout');
  return result;
}
function author(variant){
  const raw=exportFoxFace(),fixed=capture(variant),remap=new Map(KEEP.map((old,i)=>[old,i]));
  const nodes=KEEP.map((old,index)=>{
    const n=JSON.parse(JSON.stringify(raw.nodes[old]));n.index=index;n.parent=n.parent<0?-1:remap.get(n.parent);
    if(n.parent===undefined)throw Error('pruned ancestor');n.enabled=true;
    return n;
  });
  for(let side=0;side<2;side++){
    const old=side?16:9,eye=nodes[remap.get(old)];eye.pose=[side?11:-11,-5,fixed.eyeDepth[side],0,0,0,1,1,1];
    const iris=nodes[remap.get(side?18:11)],pupil=nodes[remap.get(side?19:12)],changed=variant==='eye'||variant==='combined';
    iris.pose=[0,.9,2.6,0,0,0,changed?1:1.15,changed?1:1.12,1];
    pupil.pose=[0,.9,3.4,0,0,0,changed?1:1.1,changed?1:1.05,1];
  }
  nodes.push({index:16,name:'fox-face-sheet',parent:0,kind:'gouraudMesh',enabled:true,pose:[0,0,0,0,0,0,1,1,1],
    color:0xe47b30,thickness:1,opacity:1,edgeSoftness:0,material:fixed.material,
    mesh:{vertices:fixed.vertices,normals:fixed.normals,mask:fixed.mask,indices:fixed.indices,
      holes:[fixed.extra.slice(0,24),fixed.extra.slice(24,48)],order:'centroid-depth',
      albedoA:rgb565(0xe47b30),albedoB:rgb565(0xffe8bb),thickness:175}});
  const asset='fox-patch-'+(variant==='baseline'?'compiled':variant);
  const metadata={asset,variant,extra:fixed.extra,noseZ:fixed.noseZ,root:0,
    ears:[{side:-1,outer:remap.get(2),inner:remap.get(25),tuft:remap.get(26)},
      {side:1,outer:remap.get(3),inner:remap.get(27),tuft:remap.get(28)}]};
  return {manifest:{schema:'ayatsuri-native-model-design-v1',nodes},metadata};
}
function initializer(data){let out='// HOST STUDY ONLY. Generated native immutable asset.\n';
  for(let i=0;i<data.length;i+=16)out+='  '+[...data.subarray(i,i+16)].map(x=>'0x'+x.toString(16).padStart(2,'0')).join(',')+',\n';return out;}
function writeIfChanged(file,value){
  const bytes=Buffer.isBuffer(value)?value:Buffer.from(value);
  if(fs.existsSync(file)&&fs.readFileSync(file).equals(bytes))return;
  const temporary=file+'.tmp-'+process.pid;
  try{fs.writeFileSync(temporary,bytes);fs.renameSync(temporary,file);}
  finally{if(fs.existsSync(temporary))fs.unlinkSync(temporary);}
}
function generate(folder=path.join(ROOT,'.cache/fox-compiled-assets')){
  const {compile}=require('./pack_model.cjs');fs.mkdirSync(folder,{recursive:true});
  const inputs=[SOURCE,'tools/ayatsuri/model/export_fox_patch.cjs','tools/ayatsuri/model/export_fox_face.cjs',
    'tools/ayatsuri/model/fixtures/fox_face_v4.js','tools/ayatsuri/model/pack_model.cjs','apps/character/character.js','apps/character/character_blend.js'];
  const before=Object.fromEntries(inputs.map(f=>[f,sha(fs.readFileSync(path.join(ROOT,f)))])),outputs=[];
  for(const variant of VARIANTS){
    const {manifest,metadata}=author(variant),data=compile(manifest),stem=metadata.asset;
    const files={};files[stem+'.aynm']=data;files[stem+'_bytes.inc']=initializer(data);
    files[stem+'.json']=JSON.stringify(manifest,null,2)+'\n';
    files[stem+'_data.js']='// HOST STUDY ONLY: one selected variant, no rest geometry JS graph.\n'+
      'globalThis.__foxCompiledData='+JSON.stringify({...metadata,extra:undefined})+';\n'+
      'globalThis.__foxCompiledData.extra=new Float32Array('+JSON.stringify(metadata.extra)+');\n';
    const hashes={};for(const [name,value]of Object.entries(files)){writeIfChanged(path.join(folder,name),value);hashes[name]=sha(value);}
    outputs.push({variant,asset:stem,bytes:data.length,nodes:manifest.nodes.length,vertices:30,triangles:46,sha256:hashes});
  }
  for(const [file,hash]of Object.entries(before))if(sha(fs.readFileSync(path.join(ROOT,file)))!==hash)throw Error('source changed during export: '+file);
  const proof={kind:'HOST STUDY COLD ASSETS / NOT DEFAULT FOX',source_sha256:before,outputs};
  writeIfChanged(path.join(folder,'manifest.json'),JSON.stringify(proof,null,2)+'\n');return proof;
}
function promote(check=false){
  const folder=path.join(ROOT,'apps/foxsheet'),a=author('combined'),{compile}=require('./pack_model.cjs'),data=compile(a.manifest);
  fs.mkdirSync(path.join(folder,'assets'),{recursive:true});
  const metadata={...a.metadata,asset:'fox-sheet',extra:undefined},outputs={};
  outputs['assets/fox_sheet.aynm']=data;
  outputs['assets/fox_sheet_bytes.inc']=initializer(data).replace('HOST STUDY ONLY. Generated native immutable asset.','Generated selected neutral FOX SHEET asset; do not edit.');
  outputs['fox_sheet_data.js']='// Generated selected neutral metadata. Fixed surface geometry remains native.\n'+
    'globalThis.FoxSheetData='+JSON.stringify(metadata)+';\n'+
    'globalThis.FoxSheetData.extra=new Float32Array('+JSON.stringify(a.metadata.extra)+');\n';
  for(const [name,value]of Object.entries(outputs)){
    const file=path.join(folder,name),bytes=Buffer.from(value);
    if(!fs.existsSync(file)||!fs.readFileSync(file).equals(bytes)){if(check)throw Error('stale selected FOX SHEET asset: '+name);fs.writeFileSync(file,bytes);}
  }
  return {asset:'fox-sheet',bytes:data.length,sha256:sha(data),variant:'combined',defaultFoxUnchanged:true};
}
module.exports={author,capture,generate,promote};
if(require.main===module){if(process.argv.includes('--promote')||process.argv.includes('--check-product'))console.log('FOX_SHEET_SELECTED',JSON.stringify(promote(process.argv.includes('--check-product'))));else{const i=process.argv.indexOf('--out-dir');console.log('FOX_PATCH_COMPILED_ASSETS',JSON.stringify(generate(i<0?undefined:path.resolve(process.argv[i+1]))));}}
