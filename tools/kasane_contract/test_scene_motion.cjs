// Public numerical/lifetime contracts for the optional shared motion layer.
'use strict';
const assert=require('node:assert/strict');
const motion=require('../../apps/lib/ayatsuri_motion.js');
assert.equal(globalThis.CharacterMotion,motion);
let negative=0;
function rejects(call){assert.throws(call,TypeError);negative++;}
const out=new Float64Array(3),keys=new Float32Array([1,0,10,2,10,20]);
const smooth=motion.track(keys,{stride:3,timeColumn:1,timeUnit:'frame',interpolation:'smooth'});
motion.sample(smooth,5,out);assert.deepEqual([...out],[1.5,5,15]);
motion.sample(smooth,-1,out);assert.deepEqual([...out],[1,0,10]);
motion.sample(smooth,11,out);assert.deepEqual([...out],[2,10,20]);
const step=motion.track(keys,{stride:3,timeColumn:1,timeUnit:'ms',interpolation:'step'});
motion.sample(step,8,out);assert.deepEqual([...out],[1,0,10]);
rejects(()=>motion.track(keys,{stride:3,timeColumn:1}));
rejects(()=>motion.track([0,1,0,2],{stride:2,timeUnit:'ms'}));
rejects(()=>motion.track([0,1,Infinity,2],{stride:2,timeUnit:'ms'}));
rejects(()=>motion.sample(smooth,NaN,out));rejects(()=>motion.sample(smooth,0,new Float32Array(2)));
const track=motion.track([0,1,10,3],{stride:2,timeUnit:'ms'});
const channels=['time','0'],waves=[{channel:'9',amplitude:2,rate:0,phase:Math.PI/2,mode:'add'}];
const gates=[{channel:'kick',from:4,to:6,value:3}];
const playback=motion.performance({track,channels,waves,gates});
const first=playback.evaluate(5),snapshot={...first};
assert.equal(first.time,5);assert.equal(first['0'],2);assert.equal(first['9'],2);assert.equal(first.kick,3);
channels[1]='renamed';waves[0].amplitude=1000;gates[0].value=1000;
assert.equal(playback.evaluate(2),first);assert.equal(first.kick,0);
assert.equal(playback.evaluate(5),first);assert.deepEqual({...first},snapshot);
assert.equal(playback.timeUnit,'ms');
rejects(()=>motion.performance({track,channels:['same','same']}));
rejects(()=>motion.performance({track,channels:['t','x'],gates:[{channel:'x',from:0,to:1,value:1}]}));
rejects(()=>motion.performance({track,channels:['t','x'],waves:[{channel:'new',amplitude:1,rate:1,mode:'multiply'}]}));
const extreme=motion.performance({track,channels:['t','x'],waves:[{channel:'y',amplitude:1,rate:1e308,mode:'add'}]});
const previous=extreme.evaluate(0),before={...previous};rejects(()=>extreme.evaluate(1e308));
assert.deepEqual({...previous},before);
delete globalThis.AyatsuriMotion;delete globalThis.CharacterMotion;
assert.deepEqual({...playback.evaluate(5)},snapshot); // Playback retains required helpers, no global clock/builder lookup.
console.log(`SCENE_MOTION_JS_PASS interpolation nonzero_time_column asymmetric_seek channels_snapshot borrowed_output overflow_atomic negatives=${negative}`);
