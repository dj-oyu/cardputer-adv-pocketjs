// Offline normalized export from the pinned real authoring utility, before evaluation.
'use strict';
const fs=require('node:fs'),path=require('node:path'),cp=require('node:child_process'),vm=require('node:vm'),crypto=require('node:crypto');
const ROOT=path.resolve(__dirname,'../../..'),REF='7d21b3020b649aadc343c367f60948eca4abaf62';
function git(ref,file){return cp.execFileSync('git',['show',ref+':'+file],{cwd:ROOT,encoding:'utf8'});}
function exportCloud(ref=REF){
  const commit=cp.execFileSync('git',['rev-parse',ref+'^{commit}'],{cwd:ROOT,encoding:'utf8'}).trim(),nodes=[],map=new Map(),provenance=[];
  const forbidden=()=>{throw Error('static export must not evaluate');},ctx=vm.createContext({AyatsuriCloudMotion:forbidden});
  for(const file of ['apps/character/character.js','apps/character/character_blend.js','apps/cloud/cloud_model.js']){
    const source=git(commit,file);provenance.push({file,bytes:Buffer.byteLength(source),sha256:crypto.createHash('sha256').update(source).digest('hex')});
    vm.runInContext(source,ctx,{filename:file});
  }
  const scene=ctx.Ayatsuri.scene({backend:{ellipsoidPacked:forbidden,blendEllipsoids:forbidden},evaluator:{composeNode:forbidden},geometries:[ctx.AyatsuriBlend]});
  ctx.AyatsuriModels.create({add(def){
    const h=scene.add(def),n=h._node,p=n.analytic,kind=p?p.kind:'group',record={index:nodes.length,name:n.name,parent:n.parent?map.get(n.parent):-1,
      kind,enabled:n.enabled,pose:Array.from(n.matrix.slice(12,21))};
    if(p){
      if(n.pattern)throw Error('patterns unsupported');
      Object.assign(record,{color:n.color,thickness:n.thickness,opacity:n.opacity,edgeSoftness:n.edgeSoftness});
      if(kind==='ellipsoid'){record.radii=Array.from(p.work.slice(21,24));record.material=Array.from(p.work.slice(41,49)).concat([0,0,0]);}
      else{record.material=Array.from(n.materialData);record.lobes=Array.from(p.local);record.blend=p.controls[0];}
    }
    map.set(n,record.index);nodes.push(record);return h;
  }},0);
  return {schema:'ayatsuri-native-model-design-v1',sourceCommit:commit,provenance,nodes};
}
module.exports={exportCloud,ROOT,REF,git};
if(require.main===module){const out=path.resolve(process.argv[2]||'.cache/ayatsuri-native/cloud.json'),manifest=exportCloud(process.argv[3]||REF);
  fs.mkdirSync(path.dirname(out),{recursive:true});fs.writeFileSync(out,JSON.stringify(manifest,null,2)+'\n');console.log(JSON.stringify({out,nodes:manifest.nodes.length,sourceCommit:manifest.sourceCommit}));}
