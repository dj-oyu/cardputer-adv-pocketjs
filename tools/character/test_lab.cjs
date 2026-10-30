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
      candidate+=Array.from(args[i]).join(',')+';';});},
    blendEllipsoids(...args){assert.ok(active);if(renderHook)renderHook();
      if(failRender)throw new Error('injected render failure');
      assert.equal(args.length,5);assert.ok(args[0].length>=21&&args[0].length<=168&&args[0].length%21===0);
      [9,11,3,2].forEach((n,i)=>assert.equal(args[i+1].length,n));
      for(const packet of args){assert.ok(Array.from(packet).every(Number.isFinite));candidate+=Array.from(packet).join(',')+';';}}},
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
const sample=context.AyatsuriLab.sample;
const cloudContext=vm.createContext({});
vm.runInContext(fs.readFileSync(path.join(root,'apps/cloud/cloud_motion.js'),'utf8'),cloudContext,{filename:'cloud_motion.js'});
const sampled=(ms,mode=0,breathing=false,index=2)=>{
  const inputs=JSON.parse(JSON.stringify(sample(ms,mode,breathing)));
  return index===2?cloudContext.AyatsuriCloudMotion(ms,mode,breathing,inputs):inputs;
};
const cloudRest=sampled(0);
for(const ms of [500,1999,7820,8000,10000])assert.deepEqual(sampled(ms),cloudRest,'cloud rests exactly before/after its phrase');
assert.ok(sampled(2200).body.tilt<0,'anticipation shifts away before leaning');
assert.ok(sampled(3000).body.tilt>.2,'main lean has readable weight');
assert.ok(sampled(5300).body.stretch<.9,'compression precedes extension');
assert.ok(sampled(5900).body.stretch>1.15,'release stretches upward');
assert.ok(sampled(2100).expression.gazeX>0,'gaze leads the main lean');
assert.equal(sampled(2100).expression.surprise,0,'gaze precedes surprise');
assert.equal(sampled(2350).expression.surprise,1,'brief surprise peaks before joy');
assert.equal(sampled(2350).expression.smile,0,'surprise has no joy mouth');
assert.equal(sampled(3300).expression.smile,.9,'main lean reaches joy');
assert.equal(sampled(3300).expression.surprise,0,'joy has no surprise mouth');
assert.equal(sampled(6100).expression.smile,.8,'buoyant release has a second joy phrase');
const spreadFields=['wispSpreadCenter','wispSpreadLeft','wispSpreadRight'];
assert.ok(sampled(2330).body.wispSpreadCenter>0,'spread begins at center');
assert.equal(sampled(2330).body.wispSpreadLeft,0);assert.equal(sampled(2330).body.wispSpreadRight,0);
assert.ok(sampled(2430).body.wispSpreadLeft>0,'spread reaches first tip after100ms');
assert.equal(sampled(2430).body.wispSpreadRight,0);
assert.ok(sampled(2530).body.wispSpreadRight>0,'spread reaches other tip after200ms');
assert.notEqual(sampled(3100).body.handLeft,sampled(3100).body.handRight,'hands phrase asymmetrically');
const wispFields=['wispCenter','wispLeft','wispRight'];
assert.ok(Math.abs(sampled(2390).body.wispCenter)>0,'wisp center starts first');
assert.equal(sampled(2390).body.wispLeft,0);assert.equal(sampled(2390).body.wispRight,0);
assert.ok(sampled(2470).body.wispLeft>0,'near tip follows center');
assert.equal(sampled(2470).body.wispRight,0,'far tip waits longer');
assert.ok(sampled(2550).body.wispRight<0,'far tip bends after its own delay');
assert.notEqual(Math.sign(sampled(4450).body.wispLeft),Math.sign(sampled(4600).body.wispLeft),
  'wisp recoil crosses rest instead of freezing at lean endpoint');
for(const ms of [0,2000,7820,8000,10000])for(const key of wispFields)
  assert.equal(sampled(ms).body[key],0,'wisps rest at '+ms);
