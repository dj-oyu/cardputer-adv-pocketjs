// Adapter tests use the explicit host reference; native parity is tested separately.
'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=path.resolve(__dirname,'../..'),Follow=require('../../apps/lib/ayatsuri_follow.js'),
  reference=require('./test_follow.cjs').backend;
const context=vm.createContext({AyatsuriFollow:Follow});
for(const name of ['cloud_motion.js','cloud_dynamic.js','cloud_dynamic_diagnostics.js'])
  vm.runInContext(fs.readFileSync(path.join(root,'apps/cloud',name),'utf8'),context,{filename:name});
const Dynamic=context.AyatsuriCloudDynamic,pure=context.AyatsuriCloudMotion;
let count=0,driveCount=0,failAt=-1;
const backend=Object.assign({},reference,{
  followStep(...args){if(++count===failAt)throw new Error('injected step failure');return reference.followStep(...args);},
  followDrive(...args){++driveCount;return reference.followDrive(...args);}
});
function inputs(){return {body:{tilt:0,turn:0,stretch:1,lift:0,head:0,tail:0,haloOffset:0},
  expression:{open:1,blink:0}};}
function actor(seed=42){return Dynamic.create(pure,{backend,seed});}
function state(sample){return sample.status({});}
function body(sample,time,breathing=false){const out=inputs();sample(time,0,breathing,out);return out;}
function snapshot(value){return JSON.parse(JSON.stringify(value));}
function unchanged(before,sample){const after=state(sample);assert.deepEqual(after,before);}

// Four pair descriptions initialize against the reference and remain independent.
const a=actor(),b=actor(),out=inputs();
assert.equal(a.dynamic,true);assert.equal(Dynamic.stepHz,60);
for(let frame=0;frame<=300;frame++)a(frame*1000/30,0,false,out);
const sequential=snapshot(out),at10=state(a);
assert.equal(at10.simulationStep,600);assert.equal(count,2400);assert.equal(driveCount,600);
assert.notEqual(out.body.handLeft,-out.body.handRight,'left/right responses are not mirror copies');
const beforePause=state(a),stepsBefore=count,driveBefore=driveCount;
for(let repeat=0;repeat<5;repeat++)a(10000,0,false,out);
assert.equal(count,stepsBefore);assert.equal(driveCount,driveBefore);
assert.equal(state(a).lastSubsteps,0);assert.equal(state(a).totalSteps,beforePause.totalSteps);
assert.deepEqual(snapshot(out),sequential,'same-time redraw does not resimulate');
// A renderer can fail after sample returns. Retrying the same frame is read-only.
try{throw new Error('synthetic render failure');}catch(error){assert.match(error.message,/render/);}
a(10000,0,false,out);assert.equal(count,stepsBefore);assert.equal(driveCount,driveBefore);

const direct=body(b,10000);assert.deepEqual(snapshot(direct),sequential,'bounded fresh replay matches 30Hz rendering');
const c=actor();
for(const frame of [0,1,6,17,66,83,123,171,210,263,300])body(c,frame*1000/30);
assert.deepEqual(snapshot(body(c,10000)),sequential,'irregular presentation cadence does not change simulation');
body(c,1500);assert.equal(state(c).simulationStep,90);
assert.deepEqual(snapshot(body(c,10000)),sequential,'reverse seek resets seed and deterministically replays');

// Oversized forward/reverse seek rejects before resetting, advancing or writing inputs.
const beforeRejected=state(c),untouched={body:{marker:123},expression:{marker:456}};
assert.throws(()=>c(25000,0,false,untouched),/1200 steps/);
unchanged(beforeRejected,c);assert.deepEqual(untouched,{body:{marker:123},expression:{marker:456}});
const long=actor();body(long,20000);body(long,21000);
assert.equal(state(long).simulationStep,1260,'ordinary small forward steps can run past replay horizon');
const beyond=state(long);assert.throws(()=>body(long,20500),/1200 steps/);unchanged(beyond,long);
for(const time of [-1,NaN,Infinity,86400001])assert.throws(()=>body(c,time));
unchanged(beforeRejected,c);

// The other five modes still call the original pure sampler and do not advance q/v.
const pureBefore=state(c),nativeBefore=count,phaseBefore=driveCount;
for(let mode=1;mode<6;mode++){
  const expected=inputs(),actual=inputs();pure(3500,mode,false,expected);c(3500,mode,false,actual);
  assert.deepEqual(snapshot(actual),snapshot(expected));
}
unchanged(pureBefore,c);assert.equal(count,nativeBefore);assert.equal(driveCount,phaseBefore);

// Held rest goals and drive OFF converge by dynamics, without a snap at eight seconds.
const rest=actor();body(rest,10000);const residual10=state(rest);
body(rest,20000);const residual20=state(rest);
assert.ok(residual10.wispResidual>residual20.wispResidual);
assert.ok(residual20.bodyResidual<1e-5&&residual20.bodyVelocity<1e-5);
assert.ok(residual20.handResidual<1e-5&&residual20.handVelocity<1e-5);
assert.ok(residual20.wispResidual<1e-4&&residual20.wispVelocity<1e-4);
const alive=actor();body(alive,20000,true);
assert.ok(state(alive).bodyResidual>residual20.bodyResidual,'one enabled root drive keeps a bounded live response');

