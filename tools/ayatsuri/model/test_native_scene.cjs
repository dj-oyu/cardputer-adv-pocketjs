// JS authoring/batch contracts and extracted update parity; native math is separate.
'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const {exportCloud,ROOT,REF,git}=require('./export_cloud.cjs'),{generate}=require('./prototype_cloud.cjs');
const manifest=exportCloud();generate();
const Native=require('../../../apps/lib/ayatsuri_model.js');
function mockModel(){
  const instances=[];let disposed=false;
  const model={count:19,node(name){if(disposed)throw Error('disposed');return manifest.nodes.findIndex(n=>n.name===name);},dispose(){disposed=true;},instance(){
    if(disposed)throw Error('disposed');
    const pose=manifest.nodes.flatMap(n=>n.pose),colors=manifest.nodes.map(n=>n.color||0),opacity=manifest.nodes.map(n=>n.opacity||0);
    const i={pose,colors,opacity,targetsCount:0,evalCount:0,submitted:0,dead:false,failTargets:false,
      targets(mask,p,c,a){if(i.failTargets)throw Error('target injected');
        for(let n=0;n<19;n++){const f=mask[n];assert.ok((f&~127)===0);for(let k=0;k<9;k++)if(f&(1<<Math.floor(k/3)))assert.ok(Number.isFinite(p[n*9+k]));}
        for(let n=0;n<19;n++){const f=mask[n];for(let k=0;k<9;k++)if(f&(1<<Math.floor(k/3)))pose[n*9+k]=p[n*9+k];if(f&32)colors[n]=c[n];if(f&64)opacity[n]=a[n];}
        ++i.targetsCount;
      },evaluate(camera,light){assert.equal(camera.length,9);assert.equal(light.length,6);i.camera=Array.from(camera);i.light=Array.from(light);++i.evalCount;},
      submit(){++i.submitted;},stats(out){out.nodes=19;out.evaluated=!!i.evalCount;return out;},readPose(id,out){out.set(pose.slice(id*9,id*9+9));},dispose(){i.dead=true;}};
    instances.push(i);return i;
  }};return {model,instances};
}
const f=mockModel(),a=Native.scene({model:f.model}),b=Native.scene({model:f.model});
assert.equal(Object.getPrototypeOf(a.handle(1)),Object.getPrototypeOf(b.handle(1)));
a.handle(1).place({position:[1,2,3]});assert.throws(()=>a.handle(1).place({position:[9,9,9],scale:[1,0,1]}));
assert.deepEqual(Array.from(a.poses.slice(9,12)),[1,2,3],'invalid patch leaves pending target intact');
assert.throws(()=>a.handle(1).material({opacity:.3,roughness:.2}));assert.equal(a.mask[1],1);
assert.throws(()=>a.handle(1).material({color:0xffffff,opacity:NaN}));assert.equal(a.mask[1],1);
a.handle(1).material({color:0x123456,opacity:.4});a.handle(1).visible(false);assert.equal(a.mask[1],1+32+64+8+16);
assert.throws(()=>a.render(),/evaluate/);f.instances[0].failTargets=true;assert.throws(()=>a.evaluate(0),/injected/);assert.ok(a.mask[1]);
f.instances[0].failTargets=false;a.evaluate(0);assert.equal(a.mask[1],0);a.render();assert.equal(f.instances[0].submitted,1);
assert.deepEqual(f.instances[1].pose,manifest.nodes.flatMap(n=>n.pose),'shared model instances have independent poses');
const before=Array.from(a.cameraState);assert.throws(()=>a.camera({position:[4,5,6],zoom:0}));assert.deepEqual(Array.from(a.cameraState),before);
a.light({direction:[-.5,-.4,.8],color:0xffffff});a.camera({zoom:.85,center:[120,68],projection:'orthographic'});a.evaluate(1);
const l=Math.hypot(-.5,-.4,.8);assert.deepEqual(f.instances[0].light.slice(0,3),[-.5,-.4,.8].map(x=>Math.fround(x/l)));
assert.throws(()=>a.handle(1).place({get position(){a.dispose();return [0,0,0];}}),/busy/);
f.model.dispose();b.handle(2).place({rotation:[0,.1,0]});b.evaluate(0);b.render();assert.equal(f.instances[1].submitted,1,'existing instances survive model disposal');
a.dispose();assert.throws(()=>a.handle(1));assert.throws(()=>a.evaluate(0));assert.equal(f.instances[0].dead,true);b.dispose();
// Compare every authored operation and its order to the original pinned updater.
function updater(native){
  const operations=[],ctx=vm.createContext({AyatsuriCloudMotion(){}}),handles=[];
  for(let i=0;i<19;i++)handles.push({place(p){operations.push(['place',i,JSON.parse(JSON.stringify(p))]);},material(p){operations.push(['material',i,JSON.parse(JSON.stringify(p))]);}});
  let model;
  if(native){vm.runInContext(fs.readFileSync(path.join(ROOT,'apps/cloud/cloud_shared_update.js'),'utf8'),ctx);model=ctx.AyatsuriCloudUpdate.create({handle:i=>handles[i]},0);}
  else{vm.runInContext(git(REF,'apps/cloud/cloud_model.js'),ctx);let index=0;model=ctx.AyatsuriModels.create({add(){return handles[index++];}},0);assert.equal(index,19);}
  vm.runInContext(git(REF,'apps/cloud/cloud_pose.js'),ctx);
  return {frame(ms){operations.length=0;const inputs={body:{},expression:{}};ctx.AyatsuriCloudMotion(ms,0,false,inputs);model.update(ms,inputs.body,inputs.expression);return JSON.stringify(operations);}};
}
const original=updater(false),extracted=updater(true);for(let tick=0;tick<=300;tick++)assert.equal(extracted.frame(tick*1000/30),original.frame(tick*1000/30),'extracted update tick'+tick);
console.log('AYATSURI_NATIVE_JS_PASS atomicPatch=true retryDirty=true sharedModel=2 independentDispose=true update301=exact nativeMath=UNMEASURED');
