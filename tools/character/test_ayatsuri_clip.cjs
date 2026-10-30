// Real utility + optional adapter, with a diagnostic backend (not native pixels).
'use strict';
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),assert=require('node:assert/strict');
const root=path.resolve(__dirname,'../..'),Transform=require('./transform_reference.cjs');
function setup(){
  const calls=[],candidate=[];let published=[],onDraw=null;
  const backend={ellipsoidPacked(buffer,offset,light){if(onDraw)onDraw();calls.push({kind:'normal',buffer});candidate.push(['normal',Array.from(new Float32Array(buffer,offset,8))]);},
    ellipsoidClippedPacked(buffer,offset,light,xy){if(onDraw)onDraw();const clip=Array.from(xy);calls.push({kind:'clip',buffer,clip,light:Array.from(light)});if(xy.length)candidate.push(['clip',Array.from(new Float32Array(buffer,offset,8)),clip]);},
    blendEllipsoids(){calls.push({kind:'blend'});candidate.push(['blend']);}};
  const ctx=vm.createContext({ArrayBuffer,Float32Array,Float64Array,Uint16Array,Uint8Array,Int16Array,WeakMap,pocket:{ayatsuri:Transform}});
  for(const file of ['apps/character/character.js','apps/character/character_blend.js','apps/lib/ayatsuri_clip.js','apps/fox3d/fox_face_model.js'])
    vm.runInContext(fs.readFileSync(path.join(root,file),'utf8'),ctx,{filename:file});
  function scene(){return ctx.AyatsuriClip.scene({backend,evaluator:Transform,geometries:[ctx.AyatsuriBlend],camera:{position:[0,0,400],target:[0,0,0],zoom:1,center:[120,80],projection:'orthographic'}});}
  return {ctx,backend,calls,scene,onDraw:fn=>onDraw=fn,
    draw(s){candidate.length=calls.length=0;s.render();},commit(){published=structuredClone(candidate);},picture:()=>JSON.stringify(published)};
}
const f=setup(),s=f.scene(),parent=s.add({name:'root',geometry:null}),
  globe=s.add({name:'globe',parent,geometry:{kind:'ellipsoid',radii:[10,12,5]}}),
  normal=s.add({name:'normal',position:[30,0,0],geometry:{kind:'ellipsoid',radii:[5,6,2]}}),
  iris=s.add({name:'iris',parent,position:[0,0,4],geometry:{kind:'ellipsoid',radii:[5,6,1]}}),
  rest=new Float32Array([-.8,-.6,.7,.8,-.6,.7,.8,.6,.7,-.8,.6,.7]),a=s.aperture(globe,rest);