// A deliberate external impulse preserves q, changes v once and invalidates replay history.
const kicked=actor(),base=body(kicked,3000),baseVelocity=state(kicked).bodyVelocity;
kicked.impulse('body',new Float32Array([.04,-.02]));
assert.equal(body(kicked,3000).body.tilt,base.body.tilt);
assert.ok(state(kicked).bodyVelocity!==baseVelocity);
const afterKick=state(kicked);body(kicked,3000);assert.equal(state(kicked).totalSteps,afterKick.totalSteps);
assert.notEqual(body(kicked,1000*181/60).body.tilt,base.body.tilt);
const beforeHistoryReject=state(kicked);
assert.throws(()=>body(kicked,2500),/explicit reset/);unchanged(beforeHistoryReject,kicked);
kicked.reset();assert.deepEqual(snapshot(body(kicked,3000)),snapshot(base));
assert.deepEqual(snapshot(body(b,10000)),sequential,'another model actor was not affected by the impulse');

// Public callbacks/getters cannot reenter while validation, pure sampling or status is active.
const guarded=actor();body(guarded,0);
const accessor={get body(){guarded.reset();return {};},expression:{}};
assert.throws(()=>guarded(0,0,false,accessor),/busy/);
assert.doesNotThrow(()=>body(guarded,0));
assert.throws(()=>guarded.status({set simulationStep(value){guarded.reset();}}),/busy/);
assert.doesNotThrow(()=>body(guarded,0));
const reentrant=Dynamic.create((time,mode,breathing,input)=>{reentrant.reset();return input;},{backend});
assert.throws(()=>reentrant(0,1,false,inputs()),/busy/);

// A partial four-pair failure is private/invalid, not a false aggregate rollback.
const recovery=actor();body(recovery,0);const failedInputs=inputs(),saved=snapshot(failedInputs);
failAt=count+2;assert.throws(()=>recovery(1000/60,0,false,failedInputs),/injected/);
assert.equal(state(recovery).invalid,true);assert.deepEqual(snapshot(failedInputs),saved);
failAt=-1;assert.doesNotThrow(()=>body(recovery,1000/60));assert.equal(state(recovery).invalid,false);
console.log('CLOUD_DYNAMIC_NODE_PASS fourPairs=4 fixedHz=60 sameTime/replay/bounds/driveOff/impulse/guards/recovery');
console.log(JSON.stringify({restAt10:residual10,restAt20:residual20}));

// Production omits only channels overwritten by the dynamic core. Every face
// and auxiliary shape value must remain bit-for-bit equal to the legacy sampler.
vm.runInContext(fs.readFileSync(path.join(root,'apps/cloud/cloud_pose.js'),'utf8'),context,{filename:'cloud_pose.js'});
const compact=context.AyatsuriCloudMotion,overridden=new Set(['tilt','head','handLeft','handRight','wispCenter','wispLeft','wispRight','haloOffset']);
let values=0;
for(const breathing of [false,true])for(let mode=0;mode<6;mode++)for(let frame=0;frame<=900;frame++){
  const a=inputs(),b=inputs(),t=frame*1000/30;pure(t,mode,breathing,a);compact(t,mode,breathing,b);
  assert.deepEqual(a.expression,b.expression);
  for(const key in a.body)if(mode!==0||!overridden.has(key)){assert.equal(b.body[key],a.body[key],key+' @'+frame);++values;}
}
// The production adapter has no diagnostic closures, but runs the same core.
const production=vm.createContext({AyatsuriFollow:Follow});
for(const name of ['cloud_pose.js','cloud_dynamic.js'])vm.runInContext(fs.readFileSync(path.join(root,'apps/cloud',name),'utf8'),production);
const minimal=production.AyatsuriCloudDynamic.create(production.AyatsuriCloudMotion,{backend}),full=actor();
assert.equal(minimal.status,undefined);assert.equal(minimal.reset,undefined);assert.equal(minimal.impulse,undefined);
for(let frame=0;frame<=300;frame++){
  const a=inputs(),b=inputs(),t=frame*1000/30;minimal(t,0,false,a);full(t,0,false,b);assert.deepEqual(snapshot(a),snapshot(b));
}
console.log('CLOUD_COMPACT_POSE_PARITY_PASS values='+values+' fullFrameExpression=10812 productionCore=301');
// Narrowing immutable tables early must match the former plain-table route:
// native goal/config storage was already Float32 before any numerical step.
const typedSource=fs.readFileSync(path.join(root,'apps/cloud/cloud_dynamic.js'),'utf8'),
  plainSource=typedSource.replace(/new Float32Array\((\[[\s\S]*?\])\)/g,'$1'),
  plainContext=vm.createContext({AyatsuriFollow:Follow});
assert.notEqual(plainSource,typedSource,'exercise both table representations');
vm.runInContext(plainSource,plainContext);
for(const breathing of [false,true]){
  const typed=Dynamic.create(compact,{backend}),plain=plainContext.AyatsuriCloudDynamic.create(compact,{backend});
  for(let frame=0;frame<=300;frame++){
    const a=inputs(),b=inputs(),t=frame*1000/30;typed(t,0,breathing,a);plain(t,0,breathing,b);
    assert.deepEqual(a,b,'early Float32 table narrowing @'+frame);
  }
}
console.log('CLOUD_F32_TABLE_PARITY_PASS frames=602 exactNumberEquality=true');
