'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {exportCloud,ROOT}=require('./export_cloud.cjs'),{compile}=require('./pack_model.cjs');
const m=exportCloud(),bytes=compile(m),again=compile(exportCloud());assert.ok(bytes.equals(again));
assert.equal(bytes.length,2766);assert.equal(crypto.createHash('sha256').update(bytes).digest('hex'),'f915d05ec20634dc7bb61fc5c3e9413728ab723581cf8153b51da00bd7823f97');
assert.equal(bytes.toString('ascii',0,4),'AYNM');assert.equal(bytes.readUInt32LE(12),7);
let end=172;for(let i=0;i<7;i++){
  const o=32+i*20,offset=bytes.readUInt32LE(o+4),size=bytes.readUInt32LE(o+8),count=bytes.readUInt32LE(o+12),stride=bytes.readUInt32LE(o+16);
  assert.equal(offset%8,0);assert.ok(offset>=end);assert.equal(size,count*stride);assert.equal(bytes.readUInt16LE(o),i+1);assert.equal(bytes.readUInt16LE(o+2),1);end=offset+size;
}assert.equal(end,bytes.length);
let rejected=0;function bad(fn){const copy=structuredClone(m);fn(copy);assert.throws(()=>compile(copy));++rejected;}
for(const fn of [x=>x.nodes=[],x=>x.nodes[0].parent=0,x=>x.nodes[1].parent=19,x=>x.nodes[1].name='',x=>x.nodes[1].name='雲',x=>x.nodes[1].name=x.nodes[0].name,
  x=>x.nodes[1].kind='toString',x=>x.nodes[1].enabled=1,x=>x.nodes[0].pose[0]=NaN,x=>x.nodes[0].pose[6]=0,x=>x.nodes[1].lobes[3]=-1,
  x=>x.nodes[1].blend=129,x=>x.nodes[1].material[3]=0,x=>x.nodes[1].material[0]=2,x=>x.nodes[1].opacity=Infinity,x=>x.nodes[1].color=0x1000000,
  x=>x.nodes[4].radii[1]=0,x=>x.nodes[4].material[0]=NaN])bad(fn);
const tiny=structuredClone(m);tiny.nodes=[tiny.nodes[0],tiny.nodes[4],tiny.nodes[1]];
tiny.nodes.forEach((n,i)=>{n.index=i;n.parent=i?0:-1;});const tinyBytes=compile(tiny),out=path.join(ROOT,'.cache/ayatsuri-native/tiny.aynm');
fs.mkdirSync(path.dirname(out),{recursive:true});fs.writeFileSync(out,tinyBytes);
const region=structuredClone(tiny),extra=structuredClone(region.nodes[1]);extra.index=3;extra.name='second-ellipse';region.nodes.push(extra);
const plain=compile(region);
region.nodes[1].pattern={kind:'localPlane',normal:[0,1,0],offset:.2,feather:.1,color:0x00ff00};
region.nodes[3].pattern={kind:'localPlane',normal:[1,0,0],offset:-.25,feather:1e-4,color:0xff0000};
const patterned=compile(region),dir=32+7*20,at=patterned.readUInt32LE(dir+4);
assert.equal(patterned.readUInt32LE(12),8);assert.equal(patterned.readUInt16LE(dir),8);assert.equal(patterned.readUInt16LE(dir+2),1);
assert.equal(patterned.readUInt32LE(dir+8),64);assert.equal(patterned.readUInt32LE(dir+12),2);assert.equal(patterned.readUInt32LE(dir+16),32);
assert.equal(patterned.readUInt16LE(at),1);assert.equal(patterned[at+2],4);assert.equal(patterned[at+3],0);
assert.equal(patterned.readUInt32LE(at+24),0x00ff00);assert.equal(patterned.readUInt32LE(at+28),0);
assert.ok(patterned.equals(compile(structuredClone(region))));
let regionRejected=0;function badRegion(fn){const c=structuredClone(region);fn(c);assert.throws(()=>compile(c));++regionRejected;}
for(const fn of [c=>c.nodes[0].pattern=c.nodes[1].pattern,c=>c.nodes[2].pattern=c.nodes[1].pattern,
  c=>c.nodes[1].pattern.kind='checker',c=>c.nodes[1].pattern.normal=[0,0,0],c=>c.nodes[1].pattern.normal=[0,1.001,0],
  c=>c.nodes[1].pattern.normal=[0,NaN,0],c=>c.nodes[1].pattern.normal=[0,Infinity,0],c=>c.nodes[1].pattern.normal=[0,1],
  c=>c.nodes[1].pattern.offset=2.001,c=>c.nodes[1].pattern.offset=-2.001,c=>c.nodes[1].pattern.offset=NaN,
  c=>c.nodes[1].pattern.feather=-.1,c=>c.nodes[1].pattern.feather=9e-5,c=>c.nodes[1].pattern.feather=2.001,
  c=>c.nodes[1].pattern.feather=Infinity,c=>c.nodes[1].pattern.color=0x1000000])badRegion(fn);
for(const feather of [0,1e-4,2]){const c=structuredClone(region);c.nodes[1].pattern.feather=feather;compile(c);}
for(const offset of [-2,2]){const c=structuredClone(region);c.nodes[1].pattern.offset=offset;compile(c);}
for(const normal of [[0,-1,0],[0,1.00004,0]]){const c=structuredClone(region);c.nodes[1].pattern.normal=normal;compile(c);}
badRegion(c=>c.nodes[1].pattern.normal=[0,1.00006,0]);
fs.writeFileSync(path.join(ROOT,'.cache/ayatsuri-native/region.aynm'),patterned);
fs.writeFileSync(path.join(ROOT,'.cache/ayatsuri-native/region-plain.aynm'),plain);
console.log('AYNM_REGION_PACK_PASS rows=2 deterministic=true oldBin=unchanged rejected='+regionRejected);
console.log('AYNM_PACK_PASS cloudBytes='+bytes.length+' nodes=19 deterministic=true malformed='+rejected+' tinyNodes=3 tinyBytes='+tinyBytes.length);
