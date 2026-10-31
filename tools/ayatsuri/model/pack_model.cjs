// AYNM v1 offline compiler. Runtime consumes immutable native data, not JS defs.
'use strict';
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {exportCloud,REF}=require('./export_cloud.cjs');
function int(v,a,b){if(!Number.isInteger(v)||v<a||v>b)throw TypeError('integer range');return v;}
function f32(v){if(typeof v!=='number'||!Number.isFinite(Math.fround(v)))throw TypeError('finite F32');return v;}
function unit(v){if(typeof v!=='number'||!Number.isFinite(v)||v<0||v>1)throw TypeError('unit coefficient');return v;}
function floats(a,n){if(!a||a.length!==n)throw TypeError('vector length');a.forEach(f32);return a;}
function compile(m){
  if(!m||m.schema!=='ayatsuri-native-model-design-v1'||!Array.isArray(m.nodes))throw TypeError('manifest');
  const count=int(m.nodes.length,1,256),nodes=Buffer.alloc(count*16),poses=Buffer.alloc(count*36),radii=[],lobes=[],materials=[],bindings=Buffer.alloc(count*8),names=[],regions=[];
  let nameBytes=0,ellipseCount=0,lobeCount=0;
  for(let i=0;i<count;i++){
    const n=m.nodes[i],kind={group:0,ellipsoid:1,blendEllipsoids:2}[n.kind];
    int(kind,0,2);if(n.index!==i||typeof n.enabled!=='boolean'||typeof n.name!=='string'||!(/^[\x20-\x7e]{1,127}$/).test(n.name)||names.includes(n.name))throw TypeError('identity');
    if(n.pattern!==undefined&&n.pattern!==null){
      const p=n.pattern;if((kind!==1&&kind!==2)||p.kind!=='localPlane')throw TypeError('analytic localPlane pattern');
      const normal=Array.from(floats(p.normal,3),Math.fround),length=normal.reduce((s,v)=>s+v*v,0);
      if(Math.abs(length-1)>1e-4)throw TypeError('unit localPlane normal');
      f32(p.offset);f32(p.feather);
      const limit=kind===1?2:4096;
      if(p.offset< -limit||p.offset>limit||(p.feather!==0&&(p.feather<1e-4||p.feather>limit)))throw TypeError('localPlane range');
      const region=Buffer.alloc(32);region.writeUInt16LE(i,0);region[2]=kind===1?4:5;
      normal.forEach((v,k)=>region.writeFloatLE(v,4+k*4));region.writeFloatLE(p.offset,16);region.writeFloatLE(p.feather,20);
      region.writeUInt32LE(int(p.color,0,0xffffff),24);regions.push(region);
    }
    int(n.parent,-1,i-1);floats(n.pose,9);for(let k=6;k<9;k++)if(Math.fround(n.pose[k])===0)throw TypeError('zero scale');
    n.pose.forEach((v,k)=>poses.writeFloatLE(v,i*36+k*4));
    let shape=65535,material=65535,lc=0,blend=0;
    if(kind){
      const mat=Buffer.alloc(72);floats(n.material,11).forEach((v,k)=>{
        if(k===3){if(v<1||v>128)throw TypeError('exponent');}else unit(v);mat.writeFloatLE(v,k*4);
      });
      int(n.color,0,0xffffff);mat.writeUInt32LE(n.color,44);mat.writeDoubleLE(unit(n.thickness),48);mat.writeDoubleLE(unit(n.opacity),56);mat.writeDoubleLE(unit(n.edgeSoftness),64);
      material=materials.findIndex(b=>b.equals(mat));if(material<0){material=materials.length;materials.push(mat);}
      if(kind===1){floats(n.radii,3);n.radii.forEach(v=>{if(!(Math.fround(v)>0))throw TypeError('radii');});shape=ellipseCount++;const b=Buffer.alloc(12);n.radii.forEach((v,k)=>b.writeFloatLE(v,k*4));radii.push(b);}
      else{lc=int(n.lobes.length/6,1,8);floats(n.lobes,lc*6);shape=lobeCount;lobeCount+=lc;blend=f32(n.blend);if(blend<0||blend>128)throw TypeError('blend');
        const b=Buffer.alloc(lc*24);n.lobes.forEach((v,k)=>{if(k%6>=3&&!(Math.fround(v)>0))throw TypeError('lobe radii');b.writeFloatLE(v,k*4);});lobes.push(b);}
    }
    const o=i*16;nodes.writeInt16LE(n.parent,o);nodes[o+2]=kind;nodes[o+3]=n.enabled?1:0;nodes.writeUInt16LE(shape,o+4);nodes.writeUInt16LE(lc,o+6);nodes.writeUInt16LE(material,o+8);nodes.writeUInt16LE(i,o+10);nodes.writeFloatLE(blend,o+12);
    bindings.writeUInt16LE(i,i*8);bindings.writeUInt16LE(n.name.length,i*8+2);bindings.writeUInt32LE(nameBytes,i*8+4);nameBytes+=n.name.length;names.push(n.name);
  }
  const sections=[{data:nodes,count,stride:16},{data:poses,count,stride:36},{data:Buffer.concat(radii),count:ellipseCount,stride:12},{data:Buffer.concat(lobes),count:lobeCount,stride:24},{data:Buffer.concat(materials),count:materials.length,stride:72},{data:bindings,count,stride:8},{data:Buffer.from(names.join(''),'ascii'),count:nameBytes,stride:1}];
  if(regions.length)sections.push({data:Buffer.concat(regions),count:regions.length,stride:32});
  let total=32+sections.length*20;for(const s of sections){total=(total+7)&~7;s.offset=total;total+=s.data.length;}int(total,32,512*1024);
  const out=Buffer.alloc(total);out.write('AYNM');out.writeUInt16LE(1,4);out.writeUInt16LE(32,6);out.writeUInt32LE(total,8);out.writeUInt32LE(sections.length,12);out.writeUInt32LE(32,16);out.writeUInt32LE(20,20);
  sections.forEach((s,i)=>{const o=32+i*20;out.writeUInt16LE(i+1,o);out.writeUInt16LE(1,o+2);[s.offset,s.data.length,s.count,s.stride].forEach((v,k)=>out.writeUInt32LE(v,o+4+k*4));s.data.copy(out,s.offset);});return out;
}
module.exports={compile};
if(require.main===module){const out=path.resolve(process.argv[2]||'.cache/ayatsuri-native/cloud.aynm'),m=exportCloud(process.argv[3]||REF),bytes=compile(m);
  fs.mkdirSync(path.dirname(out),{recursive:true});fs.writeFileSync(out,bytes);fs.writeFileSync(out+'.json',JSON.stringify(m,null,2)+'\n');console.log(JSON.stringify({out,bytes:bytes.length,nodes:m.nodes.length,sha256:crypto.createHash('sha256').update(bytes).digest('hex')}));}