for(let ms=0;ms<=10000;ms+=1000/240){
  const {body,expression}=sampled(ms);
  assert.ok(body.wispCenter>=-.46&&body.wispCenter<=.14);
  assert.ok(body.wispLeft>=-.49&&body.wispLeft<=1.61);
  assert.ok(body.wispRight>=-2.21&&body.wispRight<=.67);
  assert.ok(expression.surprise>=0&&expression.surprise<=1);
  assert.ok(expression.smile>=0&&expression.smile<=.9);
  assert.equal(expression.surprise*expression.smile,0,'joy and surprise never blend into one mouth');
  for(const key of spreadFields)assert.ok(body[key]>=0&&body[key]<=.8,'bounded spread '+key);
}
const cloudSamples=[];
for(let ms=0;ms<=10000;ms+=1000/60){
  const pose=sampled(ms);cloudSamples.push([ms,pose]);
  for(const group of [pose.body,pose.expression])assert.ok(Object.values(group).every(Number.isFinite));
  assert.deepEqual(pose.light,cloudRest.light,'body script leaves world light fixed');
  assert.deepEqual(pose.camera,cloudRest.camera,'body script leaves camera fixed');
}
for(const [ms,pose] of cloudSamples.reverse())assert.deepEqual(sampled(ms),pose,'reverse seeking is history independent');
for(const ms of [2000,2220,2350,2660,3050,3480,4280,4350,4430,4480,4600,
    2160,2300,2470,2780,2900,3220,3650,4140,5600,5950,6250,6750,
    2280,2380,2480,2600,2700,2800,3800,3900,4000,5650,5750,5850,6020,6120,6220,6850,6950,7050,
    4360,4440,4950,5030,5110,5000,5280,5400,5450,5660,5900,6050,6500,7650,7740,7810]){
  const a=sampled(ms-.0001),b=sampled(ms+.0001);
  for(const group of ['body','expression'])for(const key of Object.keys(a[group]))
    assert.ok(Math.abs(a[group][key]-b[group][key])<.0001,'continuous cloud '+group+'.'+key+' at '+ms);
}
// Existing human/fox input values and field sets retain the original sampler.
for(const index of [0,1])for(const ms of [0,2100,3200,5300,6700,8000])for(const breathing of [false,true]){
  const pose=sampled(ms,0,breathing,index),t=ms/1000;
  const expected={tilt:0,turn:0,stretch:1,lift:0,head:0,tail:0,haloOffset:0};
  if(t>=2&&t<5)expected.tilt=.3*Math.sin((t-2)*Math.PI*2/3);
  if(t>=5&&t<8)expected.stretch=1+.18*Math.sin((t-5)*Math.PI*2/3);
  const d=t-.18;if(d>=2&&d<5)expected.haloOffset=-3*Math.sin((d-2)*Math.PI*2/3);
  expected.tail=expected.tilt*.6;if(breathing)expected.lift=Math.sin(t*2)*1.2;
  assert.deepEqual(pose.body,expected);assert.deepEqual(pose.expression,{open:1,blink:0});
}
for(const mode of [1,2,3,4,5]){
  const base=JSON.parse(JSON.stringify(sample(3200,mode,false))),cloud=sampled(3200,mode);
  for(const key of Object.keys(base.body))assert.equal(cloud.body[key],base.body[key]);
  for(const key of Object.keys(base.expression))assert.equal(cloud.expression[key],base.expression[key]);
  assert.deepEqual(cloud.light,base.light);assert.deepEqual(cloud.camera,base.camera);
  assert.deepEqual(cloud.texture,base.texture);
  assert.equal(cloud.expression.surprise,0);
  for(const key of spreadFields)assert.equal(cloud.body[key],0,'nonBODY resets '+key);
}
const hookOrder=[],blend={},hookScene={light(){},camera(){},evaluate(){hookOrder.push('evaluate');}},
  hookContext=vm.createContext({AyatsuriBlend:blend,
    Ayatsuri:{scene(options){assert.deepEqual(Array.from(options.geometries),[blend]);return hookScene;}},
    AyatsuriModels:{needsBlend:true,create(){return {patterns:[],
      sample(ms,mode,breathing,inputs){hookOrder.push('sample');inputs.body.lift=9;},
      update(ms,body){assert.equal(body.lift,9);hookOrder.push('update');}};}}});
