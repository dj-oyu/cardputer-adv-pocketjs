// Host-only exact-double and lifetime oracle; never embedded in firmware.
(() => {
  const S=pocket.derby, has=typeof S.project==='function', oldCRS=CRS, oldPc=pc;
  let checked=0, failures=0;
  const check=v=>{if(!v)throw Error('projection contract');};
  const rejected=(f,code)=>{let error;try{f();}catch(e){error=e;}
    check(error&&(!code||error.code===code));++failures;};
  const cacheEnabled=__projectionStats().cache_enabled;
  check(typeof cacheEnabled==='boolean');
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
    check(__projectionStats().cache_bytes===(cacheEnabled?fixedEnds.length*32:0));
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
      // Actual section boundaries: zero subtract keeps OB/650 and both sides
      // on the intended samples rather than shifting them by 12.5*U.
      CRS=OC;S.course(fixedEnds,OC,DNR);setView();
      const eps=2**-20,boundaries=[OB-eps,OB,OB+eps,650-eps,650,650+eps];
      check(boundaries[0]<OB&&boundaries[2]>OB&&boundaries[3]<650&&boundaries[5]>650);
      exact(boundaries,Array(6).fill(DFR),0,out);
      CRS=SC;S.course(CH,0,DNR);pc=[0,0,0,1,200,10];setView();
      const zs=[-0,0,.019999,.02,.020001,-.02,1e-12,1e6];
      exact([0,0,0,0,0,0,0,0],zs.map(z=>z*200),0,out);
      out.fill(123);
      for(const call of [()=>native([],[],0,out),()=>native(Array(33).fill(1),Array(33).fill(1),0,new Float64Array(132)),
        ()=>native([1],[NaN],0,out),()=>native([1],[1],Infinity,out),()=>native([1],[1],0,new Float32Array(4)),
        ()=>native([1],[1],0,new BigInt64Array(4)),()=>native([1],[1],0,new Float64Array(3))])rejected(call);
      check(out.every(v=>v===123));
      for(const [progress,depths] of [[[1,2,NaN],[1,1,1]],[[1,2,3],[1,1,Infinity]]]){
        rejected(()=>native(progress,depths,0,out),'INVALID_ARGUMENT');
        check(out.every(v=>v===123));
      }
      // Native writes must respect QuickJS's immutable ArrayBuffer backing,
      // including Float64Array subviews created through both public APIs.
      for(const method of ['sliceToImmutable','transferToImmutable']){
        const backing=new ArrayBuffer(48);new Float64Array(backing).fill(123);
        const immutable=backing[method](),view=new Float64Array(immutable,8,4);
        const sentinel=Array.from(new Uint8Array(immutable));
        check(immutable.immutable===true);
        let error;try{native([1],[1],0,view);}catch(e){error=e;}
        check(error&&error.code==='INVALID_ARGUMENT');++failures;
        check(new Uint8Array(immutable).every((v,i)=>v===sentinel[i]));
      }
      const guarded=new Float64Array(6);guarded.fill(123);
      native([1],[1],0,new Float64Array(guarded.buffer,8,4));
      check(guarded[0]===123&&guarded[5]===123);
      const replace=[1];Object.defineProperty(replace,0,{get(){S.course(CH,0,DNR);return 1;}});
      rejected(()=>native(replace,[1],0,out),'BUSY');check(out.every(v=>v===123));setView();
      const updateView=[1,2,3];Object.defineProperty(updateView,1,{get(){setView();return 2;}});
      rejected(()=>native(updateView,[1,1,1],0,out),'BUSY');check(out.every(v=>v===123));
      const detach=[1],detached=new Float64Array(4);
      Object.defineProperty(detach,0,{get(){__projectionDetach(detached.buffer);return 1;}});
      rejected(()=>native(detach,[1],0,detached));
    }
    // Run course lifetime checks in every independent mode, including cache-only.
    const ends=Array.from({length:32},(_,i)=>i*40);
    CRS=OC;S.course(ends,OC,DNR);setView();
    const bytes=__projectionStats().cache_bytes;
    check(bytes===(cacheEnabled?1024:0));
    rejected(()=>S.course(ends.concat(1300),OC,DNR),'LIMIT_EXCEEDED');
    check(__projectionStats().cache_bytes===bytes);
    rejected(()=>S.course([0,OB,NaN],0,DNR),'INVALID_ARGUMENT');
    check(__projectionStats().cache_bytes===bytes);
    S.ser(0,DFR,12,0,4,0xad55,4.9); // Failed registration retained the usable view.
    if(has){
      const progress=[OB-1,OB+1,651],depths=[DFR,DFR,DFR],expected=new Float64Array(12),result=new Float64Array(12),reads=[];
      ref(progress,depths,0,expected);
      const partial=progress.slice();
      Object.defineProperty(partial,0,{get(){reads.push(0);return progress[0];}});
      Object.defineProperty(partial,1,{get(){reads.push(1);
        rejected(()=>S.course([0,OB,NaN],0,DNR),'INVALID_ARGUMENT');return progress[1];}});
      native(partial,depths,0,result);
      check(reads.join(',')==='0,1'&&__projectionStats().cache_bytes===bytes);
      check(result.every((v,i)=>Object.is(v,expected[i])));checked+=progress.length;
    }
    S.course(ends,0,DNR);check(__projectionStats().cache_bytes===0);
    __projectionReset();check(__projectionStats().cache_bytes===0);
    if(has)rejected(()=>native([1],[1],0,out),'BUSY');
    console.log('PROJECTION_PREFLIGHT '+JSON.stringify({checked_points:checked,rejected_inputs:failures,
      cache_enabled:cacheEnabled,max_cache_bytes:bytes,lifetime_checks:true}));
  } finally {restore();}
  if(has&&globalThis.__projectionEveryCall){
    S.project=(g,w,off,out)=>exact(g,w,off,out);
  }
})();
