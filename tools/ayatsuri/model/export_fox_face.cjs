// Capture the complete fixed authoring fixture offline; clips stay in the JS driver.
'use strict';
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),crypto=require('node:crypto');
const {compile}=require('./pack_model.cjs');
const ROOT=path.resolve(__dirname,'../../..'),FOLDER=path.join(ROOT,'apps/fox3d/assets');
const SOURCE_SHA256='88baa1d39a867fdbffe11ee8d79558de125f67ddd2ff22dc1bd057b0f252f6d7';
const SOURCE_LF_SHA256='88baa1d39a867fdbffe11ee8d79558de125f67ddd2ff22dc1bd057b0f252f6d7';
const SOURCE=path.join(__dirname,'fixtures/fox_face_v3.js');
function exportFoxFace(file=SOURCE){
  const source=fs.readFileSync(file,'utf8');
  if(crypto.createHash('sha256').update(source.replace(/\r\n/g,'\n')).digest('hex')!==SOURCE_LF_SHA256)throw Error('expected v3 29-node authoring fixture');
  const ctx=vm.createContext({}),provenance=[],handles=[],apertures=[];
  for(const name of ['apps/character/character.js','apps/character/character_blend.js']){
    const code=fs.readFileSync(path.join(ROOT,name),'utf8');
    provenance.push({file:name,sha256:crypto.createHash('sha256').update(code).digest('hex')});vm.runInContext(code,ctx,{filename:name});
  }
  vm.runInContext(source,ctx,{filename:file});
  const forbidden=()=>{throw Error('offline export must not evaluate or draw');};
  const scene=ctx.Ayatsuri.scene({backend:{ellipsoidPacked:forbidden,blendEllipsoids:forbidden},evaluator:{composeNode:forbidden},geometries:[ctx.AyatsuriBlend]});
  ctx.FoxFaceModel.create({add(def){const h=scene.add(def);handles.push(h);return h;},aperture(anchor,vertices){
    const a={anchor,targets:[],vertices:Array.from(vertices),empty:null};apertures.push(a);
    return {bind(h){a.targets.push(h);return this;},setVertices(v){a.vertices=Array.from(v);},setEmpty(reason){a.empty=reason;}};
  }});
  const ids=new Map(handles.map((h,i)=>[h._node,i]));
  const nodes=handles.map((h,index)=>{
    const n=h._node,p=n.analytic,kind=p?p.kind:'group',parent=n.parent?ids.get(n.parent):-1;
    if(parent===undefined)throw Error('ancestor omitted');
    const out={index,name:n.name,parent,kind,enabled:n.enabled,pose:Array.from(n.matrix.slice(12,21))};
    if(p){
      if(n.pattern)throw Error('pattern unsupported');
      Object.assign(out,{color:n.color,thickness:n.thickness,opacity:n.opacity,edgeSoftness:n.edgeSoftness});
      if(kind==='ellipsoid'){out.radii=Array.from(p.work.slice(21,24));out.material=Array.from(p.work.slice(41,49)).concat([0,0,0]);}
      else{out.material=Array.from(n.materialData);out.lobes=Array.from(p.local);out.blend=p.controls[0];}
    }
    return out;
  });
  // This metadata is an offline verification result, not an AYNM extension.
  const clips=apertures.map(a=>({anchor:ids.get(a.anchor._node),targets:a.targets.map(h=>ids.get(h._node)),vertices:a.vertices,empty:a.empty}));
  scene.dispose();return {schema:'ayatsuri-native-model-design-v1',sourceModelSha256:SOURCE_SHA256,sourceModelLfSha256:SOURCE_LF_SHA256,provenance,nodes,clips};
}
function initializer(data){let out='// Generated from fox_face.aynm; do not edit.\n';
  for(let i=0;i<data.length;i+=16)out+='  '+[...data.subarray(i,i+16)].map(x=>'0x'+x.toString(16).padStart(2,'0')).join(',')+',\n';return out;}
function generate(write=false){
  const manifest=exportFoxFace(),data=compile(manifest),inc=initializer(data),bin=path.join(FOLDER,'fox_face.aynm'),header=path.join(FOLDER,'fox_face_bytes.inc');
  if(write){fs.mkdirSync(FOLDER,{recursive:true});fs.writeFileSync(bin,data);fs.writeFileSync(header,inc);}
  else if(!fs.readFileSync(bin).equals(data)||fs.readFileSync(header,'utf8').replace(/\r\n/g,'\n')!==inc)throw Error('stale fox face asset');
  return {bytes:data.length,nodes:manifest.nodes.length,sha256:crypto.createHash('sha256').update(data).digest('hex'),ids:manifest.nodes.map(n=>n.name),clips:manifest.clips.map(a=>({anchor:a.anchor,targets:a.targets}))};
}
module.exports={exportFoxFace,generate,ROOT,SOURCE_SHA256};
if(require.main===module)console.log('FOX_FACE_ASSET_PASS',JSON.stringify(generate(process.argv.includes('--write'))));