vm.runInContext(fs.readFileSync(path.join(root,'apps/character/character_lab_scene.js'),'utf8'),hookContext);
hookContext.AyatsuriLab.create(0,{}).evaluate(0,0,false);
assert.deepEqual(hookOrder,['sample','update','evaluate'],'model sampling is explicit and precedes model update');
// Use the real utility transform tree: rotated face/hands stay rigid while the
// separate body volume inherits squash/stretch. No rendered pixels are implied.
const shapeContext=vm.createContext({});
for(const file of ['apps/character/character.js','apps/character/character_blend.js',
    'apps/cloud/cloud_motion.js','apps/cloud/cloud_model.js'])
  vm.runInContext(fs.readFileSync(path.join(root,file),'utf8'),shapeContext,{filename:file});
const shapeScene=shapeContext.Ayatsuri.scene({backend:{ellipsoid(){},blendEllipsoids(){}},
  evaluator:TransformReference,geometries:[shapeContext.AyatsuriBlend]}),shapeNodes=new Map();
const shapeModel=shapeContext.AyatsuriModels.create({add(def){
  const handle=shapeScene.add(def);shapeNodes.set(def.name,handle);return handle;
}},0);
const rigidNames=['actor','face','floating-hand-1','floating-hand1','hand-volume-1','hand-volume1',
  'pupil-1','pupil1','brow-1','brow1','cheek-1','cheek1','smile','tongue'];
const norm=(m,column)=>Math.hypot(m[column],m[column+4],m[column+8]);
const close=(a,b,label)=>assert.ok(Math.abs(a-b)<.000002,label+': '+a+' vs '+b);
for(const stretch of [.84,1,1.19]){
  const {body,expression}=sampled(0);
  Object.assign(body,{stretch,head:.31,turn:.23,tilt:-.17,handLeft:.22,handRight:-.16});
  shapeModel.update(0,body,expression);shapeScene.evaluate(0);
  for(const name of rigidNames){
    const m=shapeNodes.get(name)._node.matrix;
    for(let axis=0;axis<3;axis++)close(norm(m,axis),.9,name+' rigid axis '+axis+' stretch '+stretch);
    close(norm(m,0)/norm(m,1),1,name+' world aspect stretch '+stretch);
    close(m[0]*m[1]+m[4]*m[5]+m[8]*m[9],0,name+' no world shear');
  }
  for(const name of ['cloud-body']){
    const m=shapeNodes.get(name)._node.matrix;
    close(norm(m,0),.9/stretch,name+' changes width');
    close(norm(m,1),.9*stretch,name+' changes height');
    close(norm(m,0)/norm(m,1),1/(stretch*stretch),name+' changes world aspect');
  }
  close(shapeNodes.get('face')._node.pose[1],-12*stretch,'face translates with body surface');
  close(shapeNodes.get('face')._node.pose[5],.31,'head rotation remains nonzero');
}
const actingNames=['smile','tongue','cheek-1','cheek1','brow-1','brow1','eye-1','eye1',
  'halo-wisp0','halo-wisp1','halo-wisp2','halo-wisp3','halo-wisp4'];
function actingSnapshot(tick){
  const ms=tick*1000/30,{body,expression}=sampled(ms);
  shapeModel.update(ms,body,expression);shapeScene.evaluate(ms);
  return Object.fromEntries(actingNames.map(name=>{
    const node=shapeNodes.get(name)._node;
    // Authoring equality treats signed zero alike; canonicalize it for semantic
    // snapshots while retaining exact Float32 values everywhere else.
    return [name,{pose:Array.from(node.pose,v=>v===0?0:v),matrix:Array.from(node.matrix,v=>v===0?0:v),
      color:node.color,opacity:node.opacity}];
  }));
}
const actingRest=actingSnapshot(0),actingSurprise=actingSnapshot(72),actingJoy=actingSnapshot(100),
  actingRelease=actingSnapshot(183);
