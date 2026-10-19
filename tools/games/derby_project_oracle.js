// Host-only exact-double and lifetime oracle; never embedded in firmware.
(() => {
  const S=pocket.derby, has=typeof S.project==='function', oldCRS=CRS, oldPc=pc;
  let checked=0, failures=0;
  const check=v=>{if(!v)throw Error('projection contract');};
  const ref=(g,w,off,out)=>{
    for(let i=0;i<g.length;i++){
      const p=pj(pose(g[i]-off,w[i])),r=1/p[1],k=i*4;
      out[k]=p[0];out[k+1]=p[1];out[k+2]=p[0]*r;out[k+3]=r;
    }
  };
  const exact=(g,w,off,out)=>{
    const expected=new Float64Array(g.length*4);ref(g,w,off,expected);
    native(g,w,off,out);
    for(let i=0;i<expected.length;i++)check(Object.is(out[i],expected[i]));
    checked+=g.length;
  };
  const native=has?S.project:null;
  const restore=()=>{CRS=oldCRS;pc=oldPc;S.course(CH,CRS===OC?OC:0,DNR);};
  try {
    const g=[390,402,417,429,440,453,468,481],w=DL.slice(),out=new Float64Array(32),offset=12.5*U;
    const fixedEnds=[0,...Array.from({length:17},(_,i)=>OB+(650-OB)*i/16),1000];
    CRS=OC;S.course(fixedEnds,OC,DNR);pc=[460,-3,.6,.8,1000,120];
    const setView=()=>S.view(pc,250,17,0,0,0,1);
    setView();
    // Both arms include caller reads/packing and consumption. Native includes
    // real JS argument reads and Float64Array validation/writes, no raw-kernel
    // timing shortcut. No renderer or reference oracle is inside these spans.
    const reps=2000;let checksum=0,start=__projectionNs();
    for(let i=0;i<reps;i++){g[0]=390+i%121;ref(g,w,offset,out);checksum+=out[2];}
    const js=__projectionNs()-start;
    let batch=null;
    if(has){
      start=__projectionNs();for(let i=0;i<reps;i++){g[0]=390+i%121;native(g,w,offset,out);checksum+=out[2];}
      batch=(__projectionNs()-start)/reps;
    }
    start=__projectionNs();for(let i=0;i<reps;i++)setView();
    const viewNs=(__projectionNs()-start)/reps;
    console.log('PROJECTION_BENCH '+JSON.stringify({repetitions:reps,points:8,js_ns_per_batch:js/reps,
      native_ns_per_batch:batch,view_ns_per_call:viewNs,registered_ends:fixedEnds.length,
      counters:__projectionStats(),checksum,scope:'host complete JS/native boundary, not target cycles or fps'}));
    if(has){
      // Straight, bends, boundaries, signed zeros, negative depths and camera
      // tracks representing close/follow/orbit motion. No approximate divide.
      for(let i=0;i<2400;i++){
        CRS=i%2?OC:SC;S.course(CH,CRS===OC?OC:0,DNR);
        const a=i*.013;pc=[430+80*Math.cos(a),-20+35*Math.sin(a),Math.sin(a),Math.cos(a),200+i%1301,100];
        setView();
        for(let l=0;l<8;l++){g[l]=i%7===0?[0,OB,650,OB+1e-10,650-1e-10,1000,-0,-20][l]:370+(i%161)+l*.41;w[l]=DL[l];}
        exact(g,w,offset,out);
      }
      CRS=SC;S.course(CH,0,DNR);pc=[0,0,0,1,200,10];setView();
      const zs=[-0,0,.019999,.02,.020001,-.02,1e-12,1e6];
      exact([0,0,0,0,0,0,0,0],zs.map(z=>z*200),0,out);
      const rejected=f=>{let threw=false;try{f();}catch(e){threw=true;}check(threw);++failures;};
      out.fill(123);
      for(const call of [()=>native([],[],0,out),()=>native(Array(33).fill(1),Array(33).fill(1),0,new Float64Array(132)),
        ()=>native([1],[NaN],0,out),()=>native([1],[1],Infinity,out),()=>native([1],[1],0,new Float32Array(4)),
        ()=>native([1],[1],0,new BigInt64Array(4)),()=>native([1],[1],0,new Float64Array(3))])rejected(call);
      check(out.every(v=>v===123));
      const guarded=new Float64Array(6);guarded.fill(123);
      native([1],[1],0,new Float64Array(guarded.buffer,8,4));
      check(guarded[0]===123&&guarded[5]===123);
      const replace=[1];Object.defineProperty(replace,0,{get(){S.course(CH,0,DNR);return 1;}});
      rejected(()=>native(replace,[1],0,out));check(out.every(v=>v===123));setView();
      const detach=[1],detached=new Float64Array(4);
      Object.defineProperty(detach,0,{get(){__projectionDetach(detached.buffer);return 1;}});
      rejected(()=>native(detach,[1],0,detached));
      __projectionReset();rejected(()=>native([1],[1],0,out));
      check(__projectionStats().cache_bytes===0);
      // Bounds and cache lifetime: maximum valid registration, failed
      // replacement, straight replacement, reset; old course survives failure.
      const ends=Array.from({length:32},(_,i)=>i*40);
      S.course(ends,OC,DNR);const bytes=__projectionStats().cache_bytes;
      check(bytes===0||bytes===1024);
      rejected(()=>S.course(ends.concat(1300),OC,DNR));check(__projectionStats().cache_bytes===bytes);
      S.course(ends,0,DNR);check(__projectionStats().cache_bytes===0);
      console.log('PROJECTION_PREFLIGHT '+JSON.stringify({checked_points:checked,rejected_inputs:failures,max_cache_bytes:bytes}));
    }
  } finally {restore();}
  if(has&&globalThis.__projectionEveryCall){
    S.project=(g,w,off,out)=>exact(g,w,off,out);
  }
})();
