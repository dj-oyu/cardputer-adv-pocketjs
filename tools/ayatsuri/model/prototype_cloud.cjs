// Generate experimental adapters from the pinned update body, without shape defs.
'use strict';
const fs=require('node:fs'),path=require('node:path');
const {ROOT,REF,git}=require('./export_cloud.cjs');
function generate(ref=REF){
  const src=git(ref,'apps/cloud/cloud_model.js'),a=src.indexOf('    function pose('),b=src.indexOf('    function group('),
    c=src.indexOf('    function update(timeMs,body,expression) {'),d=src.indexOf('    return {root,parts,patterns,update',c);
  if([a,b,c,d].some(v=>v<0))throw Error('source layout changed');
  const banner='// Experimental adapter generated from '+ref+'; immutable definitions remain native.\n\'use strict\';\n';
  const update=banner+'globalThis.AyatsuriCloudUpdate={create(scene,index){\n'+
    '  const h=[];for(let i=0;i<19;i++)h.push(scene.handle(i));\n'+
    '  const root=h[0],bodyShape=h[1],head=h[2],halo=h[13],parts=[h[1],h[3],h[4],h[5],h[6],h[7],h[8],h[9],h[10],h[11],h[12],h[14],h[15],h[16],h[17],h[18]],patterns=[],\n'+
    '    scratch={position:[0,0,0],rotation:[0,0,0],scale:[1,1,1]},\n'+
    '    cloud={hands:[h[3],h[7]],pupils:[h[4],h[8]],brows:[h[5],h[9]],cheeks:[h[6],h[10]],wisps:[h[14],h[15],h[16],h[17],h[18]],mouth:h[11],tongue:h[12],tongueMaterial:{opacity:0,color:0xf29b99}};\n'+
    src.slice(a,b)+src.slice(c,d)+
    '  return {root,parts,patterns,update,name:\'CLOUD\',index,sample:AyatsuriCloudMotion,sampleFactory:typeof AyatsuriCloudDynamic===\'undefined\'?null:AyatsuriCloudDynamic.create};\n}};\n';
  const model=banner+'globalThis.AyatsuriNativeCloudModel=(()=>{let model;return function(){if(!model)model=__nativeCloudModelCreate();return model;};})();\n'+
    'globalThis.AyatsuriModels={names:[\'CLOUD\'],needsBlend:false,create:AyatsuriCloudUpdate.create};\n';
  const runtime=git(ref,'apps/cloudprobe/cloud_probe_runtime.js').replace('Ayatsuri.scene({backend:pocket.hone,geometries:[AyatsuriBlend],light,camera})','AyatsuriNative.scene({model:AyatsuriNativeCloudModel(),light,camera})');
  if(runtime===git(ref,'apps/cloudprobe/cloud_probe_runtime.js'))throw Error('probe scene construction changed');
  const files={'apps/cloud/cloud_shared_update.js':update,'apps/cloud/cloud_native_model.js':model,
    'apps/cloudprobe/native_probe_runtime.js':banner+runtime,
    'apps/cloudprobe/native_probe.js':banner+['utility','follow','motion','dynamic','update','model','probe'].map(k=>"pocket.app.load('"+k+"');").join('\n')+'\n',
    'apps/cloudprobe/native_chunks.txt':'app local.nativecloudprobe\noption strip-debug\nutility ../lib/ayatsuri_model.js\nfollow ../lib/ayatsuri_follow.js\nmotion ../cloud/cloud_pose.js\ndynamic ../cloud/cloud_dynamic.js\nupdate ../cloud/cloud_shared_update.js\nmodel ../cloud/cloud_native_model.js\nprobe native_probe_runtime.js\nfixture ../cloud/cloud_fixture.js\n'};
  for(const [file,code]of Object.entries(files))fs.writeFileSync(path.join(ROOT,file),code);
  return Object.fromEntries(Object.entries(files).map(([file,code])=>[file,Buffer.byteLength(code)]));
}
module.exports={generate};if(require.main===module)console.log(JSON.stringify(generate(process.argv[2]||REF)));
