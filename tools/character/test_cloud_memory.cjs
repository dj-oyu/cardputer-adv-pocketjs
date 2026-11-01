// Finite controller contracts with fake visual/native backends. No heap claim.
'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const ROOT=path.resolve(__dirname,'../..');
function setup(metrics=true,initialProfile){
  const logs=[],marks=[],loads=[],events=[],loaded=new Set(),scenes=[],models=[],captions=[];let candidate='',published='',failLoad='',failCommit=false,failBuiltin=false,failScene=false,failBind=false,reenter=false,failLog=false,commits=0;
  const ctx=vm.createContext({console:{log:s=>{if(failLog){failLog=false;throw Error('log injected');}logs.push(s);}}});
  if(initialProfile!==undefined)ctx.__cloudMemoryProfile=initialProfile;
  function scene(options){
    events.push('scene');
    if(failScene){failScene=false;throw Error('scene injected');}
    assert.equal(options.camera.zoom,Math.fround(.85));assert.deepEqual(Array.from(options.camera.center),[120,68]);
    const s={dead:false,disposeCalls:0,pose:null,material:null,handle:{},evaluate(ms){events.push('draw');assert.ok(!s.dead);s.ms=ms;},
      render(){assert.ok(!s.dead);candidate=JSON.stringify({ms:s.ms,pose:s.pose,material:s.material});},
      dispose(){++s.disposeCalls;s.dead=true;}};
    const root={place(){assert.ok(!s.dead);}},body={material(p){assert.ok(!s.dead);s.material={color:p.color,opacity:p.opacity};}};
    s.root=root;s.body=body;scenes.push(s);return s;
  }
  function makeModel(s){
    events.push('bind');
    if(failBind){failBind=false;throw Error('binding injected');}
    return {root:s.root,parts:[s.body],sampleFactory(){return function(ms,mode,breathing,out){
      assert.equal(mode,0);assert.equal(breathing,false);out.body.tilt=.2*Math.sin(ms*.001);out.body.stretch=1+.1*Math.sin(ms*.002);
    };},sample(){},update(ms,b){s.pose={tilt:b.tilt,stretch:b.stretch};}};
  }
  const H={resource(d){assert.equal(d.maxSegments,1024);return 17;},beginFrame(){candidate='clear';},commit(){
    assert.equal(++commits,1,'at most one commit per callback, including cancel/failure cleanup');
    if(reenter){reenter=false;assert.throws(()=>ctx.frame(0),/busy/);assert.throws(()=>ctx.AyatsuriCloudMemory.read({}),/busy/);}
    if(failCommit)throw Error('commit injected');published=candidate;
  }};
  const app={load(name){
    events.push('load:'+name);
    if(loaded.has(name))return;loads.push(name);if(name===failLoad){failLoad='';throw Error('load injected');}
    if(name==='runtime')vm.runInContext(fs.readFileSync(path.join(ROOT,'apps/cloudmem/memory_runtime.js'),'utf8'),ctx,{filename:'memory_runtime.js'});
    else if(name==='a_utility')ctx.Ayatsuri={scene};else if(name==='a_blend')ctx.AyatsuriBlend={};else if(name==='a_model')ctx.AyatsuriModels={create:makeModel};
    else if(name==='c_utility')ctx.AyatsuriNative={scene};else if(name==='c_update')ctx.AyatsuriCloudUpdate={create:makeModel};
    else assert.ok(['follow','motion','dynamic'].includes(name));loaded.add(name);
  }};
  if(metrics)app.memoryMark=label=>{assert.ok(label.length<=32);marks.push(label);events.push('mark:'+label);};
  ctx.pocket={app,hone:{},ayatsuri:{modelBuiltin(name){assert.equal(name,'cloud');if(failBuiltin){failBuiltin=false;throw Error('builtin injected');}
    events.push('builtin');
    const m={dead:false,disposeCalls:0,dispose(){++m.disposeCalls;m.dead=true;}};models.push(m);return m;}},kasane:{procedural:H,
    replace(fn){captions.length=0;fn({background(){},image(d){assert.equal(d.resource,17);},text(d){captions.push(d.text);}});}}};
  vm.runInContext(fs.readFileSync(path.join(ROOT,'apps/cloudmem/cloud_memory.js'),'utf8'),ctx,{filename:'cloud_memory.js'});
  return {logs,marks,loads,events,scenes,models,ctx,state(){return ctx.AyatsuriCloudMemory.read({});},frame(b=0){
    const before=ctx.AyatsuriCloudMemory.read({}),start=events.length;commits=0;ctx.frame(b);
    const after=ctx.AyatsuriCloudMemory.read({}),work=events.slice(start),imports=work.filter(s=>s.startsWith('load:'));
    assert.ok(imports.length<=1,'one chunk maximum per callback');
    if(imports.length)assert.ok(!work.some(s=>['builtin','scene','bind','draw'].includes(s)),'load is isolated from create/draw');
    for(const kind of ['builtin','scene','bind'])if(work.includes(kind))
      assert.ok(!work.some(s=>s!==kind&&['builtin','scene','bind','draw'].includes(s)),'construction stages are isolated');
    if(before.setupStep||after.setupStep){assert.ok(after.draws<=before.draws,'no draw advance during initialization');assert.ok(after.tick<=before.tick,'no clock advance during initialization');}
    return published+'|'+captions.join('|');},picture(){return published+'|'+captions.join('|');},
    failLoad:name=>failLoad=name,failCommit:v=>failCommit=v,failBuiltin:()=>failBuiltin=true,failScene:()=>failScene=true,failBind:()=>failBind=true,reenter:()=>reenter=true,failLog:()=>failLog=true};
}
function select(s,variant){s.frame();assert.deepEqual(s.loads,['runtime']);
  s.frame(variant==='A'?0x80:0x20);s.frame();assert.equal(s.state().variant,variant);assert.equal(s.state().phase,'READY');}
