// Offline comparison fixtures; never replace the product's default fox asset.
'use strict';
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {exportFoxFace}=require('../export_fox_face.cjs'),{compile}=require('../pack_model.cjs');
const VARIANTS=['unchanged','mask-only','mask-inset','blend-mask-only','blend-mask-inset','blend-mask-pruned'];
function model(variant){
  if(!VARIANTS.includes(variant))throw Error('material study variant');
  const m=exportFoxFace();
  if(variant==='mask-only'||variant==='mask-inset'){
    const length=Math.hypot(-.45,.86,.24);
    m.nodes[1].pattern={kind:'localPlane',normal:[-.45/length,.86/length,.24/length],offset:.45,feather:.12,color:0xffe8bb};
    if(variant==='mask-inset')for(const [index,row] of [[1,[-14,5,1,8,6,4]],[3,[-18,7,-2,4,4,3]],[5,[-7,8,13,7,4.5,5]]])
      m.nodes[6].lobes.splice(index*6,6,...row);
  }
  if(variant==='blend-mask-only'||variant==='blend-mask-inset'||variant==='blend-mask-pruned'){
    // One common model-local plane recolours the upper/outer negative-X cheek.
    // Base cream stays on the lower/positive-X surface; no skull mask is added.
    const length=Math.hypot(-.55,-.78,-.3);
    m.nodes[6].pattern={kind:'localPlane',normal:[-.55/length,-.78/length,-.3/length],offset:6,feather:2,color:0xe47b30};
    if(variant==='blend-mask-inset')for(const [index,row] of [
      [1,[-13.5,4.5,3.5,9.5,6.5,4.5]],[3,[-20,6,-1,4.5,3,3]],[5,[-7.5,8.5,13,7.5,5.5,5.5]]])
      m.nodes[6].lobes.splice(index*6,6,...row);
    if(variant==='blend-mask-pruned'){
      const expected=[-27,3,0,6,Math.fround(2.2),4];
      if(JSON.stringify(m.nodes[6].lobes.slice(18,24))!==JSON.stringify(expected))throw Error('unexpected left outer cheek lobe');
      m.nodes[6].lobes.splice(18,6);
    }
  }
  return m;
}
function section(bin,type){
  for(let k=0;k<bin.readUInt32LE(12);k++){
    const d=32+20*k;if(bin.readUInt16LE(d)===type)return bin.subarray(bin.readUInt32LE(d+4),bin.readUInt32LE(d+4)+bin.readUInt32LE(d+8));
  }
  return null;
}
function save(file,data){
  const bytes=Buffer.isBuffer(data)?data:Buffer.from(data);
  if(!fs.existsSync(file)||!fs.readFileSync(file).equals(bytes))fs.writeFileSync(file,bytes);
}
function write(folder){
  fs.mkdirSync(folder,{recursive:true});const base=compile(model('unchanged'));
  const result={schema:'fox-local-plane-host-study-v2',baseline:{variant:'unchanged',bytes:base.length,
    sha256:crypto.createHash('sha256').update(base).digest('hex'),meaning:'unmodified v4; same material-study workload/capture plan'},variants:{}};
  for(const name of VARIANTS){
    const m=model(name),bin=compile(m),prefix=path.join(folder,name);
    const sectionsEqual={};for(let type=1;type<=7;type++)sectionsEqual[type]=section(bin,type).equals(section(base,type));
    if((name==='unchanged'||name==='mask-only'||name==='blend-mask-only')&&!Object.values(sectionsEqual).every(Boolean))throw Error('mask-only changed geometry or materials');
    const region=section(bin,8),mode=region?region[2]:0;
    if(name.startsWith('blend-')&&(!region||region.readUInt16LE(0)!==6||mode!==5))throw Error('blend plane requires common-local mode5');
    let inc='// Generated host-only study asset.\n';
    for(let i=0;i<bin.length;i+=16)inc+='  '+[...bin.subarray(i,i+16)].map(x=>'0x'+x.toString(16).padStart(2,'0')).join(',')+',\n';
    save(prefix+'.aynm',bin);save(prefix+'.inc',inc);save(prefix+'.json',JSON.stringify(m,null,2)+'\n');
    const lobes=m.nodes.filter(n=>n.kind==='blendEllipsoids').reduce((sum,n)=>sum+n.lobes.length/6,0);
    result.variants[name]={bytes:bin.length,nodes:m.nodes.length,lobes,creamFaceLobes:m.nodes[6].lobes.length/6,
      changedNodes:name==='unchanged'?[]:name.startsWith('blend-')?[6]:name==='mask-only'?[1]:[1,6],
      mode,planeScope:mode===5?'blend common model-local coordinates':mode===4?'ellipsoid unit q':null,
      baseSectionsEqual:sectionsEqual,sha256:crypto.createHash('sha256').update(bin).digest('hex')};
  }
  save(path.join(folder,'manifest.json'),JSON.stringify(result,null,2)+'\n');return result;
}
module.exports={model,write,VARIANTS};
if(require.main===module)console.log(JSON.stringify(write(path.resolve(process.argv[2]||'.cache/fox-local-plane-assets'))));
