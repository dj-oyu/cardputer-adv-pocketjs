// Two 32-vertex public fans exercise the aggregate scratch boundary.
'use strict';
const fs=require('node:fs'),path=require('node:path'),{compile}=require('../model/pack_model.cjs');
const out=process.argv[2];if(!out)throw Error('output directory required');
const root={index:0,name:'root',parent:-1,kind:'group',enabled:true,pose:[0,0,0,0,0,0,1,1,1]};
const vertices=[],indices=[];
for(let j=0;j<32;j++)vertices.push(20*Math.cos(j*2*Math.PI/32),20*Math.sin(j*2*Math.PI/32),0);
for(let j=1;j<31;j++)indices.push(0,j,j+1);
const mesh=index=>({index,name:`fan${index}`,parent:0,kind:'gouraudMesh',enabled:true,
  pose:[(index*2-3)*25,0,0,0,0,0,1,1,1],material:[1,0,0,4,0,0,0,0,0,0,0],
  color:0,thickness:1,opacity:1,edgeSoftness:0,
  mesh:{vertices,indices,mask:Array(32).fill(-1),holes:[],order:'authored',albedoA:index===1?0xf800:31,albedoB:0,thickness:255}});
const blob=compile({schema:'ayatsuri-native-model-design-v1',nodes:[root,mesh(1),mesh(2)]});
fs.mkdirSync(out,{recursive:true});fs.writeFileSync(path.join(out,'skin-limit.aynm'),blob);
console.log(`SKIN_BATCH_LIMIT_FIXTURE bytes=${blob.length} meshes=2 vertices=64 triangles=60`);
