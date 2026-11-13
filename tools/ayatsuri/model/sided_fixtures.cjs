'use strict';
const fs=require('node:fs'),path=require('node:path');
const {compile}=require('./pack_model.cjs');
function design({side='single',triangles=64,backFirst=false,nodes=1,nv=3}={}){
 const root={index:0,name:'root',parent:-1,kind:'group',enabled:true,pose:[0,0,0,0,0,0,1,1,1]};
 const vertices=nv===3?[10,10,0,30,10,0,10,30,0]:Array.from({length:nv},(_,i)=>[20+10*Math.cos(i*2*Math.PI/nv),20+10*Math.sin(i*2*Math.PI/nv),0]).flat();
 const indices=Array.from({length:triangles},(_,i)=>{const t=nv===3?[0,1,2]:[0,1+i%(nv-2),2+i%(nv-2)];if((i+(backFirst?1:0))%2)[t[1],t[2]]=[t[2],t[1]];return t;}).flat();
 return {schema:side==='double'?'ayatsuri-native-model-design-v2':'ayatsuri-native-model-design-v1',nodes:[root,...Array.from({length:nodes},(_,i)=>({...root,pose:root.pose.slice(),index:i+1,name:'mesh'+i,parent:0,kind:'gouraudMesh',...(side==='double'?{materialSidedness:'double'}:{}),
  material:[0,1,0,1,0,0,0,0,0,0,0],color:0xffffff,thickness:1,opacity:1,edgeSoftness:0,
  mesh:{vertices:vertices.slice(),normals:Array.from({length:nv},()=>[0,0,1]).flat(),indices:indices.slice(),mask:Array(nv).fill(-1),holes:[],order:'authored',albedoA:0xffff,albedoB:0xffff,thickness:255}}))]};
}
function write(){
 const dir=path.resolve('.cache/ayatsuri-native');fs.mkdirSync(dir,{recursive:true});
 const fixtures={legacy64:design(),sided64:design({side:'double'}),backfirst:design({side:'double',backFirst:true}),back:design({side:'double',triangles:1,backFirst:true}),front:design({side:'double',triangles:1}),five:design({side:'double',nodes:5}),single32:design({nv:32,triangles:30}),sided32:design({side:'double',nv:32,triangles:30})};
 const angled=design({side:'double',triangles:1,backFirst:true});
 angled.nodes[1].pose[6]=1.3;angled.nodes[1].pose[7]=.7;angled.nodes[1].pose[8]=2.2;
 angled.nodes[1].mesh.normals=Array.from({length:3},()=>[.3,.4,Math.sqrt(.75)]).flat();
 angled.nodes[1].material=[.1,.6,.02,8,.2,.8,.5,.3,0,0,0];angled.nodes[1].mesh.albedoA=0xf800;angled.nodes[1].mesh.albedoB=0x07e0;angled.nodes[1].mesh.thickness=175;
 fixtures.angled=angled;
 const sorted=design({side:'double',triangles:2});
 sorted.nodes[1].mesh={...sorted.nodes[1].mesh,vertices:[10,10,3,30,10,3,10,30,3,10,10,1,30,10,1,10,30,1],normals:Array.from({length:6},()=>[0,0,1]).flat(),indices:[0,1,2,3,5,4],mask:Array(6).fill(-1),order:'centroid-depth'};
 fixtures.sorted=sorted;
 for(const [name,m] of Object.entries(fixtures))fs.writeFileSync(path.join(dir,'sided-'+name+'.aynm'),compile(m));
 return fixtures;
}
module.exports={design,write};if(require.main===module)write();
