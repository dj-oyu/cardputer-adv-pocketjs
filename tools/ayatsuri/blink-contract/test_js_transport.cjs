'use strict';
// Test-only prospective adapter, not Scene/native API code.
const fs=require('node:fs'),assert=require('node:assert/strict');
const report=JSON.parse(fs.readFileSync(process.argv[2]||'report.json','utf8'));
assert.equal(report.schema,'public-synthetic-blink-v1');
function adapter(controls){
 if(!Array.isArray(controls)||controls.length!==3)throw Error('invalid controls');
 const snapshot=[];
 for(let i=0;i<3;i++){
  if(!Object.prototype.hasOwnProperty.call(controls,i))throw Error('missing control');
  const v=controls[i];
  if(typeof v!=='number'||!Number.isFinite(v)||v<0||v>1)throw Error('invalid control');
  snapshot.push(v);
 }
 const both=snapshot[0];
 return snapshot.slice(1).map((v,k)=>{const b=Math.max(both,v),s=k?1:-1;
  return {side:s,blink:b,visible:b<.98,aperture:1-b,
   upper:[[3*s-1,1-b],[3*s+1,1-b]],lower:[[3*s-1,b-1],[3*s+1,b-1]]};});
}
let checked=0;
for(const row of report.cases){assert.deepEqual(adapter(row.controls),row.parts);checked++;}
for(const b of [.97999999,.98,.98000001]){
 const frame=adapter([b,0,0]);const packed=new Float32Array(frame.flatMap(p=>p.upper.flat()));
 assert.equal(frame[0].visible,b<.98);assert(packed.every(Number.isFinite));
}
const boundary=.97999999;assert(boundary<.98);assert(Math.fround(boundary)>=.98);
// Keep the bool made from Number, not a second decision from packed coordinates.
assert.equal(adapter([boundary,0,0])[0].visible,true);
const held=adapter([.5,0,0]),before=JSON.stringify(held);
for(const c of [[NaN,0,0],[Infinity,0,0],[-1,0,0],[2,0,0],[0,null,0],[0,true,0],[0,0,'1']]){
 assert.throws(()=>adapter(c));assert.equal(JSON.stringify(held),before);
}
for(const sparse of [Array(3),[,0,0],[0,,0],[0,0,],[0,0,0,0]]){
 assert.throws(()=>adapter(sparse));assert.equal(JSON.stringify(held),before);
}
const access=[0,0,0],reads=[0,0,0];
for(let i=0;i<3;i++)Object.defineProperty(access,i,{get(){reads[i]++;return .5;}});
assert.deepEqual(adapter(access),adapter([.5,.5,.5]));assert.deepEqual(reads,[1,1,1]);
const throwing=[0,0,0];Object.defineProperty(throwing,2,{get(){throw Error('input failed');}});
assert.throws(()=>adapter(throwing));assert.equal(JSON.stringify(held),before);
assert.equal(checked,216);
console.log(JSON.stringify({pass:true,pythonCases:checked,float32BoundaryNegative:true,
 runtime:false,scope:'Node Number test adapter only; no native renderer, OOM or private material equivalence'}));
