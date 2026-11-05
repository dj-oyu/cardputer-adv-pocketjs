// Public synthetic rigid-joint fixture; no authored character or external data.
'use strict';
const fs=require('node:fs'),path=require('node:path');
const {compile}=require('./pack_model.cjs');
const pose=[0,0,0,0,0,0,1,1,1];
const group=(index,name,parent,p=pose)=>({index,name,parent,kind:'group',enabled:true,pose:p});
const mesh=(index,name,parent,w,h,z,color)=>({index,name,parent,kind:'gouraudMesh',enabled:true,pose,
  material:[1,0,0,4,0,0,0,0,0,0,0],color:0,thickness:1,opacity:1,edgeSoftness:0,
  mesh:{vertices:[-w,-h,z,w,-h,-z,w,h,-z,-w,h,z],indices:[0,1,2,0,2,3],
    mask:[-1,-1,-1,-1],holes:[],order:'authored',albedoA:color,albedoB:color,thickness:255}});
const nodes=[
  group(0,'root',-1),group(1,'leftJoint',0,[-28,0,0,0,0,0,1,1,1]),
  mesh(2,'left',1,15,27,3,0xf800),group(3,'rightJoint',0,[32,7,-2,0,0,0,1,1,1]),
  mesh(4,'right',3,10,17,-2,0x001f)];
const bytes=compile({schema:'ayatsuri-native-model-design-v1',nodes});
const out=path.resolve(__dirname,'../../../.cache/ayatsuri-native/scene-motion.aynm');
fs.mkdirSync(path.dirname(out),{recursive:true});fs.writeFileSync(out,bytes);
const sided=nodes.map(n=>n.kind==='gouraudMesh'?{...n,materialSidedness:n.name==='left'?'double':'single'}:n);
const double=compile({schema:'ayatsuri-native-model-design-v2',nodes:sided});
fs.writeFileSync(out.replace('.aynm','-double.aynm'),double);
console.log(`SCENE_MOTION_PUBLIC_FIXTURE bytes=${bytes.length} nodes=5 joints=3 meshes=2 triangles=4`);
console.log(`SCENE_MOTION_EXPLICIT_DOUBLE_FIXTURE bytes=${double.length} left=double right=single same_geometry`);
