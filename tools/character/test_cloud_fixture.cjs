// Actual finite app probe with the explicit host numerical backend; no pixels.
'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const {backend}=require('./test_follow.cjs'),root=path.resolve(__dirname,'../..');
function setup(order){
  const logs=[],loads=[],settings={model:0,mode:0,tick:0,breathing:false};
  let actor=null,diagnostics=null,busy=false,held=0,S=null,fail=false,last=null;
  const ctx=vm.createContext({Float32Array,Uint32Array,console:{log:s=>logs.push(s)},pocket:{ayatsuri:backend,app:{load(name){
    if(loads.includes(name))return;assert.equal(actor,null);loads.push(name);load('apps/cloud/cloud_'+name+'.js');
    if(name==='fixture'){
      const attach=ctx.AyatsuriCloudFixtureDynamic.attach;
      ctx.AyatsuriCloudFixtureDynamic.attach=(sample,s,reset,read)=>{S=s;attach(sample,s,reset,read);};
    }
  }}}});
  function load(file){vm.runInContext(fs.readFileSync(path.join(root,file),'utf8'),ctx,{filename:file});}
  for(const f of ['apps/lib/ayatsuri_follow.js','apps/cloud/cloud_motion.js','apps/cloud/cloud_dynamic.js'])load(f);
  function discard(){actor=null;S=null;settings.tick=settings.mode=0;settings.breathing=false;}
  ctx.AyatsuriCloudProbe={fixture:'fixture',attachControl(factory){
    assert.equal(diagnostics,null);diagnostics=factory((op,value)=>{
      if(op==='lock'){assert.ok(!busy);busy=true;}else if(op==='unlock')busy=false;
      else if(op==='discard')discard();else if(op==='actor')return actor;
      else if(op==='write')Object.assign(settings,value);else Object.assign(value,settings);
    });return diagnostics.control;
  }};
  if(order==='debug-first')load('apps/cloud/cloud_dynamic_diagnostics.js');
  function snapshot(){
    const out=new Float32Array(30),phase={phases:new Uint32Array(3),increments:new Uint32Array(3),value:0};
    out.set(S.q);S.d.snapshot(phase);
    for(let i=0;i<3;i++){out[24+i*2]=phase.phases[i]&65535;out[25+i*2]=phase.phases[i]>>>16;}return Array.from(out);
  }
  function evaluate(tick){return actor.model.sample(tick*1000/30,settings.mode,settings.breathing,{body:{},expression:{}});}
  return {ctx,logs,loads,settings,snapshot,state:()=>S,fail:v=>fail=v,frame(buttons=0){
    let press=buttons&~held;held=buttons;
    if((press&0x4000)&&ctx.AyatsuriCloudProbe.fixture){
      discard();ctx.pocket.app.load('fixture');
      if(order==='fixture-first'&&!ctx.AyatsuriCloudDynamicDiagnostics)load('apps/cloud/cloud_dynamic_diagnostics.js');
      ctx.AyatsuriCloudProbe.enter();press&=~0x40f0;
    }
    let error=null;busy=true;
    try{
      if(press&0xa0){settings.mode=(settings.mode+1)%6;settings.tick=0;}
      if(press&0x10)settings.tick=0;if(press&0x40){settings.breathing=!settings.breathing;settings.tick=0;}
      if(diagnostics&&(press&0x40f0))diagnostics.reset();
      if(!actor)actor={model:{sample:ctx.AyatsuriCloudDynamic.create(ctx.AyatsuriCloudMotion)}};
      const tick=diagnostics?diagnostics.sample(settings.tick):settings.tick;
      evaluate(tick);let before=S?snapshot():null;
      if(diagnostics){assert.equal(diagnostics.sample(settings.tick),tick);evaluate(tick);assert.deepEqual(snapshot(),before,'render retry freezes all30');}
      if(fail)throw new Error('injected render failure');
      settings.tick=diagnostics?diagnostics.submitted(tick):tick+1;
      last={tick,before,after:S?snapshot():null};
    }catch(e){error=e;}finally{busy=false;}
    if(error&&(!ctx.AyatsuriCloudProbe.failure||!ctx.AyatsuriCloudProbe.failure(error)))throw error;
    return last;
  }};
}
function run(order){
  const s=setup(order);s.frame();assert.equal(s.loads.length,0,'ordinary startup is lazy');
  assert.equal(s.ctx.AyatsuriCloudProbe.enter,undefined);s.frame(0x4000);
  const seed=s.state().seed;let frames=1,paused=0,previous=0;
  while(!s.logs.includes('CLOUD_FIXTURE RETURN reason=complete')){
    assert.ok(frames<300);const r=s.frame();++frames;
    if(r.tick===previous)++paused;previous=r.tick;
    if(r.tick===30){assert.deepEqual(r.after.slice(0,4),r.before.slice(0,4),'retarget preservesqv');
      assert.equal(r.after[4],Math.fround(.18));assert.equal(r.after[5],Math.fround(-.09));
      assert.deepEqual(r.after.slice(6),r.before.slice(6),'retarget preserveschild/phase');}
    if(r.tick===60){assert.deepEqual(r.after.slice(0,2),r.before.slice(0,2),'impulse preservesq');
      assert.ok(r.after[2]>r.before[2]);assert.ok(r.after[3]<r.before[3]);assert.deepEqual(r.after.slice(4),r.before.slice(4));}
  }
  assert.equal(seed,42);assert.equal(frames,297);assert.equal(paused,15);assert.equal(s.settings.tick,0);
  assert.equal(s.logs.filter(v=>v.includes('START')).length,1);
  assert.ok(s.logs.includes('CLOUD_FIXTURE COMPLETE result=PASS digest=e12e1890 pause=15 seed=42'));
  assert.ok(s.state().q.every(v=>v===0));assert.equal(s.state().fixture,false);
  if(order)assert.equal(typeof s.ctx.AyatsuriCloudDynamicDiagnostics.attach,'function');
  return s.logs;
}
assert.deepEqual(run(),run(),'seeded logs replay exactly');run('debug-first');run('fixture-first');
for(const buttons of [0x4010,0x4020,0x4040,0x40f0]){
  const s=setup();s.frame();s.frame(buttons);assert.equal(s.settings.tick,1);assert.equal(s.settings.mode,0);assert.equal(s.settings.breathing,false);
  s.frame(0);s.frame(0x10);assert.ok(s.logs.includes('CLOUD_FIXTURE RETURN reason=cancel'));assert.equal(s.state().fixture,false);
}
const failed=setup();failed.frame(0x4000);failed.fail(true);failed.frame();
assert.ok(failed.logs.includes('CLOUD_FIXTURE RETURN reason=failure'));assert.equal(failed.settings.tick,0);assert.equal(failed.state(),null);
assert.throws(()=>failed.frame(),/injected render failure/,'ordinary failure propagates');
console.log('CLOUD_FIXTURE_NODE_PASS merged=true submissions=297 pause=15 retry=state30-frozen retarget=qv-preserved replay=state30-exact digest=e12e1890 cancel=reset hooks=both-orders');