a.bind(globe).bind(iris);assert.equal(s.raw,undefined);assert.equal(globe._node,undefined,'raw handles stay internal');
s.evaluate(0);f.draw(s);assert.deepEqual(f.calls.map(c=>c.kind),['clip','normal','clip']);assert.deepEqual(f.calls[0].clip,f.calls[2].clip);
const w=new Float32Array(f.calls[0].buffer),expected=[];
for(let i=0;i<4;i++){const p=i*3;expected.push(Math.fround(w[24]*rest[p]+w[25]*rest[p+1]+w[26]*rest[p+2]+w[27]),Math.fround(w[28]*rest[p]+w[29]*rest[p+1]+w[30]*rest[p+2]+w[31]));}
assert.deepEqual(f.calls[0].clip,expected,'same-frame packet projects all three local coordinates');
f.commit();const initial=f.picture(),initialClip=f.calls[0].clip;let stats={};s.stats(stats);const projects=stats.clipProjects;
a.setVertices(rest);s.light({direction:[.4,-.3,.8]});s.evaluate(0);f.draw(s);assert.deepEqual(f.calls[0].clip,initialClip);s.stats(stats);assert.equal(stats.clipProjects,projects,'same morph plus light-only does not reproject');
s.camera({position:[40,0,400]});s.evaluate(0);f.draw(s);assert.notDeepEqual(f.calls[0].clip,initialClip);s.stats(stats);assert.equal(stats.clipProjects,projects+1);
parent.place({rotation:[.1,.3,0]});assert.throws(()=>s.render(),/evaluate/);s.evaluate(0);f.draw(s);assert.deepEqual(f.calls[0].clip,f.calls[2].clip);
const moved=new Float32Array(rest);for(let i=0;i<4;i++)moved[i*3]+=.1;a.setVertices(moved);assert.throws(()=>s.render(),/evaluate/);s.evaluate(0);f.draw(s);f.commit();const accepted=f.picture();
const reversed=new Float32Array(rest);for(let i=0;i<4;i++)for(let k=0;k<3;k++)reversed[i*3+k]=rest[(3-i)*3+k];
assert.throws(()=>a.setVertices(reversed),/winding/);assert.throws(()=>s.render(),/evaluate/);assert.throws(()=>s.evaluate(0),/invalid morph/);assert.equal(f.picture(),accepted);
a.setEmpty('closed');assert.throws(()=>s.evaluate(0),/invalid morph/,'EMPTY cannot conceal invalid morph');a.setVertices(rest);s.evaluate(0);f.draw(s);
assert.deepEqual(f.calls.filter(c=>c.kind==='clip').map(c=>c.clip),[[],[]]);assert.equal(f.calls.filter(c=>c.kind==='normal').length,1);
a.setEmpty(null);s.evaluate(0);f.draw(s);assert.equal(f.calls[0].clip.length,8);
s.camera({zoom:10000});assert.throws(()=>s.evaluate(0),/range/);assert.throws(()=>s.render(),/evaluate/);assert.equal(f.picture(),accepted);
s.camera({zoom:1,position:[0,0,400]});s.evaluate(0);
// Edge-on is invalid OPEN; only the model's explicit pose-cull uses EMPTY.
parent.place({rotation:[0,Math.PI/2,0]});assert.throws(()=>s.evaluate(0),/convex|winding|aperture/);a.setEmpty('pose');s.evaluate(0);f.draw(s);assert.ok(f.calls.filter(c=>c.kind==='clip').every(c=>!c.clip.length));
parent.place({rotation:[0,0,0]});a.setEmpty(null);s.evaluate(0);
// Public-handle guards cover legacy raw utility getter-sensitive operations.
for(const call of [()=>s.light({get direction(){globe.place({position:[9,0,0]});return [0,0,1];}}),
  ()=>s.camera({get position(){globe.material({color:0xffffff});return [0,0,400];}}),
  ()=>s.add({name:'reentrant',get geometry(){globe.visible(false);return null;}}),
  ()=>globe.texture({get offset(){s.dispose();return [0,0];}})])assert.throws(call,/busy/);
