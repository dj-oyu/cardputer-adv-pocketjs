// Host-only numerical reference and regressions. Never loaded by an app chunk.
'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const Follow=require('../../apps/lib/ayatsuri_follow.js');
const PacketAdapter=require('./follow_packet_adapter.cjs');
const h=1/60,table=Array.from({length:64},(_,i)=>Math.round(32767*Math.sin(2*Math.PI*i/64)));
function error(code){const e=new Error(code);e.code=code;throw e;}
function valid(a,n){if(!a||a.length!==n)error('INVALID');for(let i=0;i<n;i++)if(!Number.isFinite(a[i]))error('INVALID');}
function coefficients(c){valid(c,14);if(c[12]<=0||c[13]<=0)error('INVALID');}
function narrow(x){const v=Math.fround(x);if(!Number.isFinite(v))error('RANGE');return v;}
const backend={
  followPrepare(c,out){
    valid(c,8);if(c[0]<=0||c[1]<=0)error('INVALID');
    for(let i=2;i<8;i++)if(c[i]<0)error('INVALID');
    const i0=c[0],i1=c[1],kc=c[6],cc=c[7],
      d0=i0+h*c[4]+h*h*c[2],d1=i1+h*c[5]+h*h*c[3],u=h*cc+h*h*kc,
      stableDet=d0*d1+u*(d0+d1),s=Math.max(d0,d1)+2*u,condition=s*s/stableDet;
    if(!Number.isFinite(stableDet)||stableDet<=0||!Number.isFinite(s)||
        !Number.isFinite(condition)||condition>1e6)error('RANGE');
    const k00=c[2]+kc,k01=-kc,k11=c[3]+kc,
      a00=i0+h*(c[4]+cc)+h*h*k00,a01=-h*cc-h*h*kc,a11=i1+h*(c[5]+cc)+h*h*k11,
      det=a00*a11-a01*a01;
    if(!Number.isFinite(det)||det<=0)error('RANGE');
    const inv00=a11/det,inv01=-a01/det,inv11=a00/det;
    const candidate=[inv00*i0,inv01*i1,inv01*i0,inv11*i1,
      h*(inv00*k00+inv01*k01),h*(inv00*k01+inv01*k11),
      h*(inv01*k00+inv11*k01),h*(inv01*k01+inv11*k11),
      h*inv00,h*inv01,h*inv01,h*inv11,i0,i1];
    if(!candidate.every(Number.isFinite))error('RANGE');out.set(candidate);
  },
  followStep(c,state,goal,force){
    coefficients(c);valid(state,4);valid(goal,2);valid(force,2);
    const e0=state[0]-goal[0],e1=state[1]-goal[1],
      v0=c[0]*state[2]+c[1]*state[3]-c[4]*e0-c[5]*e1+c[8]*force[0]+c[9]*force[1],
      v1=c[2]*state[2]+c[3]*state[3]-c[6]*e0-c[7]*e1+c[10]*force[0]+c[11]*force[1],
      q0=state[0]+h*v0,q1=state[1]+h*v1;
    const a=narrow(q0),b=narrow(q1),c0=narrow(v0),d=narrow(v1);
    state[0]=a;state[1]=b;state[2]=c0;state[3]=d;
  },
  followImpulse(c,state,j){
    coefficients(c);valid(state,4);valid(j,2);
    const a=narrow(state[2]+j[0]/c[12]),b=narrow(state[3]+j[1]/c[13]);state[2]=a;state[3]=b;
  },
  followDrive(phase,increment,out){
    valid(phase,3);valid(increment,3);if(!out||out.length!==1)error('INVALID');
    const wave=p=>{const i=p>>>26,f=(p&0x03ffffff)/67108864;
      return (table[i]+(table[(i+1)&63]-table[i])*f)/32767;};
    const sample=narrow(.55*wave(phase[0])+.30*wave(phase[1])+.15*wave(phase[2]));
    out[0]=sample;for(let i=0;i<3;i++)phase[i]=(phase[i]+increment[i])>>>0;
  },
  followCreate(config,initial){
    if(initial===undefined)return PacketAdapter.create(config,{backend:this});
    return PacketAdapter.create({inertia:[config[0],config[1]],stiffness:[config[2],config[3]],
      damping:[config[4],config[5]],couplingStiffness:config[6],couplingDamping:config[7],
      position:[initial[0],initial[1]],velocity:[initial[2],initial[3]],goal:[initial[4],initial[5]]},
      {backend:this});
  },
  driveCreate(seed){
    return PacketAdapter.drive({seed},{backend:this});
  }
};
module.exports={backend};

