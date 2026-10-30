// Exercise the installed entry and chunk loader, including model replacement.
'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=path.resolve(__dirname,'../..'),chunks={utility:'character.js',models:'character_models.js',lab:'character_lab_scene.js',diagnostics:'character_lab_control.js'};
const TransformReference=require('./transform_reference.cjs');
let active=false,commits=0,replaces=0,patches=0,textChanges=0,background=false;
let failRender=false,failCommit=false,failView=false,renderHook=null,candidate='',submitted='';
const loaded=new Set();
const tx={background(color){assert.equal(color,0xe4ece0ff);background=true;},
  image(d){assert.ok(background,'replace starts with a background');assert.equal(d.resource,17);assert.deepEqual(Array.from(d.bounds),[0,0,240,135]);},
  text(d){assert.ok(typeof d.text==='string');const capacity=d.capacity||Buffer.byteLength(d.text);return {
    setText(transaction,text){assert.equal(transaction,tx);assert.equal(typeof text,'string');
      assert.ok(Buffer.byteLength(text)<=capacity,'patch stays in fixed text capacity');++textChanges;}};}};
const context=vm.createContext({console:{log(){}},pocket:{
  app:{load(name){assert.ok(chunks[name]);assert.ok(!loaded.has(name));loaded.add(name);
    vm.runInContext(fs.readFileSync(path.join(root,'apps/character',chunks[name]),'utf8'),context,{filename:chunks[name]});}},
  shapes:{ellipsoid(...args){assert.ok(active,'draw belongs to an app-owned candidate');
    if(renderHook)renderHook();
    if(failRender)throw new Error('injected render failure');
    assert.equal(args.length,6);
    [8,9,9,8,5,9].forEach((n,i)=>{assert.equal(args[i].length,n);assert.ok(Array.from(args[i]).every(Number.isFinite));
      candidate+=Array.from(args[i]).join(',')+';';});}},
  kasane:{procedural:{resource(d){assert.equal(d.maxSegments,1024);return 17;},
    // Native beginFrame replaces an abandoned, unpublished candidate.
    beginFrame(){active=true;candidate='';},commit(){assert.ok(active);
      if(failCommit)throw new Error('injected commit failure');
      active=false;submitted=candidate;++commits;}},
    replace(callback){if(failView)throw new Error('injected view failure');
      background=false;callback(tx);assert.ok(background);++replaces;},
    patch(callback){if(failView)throw new Error('injected view failure');callback(tx);++patches;}}
}});
context.pocket.hone=context.pocket.shapes;
context.pocket.ayatsuri=TransformReference;
vm.runInContext(fs.readFileSync(path.join(root,'apps/character/character_lab.js'),'utf8'),context,{filename:'character_lab.js'});
assert.deepEqual(Array.from(loaded),['utility','models','lab']);
const frame=context.frame;
for(let model=0;model<3;model++){
  for(let mode=0;mode<6;mode++){
    frame(0);frame(0x20);frame(0);
    for(let i=0;i<10;i++)frame(0);
  }
  frame(0x10);frame(0);frame(0x40);frame(0);frame(0x4000);frame(0);
}
assert.equal(active,false);assert.ok(commits>200);assert.ok(replaces>=4);
assert.ok(patches>100);assert.ok(textChanges<40,'labels update only when their meaning changes');
assert.equal(context.AyatsuriLab.control,undefined,'ordinary app never loads diagnostics');
context.pocket.app.load('diagnostics');
const control=context.AyatsuriLab.control,state={},snapshot=()=>({...control.read(state)});
assert.equal(control.read(state),state,'read reuses caller-owned output');
control.set({model:2,mode:0,tick:90,breathing:false,paused:true});
frame(0);const repeated=submitted;
for(let i=0;i<4;i++){frame(0);assert.equal(submitted,repeated);assert.equal(snapshot().tick,90);}
assert.equal(snapshot().lastSubmittedTick,90);
for(const invalid of [{model:1,mode:6},{tick:-1},{tick:1.5},{tick:Number.MAX_SAFE_INTEGER+1},
    {model:3},{mode:-1},{breathing:1},{paused:0},{unknown:true},null]){
  const before=snapshot();assert.throws(()=>control.set(invalid),/AyatsuriLab.control/);
  assert.deepEqual(snapshot(),before,'invalid settings leave all state unchanged');
}
const beforeGetter=snapshot();
assert.throws(()=>control.set({model:1,get tick(){control.seek(0);return 2;}}),/frame is active/);
assert.deepEqual(snapshot(),beforeGetter,'reentrant settings cannot partially change state');
assert.throws(()=>control.step(0),/step/);assert.throws(()=>control.step(601),/step/);
control.step();assert.equal(snapshot().pendingSteps,1);frame(0);
assert.equal(snapshot().tick,91);assert.equal(snapshot().lastSubmittedTick,91);
assert.equal(snapshot().pendingSteps,0);
control.step(2);frame(0);frame(0);assert.equal(snapshot().tick,93);
control.seek(90);frame(0);assert.equal(submitted,repeated,'reverse seek restores exact packets');
control.seek(150);frame(0);control.seek(90);frame(0);assert.equal(submitted,repeated);
control.pause(false);assert.throws(()=>control.step(),/step/);
frame(0);assert.equal(snapshot().lastSubmittedTick,90);assert.equal(snapshot().tick,91);
frame(0);assert.equal(snapshot().lastSubmittedTick,91);assert.equal(snapshot().tick,92);
control.pause();control.step();
for(const failure of ['render','commit']){
  const before=snapshot(),prior=submitted;
  failRender=failure==='render';failCommit=failure==='commit';
  assert.throws(()=>frame(0),new RegExp('injected '+failure));
  assert.deepEqual(snapshot(),before,'precommit failure preserves cursor and pending step');
  assert.equal(submitted,prior,'precommit failure preserves submitted packet snapshot');
  failRender=failCommit=false;
}
frame(0);assert.equal(snapshot().tick,93);assert.equal(snapshot().pendingSteps,0);
control.step(2);frame(0x10);assert.equal(snapshot().pendingSteps,0,'keyboard reset clears queued steps');
assert.equal(snapshot().tick,0);frame(0);
renderHook=()=>{assert.throws(()=>control.seek(0),/frame is active/);
  assert.throws(()=>frame(0),/frame is active/);};frame(0);renderHook=null;
control.set({model:1,mode:2,tick:12,paused:true,breathing:true});frame(0);
assert.equal(snapshot().lastSubmittedModel,1);assert.equal(snapshot().lastSubmittedMode,2);
assert.equal(snapshot().lastSubmittedBreathing,true);
control.pause(false);failView=true;
assert.throws(()=>frame(0),/injected view failure/);failView=false;
assert.equal(snapshot().lastSubmittedTick,12,'commit remains published after later view failure');
assert.equal(snapshot().tick,13,'clock uses commit boundary, not view patch success');frame(0);
control.set({tick:Number.MAX_SAFE_INTEGER,paused:true});assert.throws(()=>control.step(),/step/);
control.pause(false);frame(0);assert.equal(snapshot().paused,true,'safe integer end stops clock');
assert.equal(active,false);
console.log('CHARACTER_LAB_ENTRY_PASS commits='+commits+' replacements='+replaces+' textChanges='+textChanges);