s.evaluate(0);f.onDraw(()=>{assert.throws(()=>s.dispose(),/busy/);assert.throws(()=>globe.visible(false),/busy/);});f.draw(s);f.onDraw(null);
const other=f.scene(),foreign=other.add({name:'foreign',geometry:{kind:'ellipsoid',radii:[1,1,1]}});
assert.throws(()=>a.bind(foreign),/ABI/);assert.throws(()=>s.add({name:'foreign-parent',parent:foreign,geometry:null}),/parent/);
const bow=new Float32Array([-.8,-.6,0,.8,.6,0,.8,-.6,0,-.8,.6,0]);assert.throws(()=>s.aperture(globe,bow),/folded|winding|convex/);
const star=new Float32Array(15);for(let i=0;i<5;i++){const angle=(i*2%5)*Math.PI*2/5;star[i*3]=Math.cos(angle);star[i*3+1]=Math.sin(angle);}
assert.throws(()=>s.aperture(globe,star),/intersecting|folded|winding|convex/);
s.dispose();assert.doesNotThrow(()=>s.dispose());for(const call of [()=>s.evaluate(0),()=>s.render(),()=>s.light({}),()=>globe.place({}),()=>globe.material({}),()=>globe.visible(true),()=>globe.texture({}),()=>a.setVertices(rest),()=>a.setEmpty(null),()=>a.bind(globe)])assert.throws(call,/disposed/);
other.dispose();
// Private layout mismatch must be rejected rather than interpreted incorrectly.
const wrong=setup();let raw;
wrong.ctx.Ayatsuri={scene(){return {add(){raw={_node:{owner:{},analytic:{kind:'ellipsoid',work:new Float32Array(65)},matrix:new Float32Array(21)}};return raw;},dispose(){}};}};
const unsupported=wrong.scene(),bad=unsupported.add({});assert.throws(()=>unsupported.aperture(bad,rest),/ABI/);unsupported.dispose();
// The fake native provider below uses the frozen authoring fixture as an oracle.
// It checks JS protocol/numerical controllers, not C floating-point or pixels.
const FIXTURE=path.join(root,'tools/ayatsuri/model/fixtures/fox_face_v2.js');
function oracle(){
  const handles=[],clips=[],entries=[],byBuffer=new Map();let selected=null;
  const ctx=vm.createContext({ArrayBuffer,Float32Array,Float64Array,Uint16Array,Uint8Array,Int16Array,pocket:{ayatsuri:Transform}});
  function record(buffer,geometry){const id=byBuffer.get(buffer),clip=selected&&selected.get(id);if(clip&&clip.length===0)return;entries.push({id,geometry,clip:clip?Array.from(clip):null});}
  const backend={ellipsoidPacked(buffer,offset,lighting){record(buffer,Array.from(new Float32Array(buffer,offset,8)));},
    blendEllipsoids(lobes){record(lobes.buffer,Array.from(lobes));}};
  for(const file of ['apps/character/character.js','apps/character/character_blend.js'])vm.runInContext(fs.readFileSync(path.join(root,file),'utf8'),ctx);
  vm.runInContext(fs.readFileSync(FIXTURE,'utf8'),ctx);
  const scene=ctx.Ayatsuri.scene({backend,evaluator:Transform,geometries:[ctx.AyatsuriBlend]});
  const model=ctx.FoxFaceModel.create({add(d){const h=scene.add(d);handles.push(h);return h;},aperture(anchor,vertices){
    const a={anchor,vertices:Array.from(vertices),targets:[],empty:null};clips.push(a);
    return {bind(h){a.targets.push(h);return this;},setVertices(v){a.vertices=Array.from(v);},setEmpty(mode){a.empty=mode;}};
  }});
  handles.forEach((h,i)=>{if(h._node.analytic)byBuffer.set(h._node.matrix.buffer,i);});
  function evaluate(camera,light){scene.camera({position:camera.slice(0,3),target:camera.slice(3,6),zoom:camera[6],center:camera.slice(7,9)});
    scene.light({direction:light.slice(0,3),color:0xffffff});scene.evaluate(0);}
  return {scene,model,handles,clips,evaluate,render(map){entries.length=0;selected=map;scene.render();return structuredClone(entries);}};
}
function nativeSetup(){
  const models=[];let published=null,onSubmit=null;
  function makeModel(){
    const names=['fox-head','skull','ear-outer-1','ear-outer1','jaw-hinge','lower-jaw','cream-face','oral-volume','nose-bridge','eye-socket-1','sclera-1','iris-1','pupil-1','lid-arc-left-1','lid-arc-right-1','brow-1','eye-socket1','sclera1','iris1','pupil1','lid-arc-left1','lid-arc-right1','brow1','lip','nose','ear-inner-1','ear-tuft-1','ear-inner1','ear-tuft1'];
    const m={count:29,closed:false,disposeCalls:0,instances:[],node(name){if(m.closed)throw Error('model disposed');return names.indexOf(name);},dispose(){m.closed=true;++m.disposeCalls;},instance(){
      if(m.closed)throw Error('model disposed');const o=oracle();
      const i={o,evaluated:false,closed:false,readCalls:0,submitCalls:0,last:null,lastRefs:null,failRead:false,failSubmit:false,
        targets(mask,pose,colors,opacity){assert.equal(mask.length,29);assert.equal(pose.length,261);
          for(let n=0;n<29;n++){assert.notEqual(mask[n]&24,24);assert.equal(mask[n]&~127,0);for(let k=0;k<9;k++)if(mask[n]&(1<<Math.floor(k/3)))assert.ok(Number.isFinite(pose[n*9+k]));}
          for(let n=0;n<29;n++){const h=o.handles[n],p={};if(mask[n]&1)p.position=Array.from(pose.slice(n*9,n*9+3));if(mask[n]&2)p.rotation=Array.from(pose.slice(n*9+3,n*9+6));if(mask[n]&4)p.scale=Array.from(pose.slice(n*9+6,n*9+9));
            if(mask[n]&7)h.place(p);if(mask[n]&8)h.visible(true);if(mask[n]&16)h.visible(false);if(mask[n]&32)h.material({color:colors[n]});if(mask[n]&64)h.material({opacity:opacity[n]});}
          i.evaluated=false;
        },evaluate(camera,light){o.evaluate(Array.from(camera),Array.from(light));i.evaluated=true;},
        readProjection(id,out){++i.readCalls;if(i.failRead)throw Error('projection injected');const node=o.handles[id]&&o.handles[id]._node;
          if(!i.evaluated||!node||!node.worldEnabled||!node.analytic||node.analytic.kind!=='ellipsoid')throw Error('projection requires visible evaluated ellipsoid');
          for(let k=0;k<8;k++)out[k]=node.analytic.work[24+k];
        },submitClipped(ids,counts,xy){
          if(i.failSubmit)throw Error('submit injected');assert.ok(i.evaluated);assert.equal(ids.length,counts.length);assert.equal(xy.length,16*ids.length);
          const map=new Map(),seen=new Set();for(let k=0;k<ids.length;k++){
            assert.ok(!seen.has(ids[k]));seen.add(ids[k]);const node=o.handles[ids[k]]._node;
            assert.ok(node.worldEnabled&&node.analytic.kind==='ellipsoid');assert.ok(counts[k]===0||counts[k]>=3&&counts[k]<=8);
            map.set(ids[k],Array.from(xy.slice(k*16,k*16+counts[k]*2)));
          }
          if(onSubmit)onSubmit();i.lastRefs=[ids,counts,xy];i.last={ids:Array.from(ids),counts:Array.from(counts),xy:Array.from(xy),entries:o.render(map)};++i.submitCalls;
          published=structuredClone(i.last);
        },submit(){throw Error('clip path must use submitClipped');},stats(out){out.nodes=29;out.evaluated=i.evaluated;return out;},
        dispose(){if(!i.closed){i.closed=true;o.scene.dispose();}}};
      m.instances.push(i);return i;
    }};models.push(m);return m;
  }
  const ctx=vm.createContext({ArrayBuffer,Float32Array,Float64Array,Uint16Array,Uint8Array,Int16Array,WeakMap});
  for(const file of ['apps/lib/ayatsuri_model.js','apps/lib/ayatsuri_clip.js','apps/fox3d/fox_face_model.js'])vm.runInContext(fs.readFileSync(path.join(root,file),'utf8'),ctx,{filename:file});
  const light={direction:[-.5,-.4,.8],color:0xffffff},camera={position:[0,0,400],target:[0,0,0],zoom:Math.fround(1.3),center:[120,80],projection:'orthographic'};
  function create(model=makeModel()){return {model,scene:ctx.AyatsuriClip.nativeScene({model,light,camera})};}
  return {ctx,models,makeModel,create,light,camera,onSubmit:f=>onSubmit=f,picture:()=>JSON.stringify(published)};
}
const native=nativeSetup(),created=native.create(),ns=created.scene,controller=native.ctx.FoxFaceModel.create(ns),instance=created.model.instances[0];
assert.equal(typeof native.ctx.Ayatsuri,'undefined');assert.equal(typeof native.ctx.AyatsuriBlend,'undefined','native app imports no SourceA geometry utility');
assert.equal(instance.readCalls,0,'registration must not auto-evaluate');
assert.equal(ns.handle(10),ns.node('sclera-1'),'handle lookup preserves identity');
const expectedOracle=oracle(),pose={yaw:0,pitch:0,smile:0,surprise:0,jawOpen:0},lidIds=[13,14,20,21];let frames=0,refs=null;
for(const yaw of [-35,0,35])for(const pitch of [-10,0,10])for(let j=0;j<=20;j++){
  pose.yaw=yaw*Math.PI/180;pose.pitch=pitch*Math.PI/180;pose.smile=j/20;pose.surprise=0;pose.jawOpen=0;
  controller.update(pose);ns.evaluate(0);ns.render();expectedOracle.model.update(pose);
  for(let id=0;id<29;id++){
    if(!lidIds.includes(id))assert.deepEqual(Array.from(instance.o.handles[id]._node.matrix.slice(12,21)),Array.from(expectedOracle.handles[id]._node.matrix.slice(12,21)),'non-lid v2 authoring pose node'+id);
    if(!lidIds.includes(id))assert.equal(instance.o.handles[id]._node.enabled,expectedOracle.handles[id]._node.enabled);
  }
  if(j===0)assert.ok(lidIds.every(id=>instance.o.handles[id]._node.enabled),'neutral has a thin fur-colored upper rim');
  if(j===20)assert.ok(lidIds.every(id=>instance.o.handles[id]._node.enabled),'full smile keeps the original visible arc');
  if(refs)assert.ok(instance.lastRefs.every((x,k)=>x===refs[k]),'fixed submit buffers reused per frame');else refs=instance.lastRefs;
  if(j===20)assert.equal(instance.last.counts.filter(n=>n===0).length,6,'closed eyes are six EMPTY rows');++frames;
}
assert.deepEqual(instance.last.ids,[2,25,26,3,27,28,10,11,12,17,18,19]);
function width(id){const row=instance.last.ids.indexOf(id),n=instance.last.counts[row],xs=[];for(let k=0;k<n;k++)xs.push(instance.last.xy[row*16+k*2]);return Math.max(...xs)-Math.min(...xs);}
for(const yaw of [-35,35]){
  const p={yaw:yaw*Math.PI/180,viewYaw:0};controller.update(p);ns.evaluate(0);ns.render();const before=[width(10),width(17)];
  p.viewYaw=p.yaw;controller.update(p);ns.evaluate(0);ns.render();const far=yaw>0?1:0,after=[width(10),width(17)];
  assert.ok(after[far]<before[far]*.85,'relative view reduces the far-eye exposure');assert.equal(after[1-far],before[1-far],'near-eye exposure does not shrink');
}
let out={};ns.stats(out);assert.equal(out.objects,29);assert.equal(out.apertures,8);assert.equal(out.clipTargets,12);
assert.throws(()=>ns.add({}),/immutable/);assert.throws(()=>ns.aperture(ns.handle(10),rest),/fixed/);
const old=native.picture();assert.throws(()=>controller.eyes[0].aperture.setVertices(reversed),/vector|Float32|winding/);assert.throws(()=>ns.render(),/evaluate/);assert.equal(native.picture(),old);
controller.update({});ns.evaluate(0);ns.render();ns.stats(out);const nprojects=out.clipProjects;
controller.update({});ns.light({direction:[.4,-.3,.8]});ns.evaluate(0);ns.render();ns.stats(out);assert.equal(out.clipProjects,nprojects,'native same morph + light-only skips projection');
ns.camera({position:[30,0,400]});ns.evaluate(0);ns.render();ns.stats(out);assert.ok(out.clipProjects>nprojects);
assert.throws(()=>ns.light({get direction(){ns.handle(10).visible(false);return [0,0,1];}}),/busy/);
ns.evaluate(0);native.onSubmit(()=>assert.throws(()=>ns.handle(10).place({}),/busy/));ns.render();native.onSubmit(null);
const beforeFail=native.picture();instance.failRead=true;assert.throws(()=>ns.evaluate(0),/projection injected/);assert.throws(()=>ns.render(),/evaluate/);assert.equal(native.picture(),beforeFail);instance.failRead=false;
ns.evaluate(0);instance.failSubmit=true;assert.throws(()=>ns.render(),/submit injected/);assert.equal(native.picture(),beforeFail);instance.failSubmit=false;ns.evaluate(0);ns.render();
ns.handle(10).visible(false);assert.throws(()=>ns.evaluate(0),/visible evaluated/);assert.throws(()=>ns.render(),/evaluate/);ns.handle(10).visible(true);ns.evaluate(0);ns.render();
// Group/blend/hidden bindings are deferred errors, never setup auto-evaluation.
for(const id of [0,5,7]){
  const c=native.create(),ap=c.scene.aperture(c.scene.handle(10),rest);ap.bind(c.scene.handle(id));assert.equal(c.model.instances[0].readCalls,0);
  assert.throws(()=>c.scene.evaluate(0),/visible evaluated/);assert.throws(()=>c.scene.render(),/evaluate/);c.scene.dispose();c.model.dispose();
}
const second=native.create(created.model),secondController=native.ctx.FoxFaceModel.create(second.scene);secondController.update({yaw:.2});second.scene.evaluate(0);second.scene.render();
ns.dispose();ns.dispose();assert.equal(created.model.closed,false,'scene borrows model');
secondController.update({yaw:-.2});second.scene.evaluate(0);second.scene.render();
for(const call of [()=>ns.evaluate(0),()=>ns.render(),()=>controller.root.place({}),()=>controller.eyes[0].aperture.setEmpty(null)])assert.throws(call,/disposed/);
second.scene.dispose();created.model.dispose();expectedOracle.scene.dispose();
console.log('AYATSURI_CLIP_NODE_PASS sourceA=true nativeModel=true sameFrame=true lightOnly=no-project atomicMorph=true EMPTY=explicit getterReentry=guarded dispose=guarded foxSweeps='+frames+' nodes=29 controllerNonLid=v2-authoring-exact farEye=relative-view buffers=reused nativePixels=UNMEASURED warmGuestAlloc=UNMEASURED');
