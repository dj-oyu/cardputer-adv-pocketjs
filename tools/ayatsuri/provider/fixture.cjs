// Public synthetic fans only. No external assets.
const fs=require('fs'),{compile}=require('../model/pack_model.cjs');
let nodes=[{index:0,name:'root',parent:-1,kind:'group',enabled:true,pose:[0,0,0,0,0,0,1,1,1]}];
for(let i=1;i<=23;i++){
 const v=[],idx=[];for(let j=0;j<32;j++)v.push(8*Math.cos(j*2*Math.PI/32),8*Math.sin(j*2*Math.PI/32),0);
 for(let j=1;j<31;j++)idx.push(0,j,j+1);
 nodes.push({index:i,name:'fan'+i,parent:0,kind:'gouraudMesh',enabled:true,pose:[((i-1)%6)*28-70,Math.floor((i-1)/6)*28-42,0,0,0,0,1,1,1],material:[.25,.75,0,4,0,0,0,0,0,0,0],color:0,thickness:1,opacity:1,edgeSoftness:0,mesh:{vertices:v,indices:idx,mask:Array(32).fill(-1),holes:[],order:'authored',albedoA:0xf800,albedoB:0xf800,thickness:255}});
}
fs.writeFileSync(process.argv[2],compile({schema:'ayatsuri-native-model-design-v1',nodes}));