// Execute the dedicated app's actual entry/chunk/runtime code as well.
function runtime(general=false){
  const logs=[],packets=[],loaded=new Set(),Transform=require('./transform_reference.cjs');let fail=false,failCommit=false,published='';
  const draw=(...args)=>{if(fail)throw new Error('injected runtime draw');packets.push(args.map(a=>Array.from(a)));},
    shapes={ellipsoid:draw,blendEllipsoids:draw},
    files={utility:'apps/character/character.js',blend:'apps/character/character_blend.js',follow:'apps/lib/ayatsuri_follow.js',
      motion:'apps/cloud/cloud_pose.js',dynamic:'apps/cloud/cloud_dynamic.js',model:'apps/cloud/cloud_model.js',
      probe:'apps/cloudprobe/cloud_probe_runtime.js',fixture:'apps/cloud/cloud_fixture.js'};
  const tx={background(){},image(){},text(){}};
  const ctx=vm.createContext({Float32Array,Uint32Array,console:{log:s=>logs.push(s)},
    pocket:{ayatsuri:{...backend,compose:Transform.compose},hone:shapes,app:{load(name){
      if(loaded.has(name))return;loaded.add(name);load(files[name]);}}}});
  function load(file){vm.runInContext(fs.readFileSync(path.join(root,file),'utf8'),ctx,{filename:file});}
  let lab;
  if(general){
    for(const name of ['utility','blend','follow','motion','dynamic','model'])ctx.pocket.app.load(name);
    load('apps/character/character_lab_scene.js');lab=ctx.AyatsuriLab.create(0,shapes);
  }else{
    ctx.pocket.kasane={procedural:{resource(){return 17;},beginFrame(){},commit(){
      if(failCommit)throw new Error('injected runtime commit');published=JSON.stringify(packets);
    }},replace(fn){fn(tx);}};
    load('apps/cloudprobe/cloud_probe.js');
  }
  return {logs,loaded,ctx,fail:v=>fail=v,failCommit:v=>failCommit=v,published:()=>published,frame(value){packets.length=0;
    if(general){lab.evaluate(value*1000/30,0,false);lab.scene.render();}else ctx.frame(value||0);
    return JSON.stringify(packets);
  }};
}
const ordinary=runtime(),general=runtime(true);
for(let tick=0;tick<=300;tick++)assert.equal(ordinary.frame(),general.frame(tick),'full cloud BODY packet parity tick '+tick);
assert.ok(!ordinary.loaded.has('fixture'),'probe normal runtime does not load fixture');
ordinary.frame(0x40f0);let submissions=1;
while(!ordinary.logs.includes('CLOUD_FIXTURE RETURN reason=complete')){assert.ok(submissions<300);ordinary.frame();++submissions;}
assert.equal(submissions,297);assert.ok(ordinary.logs.includes('CLOUD_FIXTURE COMPLETE result=PASS digest=e12e1890 pause=15 seed=42'));
ordinary.frame();ordinary.frame(0x4000);ordinary.frame();ordinary.frame(0x10);
assert.ok(ordinary.logs.includes('CLOUD_FIXTURE RETURN reason=cancel'));
for(const kind of ['draw','commit']){
  const s=runtime();s.frame();s.frame(0x4000);const previous=s.published();
  if(kind==='draw')s.fail(true);else s.failCommit(true);s.frame();
  assert.equal(s.ctx.AyatsuriCloudProbe.failurePending,true,'failed turn schedules cleanup');
  assert.equal(s.published(),previous,'failure retains the last committed picture');
  assert.ok(!s.logs.includes('CLOUD_FIXTURE RETURN reason=failure'),'no cleanup inside failed VM turn');
  s.fail(false);s.failCommit(false);s.frame();
  assert.equal(s.ctx.AyatsuriCloudProbe.failurePending,false);
  assert.ok(s.logs.includes('CLOUD_FIXTURE RETURN reason=failure'),'next turn performs cleanup and returns normal');
  s.fail(true);assert.throws(()=>s.frame(),/runtime draw/,'probe ordinary draw failure propagates');
}
console.log('CLOUD_PROBE_RUNTIME_NODE_PASS ordinary301=full-cloud-body-packets-exact fixture297=e12e1890 combinedEnter=priority cancel=normal failure=next-turn-discard picture=last-committed');

const recursiveLoad=runtime();recursiveLoad.frame();
const originalLoad=recursiveLoad.ctx.pocket.app.load;
recursiveLoad.ctx.pocket.app.load=function(name){
  assert.throws(()=>recursiveLoad.ctx.frame(0),/frame active/,'load-phase reentry is rejected before input state changes');
  return originalLoad(name);
};
recursiveLoad.frame(0x4000);recursiveLoad.frame(0x4000);
assert.equal(recursiveLoad.logs.filter(s=>s.startsWith('CLOUD_FIXTURE START')).length,1,'rejected load reentry preserves held Enter');
console.log('CLOUD_PROBE_LOAD_REENTRY_PASS held=unchanged');
