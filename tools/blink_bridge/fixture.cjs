"use strict";
const fs=require('node:fs'),path=require('node:path'),{compile}=require('../ayatsuri/model/pack_model.cjs');
const nodes=[{index:0,name:'root',parent:-1,kind:'group',enabled:true,pose:[0,0,0,0,0,0,1,1,1]}];
for(let side of [-1,1]){const x=side*3;nodes.push({index:nodes.length,name:side<0?'left':'right',parent:0,kind:'gouraudMesh',enabled:true,pose:[0,0,0,0,0,0,1,1,1],material:[1,0,0,4,0,0,0,0,0,0,0],color:0,thickness:1,opacity:1,edgeSoftness:0,mesh:{vertices:[x-1,-1,0,x+1,-1,0,x+1,1,0,x-1,1,0],indices:[0,1,2,0,2,3],mask:[-1,-1,-1,-1],holes:[],order:'authored',albedoA:side<0?0xf800:0x001f,albedoB:side<0?0xf800:0x001f,thickness:255}});}
fs.mkdirSync(process.argv[2],{recursive:true});fs.writeFileSync(path.join(process.argv[2],'skin-plain.aynm'),compile({schema:'ayatsuri-native-model-design-v1',nodes}));