function run(s){
  const expected=s.state().expectedDraws,rows=[];let turns=0,last=0;
  let picture=s.frame(0x4000);
  for(;;){
    const state=s.state();assert.ok(!state.failurePending);assert.ok(++turns<expected+50);
    if(state.draws!==last){assert.equal(state.draws,last+1);rows.push(picture);last=state.draws;}
    if(state.phase==='COMPLETE')break;
    picture=s.frame();
  }
  assert.equal(rows.length,expected);assert.equal(s.state().draws,expected);assert.equal(s.state().released,true);assert.equal(s.state().retained,true);return rows;
}
function firstDraw(s){
  s.frame(0x4000);let turns=0;
  while(!s.state().draws){assert.ok(++turns<16);assert.ok(!s.state().failurePending);s.frame();}
  assert.equal(s.state().draws,1);
}
function failUntilPending(s){
  let turns=0,prior;
  do{assert.ok(++turns<16);prior=s.picture();s.frame();}while(!s.state().failurePending);
  assert.equal(s.picture(),prior,'failed initialization leaves previous published picture');
}
function advance(s,until){let turns=0;while(!until(s.state())){assert.ok(++turns<120);assert.ok(!s.state().failurePending);s.frame();}}
function phases(s,expected){
  for(const [name,draws] of Object.entries(expected)){
    const line=s.logs.find(s=>s.startsWith('CLOUD_MEM PHASE '+name+' variant=')&&s.includes(' run=1 '));
    assert.ok(line,name+' logged');assert.match(line,new RegExp(' draws='+draws+' profile='),name+' boundary');
  }
}
const a=setup(),c=setup();select(a,'A');select(c,'C');
assert.equal(a.state().profile,'DEVICE_SHORT');assert.equal(a.state().expectedDraws,63);assert.equal(a.state().steadyFrames,32);
const firstA=run(a),firstC=run(c);assert.deepEqual(firstC,firstA,'A/C run pictures and captions are deterministic and equal');
assert.equal(firstA.length,63);assert.ok(c.logs.some(s=>s.includes('CLOUD_MEM COMPLETE')&&s.includes('profile=DEVICE_SHORT expectedDraws=63 steady=32')));
phases(a,{WARM:4,STEADY:36,BURST:44,EXIT:45,RECREATE1:53,EXIT1:54,RECREATE2:62,EXIT2:63});
assert.ok(a.loads.every(n=>!n.startsWith('c_')));assert.ok(c.loads.every(n=>!n.startsWith('a_')));
for(const s of [a,c]){
  const loaded=s.events.indexOf('mark:cloudmem.loaded'),created=s.events.indexOf('mark:cloudmem.created');
  assert.ok(loaded>=0&&loaded<s.events.findIndex(n=>['builtin','scene','bind'].includes(n)),'loaded excludes all creation');
  assert.ok(created<s.events.indexOf('draw'),'created excludes first update/evaluation/draw');
}
assert.equal(c.models.length,3);assert.ok(c.models.every(m=>m.dead&&m.disposeCalls===1));assert.ok(c.scenes.every(s=>s.dead&&s.disposeCalls===1));
const importedA=a.events.filter(n=>n.startsWith('load:')).length,importedC=c.events.filter(n=>n.startsWith('load:')).length;
a.frame();c.frame();assert.deepEqual(run(a),firstA,'repeat A run reproduces pictures');assert.deepEqual(run(c),firstC,'repeat C run reproduces pictures');
assert.equal(a.events.filter(n=>n.startsWith('load:')).length,importedA,'completed cold loader is released before replay');
assert.equal(c.events.filter(n=>n.startsWith('load:')).length,importedC,'completed cold loader is released before replay');
assert.equal(c.state().completedRuns,2);assert.equal(c.models.length,6);assert.ok(c.models.every(m=>m.dead&&m.disposeCalls===1));
// Long work is explicit, session-fixed, and independently compared across cold routes.
const longA=setup(true,'HOST_LONG'),longC=setup(true,'HOST_LONG');select(longA,'A');select(longC,'C');
assert.equal(longA.state().expectedDraws,1115);assert.equal(longC.state().steadyFrames,1000);
const longRowsA=run(longA),longRowsC=run(longC);assert.equal(longRowsA.length,1115);assert.deepEqual(longRowsC,longRowsA);
phases(longC,{WARM:16,STEADY:1016,BURST:1048,EXIT:1049,RECREATE1:1081,EXIT1:1082,RECREATE2:1114,EXIT2:1115});
longA.frame();longC.frame();assert.deepEqual(run(longA),longRowsA);assert.deepEqual(run(longC),longRowsC);
assert.ok(longC.logs.some(s=>s.includes('CLOUD_MEM COMPLETE')&&s.includes('profile=HOST_LONG expectedDraws=1115 steady=1000')));
assert.ok(longA.loads.every(n=>!n.startsWith('c_')));assert.ok(longC.loads.every(n=>!n.startsWith('a_')));
// Private host initialization is read once, before resource access, never an API.
const runtime=fs.readFileSync(path.join(ROOT,'apps/cloudmem/memory_runtime.js'),'utf8');
for(const value of ['',null,{},'device_short','OTHER',0]){
  const invalid=vm.createContext({__cloudMemoryProfile:value});
  assert.throws(()=>vm.runInContext(runtime,invalid),/invalid profile/);
  assert.equal(invalid.AyatsuriCloudMemory,undefined);assert.equal(invalid.frame,undefined);
}
const profile=setup(true,'DEVICE_SHORT');profile.frame();const beforeProfile=JSON.stringify(profile.state()),beforeLogs=profile.logs.length;
assert.equal(profile.ctx.AyatsuriCloudMemory.selectProfile,undefined);
profile.ctx.__cloudMemoryProfile='HOST_LONG';
assert.equal(JSON.stringify(profile.state()),beforeProfile);assert.equal(profile.logs.length,beforeLogs);
profile.ctx.AyatsuriCloudMemory.read({set profile(value){assert.equal(value,'DEVICE_SHORT');assert.throws(()=>profile.state(),/busy/);assert.throws(()=>profile.frame(),/busy/);}});
profile.frame(0x80);profile.frame();assert.equal(run(profile).length,63);
longC.ctx.__cloudMemoryProfile='DEVICE_SHORT';assert.equal(longC.state().profile,'HOST_LONG');
// Selection cannot switch, and cancellation discards the active scene/model.
const cancel=setup(false);select(cancel,'C');cancel.frame(0x80);assert.equal(cancel.state().variant,'C');cancel.frame();cancel.frame(0x4000);cancel.frame();cancel.frame(0x10);
assert.equal(cancel.state().phase,'READY');assert.equal(cancel.state().released,true);assert.ok(cancel.logs.includes('CLOUD_MEM METRICS UNAVAILABLE'));
assert.ok(cancel.models.every(m=>m.dead));assert.ok(cancel.scenes.every(s=>s.dead));
// Cancel at every initialization boundary, including an owned native model
// before scene/binding. Cached chunks do not erase the turn separation on retry.
for(const variant of ['A','C'])for(let step=1;step<=9;++step){
  const s=setup();select(s,variant);s.frame(0x4000);advance(s,state=>state.setupStep===step);
  s.frame(0x10);assert.equal(s.state().phase,'READY');assert.equal(s.state().setupStep,0);
  assert.equal(s.state().released,true);assert.equal(s.state().failurePending,false);
  assert.ok(s.models.every(m=>m.dead&&m.disposeCalls===1));assert.ok(s.scenes.every(m=>m.dead&&m.disposeCalls===1));
  s.frame();firstDraw(s);
}
// Renderer failure preserves the previous published picture, cleanup next turn.
const failure=setup();select(failure,'C');firstDraw(failure);const prior=failure.picture();failure.failCommit(true);failure.frame();
assert.equal(failure.picture(),prior);assert.equal(failure.state().failurePending,true);failure.failCommit(false);failure.frame();
assert.equal(failure.state().released,true);assert.equal(failure.state().phase,'READY');assert.equal(failure.state().failurePending,false);
assert.ok(failure.models.every(m=>m.dead));assert.ok(failure.scenes.every(s=>s.dead));
for(const variant of ['A','C'])for(const kind of ['load',...(variant==='C'?['builtin']:[]),'scene','bind']){
  const s=setup();select(s,variant);
  if(kind==='load')s.failLoad(variant==='A'?'a_model':'c_update');else if(kind==='builtin')s.failBuiltin();else if(kind==='scene')s.failScene();else s.failBind();
  s.frame(0x4000);failUntilPending(s);assert.equal(s.state().failurePending,true);
  s.frame();assert.equal(s.state().phase,'READY');assert.equal(s.state().released,true);assert.ok(s.models.every(m=>m.dead));assert.ok(s.scenes.every(scene=>scene.dead));
  firstDraw(s);assert.equal(s.state().phase,'WARM','explicit retry succeeds after '+kind+' failure');
}
const combined=setup();combined.frame(0x90);assert.equal(combined.state().variant,'A');assert.equal(combined.state().phase,'READY');assert.equal(combined.state().failurePending,false);
const noSelection=setup();noSelection.failCommit(true);noSelection.frame();assert.equal(noSelection.state().failurePending,true);
noSelection.failCommit(false);noSelection.frame(0x80);assert.equal(noSelection.state().variant,null,'failure cleanup suppresses simultaneous selection');assert.equal(noSelection.state().failurePending,false);
for(const variant of ['A','C'])for(const name of ['follow','motion','dynamic',...(variant==='A'?['a_utility','a_blend','a_model']:['c_utility','c_update'])]){
  const s=setup();select(s,variant);s.failLoad(name);s.frame(0x4000);failUntilPending(s);
  assert.ok(!s.marks.includes('cloudmem.loaded'),'failed load cannot report loaded');
  s.frame();assert.equal(s.state().released,true);firstDraw(s);
}
for(const phase of ['RECREATE1','RECREATE2'])for(const kind of ['builtin','scene','bind']){
  const s=setup();select(s,'C');s.frame(0x4000);
  const step=kind==='builtin'?0:kind==='scene'?8:9;
  advance(s,state=>state.phase===phase&&state.setupStep===step);
  if(kind==='builtin')s.failBuiltin();else if(kind==='scene')s.failScene();else s.failBind();
  failUntilPending(s);s.frame();assert.equal(s.state().released,true);assert.equal(s.state().setupStep,0);
  assert.ok(s.models.every(m=>m.dead&&m.disposeCalls===1));assert.ok(s.scenes.every(m=>m.dead&&m.disposeCalls===1));
  firstDraw(s);
}
const reentry=setup();select(reentry,'A');reentry.reenter();firstDraw(reentry);assert.equal(reentry.state().failurePending,false);
console.log('CLOUD_MEMORY_NODE_PASS route=cold-fixed noOppositeImports=true default=DEVICE_SHORT draws=63 steady=32 explicit=HOST_LONG draws=1115 steady=1000 replay=2_each_profile initialization=staged-one-chunk-or-create-stage-per-turn pictureParity=fake-backend cleanup=scene+ownedModel failures=deferred reentry=guarded turnWatchdog=UNCHANGED longDeviceRun=UNMEASURED');
