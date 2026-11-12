// Public synthetic strip and background only; no character assets.
'use strict';
const fs=require('node:fs'),path=require('node:path');
const {compile}=require('../model/pack_model.cjs');
const out=process.argv[2];if(!out)throw Error('output directory required');
const root={index:0,name:'root',parent:-1,kind:'group',enabled:true,pose:[7,-3,0,0,0,0,1,1,1]};
const strip={index:1,name:'strip',parent:0,kind:'gouraudMesh',enabled:true,pose:[4,2,0,0,0,0,1,1,1],
  material:[.25,.75,0,4,0,0,0,0,0,0,0],color:0,thickness:1,opacity:1,edgeSoftness:0,
  mesh:{vertices:[-30,-15,0,0,-15,0,30,-15,0,-30,15,0,0,15,0,30,15,0],
    indices:[0,1,4,0,4,3,1,2,5,1,5,4],mask:[-1,-1,-1,-1,-1,-1],holes:[],
    order:'authored',albedoA:0xf800,albedoB:0xf800,thickness:255}};
const back={...strip,index:2,name:'back',pose:[4,2,-5,0,0,0,1,1,1],material:[1,0,0,4,0,0,0,0,0,0,0],
  mesh:{...strip.mesh,vertices:[-35,-20,0,35,-20,0,35,20,0,-35,20,0],indices:[0,1,2,0,2,3],
    mask:[-1,-1,-1,-1],albedoA:0x001f,albedoB:0x001f}};
fs.mkdirSync(out,{recursive:true});
for(const variant of ['plain','holes','sided']){
  let nodes=[root,strip,back];
  if(variant==='holes')nodes=[root,{...strip,mesh:{...strip.mesh,holes:[[-5,-5,0,5,-5,0,0,5,0]]}},back];
  if(variant==='sided')nodes=nodes.map(n=>n.kind==='gouraudMesh'?{...n,materialSidedness:n.index===1?'double':'single'}:n);
  const blob=compile({schema:variant==='sided'?'ayatsuri-native-model-design-v2':'ayatsuri-native-model-design-v1',nodes});
  fs.writeFileSync(path.join(out,`skin-${variant}.aynm`),blob);
  console.log(`SKIN_RENDER_FIXTURE variant=${variant} bytes=${blob.length} nodes=3 strip_vertices=6`);
}
