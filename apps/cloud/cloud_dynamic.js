// One shared core; optional diagnostics attach to this same private state.
'use strict';
globalThis.AyatsuriCloudDynamic=(()=>{
  const keys=new Float32Array([0,0,0,0,0,0,0,0,0,120,-.055,.02,.12,-.07,0,0,0,0,
    141,.24,.06,.22,-.15,-.45,1.6,-2.2,-4,257,0,0,0,0,0,0,0,0,
    300,-.025,-.025,.05,-.02,.15,-.6,.8,.8,327,.04,-.04,.1,.16,-.18,.64,-.88,-1.6,390,0,0,0,0,0,0,0,0]),
    config=new Float32Array([1,1.1,32,26,8,7,1.8,.6,.7,1.2,24,18,5,6,.9,.35,
      1.6,.8,18,14,7,4,1.1,.2,1.3,2.1,13,19,4.5,8,.7,.3]);
  function read(s,i){
    s.a[i].sample(s.v);
    for(let k=0;k<2;k++){s.q[i*6+k]=s.v.position[k];s.q[i*6+2+k]=s.v.velocity[k];s.q[i*6+4+k]=s.v.goal[k];}
  }
  function reset(s){
    s.d.reset(s.seed);
    for(let i=0;i<4;i++){s.a[i].reset();read(s,i);}
    s.n=0;s.i=-1;s.z=false;
  }
  function step(s,b){
    let i=s.i<0?0:s.i;
    while(i+9<keys.length&&keys[i+9]<=s.n+1)i+=9;
    if(!s.fixture&&i!==s.i){
      for(let k=0;k<4;k++){s.g[0]=keys[i+1+k*2];s.g[1]=keys[i+2+k*2];s.a[k].retarget(s.g);}
      s.i=i;
    }
    const e=s.q[0]-s.q[4],v=s.q[2],d=s.d.step();
    // Phase advances with simulation; OFF has no force. Children get only
    // the previous accepted root response, with asymmetric model coefficients.
    s.f[0]=b?d*.09:0;s.f[1]=b?d*.025:0;s.a[0].step(s.f);
    s.f[0]=e*2.2+v*.3;s.f[1]=-e*1.5-v*.2;s.a[1].step(s.f);
    s.f[0]=e*4+v*.8;s.f[1]=e*7.5+v*1.4;s.a[2].step(s.f);
    s.f[0]=-e*5-v*.6;s.f[1]=e*9+v*1.1;s.a[3].step(s.f);
    for(let k=0;k<4;k++)read(s,k);
    ++s.n;++s.w;
  }
  function create(pure,options){
    options=options||{};
    const seed=options.seed===undefined?42:options.seed,o={backend:options.backend};
    if(typeof pure!=='function'||seed!==(seed>>>0))throw new TypeError('CloudDynamic: invalid sampler/seed');
    // n: step, i: intent offset, t/b: accepted clock/config, l/r/w: work counts,
    // z: invalid private state. No per-frame construction or diagnostic closures.
    const s={a:[],g:new Float32Array(2),f:new Float32Array(2),q:new Float32Array(24),
      v:{position:new Float32Array(2),velocity:new Float32Array(2),goal:new Float32Array(2)},
      d:AyatsuriFollow.drive({seed},o),seed,n:0,i:-1,t:null,b:false,
      l:0,r:0,w:0,z:false,busy:false,external:false,fixture:false};
    // Config is already packed; the native factory snapshots each narrow view.
    const backend=options.backend||pocket.ayatsuri,initial=new Float32Array(6);
    for(let i=0;i<4;i++){
      s.a.push(backend.followCreate(new Float32Array(config.buffer,i*32,8),initial));
      read(s,i);
    }
    function sample(t,m,b,out){
      if(s.busy)throw new Error('CloudDynamic: busy');s.busy=true;
      let changed=false;
      try{
        if(!Number.isFinite(t)||t<0||t>86400000||!Number.isInteger(m)||m<0||m>5||
          typeof b!=='boolean'||!out||!out.body||!out.expression)throw new TypeError('CloudDynamic: invalid clock/input');
        if(m!==0)return pure(t,m,b,out);
        const n=Math.floor(t*.06+1e-7),r=s.z||s.t===null||t<s.t||b!==s.b||n-s.n>120;
        if(r&&s.external)throw new RangeError('CloudDynamic: explicit reset required after external commands');
        if(r&&n>1200)throw new RangeError('CloudDynamic: seek replay exceeds 1200 steps (20 seconds)');
        s.l=0;
        if(r){changed=true;reset(s);++s.r;}
        while(s.n<n){changed=true;step(s,b);++s.l;}
        pure(t,m,b,out);
        const q=out.body;
        q.tilt=s.q[0];q.head=s.q[1];q.handLeft=s.q[6];q.handRight=s.q[7];
        q.wispCenter=s.q[12];q.wispLeft=s.q[13];q.wispRight=s.q[18];q.haloOffset=s.q[19];
        s.t=t;s.b=b;return out;
      }catch(e){if(changed)s.z=true;throw e;}finally{s.busy=false;}
    }
    sample.dynamic=true;
    if(typeof AyatsuriCloudDynamicDiagnostics!=='undefined')AyatsuriCloudDynamicDiagnostics.attach(sample,s,reset,read);
    if(typeof AyatsuriCloudFixtureDynamic!=='undefined')AyatsuriCloudFixtureDynamic.attach(sample,s,reset,read);
    return sample;
  }
  return {create,stepHz:60,maxReplaySteps:1200,maxForwardSteps:120,maxTimeMs:86400000};
})();
