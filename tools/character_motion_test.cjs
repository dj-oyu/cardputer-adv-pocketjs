// Visual performance channels reuse output and do not hide a mutable clock.
'use strict';
const assert=require('node:assert/strict');
const Motion=require('../apps/character/motion.js');
const keys=[0,2, 10,4, 20,3];
const track=Motion.track(keys,{stride:2,timeUnit:'frame',interpolation:'smooth'});
const actor=Motion.performance({track,channels:['time','scale'],
  waves:[{channel:'scale',amplitude:.1,rate:.2,mode:'multiply'},
         {channel:'sway',amplitude:2,rate:.3,mode:'add'}],
  gates:[{channel:'ear',from:3,to:6,value:1}]});
const a=actor.evaluate(5);
assert.equal(a.scale,3*(1+.1*Math.sin(5*.2)));
assert.equal(a.sway,2*Math.sin(5*.3));assert.equal(a.ear,1);
assert.strictEqual(actor.evaluate(6),a);assert.equal(a.ear,0,'gate end is excluded');
assert.equal(actor.evaluate(3).ear,1,'gate start is included');
actor.evaluate(5);const snapshot={...a};actor.evaluate(100);actor.evaluate(5);
assert.deepEqual({...a},snapshot,'seek/time order has no integration drift');
assert.equal(actor.timeUnit,'frame');assert.equal(track.keys,keys,'immutable borrowed keys');
assert.throws(()=>Motion.performance({track,channels:['x','x']}));
assert.throws(()=>Motion.performance({track,channels:['x','y'],waves:[{channel:'x',amplitude:NaN,rate:1,mode:'add'}]}));
assert.throws(()=>actor.evaluate(Infinity));
assert.throws(()=>Motion.track([0,1e308,1,-1e308],{stride:2,timeUnit:'ms'}),'interval overflow rejected before sampling');
const names=Motion.performance({track,channels:['__proto__','scale']});
assert.equal(names.evaluate(5).__proto__,5);
assert.throws(()=>Motion.performance({track,channels:['x','y'],waves:[{channel:'new',amplitude:1,rate:1,mode:'multiply'}]}));
const unsafe=Motion.performance({track,channels:['time','value'],waves:[{channel:'value',amplitude:1,rate:3,mode:'add'}]});
const safe=unsafe.evaluate(1), previous={...safe};assert.throws(()=>unsafe.evaluate(1e308));
assert.deepEqual({...safe},previous,'failed sample keeps last published channels');
const overlaps=Motion.performance({track,channels:['t','v'],gates:[
  {channel:'gate',from:0,to:4,value:1},{channel:'gate',from:2,to:6,value:2}]});
assert.equal(overlaps.evaluate(3).gate,3,'overlapping gate values add');
console.log('CHARACTER_MOTION_PASS named channels, deterministic waves/gates, borrowed keys, reused output');
