// HOST ONLY: packet-backed adapter for the numerical reference tests.
// Product apps use managed native factories instead; never load this as a chunk.
'use strict';
module.exports=(()=>{
  function fail(message){throw new TypeError('AyatsuriFollow: '+message);}
  function scalar(value){
    if(typeof value!=='number'||!Number.isFinite(value)||!Number.isFinite(Math.fround(value)))
      fail('finite Float32 value required');
    return Math.fround(value);
  }
  function pair(value){if(!value||value.length!==2)fail('two components required');}
  function backendFor(options,drive){
    const backend=options&&options.backend||
      (typeof pocket!=='undefined'?pocket.ayatsuri:null);
    if(!backend||(drive?typeof backend.followDrive!=='function':
        typeof backend.followPrepare!=='function'||typeof backend.followStep!=='function'||
        typeof backend.followImpulse!=='function'))fail('shared native follow backend required');
    return backend;
  }
  const zeroForce=new Float32Array(2);
  function followerIdle(actor){if(actor._busy)fail('busy follower');}
  const FollowerPrototype={
    retarget(value){
      followerIdle(this);this._busy=true;
      try{
        pair(value);const a=scalar(value[0]),b=scalar(value[1]);
        this._goal[0]=a;this._goal[1]=b;
      }finally{this._busy=false;}
    },
    impulse(value){
      followerIdle(this);this._busy=true;
      try{
        if(value instanceof Float32Array){
          pair(value);this._backend.followImpulse(this._prepared,this._state,value);return;
        }
        pair(value);const a=scalar(value[0]),b=scalar(value[1]);
        this._packet[0]=a;this._packet[1]=b;
        this._backend.followImpulse(this._prepared,this._state,this._packet);
      }finally{this._busy=false;}
    },
    step(force){
      followerIdle(this);this._busy=true;
      try{
        if(force instanceof Float32Array){
          pair(force);this._backend.followStep(this._prepared,this._state,this._goal,force);return;
        }
        if(force!==undefined){
          pair(force);const a=scalar(force[0]),b=scalar(force[1]);this._packet[0]=a;this._packet[1]=b;
        }
        this._backend.followStep(this._prepared,this._state,this._goal,force===undefined?zeroForce:this._packet);
      }finally{this._busy=false;}
    },
    sample(out){
      followerIdle(this);this._busy=true;
      try{
        if(!out)fail('sample output required');pair(out.position);pair(out.velocity);pair(out.goal);
        out.position[0]=this._state[0];out.position[1]=this._state[1];
        out.velocity[0]=this._state[2];out.velocity[1]=this._state[3];
        out.goal[0]=this._goal[0];out.goal[1]=this._goal[1];return out;
      }finally{this._busy=false;}
    },
    reset(){
      followerIdle(this);
      for(let i=0;i<4;i++)this._state[i]=this._initial[i];
      this._goal[0]=this._initial[4];this._goal[1]=this._initial[5];
    }
  };
  function create(desc,options){
    if(!desc)fail('description required');
    const backend=backendFor(options,false),config=new Float32Array(8),
      actor=Object.create(FollowerPrototype),storage=new ArrayBuffer(168);
    // Private implementation fields, not a public borrowed-buffer interface.
    // All methods require their owning receiver; never bind a per-actor closure.
    actor._backend=backend;actor._busy=false;
    actor._prepared=new Float64Array(storage,0,14);actor._state=new Float32Array(storage,112,4);
    actor._goal=new Float32Array(storage,128,2);actor._packet=new Float32Array(storage,136,2);
    actor._initial=new Float32Array(storage,144,6);
    pair(desc.inertia);pair(desc.stiffness);pair(desc.damping);
    config[0]=scalar(desc.inertia[0]);config[1]=scalar(desc.inertia[1]);
    config[2]=scalar(desc.stiffness[0]);config[3]=scalar(desc.stiffness[1]);
    config[4]=scalar(desc.damping[0]);config[5]=scalar(desc.damping[1]);
    config[6]=scalar(desc.couplingStiffness===undefined?0:desc.couplingStiffness);
    config[7]=scalar(desc.couplingDamping===undefined?0:desc.couplingDamping);
    if(config[0]<=0||config[1]<=0)fail('positive inertia required');
    for(let i=2;i<8;i++)if(config[i]<0)fail('nonnegative stiffness/damping required');
    if(desc.position!==undefined){pair(desc.position);actor._state[0]=scalar(desc.position[0]);actor._state[1]=scalar(desc.position[1]);}
    if(desc.velocity!==undefined){pair(desc.velocity);actor._state[2]=scalar(desc.velocity[0]);actor._state[3]=scalar(desc.velocity[1]);}
    if(desc.goal!==undefined){pair(desc.goal);actor._goal[0]=scalar(desc.goal[0]);actor._goal[1]=scalar(desc.goal[1]);}
    for(let i=0;i<4;i++)actor._initial[i]=actor._state[i];
    actor._initial[4]=actor._goal[0];actor._initial[5]=actor._goal[1];
    backend.followPrepare(config,actor._prepared);
    return actor;
  }
  function driveIdle(driver){if(driver._busy)fail('busy drive');}
  const DrivePrototype={
    reset(value){
      driveIdle(this);if(value===undefined)value=this._seed;
      if(typeof value!=='number'||value!==(value>>>0))fail('uint32 seed required');
      this._seed=value;let current=value;
      for(let i=0;i<3;i++){current=(Math.imul(1664525,current)+1013904223)>>>0;this._phase[i]=current;}
      this._output[0]=0;
    },
    step(){
      driveIdle(this);this._busy=true;
      try{this._backend.followDrive(this._phase,this._increment,this._output);return this._output[0];}
      finally{this._busy=false;}
    },
    sample(out){
      driveIdle(this);this._busy=true;
      try{
        if(!out||out.length!==1)fail('one component output required');out[0]=this._output[0];return out;
      }finally{this._busy=false;}
    },
    snapshot(out){
      driveIdle(this);this._busy=true;
      try{
        if(!out||!out.phases||out.phases.length!==3||!out.increments||out.increments.length!==3)
          fail('drive snapshot output required');
        for(let i=0;i<3;i++){out.phases[i]=this._phase[i];out.increments[i]=this._increment[i];}
        out.value=this._output[0];return out;
      }finally{this._busy=false;}
    }
  };
  function drive(desc,options){
    desc=desc||{};
    const driver=Object.create(DrivePrototype),storage=new ArrayBuffer(28);
    driver._backend=backendFor(options,true);driver._busy=false;driver._seed=42;
    driver._phase=new Uint32Array(storage,0,3);driver._increment=new Uint32Array(storage,12,3);
    driver._output=new Float32Array(storage,24,1);
    driver._increment[0]=22190664;driver._increment[1]=52255435;driver._increment[2]=80888551;
    driver.reset(desc.seed===undefined?42:desc.seed);
    return driver;
  }
  return {create,drive,stepHz:60};
})();