const mouthAspect=state=>{
  const radii=shapeNodes.get('smile')._node.analytic.work;
  return radii[22]*state.smile.pose[7]/(radii[21]*state.smile.pose[6]);
};
assert.ok(mouthAspect(actingSurprise)>1,'surprise mouth is a tall opening');
assert.ok(mouthAspect(actingSurprise)>mouthAspect(actingJoy)*4,'joy is visibly wider than surprise in face coordinates');
assert.notEqual(actingSurprise.tongue.color,actingSurprise.smile.color,'surprise keeps pink interior');
assert.equal(actingJoy.tongue.color,actingJoy.smile.color,'joy uses two joined ink strokes');
assert.ok(actingJoy.tongue.opacity>0,'second joy stroke is visible');
const endpoints=name=>{
  const node=shapeNodes.get(name)._node,p=actingJoy[name].pose,
    radius=node.analytic.work[21]*p[6],dx=Math.cos(p[5])*radius,dy=Math.sin(p[5])*radius;
  return {left:[p[0]-dx,p[1]-dy],right:[p[0]+dx,p[1]+dy]};
};
const leftStroke=endpoints('smile'),rightStroke=endpoints('tongue');
assert.ok(leftStroke.left[1]<leftStroke.right[1],'joy left outer corner rises');
assert.ok(rightStroke.right[1]<rightStroke.left[1],'joy right outer corner rises');
assert.ok(leftStroke.right[0]>rightStroke.left[0],'joy strokes overlap at the center');
for(const side of [-1,1]){
  assert.ok(actingJoy['cheek'+side].pose[1]<actingSurprise['cheek'+side].pose[1],'joy lifts cheeks');
  assert.ok(actingJoy['brow'+side].pose[1]>actingSurprise['brow'+side].pose[1],'surprise raises brows more');
  assert.ok(actingJoy['eye'+side].pose[7]<actingSurprise['eye'+side].pose[7],'joy narrows eyes');
}
assert.deepEqual(actingSnapshot(240),actingRest,'mouth/cheeks/wisps return exactly to rest');
assert.deepEqual(actingSnapshot(300),actingRest,'rest remains stable');
for(const [tick,expected] of [[183,actingRelease],[100,actingJoy],[72,actingSurprise],[0,actingRest],
    [100,actingJoy],[72,actingSurprise]])
  assert.deepEqual(actingSnapshot(tick),expected,'model expression snapshots are seek-order independent at '+tick);
shapeScene.dispose();
// Real utility ownership: model-level scratch reuse must never alias authored
// arrays into another node, and invalid reuse must not partially publish a pose.
const ownershipScene=shapeContext.Ayatsuri.scene({backend:{ellipsoidPacked(){}},evaluator:TransformReference});
const firstOwned=ownershipScene.add({name:'first',geometry:{kind:'ellipsoid',radii:[1,1,1]}}),
  secondOwned=ownershipScene.add({name:'second',geometry:{kind:'ellipsoid',radii:[1,1,1]}});
const sharedTRS={position:[1,2,3],rotation:[.1,.2,.3],scale:[1,2,3]};
firstOwned.place(sharedTRS);
sharedTRS.position[0]=7;sharedTRS.position[1]=8;sharedTRS.position[2]=9;
sharedTRS.rotation[0]=-.1;sharedTRS.rotation[1]=-.2;sharedTRS.rotation[2]=-.3;
sharedTRS.scale[0]=4;sharedTRS.scale[1]=5;sharedTRS.scale[2]=6;
secondOwned.place(sharedTRS);
const firstSubmitted=Array.from(new Float32Array([1,2,3,.1,.2,.3,1,2,3])),
  secondSubmitted=Array.from(new Float32Array([7,8,9,-.1,-.2,-.3,4,5,6]));
sharedTRS.position[0]=99;sharedTRS.scale[1]=0;
assert.throws(()=>firstOwned.place(sharedTRS),/nonzero finite scale/);
assert.deepEqual(Array.from(firstOwned._node.pose),firstSubmitted,'rejected scratch update copies no position or scale');
sharedTRS.position.fill(NaN);sharedTRS.rotation.fill(NaN);sharedTRS.scale.fill(NaN);
ownershipScene.evaluate(0);ownershipScene.render();
assert.deepEqual(Array.from(firstOwned._node.pose),firstSubmitted,'later scratch mutation cannot alter first submitted pose');
assert.deepEqual(Array.from(secondOwned._node.pose),secondSubmitted,'second submission owns its own nine scalar components');
const secondWorld=Array.from(secondOwned._node.matrix);
sharedTRS.position.fill(0);sharedTRS.rotation.fill(0);sharedTRS.scale.fill(1);
firstOwned.place(sharedTRS);assert.throws(()=>ownershipScene.render(),/evaluate before render/);
ownershipScene.evaluate(1);ownershipScene.render();
assert.deepEqual(Array.from(secondOwned._node.matrix),secondWorld,'scratch recovery updates only its submitted node');
assert.deepEqual(Array.from(firstOwned._node.pose),[0,0,0,0,0,0,1,1,1]);
ownershipScene.dispose();

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
