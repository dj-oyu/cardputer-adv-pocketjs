// Offline comparison fixtures; never replace the product's default fox asset.
'use strict';
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {exportFoxFace}=require('../export_fox_face.cjs'),{compile}=require('../pack_model.cjs');
function model(variant){
  if(!['mask-only','mask-inset'].includes(variant))throw Error('material study variant');
  const m=exportFoxFace(),length=Math.hypot(-.45,.86,.24);
  m.nodes[1].pattern={kind:'localPlane',normal:[-.45/length,.86/length,.24/length],offset:.45,feather:.12,color:0xffe8bb};
  if(variant==='mask-inset')for(const [index,row] of [[1,[-14,5,1,8,6,4]],[3,[-18,7,-2,4,4,3]],[5,[-7,8,13,7,4.5,5]]])
    m.nodes[6].lobes.splice(index*6,6,...row);
  return m;
}
function write(folder){
  fs.mkdirSync(folder,{recursive:true});const result={schema:'fox-local-plane-host-study-v1',variants:{}};
  for(const name of ['mask-only','mask-inset']){
    const m=model(name),bin=compile(m),prefix=path.join(folder,name);
    let inc='// Generated host-only study asset.\n';
    for(let i=0;i<bin.length;i+=16)inc+='  '+[...bin.subarray(i,i+16)].map(x=>'0x'+x.toString(16).padStart(2,'0')).join(',')+',\n';
    fs.writeFileSync(prefix+'.aynm',bin);fs.writeFileSync(prefix+'.inc',inc);fs.writeFileSync(prefix+'.json',JSON.stringify(m,null,2)+'\n');
    result.variants[name]={bytes:bin.length,nodes:29,changedNodes:name==='mask-only'?[1]:[1,6],sha256:crypto.createHash('sha256').update(bin).digest('hex')};
  }
  fs.writeFileSync(path.join(folder,'manifest.json'),JSON.stringify(result,null,2)+'\n');return result;
}
module.exports={model,write};
if(require.main===module)console.log(JSON.stringify(write(path.resolve(process.argv[2]||'.cache/fox-local-plane-assets'))));