function run(){
  const desc={inertia:[1,1.6],stiffness:[56,0],damping:[10,0],couplingStiffness:128,couplingDamping:16};
  const make=(d=desc,b=backend)=>Follow.create(d,{backend:b});
  const output=()=>({position:new Float32Array(2),velocity:new Float32Array(2),goal:new Float32Array(2)});
  const read=(actor,out=output())=>{assert.equal(actor.sample(out),out);return {
    position:Array.from(out.position),velocity:Array.from(out.velocity),goal:Array.from(out.goal)};};
  assert.throws(()=>Follow.create(desc),TypeError,'native backend is required; no numerical fallback');
  for(const key of ['position','velocity','goal'])for(const invalid of [null,false,NaN,[0],[0,Infinity]])
    assert.throws(()=>make({...desc,[key]:invalid}),/two components|finite Float32/);
  const actor=make(),start=read(actor),target=new Float32Array([.15,.15]);
  // Conservative supported-condition boundary, matched to the C preparation
  // guard. Rejected preparation must not overwrite the last valid coefficients.
  const prepared=new Float64Array(14),normalConfig=new Float32Array([1,1.6,56,0,10,0,128,16]);
  backend.followPrepare(normalConfig,prepared);
  const preparedBefore=Array.from(prepared),stableState=new Float32Array([.1,-.2,.3,-.4]),
    stateBefore=Array.from(stableState),stableGoal=new Float32Array([.2,.2]),zero=new Float32Array(2);
  const rejectedConfigs=[
    new Float32Array([1,1,0,0,0,0,1e18,0]),
    new Float32Array([1e-20,1e20,0,0,0,0,0,0]),
    new Float32Array([1,1,0,0,0,0,1.8e9,0]),
    new Float32Array([1,1,0,0,0,0,0,1e18])
  ];
  for(const config of rejectedConfigs){
    assert.throws(()=>backend.followPrepare(config,prepared),e=>e.code==='RANGE');
    assert.deepEqual(Array.from(prepared),preparedBefore,'condition rejection preserves prepared output');
    assert.deepEqual(Array.from(stableState),stateBefore,'preparation never changes a running state');
  }
  const expectedRecovery=stableState.slice();
  backend.followStep(new Float64Array(preparedBefore),expectedRecovery,stableGoal,zero);
  backend.followStep(prepared,stableState,stableGoal,zero);
  assert.deepEqual(stableState,expectedRecovery,'prior valid coefficients remain usable after rejected prepare');
  const boundedConfig=new Float32Array([1,1,0,0,0,0,1.7e9,0]),boundedPrepared=new Float64Array(14),
    commonMode=new Float32Array([0,0,1,1]);
  backend.followPrepare(boundedConfig,boundedPrepared);
  backend.followStep(boundedPrepared,commonMode,zero,zero);
  assert.deepEqual(Array.from(commonMode),[Math.fround(1/60),Math.fround(1/60),1,1],
    'supported high coupling preserves common-mode velocity');
  for(const bad of [rejectedConfigs[0],rejectedConfigs[1]]){
    assert.throws(()=>make({inertia:[bad[0],bad[1]],stiffness:[bad[2],bad[3]],damping:[bad[4],bad[5]],
      couplingStiffness:bad[6],couplingDamping:bad[7]}),e=>e.code==='RANGE');
    assert.deepEqual(read(actor),start,'rejected factory cannot corrupt an existing follower');
  }
  const sharingProbe=make();
  for(const method of ['retarget','impulse','step','sample','reset'])assert.equal(actor[method],sharingProbe[method]);
  assert.ok(!Object.values(actor).some(value=>typeof value==='function'),'no per-instance method closures');
  assert.equal(actor._prepared.buffer,actor._state.buffer);assert.equal(actor._goal.buffer,actor._state.buffer);
  assert.equal(actor._packet.buffer,actor._state.buffer);assert.equal(actor._initial.buffer,actor._state.buffer);
  assert.notEqual(actor._state.buffer,sharingProbe._state.buffer,'instance backing stores remain isolated');
  actor.retarget(target);target.fill(99);const held=read(actor);
  assert.deepEqual(held.position,start.position);assert.deepEqual(held.velocity,start.velocity);
  assert.deepEqual(held.goal,[Math.fround(.15),Math.fround(.15)]);
  for(let i=0;i<30;i++)actor.step();
  const flying=read(actor);actor.retarget([-.08,-.08]);
  assert.deepEqual(read(actor).position,flying.position);assert.deepEqual(read(actor).velocity,flying.velocity);
  const beforeInvalid=read(actor);
  for(const invalid of [[.2,NaN],[Infinity,0],[1e100,0],[0],[0,1,2]]){
    assert.throws(()=>actor.retarget(invalid));assert.deepEqual(read(actor),beforeInvalid);
    assert.throws(()=>actor.impulse(invalid));assert.deepEqual(read(actor),beforeInvalid);
    assert.throws(()=>actor.step(invalid));assert.deepEqual(read(actor),beforeInvalid);
  }
  assert.throws(()=>actor.step(new Float32Array([Infinity,0])));assert.deepEqual(read(actor),beforeInvalid);
  assert.throws(()=>actor.impulse(new Float32Array([0,NaN])));assert.deepEqual(read(actor),beforeInvalid);
  actor.step();for(let i=0;i<3600;i++)actor.step();
  const settled=read(actor);
  for(let i=0;i<2;i++){assert.ok(Math.abs(settled.position[i]+.08)<1e-5);assert.ok(Math.abs(settled.velocity[i])<1e-5);}
  actor.reset();assert.deepEqual(read(actor),start);
  const mass=make({...desc,inertia:[1,4]});mass.impulse(new Float32Array([1,1]));
  assert.deepEqual(read(mass).velocity,[1,.25]);assert.deepEqual(read(mass).position,[0,0]);
  const overflow=make({...desc,inertia:[1e-38,1e-38],stiffness:[0,0],damping:[0,0],couplingStiffness:0,couplingDamping:0});
  const untouched=read(overflow);assert.throws(()=>overflow.impulse([3e38,3e38]),/RANGE/);
  assert.deepEqual(read(overflow),untouched);overflow.impulse([1e-38,0]);assert.ok(read(overflow).velocity[0]>0);
  const beforeStepRange=read(overflow);assert.throws(()=>overflow.step(new Float32Array([3e38,0])),/RANGE/);
  assert.deepEqual(read(overflow),beforeStepRange);overflow.step();
  const isolated=make();isolated.retarget([.2,-.1]);for(let i=0;i<90;i++)isolated.step();
  assert.deepEqual(read(actor),start,'independent instances share no mutable motion state');
  assert.throws(()=>actor.sample({position:[0,0],velocity:[0],goal:[0,0]}));
  assert.deepEqual(read(actor),start,'invalid sample output does not change state');
  const coupled=make({...desc,inertia:[1,1],stiffness:[0,0],damping:[0,0],position:[.2,0]});
  coupled.step();const mutual=read(coupled);
  assert.ok(mutual.velocity[0]<0&&mutual.velocity[1]>0);assert.ok(Math.abs(mutual.velocity[0]+mutual.velocity[1])<1e-6);
  let reentrant;
  const guarded={...backend,followStep(...args){
    assert.throws(()=>reentrant.retarget([0,0]),/busy/);assert.throws(()=>reentrant.step(),/busy/);
    assert.throws(()=>reentrant.sample(output()),/busy/);assert.throws(()=>reentrant.reset(),/busy/);
    backend.followStep(...args);
  }};
  reentrant=make(desc,guarded);reentrant.step();
  assert.throws(()=>reentrant.retarget({length:2,get 0(){reentrant.step();return 0;},1:0}),/busy/);
  reentrant.step();
  const drive=Follow.drive({seed:42},{backend}),diagnostic={phases:[0,0,0],increments:[0,0,0],value:0};
  const otherDrive=Follow.drive({seed:42},{backend});
  for(const method of ['step','reset','sample','snapshot'])assert.equal(drive[method],otherDrive[method]);
  assert.ok(!Object.values(drive).some(value=>typeof value==='function'));
  assert.equal(drive._phase.buffer,drive._increment.buffer);assert.equal(drive._phase.buffer,drive._output.buffer);
  assert.notEqual(drive._phase.buffer,otherDrive._phase.buffer);
  const initialDrive=JSON.stringify(drive.snapshot(diagnostic)),trace=[];
  for(let i=0;i<12000;i++){const value=drive.step();assert.ok(value>=-1&&value<=1);trace.push(value);}
  const paused=JSON.stringify(drive.snapshot(diagnostic));
  for(let i=0;i<100;i++)assert.equal(JSON.stringify(drive.snapshot(diagnostic)),paused);
  drive.reset();assert.equal(JSON.stringify(drive.snapshot(diagnostic)),initialDrive);
  for(const expected of trace)assert.equal(drive.step(),expected);
  drive.reset(43);assert.notEqual(JSON.stringify(drive.snapshot(diagnostic)),initialDrive);
  const beforeSample=JSON.stringify(drive.snapshot(diagnostic));
  drive.sample({length:1,set 0(value){
    assert.throws(()=>drive.step(),/busy/);assert.throws(()=>drive.reset(),/busy/);
    assert.throws(()=>drive.snapshot(diagnostic),/busy/);
  }});
  assert.equal(JSON.stringify(drive.snapshot(diagnostic)),beforeSample,'sample cannot advance phase through reentry');
  assert.throws(()=>drive.sample({length:1,set 0(value){throw new Error('output setter');}}),/output setter/);
  assert.equal(JSON.stringify(drive.snapshot(diagnostic)),beforeSample,'sample failure preserves phase/output');
  drive.step(); // Guard was released after the output setter threw.
  for(const invalid of [-1,1.5,Infinity,NaN,0x100000000]){
    const before=JSON.stringify(drive.snapshot(diagnostic));assert.throws(()=>drive.reset(invalid));
    assert.equal(JSON.stringify(drive.snapshot(diagnostic)),before);
  }
  let parity=0,components=0;
  const vectorPath=process.argv[2]||path.resolve(__dirname,'../../.cache/ayatsuri-follow/vectors.json');
  if(fs.existsSync(vectorPath)){
    const vectors=JSON.parse(fs.readFileSync(vectorPath,'utf8'));
    for(const vector of vectors){
      let actual,status='OK',phase;
      try{
        if(vector.op==='drive'){
          phase=new Uint32Array(vector.phase);actual=new Float32Array([77]);
          backend.followDrive(phase,new Uint32Array(vector.increment),actual);
        }else{
          const coeff=new Float64Array(14);backend.followPrepare(new Float32Array(vector.config),coeff);
          if(vector.op==='prepare')actual=coeff;
          else{
            actual=new Float32Array(vector.state);
            if(vector.op==='step')backend.followStep(coeff,actual,new Float32Array(vector.goal),new Float32Array(vector.force));
            else if(vector.op==='impulse')backend.followImpulse(coeff,actual,new Float32Array(vector.impulse));
            else throw new Error('unknown vector operation '+vector.op);
          }
        }
      }catch(e){status=e.code||e.message;}
      assert.equal(status,vector.status||'OK',JSON.stringify(vector));
      if(status==='OK'){
        const expected=vector.op==='prepare'?vector.expected:new Float32Array(vector.expected);
        assert.equal(actual.length,expected.length);
        for(let i=0;i<actual.length;i++){
          assert.ok(Object.is(actual[i],expected[i]),'C/JS stored value/sign mismatch '+JSON.stringify(vector));
          components++;
        }
        if(phase)assert.deepEqual(Array.from(phase),vector.expectedPhase);
      }
      parity++;
    }
  }
  console.log('AYATSURI_FOLLOW_NODE_PASS replay=12000 fixedHz=60 cVectors='+parity+' components='+components+
    (parity?'':' C_PARITY_PENDING')+' nativeAllocation=UNMEASURED');
}
if(require.main===module)run();
