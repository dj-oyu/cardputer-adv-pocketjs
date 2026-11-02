// Small authored synthetic geometry only; no character or private assets.
const fs=require('node:fs'),path=require('node:path');
const {compile}=require('../ayatsuri/model/pack_model.cjs');
const group={index:0,name:'root',parent:-1,kind:'group',enabled:true,pose:[0,0,0,0,0,0,1,1,1]};
const patch={...group,index:1,name:'mesh',parent:0,kind:'gouraudMesh',material:[.4,.6,.025,4,.08,1,.73,.44,0,0,0],color:0xe47b30,thickness:1,opacity:1,edgeSoftness:0,
 mesh:{vertices:[-10,-10,0,10,-10,0,-10,10,0],indices:[0,1,2],mask:[-1,-1,1],holes:[],order:'authored',albedoA:0xe3c6,albedoB:0xff57,thickness:175}};
const output=path.resolve('.cache/ayatsuri-native/mesh-depth.aynm');fs.mkdirSync(path.dirname(output),{recursive:true});fs.writeFileSync(output,compile({schema:'ayatsuri-native-model-design-v1',nodes:[group,patch]}));
